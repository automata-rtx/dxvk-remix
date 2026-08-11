/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx/utility/color.slangh"

layout(binding = BLOOM_COMPOSITE_COLOR_INPUT_OUTPUT)
RWTexture2D<float4> InOutColorBuffer; 

layout(binding = BLOOM_COMPOSITE_BLOOM)
Sampler2D InBloomBuffer;

layout(push_constant)
ConstantBuffer<BloomCompositeArgs> cb;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.imageSize))
  {
    return;
  }

  float2 uv = saturate((float2(ipos) + float2(0.5, 0.5)) * cb.imageSizeInverse);

  float4 src = InBloomBuffer.SampleLevel(uv, 0);
  float4 dst = InOutColorBuffer[ipos];

  if (any(isnan(dst)) || any(isinf(dst)))
  {
    return;
  }

  float3 bloom = src.rgb * cb.intensity * cb.tint;

  // The bloom was gathered in display referred space, so meet it there. Both remaining operations
  // are defined against display values: the screen blend reads "how close to white is this pixel
  // already", and the base weight is a blend alpha against a 0..1 framebuffer. Doing either to
  // open ended linear radiance means something quite different.
  float3 base = dst.rgb;

  if (cb.displaySpace != 0)
  {
    base = linearToGamma(max(base, float3(0.0)));
  }

  if (cb.screenBlend != 0)
  {
    // Screen style blend: the brighter the image already is here, the less bloom it takes. Keeps
    // highlights glowing instead of flattening them into a uniform white blob. Uses the
    // unweighted destination, matching the hardware blend this reproduces.
    bloom *= max(float3(1.0) - base, float3(0.0));
  }

  float3 result = base * cb.baseWeight + bloom;

  if (cb.displaySpace != 0)
  {
    result = gammaToLinear(max(result, float3(0.0)));
  }

  InOutColorBuffer[ipos] = float4(result, dst.a);
}
