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

// The game's fog ramp, and what the composite needs to make the frame show exactly it.
//
// This used to be "the far half of the fog": the froxel grid owned everything inside its reach and
// this ramp was rebased at the grid's last slice to carry the rest. That split could only ever add
// fog, so nothing could correct the near field - and the near field is where a homogeneous medium
// is most wrong, because the game's ramp is flat zero until rampStart while an exponential medium
// starts accumulating at the camera. See DusklightAtmosphere.md §5.1.
//
// It is now a correction across the whole distance instead. The composite already knows the
// medium's own transmittance for the pixel, so it can ask for the residual that brings the total
// opacity to exactly the game's ramp, at every distance, with no handover to seam. What the medium
// contributes on top of a flat fog colour - a shaft, a locally lit patch of air - survives the
// correction scaled rather than being flattened, which is the whole reason to run volumetrics at
// all. DusklightAtmosphere.md §5.2.
struct DusklightCompositeArgs {
  // Radiance the fog tends towards. Carried separately from CompositeArgs' own fog colour, which
  // has already been through the legacy fog path's scale.
  vec3 fogColor;
  uint enable;

  // The game's ramp, in world units: opacity is saturate((d - rampStart) / (rampEnd - rampStart)).
  // rampStart is often negative - a scripted fog bank sets it that way so the ramp is already
  // underway at the camera.
  float rampStart;
  float rampEnd;
  // How far the fog is allowed to take its colour from the sky in the view direction rather than
  // from the palette. This is aerial perspective stated plainly: distant things fade towards
  // whatever sky is behind them, so once the sky is being simulated the fog has to follow it or the
  // two describe different weather. Carrying the same weight the sky was blended with is what keeps
  // them the same weather.
  float skyColorWeight;

  // How much of the medium a ray that hits no geometry is allowed to pick up.
  //
  // The ramp above already refuses to run on a sky pixel; the froxel grid was never told the same
  // thing, so the sky arrives dimmed and tinted by the grid's whole depth while the terrain in
  // front of it, which fades towards the sky via skyColorWeight, does not. It bites harder here
  // than upstream because this medium is an artistic quantity, not air: the game's fog closes over
  // tens of metres, so exp(-sigma * gridDepth) is large and all of it lands on the sky.
  // DusklightAtmosphere.md §14.8.
  //
  // 0 = Off, the untreated behaviour, kept as the A/B baseline.
  // 1 = Exempt, sky ignores the medium entirely. What the original did - it drew its sky dome with
  //     fog switched off, at any density.
  // 2 = Weighted, sky picks up skyFogAmount of the medium, so a genuinely foggy day still veils it.
  uint skyFogMode;
  // Only read in Weighted mode. 0 matches Exempt, 1 matches Off.
  float skyFogAmount;
  float pad0;
  float pad1;
  float pad2;
};

#endif  // DUSKLIGHT_COMPOSITE_ARGS_H
