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

// How the game's two horizon haze bands are collapsed into the one horizon colour. Selected by
// DusklightAtmosphereArgs::kasumiBlendMode; the option text on rtx.dusklight.atmosphere.kasumiBlendMode
// carries the full argument.
//
// Places one band at the sun's bearing and the other opposite it, so the horizon palette turns as the
// sun moves. Built on a reading of the pair that the game contradicts, and kept as the default anyway
// so that changing the look stays a deliberate act rather than a side effect of updating.
#define DUSKLIGHT_KASUMI_BLEND_SUN_RELATIVE 0
// Azimuth independent, which is what the game does: two dome actors paint one band each and both are
// drawn at every bearing.
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

  // The game's two horizon haze bands. The split is front/back, not sun-relative: two dome actors
  // paint one band each - d_a_vrbox.cpp:121-124 writes kasumi_inner onto vrbox_sora.bmd and
  // d_a_vrbox2.cpp:358-361 writes kasumi_outer onto vrbox_kasumiM.bmd - and both are drawn at every
  // bearing, with neither reading sun position to choose between them.
  //
  // Note "outer" is the NEAR band and "inner" the FAR one, opposite to what the English reads like.
  // The member names are the decompilation's reconstruction; the game's own labels are not, and they
  // disagree with it - the debug view prints the pair as kasumiF and kasumiB
  // (d_kankyo_debug.cpp:301,306) and the palette CSV header (d_kankyo.cpp:6582) names them near and
  // far in Japanese. rtx_dusklight_env.h:170-176 has the same derivation and, being C++, may quote
  // those labels; this file may not, and neither may anything else under src/dxvk/shaders/.
  // scripts-common/compile_shaders.py:491 reads shader source with a bare open(path, "r"), which on
  // Windows decodes the UTF-8 bytes as cp1252, and kana/kanji contain bytes that page leaves
  // undefined - a UnicodeDecodeError that takes out all three CI configs. The section signs further
  // down are not a precedent: they survive as mojibake only because 0xA7 is a defined cp1252 byte.
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
  // Which of DUSKLIGHT_KASUMI_BLEND_* above collapses the two haze bands into horizonColor. Took
  // pad0 rather than growing the struct: this is 112 bytes against a hard 128-byte push constant
  // budget, and the budget is enforced by a tripwire that only fires in a Windows build
  // (rtx_tone_mapping.cpp:43-46 documents the mechanism), so a Linux checkout would not find out
  // that it had been exceeded until CI did.
  uint kasumiBlendMode;
  // Share of the near ("front") band in DUSKLIGHT_KASUMI_BLEND_FIXED, 0..1. Stands in for that
  // band's own alpha, which is what actually decides how much of the far band it hides - the game
  // authors one (d_a_vrbox2.cpp:361 paints it) but this build's bridge does not send it, so the
  // share is a setting rather than a translation. Unused in the sun-relative mode. Took pad1.
  float kasumiFrontWeight;
};

#endif  // DUSKLIGHT_ATMOSPHERE_H
