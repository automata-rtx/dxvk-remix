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

// Dusklight water.
//
// Twilight Princess draws water as several stacked materials rather than one:
// dKy_bg_MAxx_proc (dusklight-ao d_kankyo.cpp) dispatches on the J3D material
// name and gives MA06 the murky body, MA09 the shine surface, MA02/MA10 a
// projected reflection layer, and MA00/01/04/16 the water-in fog. None of that
// reaches Remix on its own, so every one of those layers fell through
// determineMaterialData's last line to as<OpaqueMaterialData>() and became a
// rough white-ish dielectric. Stacked and alpha blended, that is the milky
// sheet.
//
// Two things make it white rather than merely wrong. The layers' albedo is a
// pure texture pass-through, and several of them sample an EFB copy that aurora
// could not produce - a depth copy, or one whose StretchRect failed - for which
// it substitutes a 1x1 texture of 0x00FFFFFF (aurora-ao dx9_texture.cpp,
// create_placeholder). White is neutral for a modulate consumer, which is what
// that placeholder was chosen for, and maximally wrong for a pass-through.
//
// So water is not fixed by correcting its albedo. It is fixed by not being an
// opaque material at all: a translucent material's look comes from its
// transmittance and index of refraction, and the albedo those layers could not
// supply stops being consulted.
//
// The game marks its own water draws by material name and carries the fact
// per-draw through the D3DMATERIAL9 side channel, which is section 1's
// "translate, don't tag" - a texture hash list would be wrong the moment the
// game reused a water texture elsewhere, and this game reuses textures
// constantly. Marking by name also survives the water moving: dungeon water
// levels rise and fall, and the material's identity does not change when they
// do.
//
// Every constant here is an option rather than a literal. None of them can be
// derived - the right transmittance distance depends on the game's unit scale
// against a path traced image - so they need to be tunable while the game runs.

#include "rtx_option.h"
#include "rtx_materials.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <unordered_set>

namespace dxvk {

  struct DusklightWater {
    friend class ImGUI;

    RTX_OPTION("rtx.dusklight.water", bool, enable, true,
               "Treat draws the game marks as water as a translucent material rather than an opaque one.\n"
               "Off returns water to the legacy opaque fallback, which is where the milky white sheet comes from - "
               "useful only for comparison. Requires a game build that marks its water; a game that does not simply "
               "never sets the flag and nothing here applies.");

    RTX_OPTION("rtx.dusklight.water", float, refractiveIndex, 1.33f,
               "Index of refraction for water. 1.33 is water's measured value at visible wavelengths.\n"
               "Remix clamps translucent IoR to the range 1 to 3.");

    RTX_OPTION("rtx.dusklight.water", Vector3, transmittanceColor, Vector3(0.42f, 0.70f, 0.75f),
               "What white light becomes after travelling transmittanceMeasurementDistance through the water.\n"
               "This is the whole of water's colour under a path tracer: shallow water shows almost none of it and "
               "deep water shows all of it, which is the depth tinting a rasterizer had to fake. Needs tuning against "
               "the image together with the distance below - the pair is what sets how quickly water reads as deep.");

    RTX_OPTION("rtx.dusklight.water", float, transmittanceMeasurementDistance, 200.0f,
               "The distance, in the game's units, over which transmittanceColor is reached.\n"
               "Unverified against this game's scale: it is a starting value, not a measurement. Smaller makes water "
               "reach its full colour sooner and so look murkier; larger makes it clearer. Tune this before "
               "transmittanceColor - most of what reads as wrong is usually this rather than the colour.");

    RTX_OPTION("rtx.dusklight.water", bool, thinWalled, false,
               "Treat water surfaces as a thin sheet rather than the boundary of a volume.\n"
               "Off, because water is a volume and the depth tinting above depends on light travelling through it. On "
               "is for surfaces that really are a film, and makes the transmittance distance nearly irrelevant.");

    RTX_OPTION("rtx.dusklight.water", float, thinWallThickness, 1.0f,
               "Sheet thickness used when thinWalled is on. Ignored otherwise.");

    RTX_OPTION("rtx.dusklight.water", bool, animateTexcoords, true,
               "Drive the water surface's texture coordinates from uvTiling and scrollSpeed below,\n"
               "in place of the texture transform the draw arrived with.\n"
               "The game's own scroll is authored for a 640x480 rasterizer at the game's own scale, "
               "and there is no reason a path traced lake has to inherit either. Off restores the "
               "draw's transform exactly as it came.");

    RTX_OPTION("rtx.dusklight.water", float, uvTiling, 1.0f,
               "How many times the water texture repeats across the surface's own UV range.\n"
               "1 is the game's mapping. Large bodies of water usually want more than that - a "
               "ripple texture stretched once across Lake Hylia reads as a smear rather than as "
               "water. This is a look control with no correct value; find one that reads naturally "
               "at the scale you are standing at.");

    RTX_OPTION("rtx.dusklight.water", Vector2, scrollSpeed, Vector2(0.02f, 0.013f),
               "Water texture scroll, in UV units per second, before uvTiling is applied.\n"
               "The two components deliberately differ so the pattern does not travel along a "
               "diagonal. Zero on both stops the animation without disabling animateTexcoords.");

    // Which layers of a body of water to drop. A lake is drawn as several stacked
    // surfaces, and stacking refracting interfaces is not what water is - nor do
    // overlapping normal maps blend correctly in Remix, so a lake wants ONE moving
    // surface.
    //
    // These are the game's own words for its passes. Twilight Princess is a Japanese
    // production and the decompilation preserves its naming, so "nami" (waves),
    // "mizugiwa" (the water's edge), "nigori" (murk) and "mera" (shimmer) are the
    // developers' labels, not a scheme invented here. An earlier revision cut on the
    // MAxx tag instead and was wrong: three of those four are all MA06, so hiding that
    // tag would have deleted a lake's waves and shoreline to be rid of its murk.
    //
    // All default off. Which layer should survive is a look decision, and dropping a
    // surface nobody asked to drop is the worse failure.
    RTX_OPTION("rtx.dusklight.water", bool, hideShimmerLayer, false,
               "Hide the water's shimmer pass (the game calls it 'mera').");

    RTX_OPTION("rtx.dusklight.water", bool, hideWavesLayer, false,
               "Hide the water's wave pass (the game calls it 'nami').");

    RTX_OPTION("rtx.dusklight.water", bool, hideShorelineLayer, false,
               "Hide the water's edge pass, where it meets the shore (the game calls it 'mizugiwa').");

    RTX_OPTION("rtx.dusklight.water", bool, hideMurkLayer, false,
               "Hide the murky body of the water (the game calls it 'nigori').");

    RTX_OPTION("rtx.dusklight.water", bool, hideAdditiveLayer, false,
               "Hide additively blended water passes (the game calls them 'kasan', which is\n"
               "Japanese for addition - and every material carrying it measured SRC_ALPHA,ONE).\n"
               "An additive pass is light over a surface rather than a surface, so it is a\n"
               "candidate for dropping when a lake has too many interfaces. Note fountains draw\n"
               "an additive pass too, so this is not lake-only.");

    RTX_OPTION("rtx.dusklight.water", bool, shorelineAsBlend, true,
               "Leave the water's edge pass ('mizugiwa') as the alpha-blended overlay it is,\n"
               "rather than making it a refracting surface.\n"
               "A translucent material in Remix has NO partial coverage - its only opacity term is\n"
               "diffuseOpacity, which feeds the diffuse layer this code disables\n"
               "(translucent_surface_material_interaction.slangh). So the moment a draw becomes\n"
               "translucent, the alpha that feathered it into the shore is gone and the boundary\n"
               "becomes a hard glass edge. That pass exists precisely to soften that boundary, so\n"
               "converting it destroys the one thing it does. Off makes it refract like the rest.");

    RTX_OPTION("rtx.dusklight.water", bool, applyToReplacements, true,
               "Keep water translucent even where a replacement material was authored for it.\n"
               "A capture cannot express water: GameCapturer::captureMaterial writes an albedo\n"
               "texture path and nothing else, so every water draw captures as an OPAQUE material\n"
               "and anything authored from that capture stays opaque unless its type is changed by\n"
               "hand. Replaced and unreplaced draws on the same lake then render as two different\n"
               "kinds of surface, which is what large chunks with hard cutoffs look like.\n"
               "With this on, an opaque replacement on a water draw keeps its authored normal map\n"
               "and gets the water treatment around it. A replacement that is ALREADY translucent\n"
               "is left completely alone - that author meant it.");

    RTX_OPTION("rtx.dusklight.water", bool, hideProjectedLayer, true,
               "Drop the game's camera-projected water overlay (MA02/MA10).\n"
               "Twilight Princess paints a fake reflection over its water using a perspective\n"
               "matrix built from the live camera. Remix traces that reflection for real off the\n"
               "water surface, so the painted one is redundant - and left in the scene it is a\n"
               "second refracting interface a few units above the first, carrying a screen-space\n"
               "image, which is what stops water reading as one continuous surface.\n"
               "Turn off to see the layer again; the same reasoning retired the blob shadows.");

    RTX_OPTION("rtx.dusklight.water", bool, log, true,
               "Log one line per distinct water material the game marks.\n"
               "This is what tells apart 'the game is not marking water' from 'water is marked and still looks wrong', "
               "which read identically in a picture. Bounded at 64 distinct materials.");

    static constexpr size_t kMaxLoggedMaterials = 64;
  };

  namespace dusklightWater {

    // The game's own answer, carried per draw. Aurora ships it in an otherwise
    // unused component of the D3DMATERIAL9 side channel it already uses for
    // self-illumination and the two-colour ramp; the field map lives in
    // aurora-ao lib/dx9/dx9_internal.hpp, set_remix_material.
    //
    // Note this is deliberately not a texture hash test. The same water texture
    // appears on non-water draws in this game, and the same water appears with
    // different textures as the level's water rises.
    inline bool isWater(const LegacyMaterialData& mat) {
      return DusklightWater::enable() && mat.getLegacyMaterial().Ambient.g >= 0.5f;
    }

    // The camera-projected overlay drawn over a water surface - MA02/MA10, which
    // dKy_bg_MAxx_proc (d_kankyo.cpp:11479) hands a C_MTXLightPerspective built from the
    // live camera fovy and aspect. Not the surface, and not something to refract through.
    inline bool isProjectedOverlay(const LegacyMaterialData& mat) {
      return DusklightWater::enable() && mat.getLegacyMaterial().Ambient.b >= 0.5f;
    }

    // Which MAxx tag this draw's material carried, or 0. Aurora ships the number
    // itself rather than an index, so 9 means MA09 and the log reads the way the
    // game's own material names do.
    inline uint32_t waterTag(const LegacyMaterialData& mat) {
      return static_cast<uint32_t>(mat.getLegacyMaterial().Ambient.a + 0.5f);
    }

    // Which layer of a body of water this draw is, classified game-side from the
    // material's name. Values are GX_AURORA_DUSKLIGHT_WATER_LAYER_* (aurora-ao
    // include/dolphin/gx/GXAurora.h), carried in D3DMATERIAL9::Power - the last
    // unused field of the side channel.
    enum class WaterLayer : uint32_t {
      Unknown = 0, Shimmer = 1, Waves = 2, Shoreline = 3,
      Murk = 4, Fountain = 5, Additive = 6, Indirect = 7,
    };

    inline WaterLayer waterLayer(const LegacyMaterialData& mat) {
      return static_cast<WaterLayer>(
        static_cast<uint32_t>(mat.getLegacyMaterial().Power + 0.5f));
    }

    // Whether this draw should keep its alpha blend rather than become a refracting
    // surface. See shorelineAsBlend: a translucent material has no partial coverage, so
    // a pass whose job is feathering an edge has nothing left to do once converted.
    inline bool keepAsBlendedOverlay(const LegacyMaterialData& mat);

    inline const char* waterLayerName(WaterLayer layer) {
      switch (layer) {
      case WaterLayer::Shimmer:   return "shimmer";
      case WaterLayer::Waves:     return "waves";
      case WaterLayer::Shoreline: return "shoreline";
      case WaterLayer::Murk:      return "murk";
      case WaterLayer::Fountain:  return "fountain";
      case WaterLayer::Additive:  return "additive";
      case WaterLayer::Indirect:  return "indirect";
      default:                    return "unknown";
      }
    }

    inline bool keepAsBlendedOverlay(const LegacyMaterialData& mat) {
      return DusklightWater::shorelineAsBlend() && waterLayer(mat) == WaterLayer::Shoreline;
    }

    // The dimensions of the texture Remix actually received, or 0x0. Reported because
    // "the wave texture looks low resolution" is otherwise unanswerable from a log - and
    // the answer matters: aurora uploads GX textures at their native size with the full
    // mip chain (dx9_texture.cpp create_from_rgba8, obj.width()/height()/mip_count()), so
    // a small number here is the game's own texture, not something lost in translation.
    inline void textureExtent(const LegacyMaterialData& mat, uint32_t& width, uint32_t& height) {
      width = 0;
      height = 0;
      const DxvkImageView* view = mat.getColorTexture().getImageView();
      if (view != nullptr && view->image().ptr() != nullptr) {
        width = view->image()->info().extent.width;
        height = view->image()->info().extent.height;
      }
    }

    // An unclassified layer is never hidden. A name nobody has taught the classifier
    // stays visible, which is the failure worth having.
    inline bool isLayerHidden(WaterLayer layer) {
      switch (layer) {
      case WaterLayer::Shimmer:   return DusklightWater::hideShimmerLayer();
      case WaterLayer::Waves:     return DusklightWater::hideWavesLayer();
      case WaterLayer::Shoreline: return DusklightWater::hideShorelineLayer();
      case WaterLayer::Murk:      return DusklightWater::hideMurkLayer();
      case WaterLayer::Additive:  return DusklightWater::hideAdditiveLayer();
      default:                    return false;
      }
    }

    // Water as a translucent material.
    //
    // The albedo and transmittance textures are deliberately absent. A translucent
    // material's colour is its transmittance, and the game's water textures are the
    // layers that arrive white - a pure texture pass-through over an EFB copy aurora
    // could not produce. Leaving them out is what stopped the milky sheet.
    //
    // No normal map either. A revision put the draw's own colour texture in that slot
    // to stop water reading as featureless glass; it was reverted on 2026-08-09 because
    // a colour texture decoded as a tangent normal is noise, and because it put a second
    // normal map on lakes that already had one authored - which Remix does not blend.
    // Surface detail comes from an authored replacement, and from nothing else.
    //
    // A replacement authored against the draw's texture hash wins over all of this,
    // because getReplacementMaterial is consulted before this is ever reached.
    inline TranslucentMaterialData makeMaterial(const LegacyMaterialData& mat) {
      TranslucentMaterialData water;

      // The game's own sampler, so wrap and filter match what the draw asked for.
      // This matters more now than it did: a scrolling UV runs off the end of the
      // 0-1 range every cycle, and only the game's wrap mode makes that tile.
      if (mat.getSampler().ptr()) {
        water.setSamplerOverride(mat.getSampler());
        water.getFilterMode() = lss::Mdl::Filter::vkToMdl(mat.getSampler()->info().magFilter);
        water.getWrapModeU() = lss::Mdl::WrapMode::vkToMdl(mat.getSampler()->info().addressModeU);
        water.getWrapModeV() = lss::Mdl::WrapMode::vkToMdl(mat.getSampler()->info().addressModeV);
      }

      water.setRefractiveIndex(DusklightWater::refractiveIndex());
      water.setTransmittanceColor(DusklightWater::transmittanceColor());
      water.setTransmittanceMeasurementDistance(DusklightWater::transmittanceMeasurementDistance());
      water.setEnableThinWalled(DusklightWater::thinWalled());
      water.setThinWallThickness(DusklightWater::thinWallThickness());

      // The diffuse layer is a painted-on opaque coat over the surface. It is
      // the one switch here that would put the milky sheet straight back.
      water.setEnableDiffuseLayer(false);
      water.setEnableEmission(false);

      return water;
    }

    // True the first time this material is seen. Bounded, and says so once when
    // it stops reporting, so a truncated log is never mistaken for a short one.
    inline bool shouldLogOnce(std::unordered_set<XXH64_hash_t>& seen, bool& truncated,
                              const char* truncLine, XXH64_hash_t textureHash) {
      if (!DusklightWater::log() || seen.count(textureHash) != 0) {
        return false;
      }
      if (seen.size() >= DusklightWater::kMaxLoggedMaterials) {
        if (!truncated) {
          truncated = true;
          Logger::info(str::format(truncLine, " cap=", DusklightWater::kMaxLoggedMaterials,
                                   " - further distinct water materials not reported"));
        }
        return false;
      }
      seen.insert(textureHash);
      return true;
    }

    inline bool shouldLog(XXH64_hash_t textureHash) {
      static std::unordered_set<XXH64_hash_t> s_seen;
      static bool s_truncated = false;
      return shouldLogOnce(s_seen, s_truncated, "dusklight.water.trunc", textureHash);
    }

    // Counted separately: a water draw whose material a replacement already
    // claimed. That is intended - authoring a replacement against the ripple
    // layers' hashes is how a real normal map gets onto the surface - but it is
    // silent, and silence here reads exactly like the mark never arriving. The
    // 2026-08-08 22:38 session saw "some blue translucency" on water with zero
    // dusklight.water lines, and this is the line that would have said which of
    // the two it was.
    inline bool shouldLogReplaced(XXH64_hash_t textureHash) {
      static std::unordered_set<XXH64_hash_t> s_seen;
      static bool s_truncated = false;
      return shouldLogOnce(s_seen, s_truncated, "dusklight.water.replaced.trunc", textureHash);
    }

    // The authored normal map out of an opaque replacement, if it has one. That is the
    // one thing worth keeping when a water draw's replacement is coerced back to water:
    // its albedo is what makes water white, and its roughness/metallic mean nothing on a
    // refracting surface, but the normal map is the ripple detail someone drew by hand.
    inline TranslucentMaterialData makeMaterialFromReplacement(const LegacyMaterialData& mat,
                                                               const MaterialData& replacement) {
      TranslucentMaterialData water = makeMaterial(mat);

      if (replacement.getType() == MaterialDataType::Opaque) {
        const TextureRef& authoredNormal = replacement.getOpaqueMaterialData().getNormalTexture();
        if (authoredNormal.isValid() && !authoredNormal.isImageEmpty()) {
          water.getNormalTexture() = authoredNormal;
        }
      }

      return water;
    }

    inline const char* materialTypeName(MaterialDataType type) {
      switch (type) {
      case MaterialDataType::Opaque:      return "opaque";
      case MaterialDataType::Translucent: return "translucent";
      case MaterialDataType::RayPortal:   return "rayportal";
      default:                            return "unknown";
      }
    }

    inline bool shouldLogProjected(XXH64_hash_t textureHash) {
      static std::unordered_set<XXH64_hash_t> s_seen;
      static bool s_truncated = false;
      return shouldLogOnce(s_seen, s_truncated, "dusklight.water.projected.trunc", textureHash);
    }

  } // namespace dusklightWater

} // namespace dxvk
