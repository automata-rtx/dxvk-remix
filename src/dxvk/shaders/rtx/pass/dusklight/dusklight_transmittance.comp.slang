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

#include "rtx/utility/shader_types.slangh"
#include "rtx/pass/dusklight/dusklight_atmosphere_common.slangh"

// How much light survives a straight path from a point in the atmosphere out to space.
//
// This is the one quantity everything else in the model is built on, and it depends only on the
// medium - not on where the sun is, not on where the camera is - so it is computed once and reused
// until the medium itself changes. That is what makes a physically based sky affordable in real
// time at all.

layout(binding = DUSKLIGHT_TRANSMITTANCE_OUTPUT)
RWTexture2D<float4> TransmittanceOutput;

layout(push_constant)
ConstantBuffer<DusklightAtmosphereArgs> cb;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.imageSize))
  {
    return;
  }

  const float2 uv = (float2(ipos) + 0.5f) / float2(cb.imageSize);

  float radiusKm;
  float cosZenith;
  dusklightTransmittanceUvToParams(uv, radiusKm, cosZenith);

  const float3 origin = float3(0.0f, 0.0f, radiusKm);
  const float3 direction = float3(sqrt(saturate(1.0f - cosZenith * cosZenith)), 0.0f, cosZenith);

  const float distanceKm = dusklightRaySphere(origin, direction, kAtmosphereTopKm);

  const float3 rayleighScattering =
    dusklightTintCoefficients(kRayleighScattering, cb.skyColor, cb.paletteInfluence);
  const float mieScatteringScale = 1.0f;

  float3 opticalDepth = float3(0.0f, 0.0f, 0.0f);

  if (distanceKm > 0.0f)
  {
    // Fixed step count with a midpoint sample. The integrand is smooth and monotone in height, so
    // this converges quickly; the cost is paid once per medium change rather than per frame.
    const int kSampleCount = 40;
    const float stepKm = distanceKm / float(kSampleCount);

    for (int i = 0; i < kSampleCount; ++i)
    {
      const float3 samplePosition = origin + direction * ((float(i) + 0.5f) * stepKm);
      const float heightKm = length(samplePosition) - kPlanetRadiusKm;

      const DusklightMedium medium = dusklightSampleMedium(heightKm, rayleighScattering, mieScatteringScale);

      opticalDepth += medium.extinction * stepKm;
    }
  }

  TransmittanceOutput[ipos] = float4(exp(-opticalDepth), 1.0f);
}
