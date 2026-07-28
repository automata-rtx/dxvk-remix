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

// Light that scattered more than once before reaching the eye.
//
// Single scattering alone gives a sky that is too dark and too saturated, because every photon that
// bounced twice is simply thrown away - and in a blue-scattering medium the ones that bounce most
// are exactly the blue ones. Hillaire's trick is to assume second and later bounces are isotropic,
// which makes them a function of altitude and sun angle only, and to sum the resulting geometric
// series in closed form rather than tracing it.
//
// Like the transmittance table this depends on the medium and not on the view, so it is computed
// once per medium change.

layout(binding = DUSKLIGHT_MULTISCATTER_TRANSMITTANCE)
Sampler2D<float4> TransmittanceLut;

layout(binding = DUSKLIGHT_MULTISCATTER_OUTPUT)
RWTexture2D<float4> MultiScatterOutput;

layout(push_constant)
ConstantBuffer<DusklightAtmosphereArgs> cb;

// Fibonacci sphere: an even spread of directions with no clustering at the poles, which a naive
// latitude/longitude sweep would give.
float3 dusklightSphereDirection(int index, int count)
{
  const float goldenAngle = 2.39996322972865332f;
  const float z = 1.0f - (2.0f * float(index) + 1.0f) / float(count);
  const float radius = sqrt(saturate(1.0f - z * z));
  const float theta = goldenAngle * float(index);

  return float3(radius * cos(theta), radius * sin(theta), z);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.imageSize))
  {
    return;
  }

  const float2 uv = (float2(ipos) + 0.5f) / float2(cb.imageSize);

  // U is the sun's cosine, V is altitude through the atmosphere.
  const float cosSunZenith = uv.x * 2.0f - 1.0f;
  const float radiusKm = lerp(kPlanetRadiusKm + 0.001f, kAtmosphereTopKm, uv.y);

  const float3 origin = float3(0.0f, 0.0f, radiusKm);
  const float3 sunDirection = float3(sqrt(saturate(1.0f - cosSunZenith * cosSunZenith)), 0.0f, cosSunZenith);

  const float3 rayleighScattering =
    dusklightTintCoefficients(kRayleighScattering, cb.skyColor, cb.paletteInfluence);
  const float mieScatteringScale = 1.0f;

  const int kDirectionCount = 32;
  const int kStepCount = 20;
  // Isotropic by assumption, so the phase function is a constant.
  const float uniformPhase = 1.0f / (4.0f * pi);

  float3 luminanceSum = float3(0.0f, 0.0f, 0.0f);
  float3 scatteringSum = float3(0.0f, 0.0f, 0.0f);

  for (int d = 0; d < kDirectionCount; ++d)
  {
    const float3 direction = dusklightSphereDirection(d, kDirectionCount);

    // Stop at the ground if the ray hits it, otherwise at the top of the atmosphere.
    const float groundDistance = dusklightRaySphere(origin, direction, kPlanetRadiusKm);
    const float topDistance = dusklightRaySphere(origin, direction, kAtmosphereTopKm);
    const float distanceKm = groundDistance > 0.0f ? groundDistance : topDistance;

    if (!(distanceKm > 0.0f))
    {
      continue;
    }

    const float stepKm = distanceKm / float(kStepCount);

    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float3 luminance = float3(0.0f, 0.0f, 0.0f);
    float3 scatteringAlongRay = float3(0.0f, 0.0f, 0.0f);

    for (int s = 0; s < kStepCount; ++s)
    {
      const float3 position = origin + direction * ((float(s) + 0.5f) * stepKm);
      const float heightKm = length(position) - kPlanetRadiusKm;

      const DusklightMedium medium = dusklightSampleMedium(heightKm, rayleighScattering, mieScatteringScale);

      const float3 stepTransmittance = exp(-medium.extinction * stepKm);

      const float2 sunUv = dusklightTransmittanceParamsToUv(
        max(length(position), kPlanetRadiusKm), dot(normalize(position), sunDirection));
      const float3 sunTransmittance = TransmittanceLut.SampleLevel(sunUv, 0).xyz;

      // Shadowed by the planet itself when the sun is below this point's horizon.
      const float sunVisibility = dusklightRaySphere(position, sunDirection, kPlanetRadiusKm) > 0.0f ? 0.0f : 1.0f;

      // Integrating the scattering term over the step analytically rather than at its midpoint;
      // this is what keeps the horizon from banding at low step counts.
      const float3 integratedScattering =
        (medium.scattering - medium.scattering * stepTransmittance) / max(medium.extinction, float3(1e-9f, 1e-9f, 1e-9f));

      luminance += throughput * sunTransmittance * sunVisibility * uniformPhase * integratedScattering;
      // The same integral without the incoming light: this is how much of an isotropic ambient the
      // ray would pick up, which is what turns the series below into a closed form.
      scatteringAlongRay += throughput * integratedScattering;

      throughput *= stepTransmittance;
    }

    if (groundDistance > 0.0f)
    {
      // One bounce off the ground, which is a surprisingly large part of the light near the horizon.
      const float3 groundPoint = origin + direction * groundDistance;
      const float3 groundNormal = normalize(groundPoint);
      const float cosSunGround = saturate(dot(groundNormal, sunDirection));

      const float2 groundUv = dusklightTransmittanceParamsToUv(kPlanetRadiusKm, cosSunGround);
      const float3 groundTransmittance = TransmittanceLut.SampleLevel(groundUv, 0).xyz;

      luminance += throughput * groundTransmittance * cosSunGround * (kGroundAlbedo / pi);
    }

    luminanceSum += luminance;
    scatteringSum += scatteringAlongRay;
  }

  const float normalization = 1.0f / float(kDirectionCount);
  const float3 luminanceAverage = luminanceSum * normalization;
  const float3 scatteringAverage = scatteringSum * normalization;

  // Sum the infinite series of further bounces: each one redistributes what the last left behind,
  // so the total is L / (1 - f) where f is the fraction that scatters again.
  const float3 reflectance = min(scatteringAverage, float3(0.999f, 0.999f, 0.999f));
  const float3 multiScattering = luminanceAverage / max(float3(1.0f, 1.0f, 1.0f) - reflectance, float3(1e-4f, 1e-4f, 1e-4f));

  MultiScatterOutput[ipos] = float4(max(multiScattering, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
