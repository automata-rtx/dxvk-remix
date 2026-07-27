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

#include "rtx/pass/dusklight/dusklight_sky.h"
#include "rtx/utility/shader_types.slangh"

// Builds the sky the game would have drawn, as a lat-long image for Remix's dome light.
//
// The game does not draw its sky from a texture. It hands the hardware a handful of colours each
// frame - one for the zenith, two for the horizon haze, three more for clouds - and lets the
// vertices of a dome model smear them into gradients. That is why the sky cannot be tagged: there
// is no texture content to fingerprint. Rebuilding the gradient here from the same colours gets
// around that permanently, and puts the sky in high dynamic range, which the game's own dome never
// was and which is what lets it light the world at a believable strength.
//
// The dome model itself is the one piece we do not have, so the vertical falloff between horizon
// and zenith is a stand-in shaped by horizonSharpness rather than a reproduction of its vertices.
//
// Layout is fixed by the sampling side (cartesianDirectionToLatLongSphere): V runs from the +Z
// pole down, U carries phi = atan2(x, y) with 0.5 at phi = 0. In light space +Z is up.

layout(binding = DUSKLIGHT_SKY_OUTPUT)
RWTexture2D<float4> SkyOutput;

layout(push_constant)
ConstantBuffer<DusklightSkyArgs> cb;

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

  // Height above the horizon as a sine, +1 at the zenith and -1 straight down.
  const float height = cos(theta);

  // Angular distance from the sun about the up axis, folded to 0..1. The game keeps two horizon
  // colours, one for the sun's side and one for away from it, and this is what chooses between
  // them. Squared so the warm side stays a glow rather than washing over half the sky.
  float deltaPhi = abs(phi - cb.sunAzimuthRadians);
  deltaPhi = min(deltaPhi, twoPi - deltaPhi);
  const float sunProximity = square(saturate(1.0f - deltaPhi / pi));

  const float3 horizonColor = lerp(cb.kasumiOuter, cb.kasumiInner, sunProximity);

  float3 color;

  if (height >= 0.0f)
  {
    // Upward: the haze gives way to the sky colour. A higher sharpness pulls the crossover down
    // towards the horizon, leaving the haze as a thin band.
    const float t = pow(saturate(height), 1.0f / max(cb.horizonSharpness, 1e-3f));
    color = lerp(horizonColor, cb.skyColor, t);
  }
  else
  {
    // Downward: a dome light wraps the whole sphere, so the lower half would otherwise light the
    // scene from below with a second copy of the sky and flatten everything out. Fade it towards a
    // dimmer ground bounce instead.
    const float t = saturate(-height);
    color = lerp(horizonColor, horizonColor * saturate(1.0f - cb.groundFraction), t);
  }

  color *= cb.intensity;

  // The game's palettes reach zero on both colours in areas with no sky, and a negative or
  // non-finite radiance here would poison every ray that misses geometry.
  color = max(color, float3(0.0f, 0.0f, 0.0f));

  if (any(isnan(color)) || any(isinf(color)))
  {
    color = float3(0.0f, 0.0f, 0.0f);
  }

  SkyOutput[ipos] = float4(color, 1.0f);
}
