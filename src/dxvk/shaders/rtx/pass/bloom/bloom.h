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

#ifndef BLOOM_H
#define BLOOM_H

#include "rtx/utility/shader_types.h"

#define BLOOM_DOWNSAMPLE_INPUT  0
#define BLOOM_DOWNSAMPLE_OUTPUT 1

#define BLOOM_DUSKLIGHT_DOWNSAMPLE_INPUT  0
#define BLOOM_DUSKLIGHT_DOWNSAMPLE_OUTPUT 1

#define BLOOM_DUSKLIGHT_PREPASS_COLOR_INPUT_OUTPUT 0

#define BLOOM_UPSAMPLE_INPUT    0
#define BLOOM_UPSAMPLE_OUTPUT   1

#define BLOOM_COMPOSITE_COLOR_INPUT_OUTPUT 0
#define BLOOM_COMPOSITE_BLOOM              1

// Number of taps in the Dusklight ring blur. The original effect spends all eight of the
// hardware's texture coordinate generators on a single ring, with no center tap.
#define BLOOM_DUSKLIGHT_RING_TAP_COUNT 8

// Push constants

struct BloomDownsampleArgs {
  float2 inputSizeInverse;
  uint2  downsampledOutputSize;
  float2 downsampledOutputSizeInverse;
  float  threshold;
};

struct BloomDusklightDownsampleArgs {
  float2 inputSizeInverse;
  uint2  downsampledOutputSize;
  float2 downsampledOutputSizeInverse;
  // Ring blur radius in normalized screen UV. The horizontal component is scaled so the ring
  // keeps the shape it had on the game's original framebuffer regardless of the display aspect.
  float2 ringRadius;
  // Subtracted from the luminance key (0.25R + 0.25G + 0.5B), which then masks the original
  // colour - not a per channel subtraction. Only read when isInitial is set.
  float  threshold;
  // Brightness multiplier applied by this step.
  float  gain;
  // Value this step's result saturates at. Zero or less leaves the result unclamped.
  float  saturationPoint;
  // Non-zero on the first step, which thresholds the input instead of ring blurring it.
  uint   isInitial;
  // Non-zero to run the pyramid in display referred (gamma) space, as the original did.
  uint   displaySpace;
};

struct BloomDusklightPrepassArgs {
  uint2  imageSize;
  // Tint of the mono overlay.
  vec3   monoColor;
  // Blend factor toward the mono version of the image. Zero disables the overlay.
  float  monoAmount;
  // Non-zero to use BT.709 luminance for the greyscale; zero replicates the red channel the
  // way the game's TEV swap tables did.
  uint   useLuminance;
  // Non-zero to apply the overlay in display referred space, where the original applied it.
  uint   displaySpace;
};

struct BloomUpsampleArgs {
  float2 inputSizeInverse;
  uint2  upsampledOutputSize;
  float2 upsampledOutputSizeInverse;
  // Weight this level contributes with when it is added to the level above it.
  float  weight;
};

struct BloomCompositeArgs {
  uint2  imageSize;
  float2 imageSizeInverse;
  float  intensity;
  // Per channel tint applied to the bloom before it is added to the image.
  vec3   tint;
  // When non-zero, bloom is attenuated by how bright the destination already is.
  uint   screenBlend;
  // Weight the base image keeps under the bloom. The game's composite scales the framebuffer
  // by its blend alpha while adding bloom on top; twilight uses this to dim the scene.
  float  baseWeight;
  // Non-zero when the bloom was gathered in display referred space, so the composite has to meet
  // it there: the screen blend and the base weight are both defined against display values.
  uint   displaySpace;
};

#endif  // BLOOM_H
