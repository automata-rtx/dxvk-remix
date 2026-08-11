/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_tone_mapping.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx.h"
#include "rtx/pass/tonemap/tonemapping.h"

#include <rtx_shaders/auto_exposure.h>
#include <rtx_shaders/auto_exposure_histogram.h>
#include "rtx_imgui.h"
#include "rtx_render/rtx_debug_view.h"

#include "rtx/utility/debug_view_indices.h"
#include "rtx_utils.h"
#include "../../util/log/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static_assert((TONEMAPPING_TONE_CURVE_SAMPLE_COUNT & 1) == 0, "The shader expects a sample count that is a multiply of 2.");

namespace dxvk {
  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class AutoExposureHistogramShader : public ManagedShader {
      SHADER_SOURCE(AutoExposureHistogramShader, VK_SHADER_STAGE_COMPUTE_BIT, auto_exposure_histogram)

      PUSH_CONSTANTS(ToneMappingAutoExposureArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(AUTO_EXPOSURE_COLOR_INPUT)
        RW_TEXTURE1D(AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(AutoExposureHistogramShader);

    class AutoExposureShader : public ManagedShader
    {
      SHADER_SOURCE(AutoExposureShader, VK_SHADER_STAGE_COMPUTE_BIT, auto_exposure)

      PUSH_CONSTANTS(ToneMappingAutoExposureArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE1D(AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT)
        RW_TEXTURE1D(AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT)
        RW_TEXTURE2D(AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT)
        RW_STRUCTURED_BUFFER(AUTO_EXPOSURE_DEBUG_STATS_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(AutoExposureShader);
  }
  
  DxvkAutoExposure::DxvkAutoExposure(DxvkDevice* device)
  : CommonDeviceObject(device), m_vkd(device->vkd())  {
  }
  
  DxvkAutoExposure::~DxvkAutoExposure()  {  }

  void DxvkAutoExposure::showImguiSettings() {

    // Drives whether the reduction pass writes its stats and whether the readback copy is
    // issued at all, so a closed panel costs nothing.
    m_debugStatsRequested = false;

    RemixGui::Checkbox("Eye Adaptation", &enabledObject());
    if (enabled()) {
      ImGui::Indent();

      RemixGui::Checkbox("Center Weighted Metering", &exposureCenterMeteringEnabledObject());
      ImGui::BeginDisabled(!exposureCenterMeteringEnabled());
      RemixGui::DragFloat("Center Metering Size", &centerMeteringSizeObject(), 0.01f, 0.01f, 1.0f);
      ImGui::EndDisabled();

      RemixGui::Separator();

      RemixGui::DragFloat("Key Value (mid grey)", &keyValueObject(), 0.005f, 0.02f, 0.60f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Adaptation Strength", &adaptationStrengthObject(), 0.01f, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Histogram Low Percentile", &histogramLowPercentileObject(), 0.005f, 0.f, 0.9f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Histogram High Percentile", &histogramHighPercentileObject(), 0.005f, 0.5f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

      RemixGui::Separator();

      RemixGui::DragFloat("Soft Limit Center (EV100)", &softLimitCenterEVObject(), 0.05f, -8.f, 16.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Soft Limit Range (EV)", &softLimitRangeEVObject(), 0.05f, 0.5f, 16.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

      RemixGui::Separator();

      RemixGui::DragFloat("Tau Brighten (s)", &tauBrightenObject(), 0.01f, 0.05f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Tau Darken (s)", &tauDarkenObject(), 0.01f, 0.02f, 2.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Deadband (EV)", &deadbandEVObject(), 0.005f, 0.f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Cut Snap Threshold (EV)", &cutSnapThresholdEVObject(), 0.1f, 0.f, 20.f, "%.1f", ImGuiSliderFlags_AlwaysClamp);

      RemixGui::Separator();

      if (ImGui::CollapsingHeader("Readout", ImGuiTreeNodeFlags_DefaultOpen)) {
        m_debugStatsRequested = true;
        ImGui::Indent();
        if (m_debugStats.valid) {
          ImGui::Text("Current EV100      %8.3f", m_debugStats.currentEV);
          ImGui::Text("Target EV100       %8.3f", m_debugStats.targetEV);
          ImGui::Text("Scene EV100 (raw)  %8.3f", m_debugStats.sceneEV);
          ImGui::Text("Exposure           %8.4f  (%+.2f EV)", m_debugStats.exposure,
                      m_debugStats.exposure > 0.f ? std::log2(m_debugStats.exposure) : 0.f);
          RemixGui::Separator();
          ImGui::Text("Window low EV100   %8.3f", m_debugStats.loPercentileEV);
          ImGui::Text("Window high EV100  %8.3f", m_debugStats.hiPercentileEV);
          ImGui::Text("Trimmed fraction   %8.1f%%", m_debugStats.trimmedFraction * 100.f);
          RemixGui::Separator();
          ImGui::TextWrapped("Histogram covers %.1f to %.1f EV100 over %d bins (%.3f EV per bin).",
                             EXPOSURE_HISTOGRAM_MIN_EV100, EXPOSURE_HISTOGRAM_MAX_EV100,
                             EXPOSURE_HISTOGRAM_SIZE - 1,
                             (EXPOSURE_HISTOGRAM_MAX_EV100 - EXPOSURE_HISTOGRAM_MIN_EV100) / float(EXPOSURE_HISTOGRAM_SIZE - 1));
        } else {
          ImGui::Text("Waiting for first readback...");
        }
        ImGui::Unindent();
      }

      RemixGui::Separator();
      ImGui::Unindent();
    }
  }

  void DxvkAutoExposure::warnOnDeprecatedOptions() {
    if (m_deprecationChecked) {
      return;
    }
    m_deprecationChecked = true;

    auto note = [](const char* name, const char* replacement) {
      Logger::info(str::format("[Deprecated Config] rtx.autoExposure.", name, " no longer has any effect. ",
                               replacement, " Please re-save your config to get rid of this message."));
    };

    if (autoExposureSpeed() != autoExposureSpeedObject().getDefaultValue()) {
      note("autoExposureSpeed", "Use rtx.autoExposure.tauBrighten and rtx.autoExposure.tauDarken, which control the two adaptation directions separately.");
    }
    if (evMinValue() != evMinValueObject().getDefaultValue() ||
        evMaxValue() != evMaxValueObject().getDefaultValue()) {
      note("evMinValue/evMaxValue", "The histogram now covers a fixed wide EV range; use rtx.autoExposure.softLimitCenterEV and rtx.autoExposure.softLimitRangeEV to bound the response.");
    }
    if (exposureAverageMode() != exposureAverageModeObject().getDefaultValue()) {
      note("exposureAverageMode", "Metering is always a percentile-trimmed log average now; use rtx.autoExposure.histogramLowPercentile and rtx.autoExposure.histogramHighPercentile.");
    }
    if (useExposureCompensation() != useExposureCompensationObject().getDefaultValue() ||
        exposureWeightCurve0() != exposureWeightCurve0Object().getDefaultValue() ||
        exposureWeightCurve1() != exposureWeightCurve1Object().getDefaultValue() ||
        exposureWeightCurve2() != exposureWeightCurve2Object().getDefaultValue() ||
        exposureWeightCurve3() != exposureWeightCurve3Object().getDefaultValue() ||
        exposureWeightCurve4() != exposureWeightCurve4Object().getDefaultValue()) {
      note("useExposureCompensation/exposureWeightCurve0..4", "The weight curve has been superseded by percentile trimming; use rtx.autoExposure.histogramLowPercentile and rtx.autoExposure.histogramHighPercentile.");
    }
  }

  void DxvkAutoExposure::createResources(Rc<DxvkContext> ctx) {
    if (m_exposure.image != nullptr) {
      return;
    }

    DxvkImageCreateInfo desc;
    desc.type = VK_IMAGE_TYPE_1D;
    desc.flags = 0;
    desc.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    desc.numLayers = 1;
    desc.mipLevels = 1;
    desc.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    desc.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    desc.tiling = VK_IMAGE_TILING_OPTIMAL;
    desc.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type = VK_IMAGE_VIEW_TYPE_1D;
    viewInfo.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel = 0;
    viewInfo.numLevels = 1;
    viewInfo.minLayer = 0;
    viewInfo.numLayers = 1;
    viewInfo.format = desc.format;

    desc.extent = VkExtent3D{ 1, 1, 1 };

    viewInfo.format = desc.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.usage = desc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_exposure.image = device()->createImage(desc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "autoexposure");
    m_exposure.view = device()->createImageView(m_exposure.image, viewInfo);
    ctx->changeImageLayout(m_exposure.image, VK_IMAGE_LAYOUT_GENERAL);

    desc.extent = VkExtent3D { EXPOSURE_HISTOGRAM_SIZE, 1, 1 };
    viewInfo.format = desc.format = VK_FORMAT_R32_UINT;
    viewInfo.usage = desc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_exposureHistogram.image = device()->createImage(desc, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXRenderTarget, "autoexposure histogram");
    m_exposureHistogram.view = device()->createImageView(m_exposureHistogram.image, viewInfo);
    ctx->changeImageLayout(m_exposureHistogram.image, VK_IMAGE_LAYOUT_GENERAL);

    // Debug stats: one device-local element written by the shader, plus a host-visible ring of
    // kMaxFramesInFlight elements. Reading the oldest slot means the CPU never waits on the GPU.
    DxvkBufferCreateInfo statsInfo;
    statsInfo.size = sizeof(AutoExposureDebugStats);
    statsInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    statsInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    statsInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    m_debugStatsGpu = device()->createBuffer(statsInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXBuffer, "autoexposure debug stats");
    ctx->clearBuffer(m_debugStatsGpu, 0, statsInfo.size, 0);

    statsInfo.size = sizeof(AutoExposureDebugStats) * kMaxFramesInFlight;
    statsInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    statsInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    statsInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    m_debugStatsHost = device()->createBuffer(statsInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, DxvkMemoryStats::Category::RTXBuffer, "autoexposure debug stats HOST");
    memset(m_debugStatsHost->mapPtr(0), 0, statsInfo.size);
  }

  void DxvkAutoExposure::dispatchAutoExposure(
    Rc<DxvkContext> ctx,
    Rc<DxvkSampler> linearSampler,
    const Resources::RaytracingOutput& rtOutput,
    const float frameTimeMilliseconds) {

    if (m_resetState || !enabled()) {
      VkClearColorValue clearColor; 
      clearColor.float32[0] = clearColor.float32[1] = clearColor.float32[2] = clearColor.float32[3] = exp2f(0.0f);

      VkImageSubresourceRange subRange = {};
      subRange.layerCount = 1;
      subRange.levelCount = 1;
      subRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

      ctx->clearColorImage(m_exposure.image, clearColor, subRange);

      clearColor.uint32[0] = clearColor.uint32[1] = clearColor.uint32[2] = clearColor.uint32[3] = 0;
      ctx->clearColorImage(m_exposureHistogram.image, clearColor, subRange);
    }

    if (enabled()) {
      // Fall back to the configured constant frame time (or 60 FPS) when the per-frame delta is 0,
      // so eye adaptation still progresses on frame 0 in deterministic mode and when advanceTime is off.
      const float fallbackMs = RtxOptions::timeDeltaBetweenFrames() > 0.f ? RtxOptions::timeDeltaBetweenFrames() : 16.6f;
      const float effectiveFrameTimeMs = frameTimeMilliseconds > 0.0f ? frameTimeMilliseconds : fallbackMs;

      // Keep the window non-degenerate no matter what the sliders say, so the reduction can never
      // divide by a zero weight.
      const float loPercentile = clamp(histogramLowPercentile(), 0.0f, 0.9f);
      const float hiPercentile = clamp(histogramHighPercentile(), loPercentile + 0.01f, 1.0f);

      ToneMappingAutoExposureArgs pushArgs = {};
      pushArgs.numPixels = rtOutput.m_finalOutputExtent.width * rtOutput.m_finalOutputExtent.height;
      pushArgs.deltaTimeSeconds = 0.001f * effectiveFrameTimeMs;
      pushArgs.evMinValue = EXPOSURE_HISTOGRAM_MIN_EV100;
      pushArgs.evRange = EXPOSURE_HISTOGRAM_MAX_EV100 - EXPOSURE_HISTOGRAM_MIN_EV100;
      pushArgs.debugMode = (ctx->getCommonObjects()->metaDebugView().debugViewIdx() == DEBUG_VIEW_EXPOSURE_HISTOGRAM);
      pushArgs.enableCenterMetering = exposureCenterMeteringEnabled();
      pushArgs.centerMeteringSize = std::max(centerMeteringSize(), 0.01f);
      pushArgs.resetState = m_resetState;
      pushArgs.lowPercentile = loPercentile;
      pushArgs.highPercentile = hiPercentile;
      pushArgs.keyValue = std::max(keyValue(), 0.001f);
      pushArgs.adaptationStrength = clamp(adaptationStrength(), 0.0f, 1.0f);
      pushArgs.tauBrighten = std::max(tauBrighten(), 1e-4f);
      pushArgs.tauDarken = std::max(tauDarken(), 1e-4f);
      pushArgs.softLimitCenterEV = softLimitCenterEV();
      pushArgs.softLimitRangeEV = std::max(softLimitRangeEV(), 1e-3f);
      pushArgs.deadbandEV = std::max(deadbandEV(), 0.0f);
      pushArgs.cutSnapThresholdEV = std::max(cutSnapThresholdEV(), 0.0f);
      pushArgs.writeDebugStats = m_debugStatsRequested;

      {
        ScopedGpuProfileZone(ctx, "Histogram");
        static_cast<RtxContext*>(ctx.ptr())->setFramePassStage(RtxFramePassStage::AutoExposure_Histogram);
        ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

        // Calculate histogram
        ctx->bindResourceView(AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT, m_exposureHistogram.view, nullptr);
        ctx->bindResourceView(AUTO_EXPOSURE_COLOR_INPUT, rtOutput.m_finalOutput.view(Resources::AccessType::Read), nullptr);

        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, AutoExposureHistogramShader::getShader());
        const VkExtent3D workgroups = util::computeBlockCount(rtOutput.m_finalOutputExtent, VkExtent3D { 16, 16, 1 });
        ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
      }

      // Calculate avg luminance
      {
        ScopedGpuProfileZone(ctx, "Exposure");
        static_cast<RtxContext*>(ctx.ptr())->setFramePassStage(RtxFramePassStage::AutoExposure_Exposure);
        DebugView& debugView = ctx->getDevice()->getCommon()->metaDebugView();

        ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);
        ctx->bindResourceView(AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT, m_exposureHistogram.view, nullptr);
        ctx->bindResourceView(AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT, m_exposure.view, nullptr);
        ctx->bindResourceView(AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT, debugView.getDebugOutput(), nullptr);
        ctx->bindResourceBuffer(AUTO_EXPOSURE_DEBUG_STATS_OUTPUT, DxvkBufferSlice(m_debugStatsGpu));
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, AutoExposureShader::getShader());
        ctx->dispatch(1, 1, 1);
      }

      if (m_debugStatsRequested) {
        // Queue this frame's stats into the host ring, then read the oldest slot, which is
        // guaranteed to have landed. The readout is a few frames stale; for a tuning panel
        // that is invisible, and it costs no synchronisation.
        const uint32_t frameIdx = device()->getCurrentFrameId();
        const uint32_t writeIdx = frameIdx % kMaxFramesInFlight;
        ctx->copyBuffer(m_debugStatsHost, sizeof(AutoExposureDebugStats) * writeIdx,
                        m_debugStatsGpu, 0, sizeof(AutoExposureDebugStats));

        const uint32_t readIdx = (frameIdx + 1) % kMaxFramesInFlight;
        if (const void* mapped = m_debugStatsHost->mapPtr(sizeof(AutoExposureDebugStats) * readIdx)) {
          memcpy(&m_debugStats, mapped, sizeof(AutoExposureDebugStats));
        }
      }
    }
  }

  void DxvkAutoExposure::dispatch(
    Rc<DxvkContext> ctx,
    Rc<DxvkSampler> linearSampler,
    const Resources::RaytracingOutput& rtOutput,
    const float frameTimeMilliseconds,
    bool resetHistory) {

    ScopedGpuProfileZone(ctx, "Auto Exposure");

    m_resetState |= resetHistory;

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    warnOnDeprecatedOptions();

    if (m_exposureHistogram.image.ptr() == nullptr || m_debugStatsGpu == nullptr) {
      createResources(ctx);
      m_resetState = true;
    }

    dispatchAutoExposure(ctx, linearSampler, rtOutput, frameTimeMilliseconds);

    m_resetState = false;
  }
}
