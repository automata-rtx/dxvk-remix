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

#ifndef DUSKLIGHT_ATMOSPHERE_H
#define DUSKLIGHT_ATMOSPHERE_H

#include "rtx/utility/shader_types.h"

#define DUSKLIGHT_TRANSMITTANCE_OUTPUT   0
#define DUSKLIGHT_MULTISCATTER_TRANSMITTANCE 0
#define DUSKLIGHT_MULTISCATTER_OUTPUT    1

// Lookup table sizes, following Hillaire 2020. Small: both are functions of the medium alone, not
// of the view, and they are smooth in every parameter.
#define DUSKLIGHT_TRANSMITTANCE_WIDTH   256
#define DUSKLIGHT_TRANSMITTANCE_HEIGHT   64
#define DUSKLIGHT_MULTISCATTER_SIZE      32

// Everything the three atmosphere passes need that is not a compile time constant of the medium
// itself. Kept under the push constant budget by leaving the Earth coefficients in the shader and
// passing only what the game moves.
struct DusklightAtmosphereArgs {
  uint2 imageSize;
  // Where the sun is. Azimuth matches the generated sky's U axis directly; elevation is signed, so
  // it goes negative once the sun is down.
  float sunAzimuthRadians;
  float sunElevationRadians;

  // Zenith colour from the game's palette.
  vec3 skyColor;
  // Radiance multiplier applied to the finished sky, whichever way it was produced.
  float intensity;

  // Horizon haze on the sun's side.
  vec3 kasumiInner;
  // 0 reproduces the game's gradient, 1 is the scattering model. Blended on the radiance rather
  // than on finished pixels, so the visible sky, the light it casts and the fog it tints all move
  // together instead of drifting apart.
  float physicalWeight;

  // Horizon haze away from the sun.
  vec3 kasumiOuter;
  // How far the medium's own coefficients are steered towards the game's palette. This is the
  // "palette as parameters, not as pixels" knob: at 0 the model runs on Earth's numbers, at 1 its
  // scattering is tinted to match what the artists picked. Tinting the coefficients keeps the sky
  // and the light it casts consistent; tinting the output would not.
  float paletteInfluence;

  // Shape of the game's gradient, used only for the stylised half.
  float horizonSharpness;
  float groundFraction;
  // Forward scattering of the haze, 0..0.95. Higher pulls the glow tighter around the sun.
  float mieAnisotropy;
  // Scales the multiple scattering term. Without it a physical sky reads too dark and too blue,
  // because single scattering alone throws away everything that bounced more than once.
  float multiScatterScale;

  // The moon, painted into the dome rather than drawn as geometry.
  //
  // The game hangs its moon on a camera-anchored billboard, which under a path tracer became an
  // occluder travelling with the player and was the measured cause of shadows appearing to wander
  // at night. rtx.dusklight.game.hideSkyBillboards removes it and fixes that, at the cost of the
  // visible moon and stars. Painting the moon here gives it back without giving the problem back:
  // a disc in the dome is correctly placed, moves with the sky rather than the camera, and is
  // structurally incapable of casting a shadow because it is not geometry at all.
  //
  // Deliberately the moon and not the sun. The sun disc stays out of this image - it is analytic
  // and sampled as a distant light, and baking a body that bright into a dome that is only ever
  // reached by ray miss would both double count it and sample it terribly.
  vec3 moonColor;
  // Absolute radiance of the disc, already faded across the dawn/dusk handover. 0 draws nothing.
  // Applied after the sky's own intensity so it does not ride the palette's brightness.
  float moonRadiance;

  float moonAngularRadiusRadians;
  // Width of the disc's edge falloff as a fraction of its radius. A hard edge aliases badly in a
  // lat-long map, where the sampling rate varies with latitude.
  float moonEdgeSoftness;
  float pad0;
  float pad1;
};

#endif  // DUSKLIGHT_ATMOSPHERE_H
