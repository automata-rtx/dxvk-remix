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
#include "rtx_gt7.h"
#include "rtx_imgui.h"

#include <algorithm>
#include <cmath>

namespace dxvk {

  namespace {
    // Transcribed from the reference. In Gran Turismo, 1.0 in the linear frame buffer
    // corresponds to REFERENCE_LUMINANCE cd/m^2.
    constexpr float kReferenceLuminance = 100.0f;

    // Literals the reference passes to initializeCurve() in initializeParameters(). midPoint,
    // linearSection and toeStrength are also mirrored in gt7.slangh, which evaluates the curve;
    // alpha is only needed here, to derive the shoulder constants.
    constexpr float kCurveAlpha = 0.25f;
    constexpr float kCurveMidPoint = 0.538f;
    constexpr float kCurveLinearSection = 0.444f;

    float physicalValueToFrameBufferValue(float physical) {
      return physical / kReferenceLuminance;
    }

    // ST-2084 inverse EOTF, transcribed from the reference.
    float inverseEotfSt2084(float v) {
      constexpr float m1 = 0.1593017578125f;
      constexpr float m2 = 78.84375f;
      constexpr float c1 = 0.8359375f;
      constexpr float c2 = 18.8515625f;
      constexpr float c3 = 18.6875f;
      constexpr float pqC = 10000.0f;

      const float y = (v * kReferenceLuminance) / pqC;
      const float ym = std::pow(std::max(y, 0.0f), m1);
      return std::exp2(m2 * (std::log2(c1 + c2 * ym) - std::log2(1.0f + c3 * ym)));
    }
  }

  Gt7Args Gt7Settings::buildArgs() {
    Gt7Args args = {};

    // initializeAsSDR(): the curve is built against paper white, and sdrCorrectionFactor_ scales
    // the result back into the 0..1 range sRGB expects.
    const float paperWhiteNits = std::max(gt7PaperWhiteNits(), 1.0f);
    const float target = physicalValueToFrameBufferValue(paperWhiteNits);

    args.peakIntensity = target;
    args.outputScale = 1.0f / target;

    // Our scene referred input has mid grey at keyValue; GT's convention puts display white at
    // 'target'. See the note in the header - this is what keeps auto exposure working unchanged.
    args.inputScale = target;

    // initializeCurve(), verbatim.
    const float k = (kCurveLinearSection - 1.0f) / (kCurveAlpha - 1.0f);
    args.kA = target * kCurveLinearSection + target * k;
    args.kB = -target * k * std::exp(kCurveLinearSection / k);
    args.kC = -1.0f / (k * target);

    // framebufferLuminanceTargetUcs_ is the I channel of ICtCp for a neutral at 'target'. The
    // ICtCp LMS rows each sum to 4096, so a neutral gives l == m == s == target and I collapses
    // to a single PQ evaluation - no need for the full matrix here.
    args.targetUcs = inverseEotfSt2084(target);

    return args;
  }

  void Gt7Settings::showImguiSettings() {
    ImGui::Indent();
    RemixGui::DragFloat("GT7 Paper White (nits)", &gt7PaperWhiteNitsObject(), 1.0f, 100.0f, 1000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);

    const Gt7Args args = buildArgs();
    ImGui::TextWrapped("Scene values are scaled by %.2fx into GT's frame buffer convention and "
                       "scaled back by %.2fx on the way out, so a scene mid grey of "
                       "rtx.autoExposure.keyValue still lands on display mid grey.",
                       args.inputScale, args.outputScale);
    ImGui::TextWrapped("Chroma is left untouched until a pixel reaches 98%% of display peak, which "
                       "is what keeps sky hue stable.");
    ImGui::Unindent();
  }

}
