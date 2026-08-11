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
#include "rtx_agx.h"
#include "rtx_imgui.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <algorithm>

namespace dxvk {

  void migrateFinalizeWithACES(RtxOption<bool>& legacy,
                               RtxOption<TonemapOperator>& target,
                               const char* legacyName,
                               const char* targetName) {
    auto boolToOperator = [](const GenericValue& src, GenericValue& dest, bool destHasExistingValue) {
      // An explicit setting on the new option always wins over the legacy one.
      if (destHasExistingValue) {
        return false;
      }
      dest.i = static_cast<int>(src.b ? TonemapOperator::ACES : TonemapOperator::None);
      return true;
    };

    if (legacy.migrateValuesTo(&target, boolToOperator)) {
      legacy.clearFromStrongerLayers(RtxOptionLayer::getDefaultLayer());
      Logger::info(str::format("[Deprecated Config] ", legacyName, " has been migrated to ", targetName,
                               ". Please re-save your config to get rid of this message."));
    }
  }

  AgxArgs AgxSettings::buildArgs() {
    AgxArgs args = {};

    // ASC-CDL style look presets. Numbers are starting points, not authority.
    //
    // Both presets move mid grey noticeably, which is inherent to applying a power to a value
    // below 1 and is not a bug. Measured display value of a linear 0.18 input, after the sRGB
    // EOTF the srgb_dither pass applies: None 0.497, Golden (0.602, 0.512, 0.280) - a strong warm
    // cast - and Punchy 0.346. agxContrast and agxSaturation ride on top if that needs pulling
    // back.
    switch (agxLook()) {
    case AgxLook::Golden:
      args.lookSlope = Vector3(1.0f, 0.9f, 0.5f);
      args.lookOffset = 0.0f;
      args.lookPower = 0.8f;
      args.lookSaturation = 0.8f;
      break;
    case AgxLook::Punchy:
      args.lookSlope = Vector3(1.0f, 1.0f, 1.0f);
      args.lookOffset = 0.0f;
      args.lookPower = 1.35f;
      args.lookSaturation = 1.4f;
      break;
    case AgxLook::None:
    default:
      args.lookSlope = Vector3(1.0f, 1.0f, 1.0f);
      args.lookOffset = 0.0f;
      args.lookPower = 1.0f;
      args.lookSaturation = 1.0f;
      break;
    }

    // The free saturation slider rides on top of the preset rather than replacing it, so the
    // presets stay a starting point rather than a cage.
    args.lookSaturation *= std::max(agxSaturation(), 0.0f);

    // Contrast scales the log2 range the curve covers, pivoting on mid grey. A narrower range
    // means the same sigmoid spans fewer stops, which is a steeper curve.
    const float contrast = std::max(agxContrast(), 0.01f);
    args.minEv = AGX_MIDDLE_GRAY_LOG2 + (AGX_DEFAULT_MIN_EV - AGX_MIDDLE_GRAY_LOG2) / contrast;
    args.maxEv = AGX_MIDDLE_GRAY_LOG2 + (AGX_DEFAULT_MAX_EV - AGX_MIDDLE_GRAY_LOG2) / contrast;

    return args;
  }

  void AgxSettings::showImguiSettings() {
    ImGui::Indent();
    RemixGui::Combo("AgX Look", &agxLookObject(), "None\0Golden\0Punchy\0");
    RemixGui::DragFloat("AgX Saturation", &agxSaturationObject(), 0.01f, 0.f, 2.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    RemixGui::DragFloat("AgX Contrast", &agxContrastObject(), 0.01f, 0.25f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

    const AgxArgs args = buildArgs();
    ImGui::Text("Range %.2f to %.2f log2 (%.1f stops)", args.minEv, args.maxEv, args.maxEv - args.minEv);
    ImGui::Unindent();
  }

}
