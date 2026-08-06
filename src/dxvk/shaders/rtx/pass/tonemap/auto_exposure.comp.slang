/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx/pass/tonemap/tonemapping.slangh"

layout(binding = AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT)
RWTexture1D<uint> InOutHistogram;

layout(binding = AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT)
RWTexture1D<float> OutExposure;

layout(binding = AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT)
RWTexture2D<float4> DebugView;

layout(binding = AUTO_EXPOSURE_DEBUG_STATS_OUTPUT)
RWStructuredBuffer<AutoExposureDebugStats> OutDebugStats;

layout(push_constant)
ConstantBuffer<ToneMappingAutoExposureArgs> cb;

groupshared float g_bins[EXPOSURE_HISTOGRAM_SIZE];

// EV100 at the centre of a histogram bin.
float binCentreEV(uint index)
{
  return histogramOffsetToNormalizedEV(float(index) + 0.5f) * cb.evRange + cb.evMinValue;
}

[shader("compute")]
[numthreads(EXPOSURE_HISTOGRAM_SIZE, 1, 1)]
void main(uint2 threadId : SV_DispatchThreadID, uint linearIndex : SV_GroupIndex)
{
  // Pull the histogram into groupshared before anyone resets it, so the serial walk below
  // reads a stable snapshot.
  g_bins[linearIndex] = float(InOutHistogram[linearIndex]);

  GroupMemoryBarrierWithGroupSync();

  // Draw the bars first; thread 0 overlays its marker after the barrier below.
  if (cb.debugMode)
  {
    const float barHeight = g_bins[linearIndex] / (10000.0f * float(EXPOSURE_HISTOGRAM_WEIGHT_SCALE));
    DebugView[uint2(linearIndex, 0)] = float4(barHeight, 0.0f, 0.0f, 1.0f);
  }

  // Device-scope, because thread 0 overwrites some of the bar texels the other threads just
  // wrote. A groupshared-only barrier orders execution but not those UAV writes.
  AllMemoryBarrierWithGroupSync();

  if (linearIndex == 0)
  {
    // The reference EV is the metered scene EV that needs no correction at all: the EV at which
    // a surface already sits at the key value. Deriving it from keyValue rather than hardcoding
    // it is what makes the stored exposure exactly exp2(referenceEV - currentEV), which in turn
    // lets the 1x1 exposure texture carry the whole adaptation state.
    const float referenceEV = calcLuminanceEV100(max(cb.keyValue, 1e-6f));

    // Recover adaptation state from the exposure texture. That texture is also consumed by
    // DLSS, DLSS-RR and XeSS as their exposure input, so it has to keep holding a plain linear
    // multiplier - storing EV there instead would silently feed those upscalers a wrong value.
    const float previousExposure = max(OutExposure[0], 1e-8f);
    float currentEV = referenceEV - log2(previousExposure);

    // Total weight over bins 1..N-1 only. Bin 0 holds sub-epsilon pixels.
    //
    // The old reduction disagreed with itself here: median mode looked for the halfway point of
    // the non-black population but walked a CDF that still included bin 0, and mean mode divided
    // by a total that included bin 0 while giving those pixels a numerator contribution of zero.
    // Both pulled the metered EV down - i.e. exposed the image too bright - in proportion to how
    // much black was on screen. Excluding bin 0 from both sides removes that class of error.
    float total = 0.0f;
    for (uint t = 1; t < EXPOSURE_HISTOGRAM_SIZE; ++t)
    {
      total += g_bins[t];
    }

    // Hold the previous value on a frame with nothing to meter (fully black frame, loading
    // screen) rather than snapping to a fallback.
    float sceneEV = currentEV;
    float trimmedFraction = 0.0f;
    float loPercentileEV = currentEV;
    float hiPercentileEV = currentEV;

    if (total > 0.0f)
    {
      const float loCount = total * cb.lowPercentile;
      const float hiCount = total * cb.highPercentile;

      float running = 0.0f;
      float weightedSum = 0.0f;
      float weight = 0.0f;
      bool haveLo = false;

      for (uint i = 1; i < EXPOSURE_HISTOGRAM_SIZE; ++i)
      {
        const float count = g_bins[i];
        if (count <= 0.0f)
        {
          continue;
        }

        const float binLo = running;
        running += count;

        // How much of this bin falls inside the [loCount, hiCount] slice of the CDF. Trimming
        // both ends is the main fix: it rejects sun and skybox pixels at the top and crushed
        // blacks at the bottom, which is why the old system lurched when a light source came
        // into frame.
        const float take = min(running, hiCount) - max(binLo, loCount);
        if (take <= 0.0f)
        {
          continue;
        }

        const float ev = binCentreEV(i);
        if (!haveLo)
        {
          loPercentileEV = ev;
          haveLo = true;
        }
        hiPercentileEV = ev;

        weightedSum += take * ev;
        weight += take;
      }

      if (weight > 0.0f)
      {
        // Bins are log2 luminance, so a count-weighted mean of bin centres is already the log
        // of the geometric mean of the trimmed set. No extra work needed.
        sceneEV = weightedSum / weight;
        trimmedFraction = weight / total;
      }
    }

    // Incomplete adaptation. At strength 0 the target never leaves the reference and auto
    // exposure does nothing; at 1 every scene normalises to the same apparent brightness, which
    // is the flat look this replaces. In between, dark rooms stay reading as dark.
    float targetEV = lerp(referenceEV, sceneEV, saturate(cb.adaptationStrength));

    // Soft limiter in place of a hard clamp. Monotonic, smooth, and asymptotically bounded to
    // +/- range, so the response approaches its limit instead of hitting a wall and sticking.
    {
      const float range = max(cb.softLimitRangeEV, 1e-3f);
      const float d = (targetEV - cb.softLimitCenterEV) / range;
      targetEV = cb.softLimitCenterEV + range * tanh(d);
    }

    const float delta = targetEV - currentEV;

    if (cb.resetState != 0 ||
        (cb.cutSnapThresholdEV > 0.0f && abs(delta) > cb.cutSnapThresholdEV))
    {
      // First frame, resolution change, camera cut, or a single-frame jump too large to be an
      // eye adapting. Snap, so load screens and cuts do not fade in over a second.
      currentEV = targetEV;
    }
    else if (abs(delta) >= cb.deadbandEV)
    {
      // Direction convention, which is easy to get backwards: currentEV and targetEV are METERED
      // SCENE EV, not applied exposure. A rising scene EV means the scene got brighter, so the
      // IMAGE has to get darker - the fast, squint-in-sunlight direction. So targetEV > currentEV
      // selects tauDarken. Reading this as "target is higher, so we are brightening" inverts the
      // whole feel of the adaptation.
      const float tau = (targetEV > currentEV) ? cb.tauDarken : cb.tauBrighten;
      const float alpha = 1.0f - exp(-cb.deltaTimeSeconds / max(tau, 1e-4f));
      currentEV = lerp(currentEV, targetEV, saturate(alpha));
    }
    // Otherwise inside the deadband: hold completely still. Without this the histogram's own
    // sampling noise keeps the exposure permanently, visibly creeping.

    const float exposure = exp2(referenceEV - currentEV);
    OutExposure[0] = exposure;

    if (cb.writeDebugStats != 0)
    {
      AutoExposureDebugStats stats;
      stats.currentEV = currentEV;
      stats.targetEV = targetEV;
      stats.sceneEV = sceneEV;
      stats.trimmedFraction = trimmedFraction;
      stats.loPercentileEV = loPercentileEV;
      stats.hiPercentileEV = hiPercentileEV;
      stats.exposure = exposure;
      stats.valid = 1;
      OutDebugStats[0] = stats;
    }

    if (cb.debugMode)
    {
      // Mark where the settled exposure currently sits, plus the edges of the trimmed window.
      const float normalizedCurrent = saturate((currentEV - cb.evMinValue) / cb.evRange);
      const uint currentIdx = uint(normalizedEVToHistogramOffset(normalizedCurrent));
      const uint loIdx = uint(normalizedEVToHistogramOffset(saturate((loPercentileEV - cb.evMinValue) / cb.evRange)));
      const uint hiIdx = uint(normalizedEVToHistogramOffset(saturate((hiPercentileEV - cb.evMinValue) / cb.evRange)));

      DebugView[uint2(loIdx, 0)] = float4(0.0f, 0.0f, 1.0f, 1.0f);
      DebugView[uint2(hiIdx, 0)] = float4(0.0f, 0.0f, 1.0f, 1.0f);
      DebugView[uint2(currentIdx, 0)] = float4(0.0f, 1.0f, 0.0f, 1.0f);
    }
  }

  // Reset for the next frame. Safe to do unsynchronised against the walk above, which reads
  // groupshared rather than the histogram image.
  InOutHistogram[linearIndex] = 0;
}
