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
#include "rtx_dlss_render_preset.h"

#include "rtx_imgui.h"
#include "rtx_options.h"

namespace dxvk {
  namespace {
    // One list per feature. The letters are not a shared namespace: super resolution preset E and
    // Ray Reconstruction preset E are different networks, which is why these are two combos over two
    // option scopes rather than one list filtered at draw time. What each letter is:
    // DLSSRenderPreset / DLSSRRRenderPreset in the header.
    //
    // File-local, and reached through showDlssRenderPresetCombo() rather than extern-ed the way
    // dlssProfileCombo and xessPresetCombo are: RemixGui::ComboWithKey deletes copy and move, so it
    // has to be defined in place, and one free function is a smaller cross-file coupling than two
    // extern declarations.
    RemixGui::ComboWithKey<DLSSRenderPreset> dlssRenderPresetCombo {
      "DLSS Render Preset",
      RemixGui::ComboWithKey<DLSSRenderPreset>::ComboEntries { {
          {DLSSRenderPreset::Default, "Default", "Let NGX choose per quality mode. It may also change the choice over the air."},
          {DLSSRenderPreset::E, "E", "The last CNN-era preset NGX still honours. Softer and more stable than the transformer presets, and cheaper."},
          {DLSSRenderPreset::J, "J", "Transformer. Close to K - slightly less ghosting, slightly more flicker."},
          {DLSSRenderPreset::K, "K", "Transformer. NGX's own default for Full Resolution, Quality and Balanced, and its best-looking preset."},
          {DLSSRenderPreset::L, "L", "Transformer. NGX's own default for Ultra Performance."},
          {DLSSRenderPreset::M, "M", "Transformer. NGX's own default for Performance."},
      } }
    };

    RemixGui::ComboWithKey<DLSSRRRenderPreset> dlssRRRenderPresetCombo {
      "Ray Reconstruction Render Preset",
      RemixGui::ComboWithKey<DLSSRRRenderPreset>::ComboEntries { {
          {DLSSRRRenderPreset::Default, "Default", "Defer to the Ray Reconstruction Model setting above, and to Transformer Model D under Denoising."},
          {DLSSRRRenderPreset::D, "D", "Transformer. NGX's default Ray Reconstruction model."},
          {DLSSRRRenderPreset::E, "E", "A later transformer than D. Required if a depth-of-field guide is ever supplied."},
          {DLSSRRRenderPreset::F, "F", "Present only in newer DLSS runtimes. An installed runtime without it falls back to its own default."},
      } }
    };
  }

  bool isRayReconstructionRenderPresetOverridden() {
    return DLSSRRRenderPresetOptions::renderPresetOverride() != DLSSRRRenderPreset::Default;
  }

  void showDlssRenderPresetCombo() {
    // isRayReconstructionEnabled() is `upscalerType() == DLSS && enableRayReconstruction()`, the same
    // predicate the menus branch on, so the list drawn here belongs to the feature that is actually
    // running. Widget writes are deferred to the end of the frame, so a click earlier in the same
    // frame cannot make these reads disagree with the branch that called us.
    if (RtxOptions::isRayReconstructionEnabled()) {
      dlssRRRenderPresetCombo.getKey(&DLSSRRRenderPresetOptions::renderPresetOverrideObject());
    } else if (RtxOptions::upscalerType() == UpscalerType::DLSS) {
      dlssRenderPresetCombo.getKey(&DLSSRenderPresetOptions::renderPresetObject());
    }
  }
}
