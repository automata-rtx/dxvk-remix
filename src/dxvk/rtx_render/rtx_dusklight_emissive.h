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
// And the score is weaker still than that: the lava pool scores **0.00**, in
// two independent runs. So the threshold has to reach zero, and at zero the
// colour gates below plus `authoredColor` are the whole rule. See §9.
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

  // Where an accepted emitter's colour comes from. GX has no emissive term, so
  // every one of these is a reading of the same data rather than a translation
  // of something the console recorded - which is exactly why it is a choice and
  // not a constant. The names match the overlay's combo.
  enum class DusklightEmissiveSource : int {
    // The albedo the shader has just reconstructed, ramp included. A surface
    // glows the colour it appears.
    ReconstructedAlbedo = 0,
    // The albedo texture, run through the material's own texture op. This is
    // what "Emit The Texture" did on 2026-08-04 and what the owner reported as
    // the right look for both the lava and the heart.
    AlbedoTexture = 1,
    // The flat colour aurora evaluated, used verbatim. Cheapest and steadiest;
    // reported as "an almost solid red" on the lava, so it is not the default.
    PresentedColor = 2,
  };

  struct DusklightEmissive {
    RTX_OPTION("rtx.dusklight.emissive", bool, enable, true,
               "Let surfaces the GameCube material evidence marks as self-illuminated actually emit light.\n"
               "No single GX fact identifies an emitter - the Goron Mines lava has GX lighting on - so aurora "
               "scores the evidence per draw and the thresholds below decide the cut. Turn this off to compare "
               "against the non-emissive rendering - the dusklight.emis log lines are still written either way, "
               "so a test session is not wasted.");
    RTX_OPTION("rtx.dusklight.emissive", float, intensity, 2.0f,
               "Radiance multiplier applied to an emissive surface's own colour.\n"
               "The colour already carries the game's idea of how bright the surface looks, so this is a flat "
               "scale rather than a per-material value. 2.0 matches what Remix already uses to make a world space "
               "UI surface read as self-lit; raise it if a big emitter glows but does not light the room around it.");
    RTX_OPTION("rtx.dusklight.emissive", DusklightEmissiveSource, colorSource,
               DusklightEmissiveSource::ReconstructedAlbedo,
               "Where an accepted emitter takes the colour it glows.\n"
               "0 Reconstructed Albedo - what the surface appears to be, two-colour ramp included. For the Goron "
               "Mines lava that is lerp(FF0000, FFFE63, texture): the texture drives the colour, and both survive.\n"
               "1 Albedo Texture - the texture through the material's own single D3D9 op. Looked right on "
               "2026-08-04, but only because the albedo was that same approximation then; now that the ramp is "
               "exact this is the worse of the two - on the lava the op is ADD, so it emits texture + red, which "
               "pins the red channel and washes the bright end to white.\n"
               "2 Presented Colour - one flat colour. A molten surface loses its crust entirely.");
    RTX_OPTION("rtx.dusklight.emissive", float, threshold, 0.0f,
               "How much GX evidence a surface needs before it is treated as an emitter (0..1).\n"
               "The game backend scores three facts: GX lighting disabled (0.50), colour authored in a register "
               "rather than per-vertex (0.25), and a TEV stage scaled past what the console could display (0.25). "
               "The Goron Mines lava scores 0.00 on all three, so the default is 0 and the colour gates below do "
               "the work - raise it toward 0.50 if too much of the world glows. The dusklight.emis log prints "
               "every surface's score, so this can be aimed rather than guessed.");
    RTX_OPTION("rtx.dusklight.emissive", bool, requireAuthoredColor, true,
               "Only let a surface glow when its colour came entirely from GameCube TEV constants.\n"
               "A colour mixed from the vertex stream is this game's baked room lighting, never an emitter, and "
               "excluding it is what makes a threshold of 0 usable at all. Turn it off only to check whether it "
               "is what is holding a surface back - the dusklight.emis log prints authored= per candidate.");
    RTX_OPTION("rtx.dusklight.emissive", float, minLuma, 0.25f,
               "Reject an emissive candidate whose presented colour is darker than this (0..1).\n"
               "Keeps dark interior geometry that scored above the threshold from glowing. Lower it if something "
               "that should glow does not.");
    RTX_OPTION("rtx.dusklight.emissive", float, minChroma, 0.20f,
               "Reject an emissive candidate whose presented colour is less saturated than this (0..1).\n"
               "White and grey candidates are overwhelmingly UI, screen copies and plain geometry, not emitters. "
               "Raise it to be stricter; set it to 0 to accept white emitters.");
    RTX_OPTION("rtx.dusklight.emissive", bool, log, true,
               "Log one line per distinct emissive candidate, accepted or rejected, with the numbers that decided it.\n"
               "Candidates rejected on colour alone are counted rather than enumerated - see the dusklight.emis.grey "
               "line - because on 2026-08-04 ninety of them spent the whole cap before the player reached the lava. "
               "Moving any control on this page makes every candidate report again, so what a setting did is "
               "recoverable from the log instead of having to be described.");

    // Covers the candidates a threshold could plausibly flip: colourless ones
    // are counted separately and do not consume it.
    static constexpr size_t kMaxLogged = 96;
  };

  // The two-colour ramp. Separate from emission but sharing the transport, so
  // it lives here rather than earning a third Dusklight header.
  struct DusklightRamp {
    RTX_OPTION("rtx.dusklight", bool, rampMaterials, true,
               "Reproduce the GameCube colour combiner directly for two-colour ramp materials.\n"
               "Most of this game's materials are lerp(colourA, colourB, texture) - one texture driving a slide "
               "between two authored colours, which is how one rupee texture yields seven rupee colours. No single "
               "D3D9 texture op carries that, so with this off they are approximated: a multiply renders black "
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

    // Aurora evaluated a presentable colour for this draw. Zero for HUD
    // (orthographic) and unevaluable draws, so those never become candidates
    // however low the threshold goes.
    //
    // The `score > 0` fallback is for an older aurora against this fork: before
    // 2026-08-05 the score was the only marker, and without the fallback a
    // mismatched pair would silently emit nothing at all.
    inline bool isCandidate(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Specular.g >= 0.5f || evidenceScore(mat) > 0.0f;
    }

    // The presented colour came entirely from TEV constants. See
    // requireAuthoredColor - this is the gate that makes threshold 0 usable.
    inline bool authoredColor(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Specular.b >= 0.5f;
    }

    inline Vector3 candidateColor(const LegacyMaterialData& mat) {
      const D3DCOLORVALUE& e = mat.getLegacyMaterial().Emissive;
      return Vector3(e.r, e.g, e.b);
    }

    inline const char* sourceName(DusklightEmissiveSource s) {
      switch (s) {
      case DusklightEmissiveSource::ReconstructedAlbedo: return "albedo";
      case DusklightEmissiveSource::AlbedoTexture:       return "texture";
      case DusklightEmissiveSource::PresentedColor:      return "constant";
      default:                                           return "?";
      }
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
          && (!DusklightEmissive::requireAuthoredColor() || authoredColor(mat))
          && lumaOf(color) >= DusklightEmissive::minLuma()
          && chromaOf(color) >= DusklightEmissive::minChroma();
    }

    // Bounded, one line per distinct material hash, accepted or not. Rejections
    // are logged too: an emitter that failed by 0.02 of chroma is a threshold
    // to move, and that is invisible if only acceptances are printed.
    //
    // Two things this got wrong on 2026-08-04, both of which cost that session's
    // evidence and are fixed here:
    //
    //  - the 96-line cap was spent on 90 chroma-zero candidates before the
    //    player reached the lava, so the one question the log existed to answer
    //    went unanswered. Colourless candidates are counted now, not
    //    enumerated, and they no longer consume the cap.
    //  - once-per-material meant dialling a threshold mid-session produced no
    //    new lines, so what a setting actually did was unrecoverable. The
    //    memory is cleared whenever a setting that decides a verdict changes.
    inline void logOnce(XXH64_hash_t materialHash, const Vector3& color, bool accepted,
                        XXH64_hash_t textureHash, float score, const LegacyMaterialData& mat) {
      if (!DusklightEmissive::log()) {
        return;
      }
      // Same single-writer assumption as matrep: the instance manager updates
      // instances on one thread.
      static std::unordered_set<XXH64_hash_t> s_seen;
      static size_t s_logged = 0;
      static bool s_truncated = false;
      static size_t s_colourless = 0;
      static size_t s_colourlessNextReport = 8;

      // Re-report everything when the rule itself moves, so a slider drag is
      // followed by evidence rather than silence.
      static XXH64_hash_t s_settings = 0;
      const struct {
        float threshold;
        float minLuma;
        float minChroma;
        uint32_t requireAuthored;
        uint32_t source;
      } settings = { DusklightEmissive::threshold(), DusklightEmissive::minLuma(),
                     DusklightEmissive::minChroma(),
                     DusklightEmissive::requireAuthoredColor() ? 1u : 0u,
                     static_cast<uint32_t>(DusklightEmissive::colorSource()) };
      const XXH64_hash_t settingsHash = XXH3_64bits(&settings, sizeof(settings));
      if (settingsHash != s_settings) {
        s_settings = settingsHash;
        s_seen.clear();
        s_logged = 0;
        s_truncated = false;
        s_colourless = 0;
        s_colourlessNextReport = 8;
      }

      if (!s_seen.insert(materialHash).second) {
        return;
      }

      // A candidate with no colour cannot be flipped by any threshold while
      // minChroma is above zero, so enumerating it teaches nothing. Counted
      // instead, and reported on each doubling - bounded at roughly log2(N)
      // lines while still showing the final magnitude.
      if (!accepted && chromaOf(color) < DusklightEmissive::minChroma()) {
        ++s_colourless;
        if (s_colourless >= s_colourlessNextReport) {
          Logger::info(str::format("dusklight.emis.grey distinct=", s_colourless,
                                   " minChroma=", DusklightEmissive::minChroma(),
                                   " - candidates rejected on colour alone, not enumerated"));
          s_colourlessNextReport *= 2;
        }
        return;
      }

      if (s_logged >= DusklightEmissive::kMaxLogged) {
        if (!s_truncated) {
          s_truncated = true;
          Logger::info(str::format("dusklight.emis.trunc cap=", DusklightEmissive::kMaxLogged,
                                   " - further candidates not reported"));
        }
        return;
      }
      ++s_logged;

      const D3DMATERIAL9& legacy = mat.getLegacyMaterial();
      Logger::info(str::format(
        "dusklight.emis mat=", std::hex, materialHash,
        " tex0hash=", textureHash, std::dec,
        " color=", color.x, ",", color.y, ",", color.z,
        " score=", score,
        " luma=", lumaOf(color),
        " chroma=", chromaOf(color),
        " authored=", authoredColor(mat) ? 1 : 0,
        // The ramp decides what the reconstructed albedo is, so it decides what
        // a ReconstructedAlbedo emitter glows. Printed here because pairing two
        // logs by hand to answer "did the ramp reach this surface" is exactly
        // the step that gets skipped.
        " ramp=", legacy.Diffuse.a >= 0.5f ? 1 : 0,
        " rampOther=", std::hex, dusklightRamp::otherColor(mat),
        " tFactor=", mat.tFactor, std::dec,
        " threshold=", DusklightEmissive::threshold(),
        " minLuma=", DusklightEmissive::minLuma(),
        " minChroma=", DusklightEmissive::minChroma(),
        " src=", sourceName(DusklightEmissive::colorSource()),
        " verdict=", accepted ? "emissive" : "rejected",
        " applied=", (accepted && DusklightEmissive::enable()) ? 1 : 0));
    }

  } // namespace dusklightEmissive

} // namespace dxvk
