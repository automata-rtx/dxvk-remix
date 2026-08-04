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
#include "rtx_context.h"
#include "rtx_dusklight_grade.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/dusklight/dusklight_grade.h"

#include <rtx_shaders/dusklight_grade.h>
#include "rtx_imgui.h"

#include <algorithm>
#include <cmath>

namespace dxvk {
  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class DusklightGradeShader : public ManagedShader
    {
      SHADER_SOURCE(DusklightGradeShader, VK_SHADER_STAGE_COMPUTE_BIT, dusklight_grade)

      PUSH_CONSTANTS(DusklightGradeArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(DUSKLIGHT_GRADE_COLOR_INPUT_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DusklightGradeShader);
  }

  DxvkDusklightGrade::DxvkDusklightGrade(DxvkDevice* device): RtxPass(device), m_vkd(device->vkd()) {
  }

  DxvkDusklightGrade::~DxvkDusklightGrade() {
  }

  DxvkDusklightGrade::EffectiveGrade DxvkDusklightGrade::resolveGrade() const {
    EffectiveGrade grade = {};
    grade.tint = Vector3(1.0f, 1.0f, 1.0f);

    if (!enable() || !DusklightEnv::enable()) {
      return grade;
    }

    // The game keeps the two ambients apart because it applied them to different geometry.
    // A single full screen multiply cannot, so mix them by how much of the screen each one
    // was responsible for.
    const float actorWeight = std::clamp(actorAmbientWeight(), 0.0f, 1.0f);
    const Vector3 actor = DusklightEnv::actorAmbient();
    const Vector3 background = DusklightEnv::bgAmbient();

    Vector3 tint = background * (1.0f - actorWeight) + actor * actorWeight;

    tint.x = std::max(tint.x, 0.0f);
    tint.y = std::max(tint.y, 0.0f);
    tint.z = std::max(tint.z, 0.0f);

    const auto luminanceOf = [](const Vector3& v) {
      return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
    };

    if (chromaOnly()) {
      // Dividing out the ambient's own luminance leaves only how it is coloured relative to
      // neutral, which is the part that survives the move to path tracing. Its absolute level
      // does not: the path tracer sets the scene's brightness, and grading against that just
      // gives auto exposure something to undo.
      const float luminance = luminanceOf(tint);

      if (luminance <= 1e-4f) {
        // A black ambient carries no hue to recover.
        return grade;
      }

      tint /= luminance;
    }

    // Rails, applied before strength so that strength always reads as a straight blend
    // between "no grade" and "the tint shown in the UI".
    const float floorValue = std::clamp(maxDarkening(), 0.0f, 1.0f);
    const float ceilValue = std::max(maxBrightening(), 1.0f);

    // Rail the overall level first. Scaling keeps the hue intact, which a per channel clamp
    // does not: a night ambient is dark in all three channels, so clamping each one against
    // the floor separately flattens it to grey and throws away the very thing being graded
    // with. In chroma only mode the luminance is 1 by construction and this does nothing.
    const float level = luminanceOf(tint);

    if (level > 1e-4f) {
      tint *= std::clamp(level, floorValue, ceilValue) / level;
    }

    // Then a per channel backstop, for tints too saturated for a level rail to bound - the
    // dominant channel of a strongly coloured ambient survives normalization several times
    // over even when its luminance is exactly 1.
    tint.x = std::clamp(tint.x, floorValue, ceilValue);
    tint.y = std::clamp(tint.y, floorValue, ceilValue);
    tint.z = std::clamp(tint.z, floorValue, ceilValue);

    const float amount = std::clamp(strength(), 0.0f, 1.0f);
    const Vector3 white(1.0f, 1.0f, 1.0f);

    tint = white + (tint - white) * amount;

    // A tint this close to white is not worth a full screen pass. Reached whenever the game
    // reports a neutral ambient or strength is 0; the bridge-off case never gets this far, it
    // returns above.
    constexpr float kNeutralEpsilon = 1e-4f;
    const bool neutral = std::fabs(tint.x - 1.0f) < kNeutralEpsilon &&
                         std::fabs(tint.y - 1.0f) < kNeutralEpsilon &&
                         std::fabs(tint.z - 1.0f) < kNeutralEpsilon;

    grade.active = !neutral;
    grade.tint = tint;

    return grade;
  }

  void DxvkDusklightGrade::showImguiSettings() {
    ImGui::Indent();
    RemixGui::Checkbox("Ambient Grade Enabled", &enableObject());

    ImGui::Indent();

    if (!DusklightEnv::enable()) {
      ImGui::TextWrapped("Waiting for the game's environment feed (rtx.dusklight.env.enable). "
                         "The grade stays neutral until the game's bridge is running.");
    }

    RemixGui::DragFloat("Strength##dusklightGrade", &strengthObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::Checkbox("Chroma Only##dusklightGrade", &chromaOnlyObject());
    RemixGui::DragFloat("Actor Ambient Weight##dusklightGrade", &actorAmbientWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Max Darkening##dusklightGrade", &maxDarkeningObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Max Brightening##dusklightGrade", &maxBrighteningObject(), 0.05f, 1.f, 8.f, "%.2f");

    const EffectiveGrade grade = resolveGrade();
    ImGui::Text("Resolved tint: %.3f, %.3f, %.3f%s", grade.tint.x, grade.tint.y, grade.tint.z,
                grade.active ? "" : " (neutral, pass skipped)");

    ImGui::Unindent();
    ImGui::Unindent();
  }

  void DxvkDusklightGrade::dispatch(Rc<RtxContext> ctx,
                                    const Resources::Resource& inOutColorBuffer) {
    const EffectiveGrade grade = resolveGrade();

    if (!grade.active) {
      return;
    }

    ScopedGpuProfileZone(ctx, "Dusklight Ambient Grade");
    // Shares the bloom's frame pass stage: it runs in the same window, immediately ahead of
    // it, and resource aliasing has no reason to tell the two apart.
    ctx->setFramePassStage(RtxFramePassStage::Bloom);

    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    const VkExtent3D outputSize = inOutColorBuffer.image->info().extent;

    DusklightGradeArgs pushArgs = {};
    pushArgs.imageSize = { outputSize.width, outputSize.height };
    pushArgs.tint = grade.tint;
    ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

    const VkExtent3D workgroups = util::computeBlockCount(outputSize, VkExtent3D { 16, 16, 1 });

    ctx->bindResourceView(DUSKLIGHT_GRADE_COLOR_INPUT_OUTPUT, inOutColorBuffer.view, nullptr);
    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, DusklightGradeShader::getShader());
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  bool DxvkDusklightGrade::isEnabled() const {
    return enable();
  }
}
