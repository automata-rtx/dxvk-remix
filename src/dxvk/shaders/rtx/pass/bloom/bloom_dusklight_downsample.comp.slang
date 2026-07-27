/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx/pass/bloom/bloom.h"
#include "rtx/pass/bloom/bloom_downsample.slangh"
#include "rtx/utility/color.slangh"

// Downsample step of the Dusklight bloom pyramid.
//
// The default Remix pyramid gets all of its blur from the downsample kernel itself. Dusklight
// instead runs an explicit blur at every level of the pyramid: eight taps spread evenly around a
// ring of a fixed screen-space radius, with no center tap, each scaled by a per-pass gain. Because
// the gain is applied once per level and the intermediate results saturate, bright cores wash out
// to white while the surrounding halo keeps a smooth, wide falloff. That saturation is not an
// artifact to be avoided - it is a large part of what the effect looks like.
//
// The first step of the chain does not blur. It thresholds instead, and the shape of that
// threshold is the single most distinctive thing about the effect. The original spends three TEV
// stages on it: two build a greyscale key out of the framebuffer through the swap tables, and the
// third multiplies the *original* colour by that key.
//
//   key    = 0.25 * R + 0.25 * G + 0.5 * B      (swap tables R,R,R,G then B,B,B,A, mixed by HALF)
//   source = colour * saturate(key - threshold)
//
// So it is a luminance keyed mask, not a per channel subtraction, and the resulting bloom carries
// the source's own hue rather than drifting towards whichever channel was brightest. The weights
// are not a standard luma either - blue counts double red or green, which is why blue highlights
// in this game bloom so much more readily than their brightness alone would suggest.

layout(binding = BLOOM_DUSKLIGHT_DOWNSAMPLE_INPUT)
Sampler2D<float4> InColorBuffer;

layout(binding = BLOOM_DUSKLIGHT_DOWNSAMPLE_OUTPUT)
RWTexture2D<float4> OutColorBuffer;

layout(push_constant)
ConstantBuffer<BloomDusklightDownsampleArgs> cb;

// Unit directions of the ring taps, evenly spaced 45 degrees apart starting at +X.
static const float2 kRingDirections[BLOOM_DUSKLIGHT_RING_TAP_COUNT] = {
  float2( 1.00000000,  0.00000000),
  float2( 0.70710678,  0.70710678),
  float2( 0.00000000,  1.00000000),
  float2(-0.70710678,  0.70710678),
  float2(-1.00000000,  0.00000000),
  float2(-0.70710678, -0.70710678),
  float2( 0.00000000, -1.00000000),
  float2( 0.70710678, -0.70710678),
};

float2 calcUV(const int2 downsampledPix)
{
  return (float2(downsampledPix) + float2(0.5)) * cb.downsampledOutputSizeInverse;
}

// Averages the ring taps and applies this pass' gain. The ring radius is expressed in screen UV
// rather than input texels, so it stays put on screen as the pyramid gets smaller - the widening
// of the halo comes from the pyramid itself, exactly as it does in the original effect.
float3 ringBlur(const int2 downsampledPix)
{
  const float2 centerUv = calcUV(downsampledPix);

  float3 result = float3(0);

  [unroll]
  for (uint i = 0; i < BLOOM_DUSKLIGHT_RING_TAP_COUNT; i++)
  {
    result += InColorBuffer.SampleLevel(centerUv + kRingDirections[i] * cb.ringRadius, 0).rgb;
  }

  return result * (cb.gain / float(BLOOM_DUSKLIGHT_RING_TAP_COUNT));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.downsampledOutputSize))
  {
    return;
  }

  float3 result;

  if (cb.isInitial != 0)
  {
    // Use the same downsample kernel as the default pyramid here. The original effect just point
    // samples the framebuffer, but a wider kernel keeps single bright pixels from making the whole
    // bloom flicker, which matters more here than the point sample's exactness.
    result = bloomDownsample13tap(InColorBuffer, calcUV(int2(ipos)), cb.inputSizeInverse);

    // The pyramid runs in whatever space the original did: display referred, so that the threshold
    // means what it meant, and so the per pass saturation below clips at white rather than at some
    // arbitrary point in an open ended range.
    if (cb.displaySpace != 0)
    {
      result = linearToGamma(max(result, float3(0)));
    }

    const float key = dot(result, float3(0.25f, 0.25f, 0.5f));

    result *= saturate(key - cb.threshold);
    result *= cb.gain;
  }
  else
  {
    result = ringBlur(int2(ipos));
  }

  if (cb.saturationPoint > 0)
  {
    result = min(result, float3(cb.saturationPoint));
  }

  OutColorBuffer[ipos] = float4(result, 0);
}
