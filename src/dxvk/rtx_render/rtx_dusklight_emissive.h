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

// Dusklight self-illumination, Remix half.
//
// GX has no emissive term, so nothing about this is a direct translation.
// Aurora scores what GX *does* say about a surface and ships that score plus
// the colour the surface presents in D3DMATERIAL9::Emissive; this half decides
// where to cut.
//
// No single GX fact identifies an emitter. The 2026-08-04 Goron Mines session
// proved both halves of that: "unlit" was true of 59% of an earlier scene, and
// the Goron Mines lava is `lit=1`, so requiring unlit could never have caught
// the one surface this feature exists for. Hence a score and a threshold rather
// than a predicate, with the threshold live in the F1 overlay.
//
// Design, the measurements, and what to do when the rule over- or under-fires:
//   aurora-ao/docs/dx9/remix-material-interface.md §9

#include "rtx_option.h"
#include "rtx_materials.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <algorithm>
#include <unordered_set>

namespace dxvk {

  struct DusklightEmissive {
    RTX_OPTION("rtx.dusklight.emissive", bool, enable, true,
               "Make surfaces that GameCube GX marked as taking no light actually emit light.\n"
               "Aurora reports the GX evidence per draw; the thresholds below decide which of those surfaces "
               "are treated as emitters. Turn this off to compare against the unlit rendering - the "
               "dusklight.emis log lines are still written either way, so a test session is not wasted.");
    RTX_OPTION("rtx.dusklight.emissive", float, intensity, 2.0f,
               "Radiance multiplier applied to an emissive surface's own colour.\n"
               "The colour already carries the game's idea of how bright the surface looks, so this is a flat "
               "scale rather than a per-material value. 2.0 matches what Remix already uses to make a world space "
               "UI surface read as self-lit; raise it if a big emitter glows but does not light the room around it.");
    RTX_OPTION("rtx.dusklight.emissive", float, threshold, 0.70f,
               "How much GX evidence a surface needs before it is treated as an emitter (0..1).\n"
               "The game backend scores three facts: GX lighting disabled (0.50), colour authored in a register "
               "rather than per-vertex (0.25), and a TEV stage scaled past what the console could display (0.25). "
               "0.70 admits anything unlit plus one other fact. Drop to 0.20 to admit the over-range materials on "
               "their own - that is the setting to try when something that clearly glows in the original does not "
               "here. The dusklight.emis log prints every surface's score, so this can be aimed rather than guessed.");
    RTX_OPTION("rtx.dusklight.emissive", float, minLuma, 0.25f,
               "Reject an emissive candidate whose presented colour is darker than this (0..1).\n"
               "Keeps unlit-but-dark interior geometry from glowing. Lower it if something that should glow does not.");
    RTX_OPTION("rtx.dusklight.emissive", float, minChroma, 0.20f,
               "Reject an emissive candidate whose presented colour is less saturated than this (0..1).\n"
               "White and grey unlit surfaces are overwhelmingly UI, screen copies and plain geometry, not emitters. "
               "Raise it to be stricter; set it to 0 to accept white emitters.");
    RTX_OPTION("rtx.dusklight.emissive", bool, useTextureColor, false,
               "Take the emitted colour from the albedo texture instead of the colour aurora evaluated.\n"
               "Off by default because this game keeps a material's colour in a GX constant and its textures are "
               "usually intensity-only masks - so a textured glow would come out white. Turn it on for a material "
               "whose texture really is the colour, at the cost of a flat glow becoming a textured one.");
    RTX_OPTION("rtx.dusklight.emissive", bool, log, true,
               "Log one line per distinct emissive candidate, accepted or rejected, with the numbers that decided it.\n"
               "Bounded; see the dusklight.emis.trunc line. This is how a test session answers which surfaces the "
               "rule caught without anyone having to describe what they saw.");

    // Small on purpose: this reports distinct *candidates*, and a scene with
    // hundreds of them means the rule is wrong, not that the cap is too low.
    static constexpr size_t kMaxLogged = 96;
  };

  // The two-colour ramp. Separate from emission but sharing the transport, so
  // it lives here rather than earning a third Dusklight header.
  struct DusklightRamp {
    RTX_OPTION("rtx.dusklight", bool, rampMaterials, true,
               "Reproduce the GameCube colour combiner directly for two-colour ramp materials.\n"
               "Most of this game's materials are lerp(colourA, colourB, texture) - one texture driving a slide "
               "between two authored colours, which is how one rupee texture yields seven rupee colours. No stock "
               "D3D9 texture op expresses that, so with this off they are approximated: a multiply renders black "
               "where the texture is dark, an add drives the bright end to white (Goron Mines lava reads "
               "red-and-white instead of red-to-orange). Turn it off to compare against that approximation.");
  };

  namespace dusklightRamp {

    // Aurora ships the ramp in the otherwise unused half of D3DMATERIAL9:
    // Diffuse.rgb is the endpoint tFactor does not carry, Diffuse.a marks the
    // material as a ramp, Ambient.r says which endpoint tFactor holds.
    inline bool isRamp(const LegacyMaterialData& mat) {
      return DusklightRamp::rampMaterials() && mat.getLegacyMaterial().Diffuse.a >= 0.5f;
    }

    inline bool tFactorIsHigh(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Ambient.r >= 0.5f;
    }

    // Packed 0x00RRGGBB, matching tFactor's byte order so the shader unpacks
    // both the same way.
    inline uint32_t otherColor(const LegacyMaterialData& mat) {
      const D3DCOLORVALUE& d = mat.getLegacyMaterial().Diffuse;
      auto quantize = [](float v) -> uint32_t {
        return uint32_t(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f)));
      };
      return (quantize(d.r) << 16) | (quantize(d.g) << 8) | quantize(d.b);
    }

  } // namespace dusklightRamp

  namespace dusklightEmissive {

    // Aurora's evidence score rides D3DMATERIAL9::Emissive.a. Nothing else in
    // this chain writes a material - the backend keeps D3DRS_LIGHTING off - so
    // a non-zero alpha can only have come from aurora.
    inline float evidenceScore(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Emissive.a;
    }

    inline bool isCandidate(const LegacyMaterialData& mat) {
      return evidenceScore(mat) > 0.0f;
    }

    inline Vector3 candidateColor(const LegacyMaterialData& mat) {
      const D3DCOLORVALUE& e = mat.getLegacyMaterial().Emissive;
      return Vector3(e.r, e.g, e.b);
    }

    inline float lumaOf(const Vector3& c) {
      return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    }

    inline float chromaOf(const Vector3& c) {
      return std::max(std::max(c.x, c.y), c.z) - std::min(std::min(c.x, c.y), c.z);
    }

    // The judgement call, isolated so there is exactly one place to argue with.
    inline bool accepts(const LegacyMaterialData& mat, const Vector3& color) {
      return evidenceScore(mat) >= DusklightEmissive::threshold()
          && lumaOf(color) >= DusklightEmissive::minLuma()
          && chromaOf(color) >= DusklightEmissive::minChroma();
    }

    // emissiveColorConstant does NOT reach the shader untouched: the fixed
    // function block in opaque_surface_material_interaction.slangh runs the
    // emissive colour through the *albedo's* texture op, substituting it for
    // the texture sample. So setting the constant to the colour we want yields
    // op(colour, tFactor) on screen, not colour.
    //
    // Rather than fight that, invert it. The ops aurora actually emits are
    // ADD(TEXTURE, TFACTOR), MODULATE(TEXTURE, TFACTOR) and
    // SELECTARG1(TEXTURE); anything else is left alone and reported, because a
    // silently wrong glow colour is exactly the failure this project keeps
    // paying for. Returns false when the op cannot be inverted.
    inline bool preimage(const LegacyMaterialData& mat, const Vector3& desired, Vector3& out) {
      const bool arg1IsTexture = mat.textureColorArg1Source == RtTextureArgSource::Texture;
      const bool arg2IsTFactor = mat.textureColorArg2Source == RtTextureArgSource::TFactor;

      // tFactor is a D3DCOLOR: 0xAARRGGBB.
      const Vector3 tFactor(float((mat.tFactor >> 16) & 0xFF) / 255.0f,
                            float((mat.tFactor >> 8) & 0xFF) / 255.0f,
                            float(mat.tFactor & 0xFF) / 255.0f);

      // isTextureFactorBlend applies one more multiply by tFactor afterwards.
      Vector3 target = desired;
      if (mat.isTextureFactorBlend) {
        for (uint32_t i = 0; i < 3; ++i) {
          if (tFactor[i] <= 0.0f) {
            return false;
          }
          target[i] /= tFactor[i];
        }
      }

      switch (mat.textureColorOperation) {
      case DxvkRtTextureOperation::SelectArg1:
        if (!arg1IsTexture) {
          return false;
        }
        out = target;
        return true;
      case DxvkRtTextureOperation::Add:
        if (!arg1IsTexture || !arg2IsTFactor) {
          return false;
        }
        for (uint32_t i = 0; i < 3; ++i) {
          out[i] = std::max(0.0f, target[i] - tFactor[i]);
        }
        return true;
      case DxvkRtTextureOperation::Modulate:
      case DxvkRtTextureOperation::Modulate2x:
      case DxvkRtTextureOperation::Force_Modulate2x:
      case DxvkRtTextureOperation::Modulate4x: {
        if (!arg1IsTexture || !arg2IsTFactor) {
          return false;
        }
        const float scale = mat.textureColorOperation == DxvkRtTextureOperation::Modulate4x ? 4.0f
                          : mat.textureColorOperation == DxvkRtTextureOperation::Modulate    ? 1.0f
                                                                                             : 2.0f;
        for (uint32_t i = 0; i < 3; ++i) {
          const float d = tFactor[i] * scale;
          if (d <= 0.0f) {
            return false;
          }
          out[i] = target[i] / d;
        }
        return true;
      }
      default:
        return false;
      }
    }

    // Bounded, one line per distinct material hash, accepted or not. Rejections
    // are logged too: an emitter that failed by 0.02 of chroma is a threshold
    // to move, and that is invisible if only acceptances are printed.
    inline void logOnce(XXH64_hash_t materialHash, const Vector3& color, bool accepted,
                        XXH64_hash_t textureHash, float score, bool invertible) {
      if (!DusklightEmissive::log()) {
        return;
      }
      // Same single-writer assumption as matrep: the instance manager updates
      // instances on one thread.
      static std::unordered_set<XXH64_hash_t> s_seen;
      static bool s_truncated = false;
      if (s_seen.count(materialHash) != 0) {
        return;
      }
      if (s_seen.size() >= DusklightEmissive::kMaxLogged) {
        if (!s_truncated) {
          s_truncated = true;
          Logger::info(str::format("dusklight.emis.trunc cap=", DusklightEmissive::kMaxLogged,
                                   " - further candidates not reported"));
        }
        return;
      }
      s_seen.insert(materialHash);
      Logger::info(str::format(
        "dusklight.emis mat=", std::hex, materialHash,
        " tex0hash=", textureHash, std::dec,
        " color=", color.x, ",", color.y, ",", color.z,
        " score=", score,
        " luma=", lumaOf(color),
        " chroma=", chromaOf(color),
        " threshold=", DusklightEmissive::threshold(),
        " minLuma=", DusklightEmissive::minLuma(),
        " minChroma=", DusklightEmissive::minChroma(),
        " invertible=", invertible ? 1 : 0,
        " verdict=", accepted ? "emissive" : "rejected",
        " applied=", (accepted && invertible && DusklightEmissive::enable()) ? 1 : 0));
    }

  } // namespace dusklightEmissive

} // namespace dxvk
