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

#ifndef DUSKLIGHT_SKY_H
#define DUSKLIGHT_SKY_H

#include "rtx/utility/shader_types.h"

#define DUSKLIGHT_SKY_OUTPUT 0

// Push constants for the generated sky dome.
//
// The image is a lat-long map consumed by Remix's existing dome light sampling, so the layout is
// fixed by cartesianDirectionToLatLongSphere: theta = acos(dir.z) down the V axis, phi =
// atan2(dir.x, dir.y) across U with 0.5 at phi = 0. The polar axis is Z in light space; the world
// is Y up, and the dome light's worldToLight transform carries that swap, so this shader can work
// purely in light space and treat +Z as up.
struct DusklightSkyArgs {
  uint2 imageSize;
  // Where the sun is, as an angle about the up axis, matching the U axis' phi directly. The haze
  // colour the game keeps for the sun's side of the sky is strongest here.
  float sunAzimuthRadians;
  float horizonSharpness;

  // Colour at the zenith.
  vec3 skyColor;
  float groundFraction;

  // Horizon haze on the sun's side.
  vec3 kasumiInner;
  float intensity;

  // Horizon haze away from the sun.
  vec3 kasumiOuter;
  float pad0;
};

#endif  // DUSKLIGHT_SKY_H
