/*
* Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
RWTexture1D<uint> OutHistogram;

layout(binding = AUTO_EXPOSURE_COLOR_INPUT)
RWTexture2D<float4> InColorBuffer;

layout(push_constant)
ConstantBuffer<ToneMappingAutoExposureArgs> cb;

groupshared uint g_localData[EXPOSURE_HISTOGRAM_SIZE];

uint inputToHistogramBucket(const float3 inputColor)
{
  const float inputLuminance = calcBt709Luminance(inputColor);

  // Count nearly black pixels in the 0th bucket. Written as a negated '>' so that a NaN
  // luminance - which compares false against everything - also lands here rather than
  // propagating into the reduction.
  const float kEpsilon = 0.00001f;
  if (!(inputLuminance > kEpsilon))
  {
    return 0;
  }

  const float ev100 = calcLuminanceEV100(inputLuminance);

  const float normalizedEV = saturate((ev100 - cb.evMinValue) / cb.evRange);

  // Remap exposure to bucket (1, size-1)
  return uint(normalizedEVToHistogramOffset(normalizedEV));
}

float centerMeteringWeight(float2 screenUV, float aspect)
{
  if (!cb.enableCenterMetering)
  {
    return 1.f;
  }

  const float weight = saturate(length( (screenUV - vec2(0.5f)) / vec2(1.0f, aspect) ) / cb.centerMeteringSize);
  return lerp(1.0f, 0.0f, weight);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 threadId : SV_DispatchThreadID, uint linearIndex : SV_GroupIndex)
{
  g_localData[linearIndex] = 0;
  
  GroupMemoryBarrierWithGroupSync();

  uint2 dimensions;
  InColorBuffer.GetDimensions(dimensions.x, dimensions.y);
  
  if (all(threadId < dimensions))
  {
    const float3 inputColor = InColorBuffer[threadId].xyz;

    // The metering weight scales how much this pixel COUNTS, not how bright it is.
    //
    // This pass used to multiply the weight into the colour and then bin the result, which
    // does something quite different: an off-centre pixel was darkened and therefore binned
    // as a darker pixel rather than as a less important one, and pixels at the very edge -
    // where the weight reaches zero - fell under the black epsilon and were binned as black.
    // A bright window at the edge of frame was recorded as a dark object.
    const uint bucketIdx = inputToHistogramBucket(inputColor);

    const float weight = centerMeteringWeight(float2(threadId) / dimensions, (float) dimensions.x / dimensions.y);
    const uint fixedPointWeight = uint(weight * float(EXPOSURE_HISTOGRAM_WEIGHT_SCALE) + 0.5f);

    if (fixedPointWeight != 0)
    {
      InterlockedAdd(g_localData[bucketIdx], fixedPointWeight);
    }
  }

  GroupMemoryBarrierWithGroupSync();

  uint localBinValue = g_localData[linearIndex];
  if (localBinValue != 0)
  {
    InterlockedAdd(OutHistogram[linearIndex], localBinValue);
  }
}
