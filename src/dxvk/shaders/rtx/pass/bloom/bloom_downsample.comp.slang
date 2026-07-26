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

layout(binding = BLOOM_DOWNSAMPLE_INPUT)
Sampler2D<float4> InColorBuffer;

layout(binding = BLOOM_DOWNSAMPLE_OUTPUT)
RWTexture2D<float4> OutColorBuffer;

layout(push_constant)
ConstantBuffer<BloomDownsampleArgs> cb;

float2 calcUV(const int2 downsampledPix)
{
  return (float2(downsampledPix) + float2(0.5)) * cb.downsampledOutputSizeInverse;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.downsampledOutputSize))
  {
    return;
  }

  float3 result = bloomDownsample13tap(InColorBuffer, calcUV(int2(ipos)), cb.inputSizeInverse);

  if (cb.threshold > 0)
  {
    // smoothly suppress values under threshold
    result *= smoothstep(0.0, cb.threshold, calcBt709Luminance(result));
  }

  OutColorBuffer[ipos] = float4(result, 0);
}
