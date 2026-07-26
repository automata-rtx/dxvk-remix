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
#include "rtx/utility/color.slangh"

// Mono overlay step of the Dusklight bloom.
//
// The game runs this before gathering bloom: the framebuffer is converted to greyscale and
// tinted, then blended back over itself by the overlay's alpha. The environment system drives
// it per palette - twilight desaturates by about 38% through this, and the wolf senses look
// leans on it too. The bloom pyramid then gathers from the overlaid image, so this must run
// before the downsample chain, exactly as it does on the original hardware.
//
// The TEV implementation built its greyscale by replicating the red channel (swap table
// R,R,R,G), not by computing luminance. That bias is visible in warm scenes, so red-replicate
// is the faithful default and BT.709 luminance is offered as an alternative.

layout(binding = BLOOM_DUSKLIGHT_PREPASS_COLOR_INPUT_OUTPUT)
RWTexture2D<float4> InOutColorBuffer;

layout(push_constant)
ConstantBuffer<BloomDusklightPrepassArgs> cb;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= cb.imageSize))
  {
    return;
  }

  float4 color = InOutColorBuffer[ipos];

  if (any(isnan(color)) || any(isinf(color)))
  {
    return;
  }

  const float grey = cb.useLuminance != 0 ? calcBt709Luminance(color.rgb) : color.r;

  color.rgb = lerp(color.rgb, grey * cb.monoColor, cb.monoAmount);

  InOutColorBuffer[ipos] = color;
}
