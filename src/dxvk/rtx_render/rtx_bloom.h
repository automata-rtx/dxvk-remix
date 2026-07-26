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

#include "dxvk_include.h"
#include "dxvk_context.h"
#include "rtx_resources.h"

namespace dxvk {

  class DxvkDevice;

  class DxvkBloom: public RtxPass {
    
  public:
    explicit DxvkBloom(DxvkDevice* device);
    ~DxvkBloom();

    DxvkBloom(const DxvkBloom&) = delete;
    DxvkBloom(DxvkBloom&&) noexcept = delete;
    DxvkBloom& operator=(const DxvkBloom&) = delete;
    DxvkBloom& operator=(DxvkBloom&&) noexcept = delete;

    void dispatch(
      Rc<RtxContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const Resources::Resource& inOutColorBuffer);

    void showImguiSettings();

  private:
    void dispatchDownsampleStep(
      Rc<DxvkContext> ctx,
      const Rc<DxvkSampler>& linearSampler,
      const Resources::Resource& inputBuffer,
      const Resources::Resource& outputBuffer,
      bool initial);

    void dispatchDusklightDownsampleStep(
      Rc<DxvkContext> ctx,
      const Rc<DxvkSampler>& linearSampler,
      const Resources::Resource& inputBuffer,
      const Resources::Resource& outputBuffer,
      const Vector2& ringRadius,
      float gain,
      bool initial);

    void dispatchUpsampleStep(
      Rc<DxvkContext> ctx,
      const Rc<DxvkSampler>& linearSampler,
      const Resources::Resource& inputBuffer,
      const Resources::Resource& outputBuffer,
      float weight);

    void dispatchComposite(
      Rc<DxvkContext> ctx,
      const Rc<DxvkSampler> &linearSampler,
      const Resources::Resource& inOutColorBuffer,
      const Resources::Resource& bloomBuffer);

    virtual void createTargetResource(Rc<DxvkContext>& ctx, const VkExtent3D& targetExtent) override;
    virtual void releaseTargetResource() override;

    virtual bool isEnabled() const override;

    Rc<vk::DeviceFn> m_vkd;

    constexpr static int MaxBloomSteps = 8;
    // Each image is 1/2 resolution of the previous.
    Resources::Resource m_bloomBuffer[MaxBloomSteps] = {};

    RTX_OPTION_ENV("rtx.bloom", bool, enable, true, "RTX_BLOOM_ENABLE", "Enable bloom - glowing halos around intense, bright areas.");
    RTX_OPTION("rtx.bloom", float, burnIntensity, 1.0f, "Amount of bloom to add to the final image.");
    RTX_OPTION("rtx.bloom", float, luminanceThreshold, 0.25f,
               "Adjust the bloom threshold to suppress blooming of the dim areas. "
               "Pixels with luminance lower than the threshold are multiplied by "
               "the weight value that smoothly transitions from 1.0 (at luminance=threshold) to 0.0 (at luminance=0).");
    RTX_OPTION_ARGS("rtx.bloom", int, steps, 5,
                    "Number of downsampling steps to perform [1..8]. A higher value produces a wider blooming radius.",
                    args.minValue = 1,
                    args.maxValue = MaxBloomSteps);

    // Dusklight bloom.
    //
    // A port of the pyramid Dusklight uses for its 'improved' bloom, for games whose original
    // bloom looked like this and whose art was built around it. The differences that matter are
    // all in this group: a threshold that is subtracted per channel instead of weighted by
    // luminance, an explicit ring blur at every level of the pyramid, a per-level gain that is
    // allowed to saturate, and levels that are weighted geometrically on the way back up rather
    // than summed at full strength.
    //
    // The defaults reproduce Dusklight's own defaults. blurSize/blurRatio deliberately keep the
    // game's 0..255 parameter range so values can be carried straight over from it.

    RTX_OPTION("rtx.bloom", bool, dusklight, false,
               "Replaces the bloom pyramid with a port of Dusklight's 'improved' bloom.\n"
               "Blurs an eight tap ring at every level of the pyramid, thresholds by subtracting from each channel rather than by weighting with luminance, "
               "and weights the levels geometrically as they are combined back together. Produces a softer and wider halo with saturated, washed out cores, "
               "which is what bloom looked like on the hardware these games were built for.\n"
               "Uses its own threshold (rtx.bloom.dusklightThreshold) rather than rtx.bloom.luminanceThreshold. rtx.bloom.steps and rtx.bloom.burnIntensity still apply.");
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightThreshold, 0.5f,
                    "Value subtracted from every colour channel before Dusklight bloom is gathered. Only used when rtx.bloom.dusklight is enabled.\n"
                    "Pixels below the threshold do not bloom at all and pixels above it bloom in proportion to how far above they are, giving a harder cut than the "
                    "smooth luminance rolloff of the default bloom. Subtracting per channel also pushes coloured highlights further towards their dominant hue.\n"
                    "Note this is in the linear HDR range the image is in before tonemapping, not a 0..1 display value.",
                    args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightBlurSize, 64.0f,
                    "Radius of the ring blur applied at each pyramid level, in the same 0..255 range the game uses. Only used when rtx.bloom.dusklight is enabled.\n"
                    "The radius is normalized against the game's original framebuffer height, so the halo covers the same fraction of the screen at any resolution.",
                    args.minValue = 0.0f,
                    args.maxValue = 255.0f);
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightBlurRatio, 128.0f,
                    "Overall brightness of the gathered bloom, in the same 0..255 range the game uses. Only used when rtx.bloom.dusklight is enabled.\n"
                    "The total gain is spread evenly across the blur passes so that changing rtx.bloom.steps does not change how bright the bloom is, only how wide it is.",
                    args.minValue = 0.0f,
                    args.maxValue = 255.0f);
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightFalloff, 0.25f,
                    "How much weight the wider pyramid levels keep as they are combined back into the narrower ones. Only used when rtx.bloom.dusklight is enabled.\n"
                    "Lower values concentrate the bloom close to its source, higher values spread it further out. At 1.0 every level contributes at full strength, "
                    "which is how the default bloom pyramid behaves.",
                    args.minValue = 0.01f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightSaturationPoint, 1.0f,
                    "Value the bloom saturates at after each pass. Only used when rtx.bloom.dusklight is enabled.\n"
                    "The original effect ran in 8 bit and clipped at white on every pass, and that clipping is a large part of why bright sources bloom as a solid "
                    "washed out core. Raise this to keep more of the highlight range, or set it to 0 to leave the bloom unclamped.",
                    args.minValue = 0.0f);
    RTX_OPTION("rtx.bloom", Vector3, dusklightTint, Vector3(1.0f, 1.0f, 1.0f),
               "Colour the Dusklight bloom is tinted with before it is added to the image. Only used when rtx.bloom.dusklight is enabled.");
    RTX_OPTION("rtx.bloom", bool, dusklightScreenBlend, false,
               "Adds the Dusklight bloom with a screen style blend instead of a plain additive one. Only used when rtx.bloom.dusklight is enabled.\n"
               "Bloom is attenuated by how bright the image already is, so areas that are close to white glow rather than clipping further. "
               "The game switches this on for scenes it wants to keep readable under heavy bloom.");
  };
  
}
