/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
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

  // Ambient mood grade driven by the game's environment feed.
  //
  // Fixed function era games carried a per area, per time of day, per weather ambient colour
  // that every surface was tinted by during shading. Path tracing replaces that lighting
  // wholesale, so the mood it carried disappears even though the game still computes it and
  // still pushes it to us (rtx.dusklight.env.actorAmbient / bgAmbient). This pass puts the
  // chromatic part of it back as a single multiply over the linear HDR image.
  //
  // It runs immediately before bloom, which is where the ambient sat in the original frame
  // too: the ambient was applied during shading, and the bloom and its overlays came after.
  // Being its own pass rather than another step inside the bloom keeps it working - and
  // failing - independently of whether bloom is enabled at all.
  class DxvkDusklightGrade: public RtxPass {

  public:
    explicit DxvkDusklightGrade(DxvkDevice* device);
    ~DxvkDusklightGrade();

    DxvkDusklightGrade(const DxvkDusklightGrade&) = delete;
    DxvkDusklightGrade(DxvkDusklightGrade&&) noexcept = delete;
    DxvkDusklightGrade& operator=(const DxvkDusklightGrade&) = delete;
    DxvkDusklightGrade& operator=(DxvkDusklightGrade&&) noexcept = delete;

    void dispatch(
      Rc<RtxContext> ctx,
      const Resources::Resource& inOutColorBuffer);

    void showImguiSettings();

  private:
    // The multiplier the image is graded with this frame, and whether it differs from white
    // by enough to be worth a full screen pass.
    struct EffectiveGrade {
      bool    active;
      Vector3 tint;
    };

    EffectiveGrade resolveGrade() const;

    virtual bool isEnabled() const override;

    Rc<vk::DeviceFn> m_vkd;

    RTX_OPTION("rtx.dusklight.grade", bool, enable, false,
               "Grades the image with the ambient colour the game's environment system reports through rtx.dusklight.env.actorAmbient and bgAmbient.\n"
               "The game's ambient term used to tint every surface during shading; the path tracer lights the scene itself, so this reapplies the part of it that "
               "carried the mood - the colour. Time of day, weather, area palettes and story events all move it.\n"
               "Has no effect unless the game's bridge is feeding Remix (rtx.dusklight.env.enable).");
    RTX_OPTION_ARGS("rtx.dusklight.grade", float, strength, 0.65f,
                    "How far the image is graded towards the game's ambient colour, 0..1. At 0 the grade is off; at 1 the resolved tint is applied in full.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION("rtx.dusklight.grade", bool, chromaOnly, true,
               "Normalizes the ambient tint by its own luminance, so the grade changes the colour of the image without changing how bright it is.\n"
               "This is almost always what you want: the path tracer already decides the scene's brightness, and a grade that darkens the frame just gets "
               "undone by auto exposure a moment later, leaving a pumping image. Turn it off to let the game's ambient drive overall brightness too, "
               "which tracks night and interiors more literally at the cost of fighting exposure.");
    RTX_OPTION_ARGS("rtx.dusklight.grade", float, actorAmbientWeight, 0.25f,
                    "How much the actor ambient contributes to the grade relative to the background ambient, 0..1.\n"
                    "The game keeps separate ambients for actors and for room geometry, but the grade is a single multiply over the whole image, so the two are "
                    "mixed by this weight. The default leans on the background ambient because that is what most of the screen is.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.grade", float, maxDarkening, 0.35f,
                    "How far down the grade is allowed to take the image, before rtx.dusklight.grade.strength is applied.\n"
                    "The rail is applied to the tint's overall level first, by scaling, so that a dark ambient is lifted without losing the colour it was graded "
                    "for - and then per channel as a backstop for palettes that drive a single channel to near zero, which some story and debug states do. "
                    "Only the backstop can fire while rtx.dusklight.grade.chromaOnly is on, since normalization already fixes the level at 1.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.grade", float, maxBrightening, 2.0f,
                    "How far up the grade is allowed to take the image, before rtx.dusklight.grade.strength is applied. The counterpart rail to "
                    "rtx.dusklight.grade.maxDarkening, applied the same way.\n"
                    "This one earns its keep under rtx.dusklight.grade.chromaOnly: normalizing a strongly saturated ambient leaves its dominant channel several "
                    "times above 1, and this bounds how far that channel can push the image.",
                    args.minValue = 1.0f);
  };

}
