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
#include "rtx_context.h"
#include "rtx_bloom.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/bloom/bloom.h"

#include <rtx_shaders/bloom_downsample.h>
#include <rtx_shaders/bloom_dusklight_downsample.h>
#include <rtx_shaders/bloom_dusklight_prepass.h>
#include <rtx_shaders/bloom_upsample.h>
#include <rtx_shaders/bloom_composite.h>
#include "rtx_imgui.h"

#include <cmath>

namespace dxvk {
  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class BloomDownsampleShader : public ManagedShader
    {
      SHADER_SOURCE(BloomDownsampleShader, VK_SHADER_STAGE_COMPUTE_BIT, bloom_downsample)

      PUSH_CONSTANTS(BloomDownsampleArgs)

      BEGIN_PARAMETER()
        SAMPLER2D(BLOOM_DOWNSAMPLE_INPUT)
      RW_TEXTURE2D(BLOOM_DOWNSAMPLE_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(BloomDownsampleShader);

    class BloomDusklightDownsampleShader : public ManagedShader
    {
      SHADER_SOURCE(BloomDusklightDownsampleShader, VK_SHADER_STAGE_COMPUTE_BIT, bloom_dusklight_downsample)

      PUSH_CONSTANTS(BloomDusklightDownsampleArgs)

      BEGIN_PARAMETER()
        SAMPLER2D(BLOOM_DUSKLIGHT_DOWNSAMPLE_INPUT)
        RW_TEXTURE2D(BLOOM_DUSKLIGHT_DOWNSAMPLE_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(BloomDusklightDownsampleShader);

    class BloomDusklightPrepassShader : public ManagedShader
    {
      SHADER_SOURCE(BloomDusklightPrepassShader, VK_SHADER_STAGE_COMPUTE_BIT, bloom_dusklight_prepass)

      PUSH_CONSTANTS(BloomDusklightPrepassArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(BLOOM_DUSKLIGHT_PREPASS_COLOR_INPUT_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(BloomDusklightPrepassShader);

    class BloomUpsampleShader : public ManagedShader
    {
      SHADER_SOURCE(BloomUpsampleShader, VK_SHADER_STAGE_COMPUTE_BIT, bloom_upsample)

      PUSH_CONSTANTS(BloomUpsampleArgs)

      BEGIN_PARAMETER()
        SAMPLER2D(BLOOM_UPSAMPLE_INPUT)
        RW_TEXTURE2D(BLOOM_UPSAMPLE_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(BloomUpsampleShader);

    class CompositeShader : public ManagedShader
    {
      SHADER_SOURCE(CompositeShader, VK_SHADER_STAGE_COMPUTE_BIT, bloom_composite)

      PUSH_CONSTANTS(BloomCompositeArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(BLOOM_COMPOSITE_COLOR_INPUT_OUTPUT)
        SAMPLER2D(BLOOM_COMPOSITE_BLOOM)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(CompositeShader);
  }
  
  DxvkBloom::DxvkBloom(DxvkDevice* device): RtxPass(device), m_vkd(device->vkd()) {
  }
  
  DxvkBloom::~DxvkBloom()  {
  }

  void DxvkBloom::showImguiSettings() {
    ImGui::Indent();
    RemixGui::Checkbox("Bloom Enabled", &enableObject());
    ImGui::Indent();
    RemixGui::DragFloat("Intensity##bloom", &burnIntensityObject(), 0.05f, 0.f, 5.f, "%.2f");

    RemixGui::Checkbox("Dusklight Bloom##bloom", &dusklightObject());

    if (dusklight()) {
      ImGui::Indent();
      RemixGui::Checkbox("Follow Game##bloomDusklight", &dusklightFollowGameObject());

      const bool feedActive = dusklightFollowGame() && DusklightEnv::enable();
      if (feedActive) {
        ImGui::TextWrapped("Driven by the game's environment feed (rtx.dusklight.env.*).");
        RemixGui::DragFloat("Threshold Scale##bloomDusklight", &dusklightThresholdScaleObject(), 0.01f, 0.f, 10.f, "%.2f");
      } else {
        RemixGui::DragFloat("Threshold##bloomDusklight", &dusklightThresholdObject(), 0.01f, 0.f, 100.f, "%.2f");
        RemixGui::DragFloat("Blur Size##bloomDusklight", &dusklightBlurSizeObject(), 1.f, 0.f, 255.f, "%.0f");
        RemixGui::DragFloat("Blur Ratio##bloomDusklight", &dusklightBlurRatioObject(), 1.f, 0.f, 255.f, "%.0f");
        RemixGui::ColorEdit3("Tint##bloomDusklight", &dusklightTintObject());
        RemixGui::Checkbox("Screen Blend##bloomDusklight", &dusklightScreenBlendObject());
        RemixGui::DragFloat("Base Weight##bloomDusklight", &dusklightBaseWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
        RemixGui::ColorEdit3("Mono Color##bloomDusklight", &dusklightMonoColorObject());
        RemixGui::DragFloat("Mono Amount##bloomDusklight", &dusklightMonoAmountObject(), 0.01f, 0.f, 1.f, "%.2f");
      }

      RemixGui::Checkbox("Display Referred##bloomDusklight", &dusklightDisplaySpaceObject());
      RemixGui::Checkbox("Mono Uses Luminance##bloomDusklight", &dusklightMonoUseLuminanceObject());
      RemixGui::DragFloat("Level Falloff##bloomDusklight", &dusklightFalloffObject(), 0.01f, 0.01f, 1.f, "%.2f");
      RemixGui::DragFloat("Saturation Point##bloomDusklight", &dusklightSaturationPointObject(), 0.05f, 0.f, 100.f, "%.2f");
      ImGui::Unindent();
    } else {
      RemixGui::DragFloat("Threshold##bloom", &luminanceThresholdObject(), 0.05f, 0.f, 100.f, "%.2f");
    }

    RemixGui::SliderInt("Radius##bloom", &stepsObject(), 4, MaxBloomSteps);
    ImGui::Unindent();
    ImGui::Unindent();
  }

  DxvkBloom::EffectiveDusklightParams DxvkBloom::resolveDusklightParams() const {
    EffectiveDusklightParams p = {};

    p.followingGame = dusklight() && dusklightFollowGame() && DusklightEnv::enable();

    if (p.followingGame) {
      p.pyramidEnabled = DusklightEnv::bloomEnable();
      p.threshold = std::max(DusklightEnv::bloomThreshold(), 0.0f) * dusklightThresholdScale();
      p.blurSize = DusklightEnv::bloomBlurSize();
      p.blurRatio = DusklightEnv::bloomBlurRatio();
      p.tint = DusklightEnv::bloomTint();
      p.screenBlend = DusklightEnv::bloomScreenBlend();
      p.baseWeight = std::clamp(DusklightEnv::bloomBaseWeight(), 0.0f, 1.0f);
      p.monoColor = DusklightEnv::monoColor();
      p.monoAmount = std::clamp(DusklightEnv::monoAmount(), 0.0f, 1.0f);
    } else {
      p.pyramidEnabled = true;
      p.threshold = dusklightThreshold();
      p.blurSize = dusklightBlurSize();
      p.blurRatio = dusklightBlurRatio();
      p.tint = dusklightTint();
      p.screenBlend = dusklightScreenBlend();
      p.baseWeight = std::clamp(dusklightBaseWeight(), 0.0f, 1.0f);
      p.monoColor = dusklightMonoColor();
      p.monoAmount = std::clamp(dusklightMonoAmount(), 0.0f, 1.0f);
    }

    return p;
  }

  DxvkBloom::Stage DxvkBloom::activeStage() const {
    return dusklight() && dusklightDisplaySpace() ? Stage::PostTonemap : Stage::PreTonemap;
  }

  void DxvkBloom::dispatch(Rc<RtxContext> ctx,
                           Rc<DxvkSampler> linearSampler,
                           const Resources::Resource& inOutColorBuffer,
                           Stage stage) {
    if (stage != activeStage()) {
      return;
    }

    ScopedGpuProfileZone(ctx, "Bloom");
    ctx->setFramePassStage(RtxFramePassStage::Bloom);

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const EffectiveDusklightParams dl = resolveDusklightParams();

    if (dusklight() && dl.monoAmount > 0.0f) {
      // The game applies the mono overlay before gathering bloom, so the pyramid below sees
      // the overlaid image.
      dispatchDusklightPrepass(ctx, inOutColorBuffer, dl.monoColor, dl.monoAmount);
    }

    const bool pyramidActive = enable() && burnIntensity() > 0.0f && (!dusklight() || dl.pyramidEnabled);

    if (!pyramidActive) {
      return;
    }

    const Resources::Resource* res[] = {
      &inOutColorBuffer,
      &m_bloomBuffer[0],
      &m_bloomBuffer[1],
      &m_bloomBuffer[2],
      &m_bloomBuffer[3],
      &m_bloomBuffer[4],
      &m_bloomBuffer[5],
      &m_bloomBuffer[6],
      &m_bloomBuffer[7],
    };
    static_assert(MaxBloomSteps == std::size(res) - 1);

    const int bloomDepth = std::clamp(steps(), 1, MaxBloomSteps);

    if (!dusklight()) {
      for (int i = 0; i < bloomDepth; i++) {
        dispatchDownsampleStep(ctx, linearSampler, *res[i], *res[i + 1], i == 0);
      }

      for (int i = bloomDepth; i > 1; i--) {
        dispatchUpsampleStep(ctx, linearSampler, *res[i], *res[i - 1], 1.0f);
      }
    } else {
      // The blur radius Dusklight uses is expressed against the height of the game's original
      // framebuffer, which keeps the halo the same size relative to the screen at any resolution.
      constexpr float kSourceFramebufferHeight = 448.0f;
      // Aspect the effect was authored at. Scaling the horizontal radius by this over the current
      // aspect preserves the shape the ring had on the original 4:3 display.
      constexpr float kSourceFramebufferAspect = 1.3571428f;
      // Divisor that maps the game's 0..255 blur size onto a screen UV radius.
      constexpr float kBlurSizeToUv = 1.0f / 6400.0f;

      const VkExtent3D fullSize = inOutColorBuffer.image->info().extent;
      const float fullWidth = static_cast<float>(std::max(fullSize.width, 1u));
      const float fullHeight = static_cast<float>(std::max(fullSize.height, 1u));
      const float aspect = fullWidth / fullHeight;

      const float blurScale = std::max(dl.blurSize, 0.0f) * (kSourceFramebufferHeight / fullHeight) * kBlurSizeToUv;
      const Vector2 ringRadius(blurScale * (kSourceFramebufferAspect / aspect), blurScale);

      // Dusklight spreads the total brightness evenly over its blur passes so that each one
      // contributes the same factor and the product across the pyramid stays fixed. Doing the same
      // means the depth of the pyramid changes how wide the bloom is without changing how bright
      // it is. Every step past the first blurs; the first only thresholds, and picks up the gain
      // itself only when the pyramid is too shallow to have any blur passes at all.
      // One blur per level below the threshold step. The original runs five of them (its divStart
      // 2 through divNum 6), which corresponds to rtx.bloom.steps = 6 here - the default of 5 is
      // one short, so the gain lands differently and the halo stops one level narrower.
      const int blurPassCount = std::max(bloomDepth - 1, 1);
      const float totalGain = std::max(dl.blurRatio, 0.0f) * 16.0f / 255.0f;
      const float gainPerPass = std::pow(totalGain, 1.0f / static_cast<float>(blurPassCount));
      const float initialGain = bloomDepth > 1 ? 1.0f : gainPerPass;

      for (int i = 0; i < bloomDepth; i++) {
        const bool initial = (i == 0);

        dispatchDusklightDownsampleStep(ctx, linearSampler, *res[i], *res[i + 1], ringRadius,
                                        initial ? initialGain : gainPerPass, initial, dl.threshold);
      }

      // Each level is folded into the one above it with a weight that falls off geometrically
      // with how far down the pyramid it came from, so the wide levels sit under the narrow ones
      // instead of drowning them out the way an unweighted sum would.
      const float falloff = std::clamp(dusklightFalloff(), 0.01f, 1.0f);

      // The original's exponent counts levels from the top of the blur chain, not from the top of
      // the pyramid: alpha = falloff^(1/(i - divStart + 1)) with divStart 2, i.e. one less than
      // the level index. Getting this off by one leaves every level slightly too faint.
      for (int i = bloomDepth; i > 1; i--) {
        dispatchUpsampleStep(ctx, linearSampler, *res[i], *res[i - 1],
                             std::pow(falloff, 1.0f / static_cast<float>(i - 1)));
      }
    }

    if (dusklight()) {
      dispatchComposite(ctx, linearSampler, inOutColorBuffer, m_bloomBuffer[0],
                        dl.tint, dl.screenBlend, dl.baseWeight);
    } else {
      dispatchComposite(ctx, linearSampler, inOutColorBuffer, m_bloomBuffer[0],
                        Vector3(1.0f, 1.0f, 1.0f), false, 1.0f);
    }
  }

  void DxvkBloom::dispatchDownsampleStep(
    Rc<DxvkContext> ctx,
    const Rc<DxvkSampler>& linearSampler,
    const Resources::Resource& inputBuffer,
    const Resources::Resource& outputBuffer,
    bool initial) {
    ScopedGpuProfileZone(ctx, "Bloom Downsample");

    const VkExtent3D inputSize = inputBuffer.image->info().extent;
    const VkExtent3D outputSize = outputBuffer.image->info().extent;

    // Prepare shader arguments
    BloomDownsampleArgs pushArgs = {};
    pushArgs.inputSizeInverse = { 1.0f / float(inputSize.width), 1.0f / float(inputSize.height) };
    pushArgs.downsampledOutputSize = { outputSize.width, outputSize.height };
    pushArgs.downsampledOutputSizeInverse = { 1.0f / float(outputSize.width), 1.0f / float(outputSize.height) };
    pushArgs.threshold = initial ? std::max(0.01f, luminanceThreshold()) : -1;
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    const VkExtent3D workgroups = util::computeBlockCount(outputSize, VkExtent3D{ 16, 16, 1 });

    ctx->bindResourceView(BLOOM_DOWNSAMPLE_INPUT, inputBuffer.view, nullptr);
    ctx->bindResourceSampler(BLOOM_DOWNSAMPLE_INPUT, linearSampler);
    ctx->bindResourceView(BLOOM_DOWNSAMPLE_OUTPUT, outputBuffer.view, nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, BloomDownsampleShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void DxvkBloom::dispatchDusklightPrepass(
    Rc<DxvkContext> ctx,
    const Resources::Resource& inOutColorBuffer,
    const Vector3& monoColor,
    float monoAmount) {
    ScopedGpuProfileZone(ctx, "Bloom Dusklight Prepass");

    const VkExtent3D imageSize = inOutColorBuffer.image->info().extent;

    BloomDusklightPrepassArgs pushArgs = {};
    pushArgs.imageSize = { imageSize.width, imageSize.height };
    pushArgs.monoColor = monoColor;
    pushArgs.monoAmount = monoAmount;
    pushArgs.useLuminance = dusklightMonoUseLuminance() ? 1u : 0u;
    pushArgs.displaySpace = dusklightDisplaySpace() ? 1u : 0u;
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    const VkExtent3D workgroups = util::computeBlockCount(imageSize, VkExtent3D{ 16, 16, 1 });

    ctx->bindResourceView(BLOOM_DUSKLIGHT_PREPASS_COLOR_INPUT_OUTPUT, inOutColorBuffer.view, nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, BloomDusklightPrepassShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void DxvkBloom::dispatchDusklightDownsampleStep(
    Rc<DxvkContext> ctx,
    const Rc<DxvkSampler>& linearSampler,
    const Resources::Resource& inputBuffer,
    const Resources::Resource& outputBuffer,
    const Vector2& ringRadius,
    float gain,
    bool initial,
    float thresholdValue) {
    ScopedGpuProfileZone(ctx, "Bloom Dusklight Downsample");

    const VkExtent3D inputSize = inputBuffer.image->info().extent;
    const VkExtent3D outputSize = outputBuffer.image->info().extent;

    // Prepare shader arguments
    BloomDusklightDownsampleArgs pushArgs = {};
    pushArgs.inputSizeInverse = { 1.0f / float(inputSize.width), 1.0f / float(inputSize.height) };
    pushArgs.downsampledOutputSize = { outputSize.width, outputSize.height };
    pushArgs.downsampledOutputSizeInverse = { 1.0f / float(outputSize.width), 1.0f / float(outputSize.height) };
    pushArgs.ringRadius = { ringRadius.x, ringRadius.y };
    pushArgs.threshold = initial ? std::max(thresholdValue, 0.0f) : -1.0f;
    pushArgs.gain = gain;
    pushArgs.saturationPoint = std::max(dusklightSaturationPoint(), 0.0f);
    pushArgs.isInitial = initial ? 1u : 0u;
    pushArgs.displaySpace = dusklightDisplaySpace() ? 1u : 0u;
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    const VkExtent3D workgroups = util::computeBlockCount(outputSize, VkExtent3D{ 16, 16, 1 });

    ctx->bindResourceView(BLOOM_DUSKLIGHT_DOWNSAMPLE_INPUT, inputBuffer.view, nullptr);
    ctx->bindResourceSampler(BLOOM_DUSKLIGHT_DOWNSAMPLE_INPUT, linearSampler);
    ctx->bindResourceView(BLOOM_DUSKLIGHT_DOWNSAMPLE_OUTPUT, outputBuffer.view, nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, BloomDusklightDownsampleShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void DxvkBloom::dispatchUpsampleStep(
    Rc<DxvkContext> ctx,
    const Rc<DxvkSampler>& linearSampler,
    const Resources::Resource& inputBuffer,
    const Resources::Resource& outputBuffer,
    float weight) {
    ScopedGpuProfileZone(ctx, "Bloom Upsample");

    VkExtent3D inputSize = inputBuffer.image->info().extent;
    VkExtent3D outputSize = outputBuffer.image->info().extent;

    // Prepare shader arguments
    BloomUpsampleArgs pushArgs = {};
    pushArgs.inputSizeInverse = { 1.f / float(inputSize.width), 1.f / float(inputSize.height) };
    pushArgs.upsampledOutputSize = { outputSize.width, outputSize.height };
    pushArgs.upsampledOutputSizeInverse = { 1.f / float(outputSize.width), 1.f / float(outputSize.height) };
    pushArgs.weight = weight;

    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    VkExtent3D workgroups = util::computeBlockCount(outputSize, VkExtent3D{ 16, 16, 1 });

    ctx->bindResourceView(BLOOM_UPSAMPLE_INPUT, inputBuffer.view, nullptr);
    ctx->bindResourceSampler(BLOOM_UPSAMPLE_INPUT, linearSampler);
    ctx->bindResourceView(BLOOM_UPSAMPLE_OUTPUT, outputBuffer.view, nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, BloomUpsampleShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void DxvkBloom::dispatchComposite(
    Rc<DxvkContext> ctx,
    const Rc<DxvkSampler> &linearSampler,
    const Resources::Resource& inOutColorBuffer,
    const Resources::Resource& bloomBuffer,
    const Vector3& tint,
    bool screenBlend,
    float baseWeight)
  {
    ScopedGpuProfileZone(ctx, "Composite");

    VkExtent3D outputSize = inOutColorBuffer.image->info().extent;

    // Prepare shader arguments
    BloomCompositeArgs pushArgs = {};
    pushArgs.imageSize = { outputSize.width, outputSize.height };
    pushArgs.imageSizeInverse = { 1.f / float(outputSize.width), 1.f / float(outputSize.height) };
    // Remix's own pyramid gathers broadly from unthresholded HDR and needs heavy attenuation
    // here; the 0.01 is calibrated for it. The Dusklight path must not inherit that. Its
    // brightness is already fully specified by the game's own blur ratio, and the original
    // composite adds its bloom at full strength - modulated only by the blend colour and the
    // hardware blend factors, with no scale factor anywhere. Inheriting Remix's attenuation made
    // ours a hundred times too faint, which is exactly why it needed every brightness knob pinned
    // to its maximum to show up at all, and why turning it off looked closer to the original.
    pushArgs.intensity = std::max(burnIntensity(), 0.0f) * (dusklight() ? 1.0f : 0.01f);
    pushArgs.tint = tint;
    pushArgs.screenBlend = screenBlend ? 1u : 0u;
    pushArgs.baseWeight = baseWeight;
    pushArgs.displaySpace = (dusklight() && dusklightDisplaySpace()) ? 1u : 0u;
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    VkExtent3D workgroups = util::computeBlockCount(outputSize, VkExtent3D{ 16 , 16, 1 });

    ctx->bindResourceView(BLOOM_COMPOSITE_COLOR_INPUT_OUTPUT, inOutColorBuffer.view, nullptr);
    ctx->bindResourceView(BLOOM_COMPOSITE_BLOOM, bloomBuffer.view, nullptr);
    ctx->bindResourceSampler(BLOOM_COMPOSITE_BLOOM, linearSampler);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, CompositeShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void DxvkBloom::createTargetResource(Rc<DxvkContext>& ctx, const VkExtent3D& targetExtent) {
    for (uint32_t i = 0; i < std::size(m_bloomBuffer); i++) {
      const uint32_t divisor = (1U << (i + 1));

      m_bloomBuffer[i] = Resources::createImageResource(
        ctx,
        "bloom buffer",
        {
          util::ceilDivide(targetExtent.width, divisor),
          util::ceilDivide(targetExtent.height, divisor),
          1
        },
        VK_FORMAT_R16G16B16A16_SFLOAT);
    }
  }

  void DxvkBloom::releaseTargetResource() {
    for (auto& i : m_bloomBuffer) {
      i.reset();
    }
  }

  bool DxvkBloom::isEnabled() const {
    if (!enable()) {
      return false;
    }

    if (burnIntensity() > 0.f) {
      return true;
    }

    // The mono overlay runs even when the bloom pyramid contributes nothing, matching the
    // game, where the overlay draws regardless of whether bloom gathers this frame.
    return dusklight() && resolveDusklightParams().monoAmount > 0.0f;
  }
}
