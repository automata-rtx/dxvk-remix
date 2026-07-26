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
#include "rtx/pass/dusklight/dusklight_grade.h"

// Ambient mood grade.
//
// Games that shaded through a fixed function pipeline carried a per area, per time of day,
// per weather ambient colour that every surface in the scene was tinted by. Path tracing
// replaces that lighting outright, so the mood the ambient carried is lost even though the
// game still computes it. This pass puts the chromatic part of it back, as a single multiply
// over the linear HDR image before bloom is gathered - which is where it sits in the original
// frame too, since the ambient was applied during shading and the bloom ran after.
//
// The tint is a frame constant; all of the response shaping (luminance normalization, range
// rails, strength) happens on the CPU where it can be inspected in the UI.

layout(binding = DUSKLIGHT_GRADE_COLOR_INPUT_OUTPUT)
RWTexture2D<float4> InOutColorBuffer;

layout(push_constant)
ConstantBuffer<DusklightGradeArgs> cb;

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

  color.rgb *= cb.tint;

  InOutColorBuffer[ipos] = color;
}
