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

// How the two horizon haze bands become one horizon colour. See DusklightAtmosphereArgs::
// kasumiBlendMode and DusklightAtmosphere.md section 12.2.
// Rotates the palette with the sun's compass bearing. Built on a reading of the two bands that the
// game contradicts, kept as the default so enabling the correction is a deliberate act.
#define DUSKLIGHT_KASUMI_BLEND_SUN_RELATIVE 0
// Azimuth independent, which is what the game does: it paints one band onto each of two dome
// shells and draws both at every bearing.
#define DUSKLIGHT_KASUMI_BLEND_FIXED        1

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

  // The game's two horizon haze bands. The split is front/back, not sun-relative: the game paints
  // one onto each of two dome shells at every azimuth, and no code path in it relates either field
  // to sun position. Note "outer" is the NEAR band and "inner" the FAR one, opposite to what the
  // English reads like - the member names are the decompilation's reconstruction, while the game's
  // own labels (前 mae / 奥 oku, kasumiF / kasumiB) say front and back.
  // dusklight-ao/docs/japanese-naming.md section 6.
  //
  // The far ("back") band.
  vec3 kasumiInner;
  // 0 reproduces the game's gradient, 1 is the scattering model. Blended on the radiance rather
  // than on finished pixels, so the visible sky, the light it casts and the fog it tints all move
  // together instead of drifting apart.
  float physicalWeight;

  // The near ("front") band, despite being the one called "outer".
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

  // The moon, painted into the dome rather than drawn as geometry. The game's camera-anchored moon
  // billboard was the measured cause of night shadows wandering with the player; hideSkyBillboards
  // removes it and takes the visible moon with it. A disc painted here cannot cast a shadow because
  // it is not geometry. Deliberately the moon only - the sun is analytic and NEE-sampled, so baking
  // something that bright into an image only reached by ray miss would double count it.
  // DusklightAtmosphere.md §13.1.
  vec3 moonColor;
  // Absolute radiance of the disc, already faded across the dawn/dusk handover. 0 draws nothing.
  // Applied after the sky's own intensity so it does not ride the palette's brightness.
  float moonRadiance;

  float moonAngularRadiusRadians;
  // Width of the disc's edge falloff as a fraction of its radius. A hard edge aliases badly in a
  // lat-long map, where the sampling rate varies with latitude.
  float moonEdgeSoftness;
  // How the two kasumi bands are combined into the one horizon colour. Values are
  // DUSKLIGHT_KASUMI_BLEND_*, below. Took pad0/pad1 rather than growing the struct, so the push
  // constant footprint is unchanged.
  uint kasumiBlendMode;
  // Share of the near ("front") band in DUSKLIGHT_KASUMI_BLEND_FIXED, 0..1. Stands in for the
  // near band's alpha, which is what actually decides its coverage in the game and which the
  // bridge does not push. Unused in the sun-relative mode.
  float kasumiFrontWeight;
};

#endif  // DUSKLIGHT_ATMOSPHERE_H
