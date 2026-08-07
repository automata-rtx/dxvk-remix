/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
#pragma once

#include "rtx_option.h"
#include "rtx/pass/tonemap/tonemapping.h"

namespace dxvk {

  // Setup for the GT7 operator, mirroring the reference implementation's
  // initializeAsSDR() / initializeCurve() (see
  // src/dxvk/shaders/rtx/pass/tonemap/reference/gt7_tone_mapping.cpp).
  //
  // Everything that does not vary per pixel is resolved here rather than in the shader, both
  // because it is cheaper and because keeping the setup as a literal transcription makes it
  // diffable against the reference.
  class Gt7Settings {
  public:
    // Builds the shader-side argument block for SDR output.
    static Gt7Args buildArgs();

    static void showImguiSettings();

    // GT7 is display referred: in Gran Turismo, frame buffer 1.0 means 100 cd/m^2 and SDR paper
    // white is 250 cd/m^2, so a pixel has to reach 2.5 to render as display white. Our pipeline
    // hands the operator a scene referred value whose mid grey sits at
    // rtx.autoExposure.keyValue. The ratio of the two conventions is the input scale, which the
    // operator's own sdrCorrectionFactor then undoes on the way out - so auto exposure keeps
    // working unchanged and needs no GT7 special case.
    //
    // Raising this does not simply brighten the image: it moves the whole scene further up the
    // curve, so the shoulder and the chroma fade engage earlier. It is a headroom control, not
    // an exposure control - use rtx.tonemap.exposureBias or the auto exposure key value for
    // brightness.
    RTX_OPTION_ARGS("rtx.tonemap", float, gt7PaperWhiteNits, 250.0f,
                    "SDR paper white for the GT7 operator, in cd/m^2. 250 is the value the reference implementation is calibrated around; frame buffer 1.0 is defined as 100 cd/m^2, so this also sets how much headroom sits above mid grey before the shoulder and the chroma fade engage.",
                    args.minValue = 100.0f,
                    args.maxValue = 1000.0f);

    // An addition to the reference, not part of it. Inert at 1.0, where the operator stays
    // bit-identical to Polyphony's implementation.
    //
    // Applied to the ICtCp chroma, so it adds saturation without rotating hue - scaling Ct and Cp
    // by the same factor cannot change atan2(Cp, Ct). Measured ICtCp hue drift is under 1 degree
    // at 1.5 and under 4 at 2.0, the residual being gamut clipping on deeply saturated blues on
    // the way back to Rec.709. Note that CIELAB reports far larger drift on blues; that is
    // CIELAB's own blue non-linearity rather than an error in the boost.
    //
    // Gated on intensity so it lifts lit surfaces and leaves the sky and bright emissives alone -
    // see kGt7SatBoostKneeStart in gt7.slangh for the measured ranges it was chosen from.
    RTX_OPTION_ARGS("rtx.tonemap", float, gt7SaturationBoost, 1.0f,
                    "Saturation boost for the GT7 operator, applied to lit surfaces only. 1.0 is off and leaves the operator exactly as its reference implementation. The boost fades out with pixel intensity so that sky and bright emissives such as lava keep the colour the operator gives them - at 2.0 the sky gains roughly 4 percent chroma while lava gains none. Applied in ICtCp so it does not rotate hue; beyond about 1.5 deeply saturated blues start to clip against the display gamut.",
                    args.minValue = 1.0f,
                    args.maxValue = 2.0f);
  };

}
