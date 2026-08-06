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

  // Which operator finishes the image. Shared by the global and local tone mapping paths, which
  // each carry their own selection.
  // GT7's own settings live in rtx_gt7.h; the enum stays here because both tone mapping paths
  // already include this header.
  enum class TonemapOperator : uint32_t {
    None = 0,
    ACES = 1,
    AgX = 2,
    GT7 = 3,
  };

  static_assert(static_cast<uint32_t>(TonemapOperator::None) == tonemapOperatorNone);
  static_assert(static_cast<uint32_t>(TonemapOperator::ACES) == tonemapOperatorACES);
  static_assert(static_cast<uint32_t>(TonemapOperator::AgX) == tonemapOperatorAgX);
  static_assert(static_cast<uint32_t>(TonemapOperator::GT7) == tonemapOperatorGT7);

  enum class AgxLook : uint32_t {
    None = 0,
    Golden = 1,
    Punchy = 2,
  };

  // Migrates a legacy finalizeWithACES boolean into a TonemapOperator option and logs a single
  // deprecation notice if a config actually carried a value. Call once, before first use.
  //
  // false maps to None and true maps to ACES. Mapping false to ACES would silently switch an
  // existing config to a curve it explicitly turned off, which is the more surprising of the two
  // readings; a config that never mentioned the boolean keeps the new option's default.
  void migrateFinalizeWithACES(RtxOption<bool>& legacy,
                               RtxOption<TonemapOperator>& target,
                               const char* legacyName,
                               const char* targetName);

  // AgX look and range controls. One shared set: a look is a look regardless of which tone
  // mapping path is hosting the operator, and having two independent copies would just be a way
  // to get them out of step.
  class AgxSettings {
  public:
    // Resolves the look preset and the free sliders into the shader-side argument block.
    static AgxArgs buildArgs();

    // Look preset plus free controls. Drawn by whichever tone mapper currently owns the UI.
    static void showImguiSettings();

    RTX_OPTION("rtx.tonemap", AgxLook, agxLook, AgxLook::None,
               "AgX look transform, applied between the contrast sigmoid and the outset matrix.\n"
               "Supported enum values are 0 = None (pass through), 1 = Golden (warm, slightly lifted, desaturated), 2 = Punchy (higher contrast and saturation).\n"
               "The presets are starting points; rtx.tonemap.agxSaturation and rtx.tonemap.agxContrast adjust on top of whichever is selected.");

    RTX_OPTION_ARGS("rtx.tonemap", float, agxSaturation, 1.0f,
                    "Saturation multiplier applied on top of the selected AgX look. 1.0 leaves the look's own saturation alone, 0.0 is monochrome.",
                    args.minValue = 0.0f,
                    args.maxValue = 2.0f);

    RTX_OPTION_ARGS("rtx.tonemap", float, agxContrast, 1.0f,
                    "Contrast of the AgX curve, expressed as a scale on its dynamic range about mid grey. Values above 1.0 narrow the range the curve covers and so raise contrast; values below widen it and flatten the image. 1.0 is the reference AgX range of -12.47 to +4.03 in log2 units.",
                    args.minValue = 0.25f,
                    args.maxValue = 4.0f);
  };

}
