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

// The game's own fog ramp, applied on top of whatever the medium already did.
//
// Two ways of splitting the work, selected by fogRampMode, and the difference is the whole point of
// the 2026-08-13 rework.
//
// Handover (the original) split by distance: the froxel grid owned everything inside its reach and
// the ramp owned everything past it, rebased at the handover so neither counted the same air twice.
// That makes the medium responsible for reproducing the fog's appearance in the near field, and a
// homogeneous medium cannot do it - the game's ramp is exactly zero before fogStartZ and an
// exponential starts extinguishing at the camera, so the near field arrives hazed where the
// original is crisp.
//
// Top up splits by job instead. The ramp owns the fog's appearance at every distance, applied as
// the residual against what the medium already achieved; the medium owns the light, and its density
// becomes free to set for shaft quality rather than pinned to reproducing an opacity curve. The
// residual is exact rather than approximate - see the derivation at the use site in
// composite.comp.slang. DusklightAtmosphere.md §5.2.
struct DusklightCompositeArgs {
  // Radiance the far ramp tends towards. By default this is the fully resolved ambient - palette
  // steered towards the dome's sphere mean - which is exactly what the medium's own in-scatter
  // blends towards, so both halves of the fog reach one colour. With
  // rtx.dusklight.atmosphere.fogColorDirectional on it is the palette-derived radiance instead and
  // skyColorWeight below does the dome blend per pixel; see that field.
  //
  // Still carried separately from CompositeArgs' own fog colour. That one used to have been through
  // rtx.fogColorScale; since 2026-08-17 the legacy path is fed the same resolved radiance and the
  // scale is bypassed while the atmosphere is active, so the two now agree - but they are still two
  // fields with two lifetimes and merging them would couple this struct to the legacy fog's.
  vec3 fogColor;
  uint enable;

  // The game's ramp, in world units. rampStart is often negative - a scripted fog bank sets it that
  // way so the ramp is already underway at the camera.
  float rampStart;
  float rampEnd;
  // Where the froxel grid stops. Read only in Handover mode, where it is the distance the ramp is
  // rebased at; Top up mode runs the ramp from the camera and needs no handover at all.
  float handoverDistance;
  // How far the fog is allowed to take its colour from the sky in the view direction rather than
  // from the palette. This is aerial perspective stated plainly: distant things fade towards
  // whatever sky is behind them, which is also why the original authored its fog colour in the same
  // palette entry as its sky.
  //
  // ZERO BY DEFAULT SINCE 2026-08-17, with fogColor above carrying the already-resolved ambient
  // instead. The reason is the top-up identity: the medium's ambient in-scatter is isotropic and can
  // only be given one colour per frame (the dome's sphere mean), so a far ramp that samples the same
  // dome *directionally* blends towards a different colour than the near half does, and
  // "S * (1 - f) + A * f" silently has two A's in it wherever the dome is not uniform - which
  // outdoors is everywhere. Agreement can only be reached by making the far half isotropic too,
  // because the near half has nowhere to put a direction. rtx.dusklight.atmosphere.fogColorDirectional
  // puts the directional sample back for anyone who prefers the tint to the identity, and is what
  // makes this non-zero.
  //
  // It used to carry the physical sky's blend weight, on the reasoning that the palette stops
  // describing the sky once the sky is simulated. True, but too narrow: the game's fog colour is its
  // sky colour under *either* model. When it is non-zero now it carries
  // rtx.dusklight.atmosphere.skyAmbientWeight - the same weight the near half's ambient was blended
  // with - which is as close as the directional path can get to agreeing.
  float skyColorWeight;

  // How much of the medium a ray that hits no geometry is allowed to pick up.
  //
  // The far ramp above already refuses to run on a sky pixel; the froxel grid was never told the
  // same thing, so the sky arrives dimmed and tinted by the grid's whole depth while the terrain in
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

  // 0 = Handover, the original distance split, kept as the A/B baseline.
  // 1 = Top up, the ramp reaches the game's exact opacity at every distance.
  // 2 = Exact ramp. Takes the same code path as Top up here, deliberately: the difference lives in
  //     the medium, which is given sigma(d) = 1 / (end - d) by the fork's sampleDensityField
  //     override in rtx/algorithm/volume_lighting.slangh. The residual computed below then comes out
  //     *flat* rather than merely small - zero when the game's ramp starts at or beyond the camera,
  //     and a constant -start / (end - start) when it starts behind it, which is fog the original
  //     had already applied before the camera and no medium can reproduce. The residual is kept
  //     rather than skipped because it carries that constant, because it is the safety net for the
  //     divergence clamp at the ramp's very end and for a froxel grid the metre ceiling stopped
  //     short, and because a residual that *varies with distance* here is how the mode reports being
  //     broken.
  uint fogRampMode;
  // Trim on the dome-derived colour. Applied here as well as on the host's sphere average, or the
  // near and far halves would be trimmed by different amounts.
  float skyAmbientScale;
};

#endif  // DUSKLIGHT_COMPOSITE_ARGS_H
