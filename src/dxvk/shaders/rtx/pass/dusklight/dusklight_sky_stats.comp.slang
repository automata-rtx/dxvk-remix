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
#include "rtx/pass/dusklight/dusklight_sky_stats.h"
#include "rtx/pass/dusklight/dusklight_atmosphere.h"

// One dispatch, one workgroup: the mean radiance of the generated dome, weighted by solid angle.
// See dusklight_sky_stats.h for why the fog needs it at all.

layout(binding = DUSKLIGHT_SKY_STATS_INPUT)
RWTexture2D<float4> SkyInput;

layout(binding = DUSKLIGHT_SKY_STATS_OUTPUT)
RWStructuredBuffer<DusklightSkyStats> OutStats;

layout(push_constant)
ConstantBuffer<DusklightAtmosphereArgs> cb;

// xyz is the weighted radiance sum, w the weight sum. Carried together so one reduction serves both
// and the divide happens once, in thread 0.
groupshared float4 g_accum[DUSKLIGHT_SKY_STATS_THREADS];

[shader("compute")]
[numthreads(DUSKLIGHT_SKY_STATS_THREADS, 1, 1)]
void main(uint linearIndex : SV_GroupIndex)
{
  const float kPi = 3.14159265358979323846f;

  const uint2 imageSize = max(cb.imageSize, uint2(1, 1));
  const uint texelCount = imageSize.x * imageSize.y;

  float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);

  // Strided rather than blocked, so consecutive threads read consecutive texels on every iteration.
  for (uint i = linearIndex; i < texelCount; i += DUSKLIGHT_SKY_STATS_THREADS)
  {
    const uint2 coordinate = uint2(i % imageSize.x, i / imageSize.x);

    // The image is a lat-long map: V is theta measured from the pole, so a texel's solid angle is
    // proportional to sin(theta). Without this weight the poles - which are a handful of steradians
    // spread across a whole row each - would count as much as the horizon band the fog actually
    // sits in.
    const float theta = ((float(coordinate.y) + 0.5f) / float(imageSize.y)) * kPi;
    const float weight = max(sin(theta), 0.0f);

    const float3 radiance = SkyInput[coordinate].xyz;

    // The generator already refuses to emit these, but it is cheap to be certain: one non finite
    // texel here would poison the fog colour for every froxel in the scene.
    if (!any(isnan(radiance)) && !any(isinf(radiance)))
    {
      accum += float4(max(radiance, float3(0.0f, 0.0f, 0.0f)) * weight, weight);
    }
  }

  g_accum[linearIndex] = accum;

  GroupMemoryBarrierWithGroupSync();

  for (uint stride = uint(DUSKLIGHT_SKY_STATS_THREADS) / 2u; stride > 0u; stride >>= 1u)
  {
    if (linearIndex < stride)
    {
      g_accum[linearIndex] += g_accum[linearIndex + stride];
    }

    GroupMemoryBarrierWithGroupSync();
  }

  if (linearIndex == 0)
  {
    const float4 total = g_accum[0];

    DusklightSkyStats stats;
    stats.ambient = total.w > 0.0f ? total.xyz / total.w : float3(0.0f, 0.0f, 0.0f);
    stats.valid = total.w > 0.0f ? 1u : 0u;

    OutStats[0] = stats;
  }
}
