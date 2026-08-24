/*
* Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
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

#include "../dxvk_include.h"
#include "rtx_resources.h"
#include "dxvk_image.h"

// this gets included from other modules, so use full path to external --- ugly!
#ifdef _M_X64
#include "../../../external/ngx_sdk_dldn/include/nvsdk_ngx.h"
#else
#include "../../../external/ngx_sdk_dldn_arm64/include/nvsdk_ngx.h"
#endif

namespace dxvk {

  class NGXDLSSContext;
  class DxvkCommandList;
  class DxvkBarrierSet;
  class DxvkContext;

  enum class DLSSProfile : uint32_t {
    UltraPerf = 0,
    MaxPerf,
    Balanced,
    MaxQuality,
    Auto,
    FullResolution,
    Invalid
  };

  enum class PathTracerPreset : int {
    Default,
    RayReconstruction,
  };

  // DLSS render presets, spelled as the numbers NGX itself gives them.
  //
  // WHY NUMBERS RATHER THAN NVSDK_NGX_*_Hint_Render_Preset_* ENUMERATORS. Two
  // reasons, and the second is the one that would cost a build:
  //
  //  - The preset is a number handed to the DLSS runtime at feature creation,
  //    and it is that runtime - not the header we compile against, and not
  //    necessarily the DLL sitting next to the game - that decides what the
  //    number means. The NVIDIA App's global DLSS Override substitutes a newer
  //    DLL at load time, so the presets on offer are whatever that substituted
  //    runtime implements. Binding to header symbols would put them out of
  //    reach for no gain; RR preset F, below, is the worked example.
  //  - The NGX SDK is not in this tree; packman fetches it at build time
  //    (`ngx_sdk_dldn`, packman-external.xml). An enumerator missing from the
  //    pinned copy is therefore a Windows build failure that no Linux check can
  //    see - and the pinned copy is demonstrably older than NVIDIA's published
  //    header, because rtx_ray_reconstruction.cpp still compiles
  //    NVSDK_NGX_RayReconstruction_Hint_Render_Preset_A, which the published
  //    header has removed. J/K/L/M may well not be in it.
  //
  // Letters, numbers and what each one is: NVIDIA/DLSS
  // `include/nvsdk_ngx_defs.h:72-88` (super resolution) and
  // `include/nvsdk_ngx_defs_dlssd.h:38-54` (ray reconstruction), read
  // 2026-08-23. Presets those headers mark "do not use, reverts to default
  // behavior" are absent - offering a letter that silently does nothing is
  // worse than not offering it - with one deliberate exception, RR preset F,
  // which that header calls unused and which a substituted runtime does
  // implement. Established by looking at it, not by reading a header.

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
    F = 6,        // Functional only when nvngx_dlssd.dll is substituted for a newer one, which the NVIDIA App's
                  // DLSS Override does globally. On the shipped DLL, NGX's header calls F unused and it reverts
                  // to default. What has and has not been looked at: dusklight-ao/docs/remix-open-issues.md.
  };

  const char* dlssProfileToString(DLSSProfile dlssProfile);

  class DxvkDLSS : public CommonDeviceObject, public RtxPass {
  public:
    enum class MotionVectorScale : uint32_t {
      Absolute,   ///< Motion vectors are provided in absolute screen space length (pixels).
      Relative,   ///< Motion vectors are provided in relative screen space length (pixels divided by screen width/height).
    };

    explicit DxvkDLSS(DxvkDevice* device);
    ~DxvkDLSS();
    static NVSDK_NGX_PerfQuality_Value profileToQuality(DLSSProfile profile);

    bool supportsDLSS() const;

    void setSetting(const uint32_t displaySize[2], const DLSSProfile profile, uint32_t outRenderSize[2]);
    // Gets the profile DLSS is currently using (the actual profile, not the settings-based one
    // which may be Auto for example).
    DLSSProfile getCurrentProfile() const;
    // Gets the input (the potentially lower resolution) size to be provided to DLSS.
    void getInputSize(uint32_t& width, uint32_t& height) const;
    // Gets the output (the potentially upscaled higher resolution) size to be provided to DLSS.
    void getOutputSize(uint32_t& width, uint32_t& height) const;

    void dispatch(
      Rc<RtxContext> ctx,
      DxvkBarrierSet& barriers,
      const Resources::RaytracingOutput& rtOutput,
      bool resetHistory = false);

    void showImguiSettings();

    void onDestroy();

    void release();

    // Note: read by DxvkDLSS only. DxvkRayReconstruction derives from this class but takes its
    // preset from rtx.rayreconstruction.renderPresetOverride instead - the two features have
    // separate preset spaces, and the same letter does not name the same network in both.
    RTX_OPTION("rtx.dlss", DLSSRenderPreset, renderPreset, DLSSRenderPreset::Default,
               "Which DLSS super resolution preset (network) to ask NGX for.\n"
               "Default leaves the choice to NGX, which picks per quality mode and may change it over the air.\n"
               "5: preset E, the last CNN-era preset still honoured. 10: J. 11: K. 12: L. 13: M - all transformer.\n"
               "Only consulted when DLSS is upscaling without Ray Reconstruction; DLSS-RR has its own preset option.\n"
               "Changing this recreates the DLSS feature, which costs a frame.");

  protected:
    virtual bool isEnabled() const override;

    static DLSSProfile getAutoProfile(uint32_t displayWidth, uint32_t displayHeight);

    void initializeDLSS(Rc<DxvkContext> pRenderContext);

    bool useDlssAutoExposure() const;

    // Options
    DLSSProfile                 mProfile = DLSSProfile::Invalid;
    DLSSProfile                 mActualProfile = DLSSProfile::Invalid;
    MotionVectorScale           mMotionVectorScale = MotionVectorScale::Absolute;
    bool                        mIsHDR = true;
    float                       mPreExposure = 1.f;
    bool                        mAutoExposure = false;
    bool                        mInverseDepth = false;

    bool                        mRecreate = true;
    DLSSRenderPreset            mPrevRenderPreset = DLSSRenderPreset::Default;  ///< Preset the live feature was created with.
    uint32_t                    mInputSize[2] = {};            ///< Input size in pixels.
    uint32_t                    mDLSSOutputSize[2] = {};       ///< DLSS output size in pixels.

    bool                        mBiasCurrentColorEnabled = false;
    std::unique_ptr < NGXDLSSContext > m_dlssContext;
  };
}  // namespace dxvk
