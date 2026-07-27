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

#ifndef DUSKLIGHT_COMPOSITE_ARGS_H
#define DUSKLIGHT_COMPOSITE_ARGS_H

#include "rtx/utility/shader_types.h"

// The far half of the fog.
//
// Volumetrics and a distance ramp are not two implementations of one effect; they are good at
// different things and bad at the other's job. A froxel grid can put light shafts and a glow around
// a torch in the air, but it stops at the end of its slices, and this game's fog regularly runs
// hundreds of metres past that. A distance ramp reaches as far as you like and closes to fully
// opaque exactly where the original did - an exponential medium only ever asymptotes towards that -
// but it can never make a shaft.
//
// So the split is by distance rather than by system: the grid owns everything inside its reach, the
// game's own ramp owns everything past it, and the ramp is rebased at the handover so the two never
// count the same air twice.
struct DusklightCompositeArgs {
  // Radiance the far ramp tends towards. Carried separately from CompositeArgs' own fog colour,
  // which has already been through the legacy fog path's scale.
  vec3 fogColor;
  uint enable;

  // The game's ramp, in world units. rampStart is often negative - a scripted fog bank sets it that
  // way so the ramp is already underway at the camera.
  float rampStart;
  float rampEnd;
  // Where the froxel grid stops and this takes over.
  float handoverDistance;
  float pad0;
};

#endif  // DUSKLIGHT_COMPOSITE_ARGS_H
