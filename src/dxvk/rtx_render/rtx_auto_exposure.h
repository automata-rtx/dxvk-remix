/*
* Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
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

#include "dxvk_format.h"
#include "dxvk_include.h"
#include "dxvk_context.h"
#include "rtx_resources.h"

#include "../spirv/spirv_code_buffer.h"
#include "../util/util_matrix.h"
#include "rtx_options.h"

#include "rtx/pass/tonemap/tonemapping.h"

namespace dxvk {

  class DxvkDevice;

  class DxvkAutoExposure: public CommonDeviceObject {
  public:
    explicit DxvkAutoExposure(DxvkDevice* device);
    ~DxvkAutoExposure();

    void dispatch(
      Rc<DxvkContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const Resources::RaytracingOutput& rtOutput,
      const float frameTimeMilliseconds,
      bool resetHistory = false);

    void showImguiSettings();

    void createResources(Rc<DxvkContext> ctx);
    const Resources::Resource& getExposureTexture() const { return m_exposure; }

  private:

    void dispatchAutoExposure(
      Rc<DxvkContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const Resources::RaytracingOutput& rtOutput,
      const float frameTimeMilliseconds);

    // Logs a one-time notice for any deprecated option a config still sets. The options stay
    // registered as no-ops so existing rtx.conf files neither spam nor fail to load.
    void warnOnDeprecatedOptions();

    Rc<vk::DeviceFn> m_vkd;

    Resources::Resource m_exposure;
    Resources::Resource m_exposureHistogram;

    // Debug stats readback. Written by the reduction pass, copied into a host-visible ring and
    // read kMaxFramesInFlight frames later, so nothing ever stalls waiting on the GPU.
    Rc<DxvkBuffer> m_debugStatsGpu;
    Rc<DxvkBuffer> m_debugStatsHost;
    AutoExposureDebugStats m_debugStats = {};
    bool m_debugStatsRequested = false;

    bool m_resetState = true;
    bool m_deprecationChecked = false;

    // Retained only so that configs setting rtx.autoExposure.exposureAverageMode still parse.
    enum ExposureAverageMode : uint32_t {
      Mean = 0,
      Median
    };

    RTX_OPTION("rtx.autoExposure", bool, enabled, true, "Automatically adjusts exposure so that the image won't be too bright or too dark.");

    RTX_OPTION("rtx.autoExposure", bool,  exposureCenterMeteringEnabled, false, "Gives higher weight to pixels around the screen center.");
    RTX_OPTION_ARGS("rtx.autoExposure", float, centerMeteringSize, 0.5f, "The importance of pixels around the screen center.",
                    args.minValue = 0.01f,
                    args.maxValue = 1.0f);

    // Metering
    RTX_OPTION_ARGS("rtx.autoExposure", float, keyValue, 0.18f, "Target mid grey. The luminance the metered scene is driven towards at full adaptation strength.",
                    args.minValue = 0.02f,
                    args.maxValue = 0.60f);
    RTX_OPTION_ARGS("rtx.autoExposure", float, adaptationStrength, 0.85f, "How completely the scene is normalised. 1.0 makes every scene read as equally bright, 0.0 disables auto exposure entirely. Below 1.0 dark rooms keep reading as darker than bright exteriors.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.autoExposure", float, histogramLowPercentile, 0.25f, "Fraction of the luminance histogram trimmed off the dark end before averaging. Rejects crushed blacks.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.9f);
    // Rule of thumb, measured against synthetic blowouts: a bright region is rejected outright
    // while it covers less than (1 - histogramHighPercentile) of the frame, and its influence
    // then grows smoothly past that. At the 0.95 default a lamp or muzzle flash covering up to
    // 5% of the screen moves exposure not at all; at 10% it moves it by roughly 0.8 EV, and at
    // 20% by roughly 2 EV. Lower this if large blowouts are still pumping the exposure.
    RTX_OPTION_ARGS("rtx.autoExposure", float, histogramHighPercentile, 0.95f, "Upper edge of the luminance histogram window used for averaging. Rejects skybox and sun pixels. A bright region is rejected entirely while it covers less than (1 - this) of the frame. Clamped to stay above rtx.autoExposure.histogramLowPercentile.",
                    args.minValue = 0.5f,
                    args.maxValue = 1.0f);

    // Limiting
    RTX_OPTION_ARGS("rtx.autoExposure", float, softLimitCenterEV, 1.5f, "Centre of the soft exposure limiter, in EV100. The metered scene EV is compressed smoothly around this point instead of being hard clamped.",
                    args.minValue = -8.0f,
                    args.maxValue = 16.0f);
    RTX_OPTION_ARGS("rtx.autoExposure", float, softLimitRangeEV, 6.0f, "Half-width of the soft exposure limiter, in EV. The metered scene EV is asymptotically bounded to rtx.autoExposure.softLimitCenterEV plus or minus this value.",
                    args.minValue = 0.5f,
                    args.maxValue = 16.0f);

    // Temporal adaptation
    RTX_OPTION_ARGS("rtx.autoExposure", float, tauBrighten, 1.00f, "Time constant in seconds for the direction that brightens the image, i.e. walking into a dark room. Deliberately the slow direction, matching how eyes recover in the dark.",
                    args.minValue = 0.05f,
                    args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.autoExposure", float, tauDarken, 0.18f, "Time constant in seconds for the direction that darkens the image, i.e. stepping into sunlight. Deliberately the fast direction.",
                    args.minValue = 0.02f,
                    args.maxValue = 2.0f);
    RTX_OPTION_ARGS("rtx.autoExposure", float, deadbandEV, 0.10f, "Exposure holds completely still while the target is within this many EV of the current value. Suppresses low amplitude hunting caused by histogram sampling noise. Values below roughly 0.1 sit under the histogram's own quantisation floor and will not reliably engage.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.5f);
    RTX_OPTION_ARGS("rtx.autoExposure", float, cutSnapThresholdEV, 4.0f, "A single frame demanding more than this many EV of change snaps instead of easing, so level loads and camera cuts do not fade in. Set to 0 to disable snapping.",
                    args.minValue = 0.0f,
                    args.maxValue = 20.0f);

    // Deprecated. Kept registered as no-ops so that existing configs load without unknown-option
    // spam; each logs a one-time notice from warnOnDeprecatedOptions() if a config still sets it.
    RTX_OPTION("rtx.autoExposure", float, autoExposureSpeed, 5.f, "Deprecated, no longer has any effect. Replaced by rtx.autoExposure.tauBrighten and rtx.autoExposure.tauDarken, which model the two adaptation directions separately.");
    RTX_OPTION("rtx.autoExposure", float, evMinValue, -2.0f, "Deprecated, no longer has any effect. The histogram now covers a fixed wide EV range and limiting is done by rtx.autoExposure.softLimitCenterEV and rtx.autoExposure.softLimitRangeEV.");
    RTX_OPTION("rtx.autoExposure", float, evMaxValue, 5.f, "Deprecated, no longer has any effect. The histogram now covers a fixed wide EV range and limiting is done by rtx.autoExposure.softLimitCenterEV and rtx.autoExposure.softLimitRangeEV.");
    RTX_OPTION("rtx.autoExposure", ExposureAverageMode, exposureAverageMode, ExposureAverageMode::Median, "Deprecated, no longer has any effect. Metering is always a percentile-trimmed log average now, controlled by rtx.autoExposure.histogramLowPercentile and rtx.autoExposure.histogramHighPercentile.");
    RTX_OPTION("rtx.autoExposure", bool, useExposureCompensation, false, "Deprecated, no longer has any effect. Superseded by percentile trimming.");
    RTX_OPTION("rtx.autoExposure", float, exposureWeightCurve0, 1.f, "Deprecated, no longer has any effect. Superseded by percentile trimming.");
    RTX_OPTION("rtx.autoExposure", float, exposureWeightCurve1, 1.f, "Deprecated, no longer has any effect. Superseded by percentile trimming.");
    RTX_OPTION("rtx.autoExposure", float, exposureWeightCurve2, 1.f, "Deprecated, no longer has any effect. Superseded by percentile trimming.");
    RTX_OPTION("rtx.autoExposure", float, exposureWeightCurve3, 1.f, "Deprecated, no longer has any effect. Superseded by percentile trimming.");
    RTX_OPTION("rtx.autoExposure", float, exposureWeightCurve4, 1.f, "Deprecated, no longer has any effect. Superseded by percentile trimming.");
  };
  
}
