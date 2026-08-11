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
#include "rtx/pass/dusklight/dusklight_sky.h"
#include "rtx/pass/dusklight/dusklight_atmosphere_common.slangh"

// Builds the sky the game would have drawn, and the sky physics would have drawn, and blends
// between them.
//
// The game does not draw its sky from a texture. It hands the hardware a handful of colours each
// frame - one for the zenith, two for the horizon haze, three more for clouds - and lets the
// vertices of a dome model smear them into gradients. Rebuilding it here from the same colours
// puts the sky in high dynamic range, which the game's own dome never was.
//
// The reason this comment used to give - "no texture, so the sky can never be tagged" - was wrong.
// Geometry-hash tagging and the API's SKY category both work without a texture. See
// DusklightAtmosphere.md §14.9 before repeating it.
//
// The two halves are blended as radiance, before anything downstream sees either. That matters:
// this one image is the visible sky, the light the sky casts, and the colour distant geometry
// fades towards, so blending here keeps all three describing the same sky. Blending finished
// pixels instead would let the sky you look at drift away from the light you stand in.

layout(binding = DUSKLIGHT_SKY_OUTPUT)
RWTexture2D<float4> SkyOutput;

layout(binding = DUSKLIGHT_SKY_TRANSMITTANCE)
Sampler2D<float4> TransmittanceLut;

layout(binding = DUSKLIGHT_SKY_MULTISCATTER)
Sampler2D<float4> MultiScatterLut;

layout(push_constant)
ConstantBuffer<DusklightAtmosphereArgs> cb;

// The game's own gradient. Its dome model is the one piece we do not have, so the vertical falloff
// between horizon and zenith is a stand-in shaped by horizonSharpness rather than a reproduction of
// that model's vertices.
float3 evaluateStylisedSky(float height, float3 horizonColor)
{
  if (height >= 0.0f)
  {
    const float t = pow(saturate(height), 1.0f / max(cb.horizonSharpness, 1e-3f));
    return lerp(horizonColor, cb.skyColor, t);
  }

  return horizonColor;
}

// Single scattering along the view ray, plus the precomputed multiple scattering, after
// Hillaire 2020.
float3 evaluatePhysicalSky(float3 direction, float3 sunDirection)
{
  const float3 rayleighScattering =
    dusklightTintCoefficients(kRayleighScattering, cb.skyColor, cb.paletteInfluence);
  // The haze colours steer how much Mie there is rather than what colour it is: Mie scattering is
  // almost wavelength independent, so a coloured haze is a contradiction. Their brightness is a
  // good proxy for how thick the air reads in a given palette.
  const float hazeLevel = dot(cb.kasumiInner + cb.kasumiOuter, float3(0.2126f, 0.7152f, 0.0722f)) * 0.5f;
  const float mieScatteringScale = lerp(1.0f, saturate(hazeLevel) * 4.0f + 0.25f, saturate(cb.paletteInfluence));

  // Just above the ground. The game's camera is effectively at sea level on this scale.
  const float3 origin = float3(0.0f, 0.0f, kPlanetRadiusKm + 0.0005f);

  const float groundDistance = dusklightRaySphere(origin, direction, kPlanetRadiusKm);
  const float topDistance = dusklightRaySphere(origin, direction, kAtmosphereTopKm);
  const float distanceKm = groundDistance > 0.0f ? groundDistance : topDistance;

  if (!(distanceKm > 0.0f))
  {
    return float3(0.0f, 0.0f, 0.0f);
  }

  const float cosTheta = dot(direction, sunDirection);
  const float rayleighPhase = dusklightRayleighPhase(cosTheta);
  const float miePhase = dusklightMiePhase(cosTheta, clamp(cb.mieAnisotropy, 0.0f, 0.95f));

  const int kStepCount = 32;
  const float stepKm = distanceKm / float(kStepCount);

  float3 throughput = float3(1.0f, 1.0f, 1.0f);
  float3 luminance = float3(0.0f, 0.0f, 0.0f);

  for (int i = 0; i < kStepCount; ++i)
  {
    const float3 position = origin + direction * ((float(i) + 0.5f) * stepKm);
    const float radiusKm = max(length(position), kPlanetRadiusKm);
    const float heightKm = radiusKm - kPlanetRadiusKm;

    const DusklightMedium medium = dusklightSampleMedium(heightKm, rayleighScattering, mieScatteringScale);
    const float3 stepTransmittance = exp(-medium.extinction * stepKm);

    const float3 up = position / radiusKm;
    const float cosSunZenith = dot(up, sunDirection);

    const float3 sunTransmittance =
      TransmittanceLut.SampleLevel(dusklightTransmittanceParamsToUv(radiusKm, cosSunZenith), 0).xyz;

    // The planet blocks the sun below its own horizon. Without this the ground glows after sunset.
    const float sunVisibility = dusklightRaySphere(position, sunDirection, kPlanetRadiusKm) > 0.0f ? 0.0f : 1.0f;

    const float3 phaseWeighted =
      medium.rayleighScattering * rayleighPhase + float3(medium.mieScattering * miePhase);

    const float3 multiScatter =
      MultiScatterLut.SampleLevel(
        float2(cosSunZenith * 0.5f + 0.5f,
               saturate((radiusKm - kPlanetRadiusKm) / (kAtmosphereTopKm - kPlanetRadiusKm))), 0).xyz;

    // Integrating scattering across the step in closed form rather than sampling its midpoint;
    // this is what keeps the horizon from banding at a step count this low.
    const float3 inverseExtinction = 1.0f / max(medium.extinction, float3(1e-9f, 1e-9f, 1e-9f));

    const float3 directTerm =
      (phaseWeighted - phaseWeighted * stepTransmittance) * inverseExtinction * sunTransmittance * sunVisibility;
    const float3 multiTerm =
      (medium.scattering - medium.scattering * stepTransmittance) * inverseExtinction * multiScatter * cb.multiScatterScale;

    luminance += throughput * (directTerm + multiTerm);
    throughput *= stepTransmittance;
  }

  if (groundDistance > 0.0f)
  {
    const float3 groundPoint = origin + direction * groundDistance;
    const float3 groundNormal = normalize(groundPoint);
    const float cosSunGround = saturate(dot(groundNormal, sunDirection));

    const float3 groundTransmittance =
      TransmittanceLut.SampleLevel(dusklightTransmittanceParamsToUv(kPlanetRadiusKm, cosSunGround), 0).xyz;

    luminance += throughput * groundTransmittance * cosSunGround * (kGroundAlbedo / pi);
  }

  return luminance;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.imageSize))
  {
    return;
  }

  const float2 uv = (float2(ipos) + 0.5f) / float2(cb.imageSize);

  // Invert the sampling mapping: V is theta from the pole, U is phi about it.
  const float theta = uv.y * pi;
  const float phi = (uv.x - 0.5f) * twoPi;

  const float sinTheta = sin(theta);
  const float3 direction = float3(sinTheta * sin(phi), sinTheta * cos(phi), cos(theta));

  // Height above the horizon as a sine, +1 at the zenith and -1 straight down.
  const float height = direction.z;

  // The game's two horizon haze bands are front and back, not sun-relative. It paints kasumi_outer
  // onto vrbox_kasumiM.bmd and kasumi_inner onto a material of vrbox_sora.bmd, and draws both at
  // every bearing; no code path in it reads sun position to pick between them. "Outer" is the NEAR
  // band, opposite to what the English reads like - the game's own labels say so ("mae" = front and
  // "oku" = back on the HIO sliders, kasumiF / kasumiB in the debug view).
  // dusklight-ao/docs/japanese-naming.md section 6 quotes them in the original kanji; this file
  // cannot, because compile_shaders.py reads shader source in the platform's default encoding.
  //
  // Two ways of collapsing them into one colour, selectable because the first is the one that
  // shipped and the second is the one the game supports. DusklightAtmosphere.md section 12.2.
  float3 horizonColor;

  if (cb.kasumiBlendMode == DUSKLIGHT_KASUMI_BLEND_FIXED)
  {
    // What the game does, minus the coverage. The near band lies in front of the far one at every
    // azimuth, so the mix is a constant - but *how much* of the far band it hides is the near
    // band's alpha, which the bridge does not push, so the share is a setting rather than a
    // translation. Nothing here varies with the sun, which is the point: with a static palette the
    // horizon now stays put, and so do the sky light and the fog colour that come off this image.
    horizonColor = lerp(cb.kasumiInner, cb.kasumiOuter, saturate(cb.kasumiFrontWeight));
  }
  else
  {
    // The original, kept as the default. Angular distance from the sun about the up axis, folded to
    // 0..1, squared so the far band stays a glow rather than washing over half the sky. This puts
    // the *far* band at the sun and turns the horizon palette with the sun's compass bearing, which
    // is behaviour the game does not have.
    float deltaPhi = abs(phi - cb.sunAzimuthRadians);
    deltaPhi = min(deltaPhi, twoPi - deltaPhi);
    const float sunProximity = square(saturate(1.0f - deltaPhi / pi));

    horizonColor = lerp(cb.kasumiOuter, cb.kasumiInner, sunProximity);
  }

  float3 color = evaluateStylisedSky(height, horizonColor);

  const float weight = saturate(cb.physicalWeight);

  if (weight > 0.0f)
  {
    const float cosElevation = cos(cb.sunElevationRadians);
    const float3 sunDirection = float3(cosElevation * sin(cb.sunAzimuthRadians),
                                       cosElevation * cos(cb.sunAzimuthRadians),
                                       sin(cb.sunElevationRadians));

    const float3 physical = evaluatePhysicalSky(direction, sunDirection);

    // Blended as radiance, which is the whole point - see the note at the top of this file.
    color = lerp(color, physical, weight);
  }

  // A dome light wraps the whole sphere, so the lower half would otherwise light the scene from
  // below with a second copy of the sky and flatten everything out. The physical model's own ground
  // bounce is real, but this game's ground is path traced geometry that already bounces light, so
  // it would be counted twice.
  if (height < 0.0f)
  {
    color *= lerp(1.0f, saturate(1.0f - cb.groundFraction), saturate(-height));
  }

  color *= cb.intensity;

  // The moon, after the sky's intensity so its radiance is absolute rather than a multiple of
  // however bright the palette happens to be, and after the ground attenuation so a moon below the
  // horizon is not dimmed into a smear - it simply is not drawn, because the host sends 0.
  //
  // Reusing sunAzimuth/sunElevation is deliberate, not a copy-paste: the game pushes one celestial
  // direction, whichever body is driving the light, so at night that pair *is* the moon's.
  // DusklightAtmosphere.md §13.1.
  if (cb.moonRadiance > 0.0f)
  {
    const float cosMoonElevation = cos(cb.sunElevationRadians);
    const float3 moonDirection = float3(cosMoonElevation * sin(cb.sunAzimuthRadians),
                                        cosMoonElevation * cos(cb.sunAzimuthRadians),
                                        sin(cb.sunElevationRadians));

    const float angle = acos(clamp(dot(direction, moonDirection), -1.0f, 1.0f));
    const float edge = max(cb.moonEdgeSoftness, 1e-4f) * cb.moonAngularRadiusRadians;
    const float disc = 1.0f - smoothstep(cb.moonAngularRadiusRadians - edge,
                                         cb.moonAngularRadiusRadians + edge,
                                         angle);

    color += cb.moonColor * (cb.moonRadiance * disc);
  }

  // The game's palettes reach zero on both colours in areas with no sky, and a negative or
  // non-finite radiance here would poison every ray that misses geometry.
  color = max(color, float3(0.0f, 0.0f, 0.0f));

  if (any(isnan(color)) || any(isinf(color)))
  {
    color = float3(0.0f, 0.0f, 0.0f);
  }

  SkyOutput[ipos] = float4(color, 1.0f);
}
