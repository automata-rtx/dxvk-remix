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

#include <cstdint>

#include "rtx_option.h"

namespace dxvk {
  // Lets the user pick which DLSS network runs, for both DLSS super resolution and DLSS Ray
  // Reconstruction. Remix exposes the DLSS quality mode but not the render preset - the network
  // itself - so without this the choice is NGX's, and revisable over the air.
  //
  // Everything the feature owns lives in this pair of files: the enums, both options, the two
  // dropdowns and the one predicate the greyed-out controls ask. The files that DxvkDLSS,
  // DxvkRayReconstruction and the two menus already occupy keep only single-line hooks into it,
  // which is what lets this fork rebase onto upstream without hand-merging the feature every time.
  //
  // WHY NUMBERS RATHER THAN NVSDK_NGX_*_Hint_Render_Preset_* ENUMERATORS. Two reasons, and the
  // second is the one that would cost a build:
  //
  //  - The preset is a number handed to the DLSS runtime at feature creation, and it is that
  //    runtime - not the header we compile against - that decides what the number means. A DLL
  //    newer than our SDK can honour a preset its header calls unused, and binding to header
  //    symbols would put that out of reach for no gain.
  //  - The NGX SDK is not in this tree; packman fetches it at build time (`ngx_sdk_dldn`,
  //    packman-external.xml). An enumerator missing from the pinned copy is therefore a Windows
  //    build failure that no Linux check can see - and the pinned copy is demonstrably older than
  //    NVIDIA's published header, because rtx_ray_reconstruction.cpp still compiles
  //    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_A, which the published header has removed.
  //    J/K/L/M may well not be in it either.
  //
  // Letters, numbers and what each one is: NVIDIA/DLSS `include/nvsdk_ngx_defs.h:72-88` (super
  // resolution) and `include/nvsdk_ngx_defs_dlssd.h:38-54` (ray reconstruction), read 2026-08-23.
  // Presets those headers mark "do not use, reverts to default behavior" are deliberately absent
  // here - offering a letter that silently does nothing is worse than not offering it.

  // Super resolution presets, used when DLSS is upscaling without Ray Reconstruction.
  enum class DLSSRenderPreset : uint32_t {
    Default = 0,  // No override; NGX picks per quality mode, and may change it over the air.
    E = 5,        // The last CNN-era preset NGX still honours. Marked deprecated rather than "do not use".
    J = 10,       // Transformer. Slightly less ghosting than K, at the cost of more flicker.
    K = 11,       // Transformer. NGX's own default for DLAA, Quality and Balanced. Best quality, highest cost.
    L = 12,       // Transformer. NGX's own default for Ultra Performance.
    M = 13,       // Transformer. NGX's own default for Performance.
  };

  // Ray Reconstruction presets, used when DLSS-RR is denoising and upscaling.
  enum class DLSSRRRenderPreset : uint32_t {
    Default = 0,  // No override; the CNN/Transformer and Transformer-D settings decide, as they always did.
    D = 4,        // Transformer. NGX's default RR model.
    E = 5,        // Later transformer. NGX requires it if a depth-of-field guide is supplied.
    F = 6,        // NGX's published header calls this unused; newer DLSS runtimes ship it, so it is offered.
  };

  // The two presets sit in separate option scopes because they are separate preset spaces: the same
  // letter does not name the same network in both, and only one of the two features runs at a time.
  //
  // Named "render preset" throughout because two other things in this tree are already called a DLSS
  // preset - rtx_user_menu.cpp's "DLSS Preset" is Disabled/Enabled/Custom, and
  // rtx_ray_reconstruction.cpp's "DLSS-RR Preset" picks a path tracer preset. DLSSRenderPreset is
  // also one capital away from the existing DlssPreset, which is why it is not called DLSSPreset.

  // Read where the DLSS feature is created. DxvkRayReconstruction takes its preset from
  // DLSSRRRenderPresetOptions instead, and never consults this one.
  //
  // UserSetting on both options because both dropdowns are drawn in the simplified player menu, and
  // that menu's Save Settings button saves only the user layer: an unflagged option's edit lands in
  // rtx.conf instead, leaving the button dark and the choice gone at the next launch. Every control
  // beside them there - upscalerType, enableRayReconstruction, qualityDLSS, rayreconstruction.model -
  // carries the same flag.
  struct DLSSRenderPresetOptions {
    RTX_OPTION_ARGS("rtx.dlss", DLSSRenderPreset, renderPreset, DLSSRenderPreset::Default,
                    "Which DLSS super resolution preset (network) to ask NGX for.\n"
                    "0: Default, leaving the choice to NGX, which picks per quality mode and may change it over the air.\n"
                    "5: preset E, the last CNN-era preset still honoured. 10: J. 11: K. 12: L. 13: M - all transformer.\n"
                    "Only consulted when DLSS is upscaling without Ray Reconstruction; DLSS-RR has its own preset option.\n"
                    "Changing this recreates the DLSS feature, which costs a frame.",
                    args.flags = RtxOptionFlags::UserSetting);
  };

  // Overrides the preset that DxvkRayReconstruction's `model` and `enableTransformerModelD` would
  // otherwise select. Those two are a two-bit spelling of the same choice and feed nothing else, so
  // while this is set to anything but Default they have no effect at all - which is why the menus
  // grey them out rather than leaving two live-looking controls that do nothing.
  struct DLSSRRRenderPresetOptions {
    RTX_OPTION_ARGS("rtx.rayreconstruction", DLSSRRRenderPreset, renderPresetOverride, DLSSRRRenderPreset::Default,
                    "Which DLSS Ray Reconstruction preset (network) to ask NGX for, overriding the model settings.\n"
                    "0: Default, deferring to rtx.rayreconstruction.model and rtx.rayreconstruction.enableTransformerModelD.\n"
                    "4: preset D, NGX's default transformer. 5: E, a later transformer. 6: F.\n"
                    "NGX's published header describes F as unused; it is offered here because newer DLSS runtimes ship it.\n"
                    "A preset the installed runtime does not have falls back to that runtime's default behaviour rather than failing.\n"
                    "Changing this recreates the DLSS-RR feature, which costs a frame.",
                    args.flags = RtxOptionFlags::UserSetting);
  };

  // True while an explicit Ray Reconstruction render preset supersedes the Ray Reconstruction Model
  // combo and the Transformer Model D checkbox. Both controls ask this before drawing themselves.
  bool isRayReconstructionRenderPresetOverridden();

  // Draws the render preset dropdown for whichever DLSS feature is running: the Ray Reconstruction
  // list when DLSS-RR is on, the super resolution list otherwise, and nothing at all when DLSS is
  // not the upscaler. Call it from the branch that already establishes DLSS is running; the
  // upscaler check inside is a floor, not the decision, so that a caller placed anywhere else draws
  // nothing rather than writing a preset no upscaler reads.
  void showDlssRenderPresetCombo();
}
