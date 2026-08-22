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
#include "rtx_dusklight_env.h"

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

    // The Dusklight path wants to run after tone mapping and the default pyramid before it, so
    // both points call in and the pass takes the one it is configured for.
    enum class Stage {
      PreTonemap,
      PostTonemap,
    };

    void dispatch(
      Rc<RtxContext> ctx,
      Rc<DxvkSampler> linearSampler,
      const Resources::Resource& inOutColorBuffer,
      Stage stage);

    // Which side of tone mapping this pass wants to run on given the current options.
    Stage activeStage() const;

    void showImguiSettings();
    // The Dusklight half, shown from the Dusklight tab instead. Same pass, same options; the two
    // are split because one configures this renderer and the other reproduces a specific game.
    void showDusklightImguiSettings();

  private:
    // Values the bloom actually runs with this dispatch: the manual options, or the game's
    // kankyo feed when rtx.bloom.dusklightFollowGame is active.
    struct EffectiveDusklightParams {
      bool  followingGame;
      bool  pyramidEnabled;
      float threshold;
      float blurSize;
      float blurRatio;
      Vector3 tint;
      bool  screenBlend;
      float baseWeight;
      Vector3 monoColor;
      float monoAmount;
    };

    EffectiveDusklightParams resolveDusklightParams() const;

    void dispatchDusklightPrepass(
      Rc<DxvkContext> ctx,
      const Resources::Resource& inOutColorBuffer,
      const Vector3& monoColor,
      float monoAmount);

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
      bool initial,
      float thresholdValue);

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
      const Resources::Resource& bloomBuffer,
      const Vector3& tint,
      bool screenBlend,
      float baseWeight);

    virtual void createTargetResource(Rc<DxvkContext>& ctx, const VkExtent3D& targetExtent) override;
    virtual void releaseTargetResource() override;

    virtual bool isEnabled() const override;

    Rc<vk::DeviceFn> m_vkd;

    constexpr static int MaxBloomSteps = 8;
    // Each image is 1/2 resolution of the previous.
    Resources::Resource m_bloomBuffer[MaxBloomSteps] = {};

    RTX_OPTION_ENV("rtx.bloom", bool, enable, true, "RTX_BLOOM_ENABLE", "Enable bloom - glowing halos around intense, bright areas.");
    RTX_OPTION("rtx.bloom", float, burnIntensity, 1.0f,
               "Amount of bloom to add to the final image.\n"
               "The default pyramid is attenuated by a further fixed factor of 0.01 on top of this, which is calibrated for how broadly it gathers. "
               "The Dusklight pyramid is not: its brightness is already carried by rtx.bloom.dusklightBlurRatio, and the effect it reproduces composited "
               "at full strength. So the same value here means something about a hundred times stronger with rtx.bloom.dusklight enabled.");
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
    // all in this group: a threshold that cuts hard against a blue weighted key instead of rolling
    // off smoothly against BT.709 luma the way the default pyramid's does, an explicit ring blur at
    // every level of the pyramid, a per-level gain that is allowed to saturate, and levels that are
    // weighted geometrically on the way back up rather than summed at full strength.
    //
    // The threshold's exact shape is stated where it is implemented, measured from the original's
    // three TEV stages: shaders/rtx/pass/bloom/bloom_dusklight_downsample.comp.slang:34-42. Read
    // that before rewording anything below - describing this as a per channel subtraction is the
    // mistake these strings carried for months, and it predicts the opposite of what the code does.
    //
    // The defaults reproduce Dusklight's own defaults. blurSize/blurRatio deliberately keep the
    // game's 0..255 parameter range so values can be carried straight over from it.

    RTX_OPTION("rtx.bloom", bool, dusklight, false,
               "Replaces the bloom pyramid with a port of Dusklight's 'improved' bloom.\n"
               "Blurs an eight tap ring at every level of the pyramid, thresholds with a hard cut against a blue weighted luminance key rather than the default pyramid's smooth rolloff "
               "against BT.709 luminance, and weights the levels geometrically as they are combined back together. Produces a softer and wider halo with saturated, washed out cores, "
               "which is what bloom looked like on the hardware these games were built for.\n"
               "Uses its own threshold (rtx.bloom.dusklightThreshold) rather than rtx.bloom.luminanceThreshold. rtx.bloom.steps and rtx.bloom.burnIntensity still apply.");
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightThreshold, 0.5f,
                    "Threshold the Dusklight bloom is keyed against. Subtracted from one weighted luminance key, not from each colour channel. Only used when rtx.bloom.dusklight is enabled.\n"
                    "The key is 0.25*R + 0.25*G + 0.5*B, and the whole colour is then scaled by saturate(key - threshold), so the bloom keeps the source's own hue instead of drifting towards "
                    "whichever channel was brightest. A pixel whose key is at or below the threshold does not bloom at all, which is a harder cut than the smooth luminance rolloff of the default "
                    "bloom. A colour blooms on that keyed brightness rather than on any one channel clearing the threshold, so a colour with all of its energy in one channel may not bloom at all - "
                    "pure red at full intensity keys to 0.25 and never clears the default 0.5.\n"
                    "The weights are not a standard luma either: blue counts double red or green, so blue keys higher than a red or green of the same brightness. That is a relative advantage "
                    "only - pure blue at full intensity keys to exactly 0.50 and still contributes nothing at the default threshold, so blue needs company in another channel to bloom.\n"
                    "With rtx.bloom.dusklightDisplaySpace on (the default) the pyramid runs after tone mapping on display referred colour, so this is a fraction of display white on a 0..1 image - "
                    "the same range as the game's own 0..1 bloom threshold, which is why rtx.bloom.dusklightThresholdScale can be left at 1.0. It is a linear pre-tonemap value only when that "
                    "option is turned off.",
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
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightBaseWeight, 1.0f,
                    "Weight the base image keeps when the Dusklight bloom is composited over it. Only used when rtx.bloom.dusklight is enabled.\n"
                    "The game's composite scales the framebuffer by its blend alpha while adding bloom on top - twilight dims the scene to about 0.82 this way. "
                    "1.0 leaves the base image untouched.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    // Which game state actually turns this on, because "try wolf senses" was the wrong answer for
    // months. The overlay's amount is the game's bloom table mSaturateSubtractA and this tint is the
    // same entry's mSaturateSubtractR/G/B - not its mColorR/G/B, which is the separate triple behind
    // rtx.bloom.dusklightTint (dusklight-ao src/d/d_kankyo_data.cpp:14-17, blended into the two
    // GXColors at d_kankyo.cpp:2605-2640). The twilight entries, tables 1 and 2, set
    // mSaturateSubtractA to 0x60 over a white mSaturateSubtract RGB. The wolf senses entry, table 3,
    // leaves mSaturateSubtractA at 0x00, and senses points every bloom slot at that entry
    // (d_kankyo.cpp:2545-2546), so senses does not turn the overlay on. It is not neutral to bloom -
    // that entry also drives mThreshold to 0x00 and mColor to 0x60, 0xBA, 0xEC, which does reach
    // rtx.bloom.dusklightTint - it is simply not a route to this option. Unverified caveat: a live
    // field_0x12fc override re-points the two end slots straight after (d_kankyo.cpp:2549-2554), so
    // something driving that could blend mono back in. Nothing was traced doing so.
    RTX_OPTION("rtx.bloom", Vector3, dusklightMonoColor, Vector3(1.0f, 1.0f, 1.0f),
               "Tint of the full-screen mono overlay applied before the Dusklight bloom is gathered. Only used when rtx.bloom.dusklight is enabled.\n"
               "The image is converted to greyscale, multiplied by this colour, and blended back in by rtx.bloom.dusklightMonoAmount. "
               "The game's environment system drives this for twilight, through bloom tables 1 and 2; its wolf senses entry leaves the overlay off.");
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightMonoAmount, 0.0f,
                    "Strength of the full-screen mono (desaturate and tint) overlay, 0..1. Only used when rtx.bloom.dusklight is enabled.\n"
                    "Applied before the bloom is gathered, so the bloom sees the overlaid image, exactly as on the original hardware. "
                    "Twilight runs this at about 0.38 with a white tint - pure desaturation.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION("rtx.bloom", bool, dusklightDisplaySpace, true,
               "Runs the Dusklight bloom after tone mapping, on display referred values, which is where the original ran.\n"
               "The effect was authored against an 8 bit framebuffer holding finished display colours: its threshold is a fraction of display white, its "
               "intermediate buffers clip at white, and its screen blend and base weight are both defined against a 0..1 image. Gathering it from open ended "
               "pre-tonemap radiance instead changes all four - the threshold stops meaning anything fixed, the clipping that gives bright cores their washed "
               "out look either never happens or eats the image, and blurring linear radiance concentrates halos far more tightly than blurring display values "
               "does. Turn this off only to compare against the pre-tonemap behaviour.");
    RTX_OPTION("rtx.bloom", bool, dusklightMonoUseLuminance, false,
               "Uses BT.709 luminance for the mono overlay's greyscale instead of replicating the red channel.\n"
               "The game's TEV implementation replicated red, which reads slightly differently in warm scenes; red is the faithful default, "
               "luminance is the technically correct alternative.");
    RTX_OPTION("rtx.bloom", bool, dusklightFollowGame, true,
               "Drives the Dusklight bloom from the environment state the game pushes through the Remix API (rtx.dusklight.env.*) instead of the manual rtx.bloom.dusklight* values.\n"
               "Threshold, blur size, brightness, tint, blend mode, base weight and the mono overlay all track the game's per-area, per-time-of-day, per-weather palettes. "
               "Has no effect unless the game's bridge is active (rtx.dusklight.env.enable). The manual values still apply when the feed is absent.");
    RTX_OPTION_ARGS("rtx.bloom", float, dusklightThresholdScale, 1.0f,
                    "Calibration factor applied to the game's 0..1 bloom threshold before this pass consumes it. "
                    "Only used while rtx.bloom.dusklightFollowGame is consuming the game feed.\n"
                    "With rtx.bloom.dusklightDisplaySpace on - which is the default - the pyramid runs after tone mapping on display referred 0..1 colour, "
                    "the same range the game's threshold was authored against, so the game's value maps straight across and this should be left at 1.0.\n"
                    "It exists for the pre-tonemap path (dusklightDisplaySpace off), where the scene sits in an open ended linear range and the threshold has no "
                    "fixed meaning. There it is the one knob that needs tuning per setup, and disabling auto exposure makes it easier to calibrate by holding the "
                    "scene range still.",
                    args.minValue = 0.0f);
  };
  
}
