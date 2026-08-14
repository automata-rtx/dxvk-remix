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
// A GameCube surface is self-lit when its TEV colour program never reads the
// rasterized channel: its colour is then fixed whatever the lights do, which is
// what the console draws as full-bright and what a path tracer has to emit to
// reproduce. That fact is the basis of this rule.
//
// It is not sufficient on its own, and the measurement says why. Of the 20
// self-lit materials in the 2026-08-05 Goron Mines session, 9 were EFB copies
// and full-screen quads - a white screen blit is self-lit and must not light
// the room. Every one of those is a bare texture pass-through with no colour of
// its own, and every real emitter carries a colour authored in TEV constants
// over an intensity mask. So:
//
//     emissive  =  self-lit  AND  has a colour of its own  AND  that colour
//                  reads as a glow (saturated, or bright)
//
// Three structural facts and one colour test. No score, no threshold, nothing
// to dial to get a correct picture - replayed over that session it accepts 6 of
// 77 materials, all five lava and fire surfaces plus one warm glow texture,
// with no false positives.
//
// The evidence score aurora still ships is reported in the log and no longer
// decides anything; two revisions cut on it and both missed the lava, which
// scores 0.00.
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
               "Let self-illuminated GameCube surfaces emit light.\n"
               "A surface qualifies when its GX colour program never reads the lit channel, it carries a colour "
               "authored in GX constants rather than a plain texture, and that colour reads as a glow. Nothing "
               "here needs tuning. Turn it off to compare against the non-emissive rendering - the dusklight.emis "
               "log lines are still written either way, so a test session is not wasted.");
    RTX_OPTION("rtx.dusklight.emissive", float, brightness, 10.0f,
               "How brightly an emissive surface glows - a target brightness, not a multiplier.\n"
               "A flat multiplier made a dark saturated colour glow dimly and a pale one glow fiercely, purely "
               "because of how bright the authored colour happened to be: the accepted materials in one measured "
               "scene span luma 0.30 to 0.92, a 3x spread nobody chose. This divides that out, so the dial means "
               "the same thing on every surface.\n"
               "10.0 is measured, not guessed: it is the value the Goron Mines lava was dialled to in game on "
               "2026-08-06 to read as properly molten. 1.0 would put an emitter at roughly the brightness of a "
               "fully lit white surface, which is not what a self-lit surface in a dark cave should look like. "
               "Calibrated in one dark interior, so a bright exterior may want less.");
    RTX_OPTION_ARGS("rtx.dusklight.emissive", float, brightnessLumaWeight, 1.0f,
                    "How much an emitter's own brightness is divided out of rtx.dusklight.emissive.brightness, 0..1.\n"
                    "At 1.0 - the original behaviour and still the default - the dial is a target brightness: a dark saturated colour and a pale one reach the "
                    "same result, because the colour's luminance is divided out. At 0.0 the dial is a plain multiplier and a dark colour stays dark.\n"
                    "It exists because the normalisation inverts the ordering for pickups. Measured in one session: a green rupee at luma 0.36 was handed "
                    "radiance 28.1 while the Goron Mines lava at luma 0.55 got 18.3 - the rupee glowing half again as hard as molten rock, purely because its "
                    "authored green is darker. Lower this and dark emitters come down without touching pale ones.\n"
                    "This is a shape control, not a classifier. It cannot tell a rupee from lava; it only stops darkness alone earning brightness. Per-object "
                    "control needs the game to mark the draw.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.emissive", float, pickupBrightness, 2.0f,
                    "The same dial as rtx.dusklight.emissive.brightness, for dropped pickups only - rupees, hearts, arrows.\n"
                    "They need their own number because the emissive rule cannot separate them from lava and should not be asked to: full-bright in GX means "
                    "'do not shade me', which is true of a rupee and of molten rock alike, and it does not mean 'light the room'. The game marks the draw "
                    "instead, over the per-draw metadata export rather than a repurposed D3DMATERIAL9 field.\n"
                    "2.0 against the world's 10.0 is a starting point and not a measurement. A pickup still has to read as glowing rather than as a lit object, "
                    "so the useful range is well above zero.",
                    args.minValue = 0.0f,
                    args.maxValue = 50.0f);
    RTX_OPTION_ARGS("rtx.dusklight.emissive", float, pickupBrightnessLumaWeight, 0.0f,
                    "The pickup half of rtx.dusklight.emissive.brightnessLumaWeight, 0..1.\n"
                    "Defaults to 0 - a plain multiplier - where the world defaults to 1. That is the whole point of splitting them: dividing out the colour's "
                    "own luminance hands the darkest colour the largest boost, which is right for making one dial mean the same thing across lava and fire and "
                    "wrong for a set of objects whose colours are the game's own colour coding. A green rupee should not out-glow a yellow one for being "
                    "darker green.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.emissive", float, maxRadiance, 0.0f,
                    "Ceiling on any single emitter's radiance. 0 disables it, which is the default.\n"
                    "Blunt on purpose, and it is the control that can be set from a log rather than by eye: every dusklight.emis line prints the radiance the "
                    "surface asked for, so a ceiling can be chosen against the numbers a session actually produced.",
                    args.minValue = 0.0f,
                    args.maxValue = 256.0f);
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
    RTX_OPTION("rtx.dusklight.emissive", float, glowChroma, 0.50f,
               "A colour this saturated counts as a glow (0..1).\n"
               "Either this or Brightness Counts As Glow is enough - an authored glow is a strong colour or it is "
               "near-white-hot, and a muted mid-tone is a surface colour. Nothing needs tuning here; it is an "
               "option so that an over- or under-firing scene can be corrected without a rebuild.");
    RTX_OPTION("rtx.dusklight.emissive", float, glowLuma, 0.70f,
               "A colour this bright counts as a glow even if it is not saturated (0..1).\n"
               "Catches white-hot and pale-warm emitters, which have little chroma but are far brighter than "
               "any surface colour in this game.");
    RTX_OPTION("rtx.dusklight.emissive", bool, log, true,
               "Log one line per distinct emissive candidate, accepted or rejected, with the numbers that decided it.\n"
               "Candidates rejected on colour alone are counted rather than enumerated - see the dusklight.emis.grey "
               "line - because on 2026-08-04 ninety of them spent the whole cap before the player reached the lava. "
               "Moving any control on this page makes every candidate report again, so what a setting did is "
               "recoverable from the log instead of having to be described.");

    // Covers the candidates worth enumerating; colourless ones are counted
    // separately and do not consume it.
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

    // Aurora ships the ramp through the D3DMATERIAL9 side channel: Diffuse.rgb
    // is the endpoint tFactor does not carry, Diffuse.a marks the material as a
    // ramp, Ambient.r says which endpoint tFactor holds.
    //
    // The struct is no longer "the otherwise unused half" it was described as
    // here: Ambient.g/.b went to HD texture packs on 2026-08-05
    // (rtx_dusklight_texrep.h) and Power to water on 2026-08-11
    // (rtx_dusklight_water.h, all three of its facts packed into the one field).
    // Ambient.a is the only spare left. The full field map lives in one place -
    // aurora-ao/docs/dx9/remix-material-interface.md §2, mirrored in
    // documentation/DusklightSideChannels.md - and any new claim on this struct
    // must be added there in the same commit rather than only in a comment.
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
    // (orthographic) and unevaluable draws.
    //
    // The `score > 0` fallback is for an older aurora against this fork: before
    // 2026-08-05 the score was the only marker, and without it a mismatched
    // pair would silently emit nothing at all.
    inline bool isCandidate(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Specular.g >= 0.5f || evidenceScore(mat) > 0.0f;
    }

    // SELF-LIT: no TEV colour stage reads the rasterized channel, so the
    // surface's colour is fixed whatever the lights do. This is the basis of
    // the rule, not a weighted signal.
    inline bool selfLit(const LegacyMaterialData& mat) {
      return mat.getLegacyMaterial().Specular.a >= 0.5f;
    }

    // The material has a colour of its own: authored in TEV constants, not
    // mixed from the vertex stream and not a bare texture pass-through. The
    // pass-through case is what keeps EFB copies and full-screen quads out -
    // 9 of the 20 self-lit materials in the measured scene were exactly that.
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

    // Radiance for an accepted emitter.
    //
    // GX records nothing about how brightly a surface should glow, so this is a
    // derivation rather than a translation - but it is derived from the one
    // thing GX does say, which is how bright the authored colour is.
    //
    // Dividing by that luma makes `brightness` a target rather than a
    // multiplier. Two consequences, and the second is the point:
    //
    //  - a dark saturated emitter and a pale one reach the same brightness at
    //    the same setting, instead of the pale one being 3x hotter for free;
    //  - a surface whose colour *sweeps* - the lava's lerp(FF0000, FFFE63,
    //    texture) - is normalised by its dark end, so its bright end overshoots
    //    and reads as a white-hot core. A flat colour like a pickup glow has
    //    nothing to overshoot with and stays even. That is the lava-versus-heart
    //    difference falling out of the data rather than being tagged in.
    //
    // The floor is a constant rather than an option because it exists only to
    // stop a near-black colour asking for an unbounded multiplier; 0.20 caps
    // the boost at 5x. Nothing is expected to sit near it - the glow test
    // already requires a colour that is saturated or bright.
    static constexpr float kLumaFloor = 0.20f;

    // MEASURED 2026-08-13, from the owner's session log, and it is the opposite of what the
    // paragraph above predicts for pickups:
    //
    //   green rupee   color 0,0.60,0.03   luma 0.355   radiance 28.14
    //   Goron lava    color 1,0.42,0      luma 0.545   radiance 18.34
    //   yellow item   color 1,1,0.20      luma 0.909   radiance 11.00
    //
    // The dark saturated pickup comes out **brighter than the lava**, because normalising by luma
    // hands the darkest colour the largest multiplier and a rupee's green is darker than molten
    // rock's orange. The overshoot argument above is real but only applies to a colour that sweeps;
    // it does not stop a flat dark colour being handed 2.8x where the lava gets 1.8x.
    //
    // The two dials below make that correctable without a per-draw signal. They are NOT the same
    // thing as knowing a draw is a dropped item - that needs the game to mark it and would need a
    // transport - so they are deliberately default-inert and change nothing until set.
    // A pickup is not a light source, and the emissive rule cannot see the difference.
    //
    // Full-bright in GX means "do not shade me", which is true of a rupee and of lava alike; the
    // console draws both without reading the lights. It does not mean "light the room", and that is
    // where the two come apart. Nothing in the material says which is which, so the game says it -
    // through the per-draw metadata channel, not through a repurposed D3DMATERIAL9 field, because
    // the channels ran out and the channels were never the real constraint.
    // rtx_dusklight_drawmeta.h.
    inline bool isPickup(const LegacyMaterialData& material) {
      return (material.dusklightDrawMeta.flags & DusklightDrawMeta::kFlagPickup) != 0;
    }

    inline float radianceFor(const Vector3& color, bool pickup) {
      // At 1.0 this is the original target-brightness behaviour: divide out the colour's own
      // brightness so the dial means the same thing on every surface. At 0.0 it is a flat
      // multiplier, which stops a dark colour being boosted past a bright one. In between is the
      // useful range, because the normalisation is right about pale surfaces and wrong about dark
      // ones.
      // A pickup takes its own pair of dials. Same shape, separate numbers, so the lava can stay
      // molten while a dropped rupee stops out-glowing it.
      const float target = pickup ? DusklightEmissive::pickupBrightness() : DusklightEmissive::brightness();
      const float weight = std::clamp(pickup ? DusklightEmissive::pickupBrightnessLumaWeight()
                                             : DusklightEmissive::brightnessLumaWeight(),
                                      0.0f, 1.0f);
      const float luma = std::max(lumaOf(color), kLumaFloor);
      const float divisor = std::max(1.0f + (luma - 1.0f) * weight, kLumaFloor);

      const float radiance = target / divisor;

      // A ceiling, off at 0. Blunt on purpose: it is the one control that can be reasoned about
      // from a log line without knowing what the surface is, since every dusklight.emis line prints
      // the radiance it asked for.
      const float ceiling = DusklightEmissive::maxRadiance();

      return ceiling > 0.0f ? std::min(radiance, ceiling) : radiance;
    }

    // The rule. Three structural facts and one colour test - no score, no
    // threshold, nothing to dial to get a correct picture.
    //
    // Replayed over the 2026-08-05 Goron Mines log it accepts 6 of 77
    // materials: all five lava and fire surfaces plus one bright warm glow
    // texture, and no false positives. Each clause earns its place there -
    // 27 materials are rejected by self-lit, 9 by authored colour (every one
    // of them an EFB copy or full-screen quad), 5 by the colour test.
    inline bool accepts(const LegacyMaterialData& mat, const Vector3& color) {
      return selfLit(mat)
          && authoredColor(mat)
          && (chromaOf(color) >= DusklightEmissive::glowChroma()
              || lumaOf(color) >= DusklightEmissive::glowLuma());
    }

    // Bounded, one line per distinct material hash, accepted or not. Rejections
    // are logged too: an emitter that failed by 0.02 of chroma is a constant to
    // move, and that is invisible if only acceptances are printed.
    //
    // Two things this got wrong on 2026-08-04, both of which cost that session's
    // evidence and are fixed here:
    //
    //  - the 96-line cap was spent on 90 chroma-zero candidates before the
    //    player reached the lava, so the one question the log existed to answer
    //    went unanswered. Colourless candidates are counted now, not
    //    enumerated, and they no longer consume the cap.
    //  - once-per-material meant changing a setting mid-session produced no new
    //    lines, so what it actually did was unrecoverable. The memory is
    //    cleared whenever a setting that decides a verdict changes.
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
        float glowChroma;
        float glowLuma;
        float brightness;
        uint32_t source;
        uint32_t enabled;
      } settings = { DusklightEmissive::glowChroma(), DusklightEmissive::glowLuma(),
                     DusklightEmissive::brightness(),
                     static_cast<uint32_t>(DusklightEmissive::colorSource()),
                     DusklightEmissive::enable() ? 1u : 0u };
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

      // A candidate with no colour at all cannot become an emitter under any
      // setting, so enumerating it teaches nothing. Counted instead, and
      // reported on each doubling - bounded at roughly log2(N) lines while
      // still showing the final magnitude.
      // Rejected for having no colour at all: overwhelmingly UI, screen copies
      // and plain geometry, and no setting can flip them.
      if (!accepted && chromaOf(color) < 0.05f) {
        ++s_colourless;
        if (s_colourless >= s_colourlessNextReport) {
          Logger::info(str::format("dusklight.emis.grey distinct=", s_colourless,
                                   " - colourless candidates, not enumerated"));
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
        " selfLit=", selfLit(mat) ? 1 : 0,
        " authored=", authoredColor(mat) ? 1 : 0,
        // The ramp decides what the reconstructed albedo is, so it decides what
        // a ReconstructedAlbedo emitter glows. Printed here because pairing two
        // logs by hand to answer "did the ramp reach this surface" is exactly
        // the step that gets skipped.
        " ramp=", legacy.Diffuse.a >= 0.5f ? 1 : 0,
        " rampOther=", std::hex, dusklightRamp::otherColor(mat),
        " tFactor=", mat.tFactor, std::dec,
        " glowChroma=", DusklightEmissive::glowChroma(),
        " glowLuma=", DusklightEmissive::glowLuma(),
        // What this surface will actually emit at, after the per-material
        // derivation - so "why is that one dimmer" is answered by the log.
        " pickup=", isPickup(mat),
        " radiance=", radianceFor(color, isPickup(mat)),
        " src=", sourceName(DusklightEmissive::colorSource()),
        " verdict=", accepted ? "emissive" : "rejected",
        " applied=", (accepted && DusklightEmissive::enable()) ? 1 : 0));
    }

  } // namespace dusklightEmissive

} // namespace dxvk
