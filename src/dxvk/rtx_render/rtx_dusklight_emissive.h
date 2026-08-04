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
// Aurora ships the strongest statement GX can make - "this colour channel
// takes no light and its colour is authored in a register" - plus the colour
// the surface presents, in D3DMATERIAL9::Emissive. That statement alone is far
// too broad to act on: 69 of the 117 materials in the 2026-08-04 session had
// lighting disabled. This half applies the part that is a judgement call, and
// keeps it in options so it moves from the F1 overlay rather than a rebuild.
//
// Design, the measurement behind the thresholds, and what to do when the rule
// over- or under-fires:
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

  namespace dusklightEmissive {

    // Aurora's verdict rides D3DMATERIAL9::Emissive.a. Nothing else in this
    // chain writes a material - the backend keeps D3DRS_LIGHTING off - so a
    // non-zero alpha can only have come from aurora.
    inline bool isCandidate(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Emissive.a >= 0.5f;
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
    inline bool accepts(const Vector3& color) {
      return lumaOf(color) >= DusklightEmissive::minLuma()
          && chromaOf(color) >= DusklightEmissive::minChroma();
    }

    // Bounded, one line per distinct material hash, accepted or not. Rejections
    // are logged too: an emitter that failed by 0.02 of chroma is a threshold
    // to move, and that is invisible if only acceptances are printed.
    inline void logOnce(XXH64_hash_t materialHash, const Vector3& color, bool accepted,
                        XXH64_hash_t textureHash) {
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
        " luma=", lumaOf(color),
        " chroma=", chromaOf(color),
        " minLuma=", DusklightEmissive::minLuma(),
        " minChroma=", DusklightEmissive::minChroma(),
        " verdict=", accepted ? "emissive" : "rejected",
        " applied=", (accepted && DusklightEmissive::enable()) ? 1 : 0));
    }

  } // namespace dusklightEmissive

} // namespace dxvk
