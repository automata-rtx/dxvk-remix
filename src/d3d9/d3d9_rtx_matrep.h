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

// Dusklight material translation report, Remix half.
//
// Aurora encodes a GameCube material into D3D9 fixed-function stages; this
// runtime reconstructs a PBR material from ONE of those stages. Neither side
// could previously observe what the other made of it, which is why a colour
// defect took three sessions and one wrong shipped fix to locate.
//
// This prints what the reconstruction actually produced, including a literal
// rendering of the albedo expression the shader will evaluate. Pair it with
// aurora's matrep.* lines; the join key is the texture pointer.
//
// Full format, worked examples and the reading procedure:
//   aurora-ao/docs/dx9/material-report.md
// Why the interface behaves this way:
//   aurora-ao/docs/dx9/remix-material-interface.md

#include "../dxvk/rtx_render/rtx_option.h"
#include "../dxvk/rtx_render/rtx_types.h"
#include "../dxvk/rtx_render/rtx_options.h"
#include "../dxvk/rtx_render/rtx_dusklight_env.h"
#include "../dxvk/rtx_render/rtx_dusklight_game.h"
#include "../util/util_string.h"

#include <string>
#include <unordered_set>

namespace dxvk {

  struct DusklightMatrep {
    RTX_OPTION_FLAG("rtx.dusklight", bool, matrep, false, RtxOptionFlags::NoSave,
                    "Log one material translation report line per distinct reconstructed material.\n"
                    "Pairs with aurora's matrep.* lines to show what a GameCube material became on the way through "
                    "D3D9 fixed-function. Off by default; NoSave so it never persists into a config. "
                    "Bounded at 1024 distinct materials per run.");
    // Deliberately NOT aurora's cap, which is 512 (kMatrepMaxMaterials, dx9_internal.hpp).
    // Corrected 2026-08-16: the comment here used to claim the two matched, which has never been
    // true - and matching integers would not have made the two halves truncate together anyway,
    // because they count different populations. Aurora caps distinct *material identities*; this
    // half caps distinct reconstruction *shapes* (shapeKey below), which excludes tFactor's value
    // precisely because including it burned 1024 entries on a few dozen materials in 14 seconds
    // (d3d9_rtx.cpp, the note above the shapeKey call). Each half prints its own .trunc line
    // naming its own cap, so a truncated log always says which side truncated and at what.
    static constexpr size_t kMaxMaterials = 1024;
  };

  namespace matrep {

    inline const char* argName(RtTextureArgSource s) {
      switch (s) {
      case RtTextureArgSource::None:         return "1.0";
      case RtTextureArgSource::Texture:      return "TEX";
      case RtTextureArgSource::VertexColor0: return "VertexColor0";
      case RtTextureArgSource::TFactor:      return "tFactor";
      default:                               return "?";
      }
    }

    // Compact blend state. Here because an **additive draw is the one thing in
    // the whole GX stream that unambiguously means "add light"** - GX_BM_BLEND
    // with GX_BL_ONE/GX_BL_ONE reaches Remix as BlendType::kEmissive, and the
    // emissive-blend override in rtx_instance_manager.cpp claims that draw one
    // branch *before* the Dusklight rule is consulted. Neither log carried the
    // blend state, so whether any of this game's draws take that path has never
    // been checked - which is why the emissive work went looking in TEV state.
    enum class BlendClass : uint8_t {
      Off = 0,
      Alpha,
      Additive,       // ONE/ONE               -> kEmissive
      AdditiveAlpha,  // SRC_ALPHA/ONE         -> kAlphaEmissive
      Multiply,
      Subtract,
      Other,
    };

    inline BlendClass blendClass(const DxvkBlendMode& b) {
      if (!b.enableBlending) {
        return BlendClass::Off;
      }
      if (b.colorBlendOp == VK_BLEND_OP_REVERSE_SUBTRACT) {
        return BlendClass::Subtract;
      }
      if (b.colorSrcFactor == VK_BLEND_FACTOR_DST_COLOR && b.colorDstFactor == VK_BLEND_FACTOR_ZERO) {
        return BlendClass::Multiply;
      }
      if (b.colorBlendOp == VK_BLEND_OP_ADD) {
        if (b.colorSrcFactor == VK_BLEND_FACTOR_ONE && b.colorDstFactor == VK_BLEND_FACTOR_ONE) {
          return BlendClass::Additive;
        }
        if (b.colorSrcFactor == VK_BLEND_FACTOR_SRC_ALPHA && b.colorDstFactor == VK_BLEND_FACTOR_ONE) {
          return BlendClass::AdditiveAlpha;
        }
        if (b.colorSrcFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
            b.colorDstFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) {
          return BlendClass::Alpha;
        }
      }
      return BlendClass::Other;
    }

    inline const char* blendName(const DxvkBlendMode& b) {
      switch (blendClass(b)) {
      case BlendClass::Off:           return "off";
      case BlendClass::Alpha:         return "alpha";
      case BlendClass::Additive:      return "additive";
      case BlendClass::AdditiveAlpha: return "additiveAlpha";
      case BlendClass::Multiply:      return "multiply";
      case BlendClass::Subtract:      return "subtract";
      default:                        return "other";
      }
    }

    // How the stage generated and transformed its texture coordinates.
    //
    // Here because this game's water is not one material but three stacked draws, and two
    // of them are decided entirely by this state. dKy_bg_MAxx_proc (dusklight-ao
    // d_kankyo.cpp) dispatches on the J3D material *name*: MA06 is the murky body, MA09
    // the shine surface, and MA02/MA10 a reflection layer whose texture matrix is rebuilt
    // every frame from the camera FOV via C_MTXLightPerspective, i.e. a projective texgen.
    //
    // Remix drops exactly that. d3d9_rtx_utils.cpp ignores D3DTTFF_PROJECTED and clamps any
    // element count past 2, in both cases with a ONCE() info log and a // Todo. Camera-space
    // reflection and spheremap TCI collapse to TexGenMode::None the same way. All of those
    // are silent per-draw, so a water layer arriving with quietly wrong UVs has been
    // indistinguishable from one that translated cleanly. These fields are what tells them
    // apart in a log rather than in pixels.
    enum class TciClass : uint8_t {
      PassThru = 0,
      CameraSpacePosition,
      CameraSpaceNormal,
      CameraSpaceReflectionVector,  // collapses to TexGenMode::None
      SphereMap,                    // collapses to TexGenMode::None
      Unknown,
    };

    inline const char* tciName(TciClass c) {
      switch (c) {
      case TciClass::PassThru:                    return "PassThru";
      case TciClass::CameraSpacePosition:         return "CameraSpacePosition";
      case TciClass::CameraSpaceNormal:           return "CameraSpaceNormal";
      case TciClass::CameraSpaceReflectionVector: return "CameraSpaceReflection(dropped)";
      case TciClass::SphereMap:                   return "SphereMap(dropped)";
      default:                                    return "?";
      }
    }

    inline const char* texgenName(TexGenMode m) {
      switch (m) {
      case TexGenMode::None:                 return "None";
      case TexGenMode::ViewPositions:        return "ViewPositions";
      case TexGenMode::CascadedViewPositions:return "CascadedViewPositions";
      case TexGenMode::ViewNormals:          return "ViewNormals";
      default:                               return "?";
      }
    }

    // Texture transform / texgen state for the stage the material was reconstructed from.
    // Not carried on LegacyMaterialData, so it travels alongside it. The caller fills this
    // from d3d9State with an explicit switch rather than arithmetic on the D3DTSS_TCI_*
    // encoding, so it stays correct if those values ever move.
    struct StageXform {
      // D3DTTFF element count: 0 when the transform is disabled, otherwise 1-4. Anything
      // above 2 is clamped by Remix and the extra elements never reach the shader.
      uint8_t elementCount = 0;
      // D3DTTFF_PROJECTED was requested. Remix does not implement the projective divide,
      // so when this is 1 the stage's UVs are wrong by construction.
      uint8_t projected = 0;
      TciClass tci = TciClass::PassThru;
      TexGenMode texgen = TexGenMode::None;
      // The divisor row itself, straight from the game's matrix. Printed for projected
      // stages because the encoding that carries it is lossy in exactly one respect - it
      // drops the w coefficient by dividing through - and a log that only says "projected"
      // cannot distinguish an encoder producing (0, 0, -1, 0) from one producing rubbish.
      // Raw rather than encoded: the encoding is deterministic from this, and this is the
      // thing that can be checked against what C_MTXLightPerspective ought to have built.
      float divisorRow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // Empty for a stage that is not projected, so the field costs nothing on the vast
    // majority of lines.
    inline std::string divisorRowText(const StageXform& x) {
      if (x.projected == 0) {
        return std::string();
      }
      return str::format(" projRow=(", x.divisorRow[0], ",", x.divisorRow[1], ",",
                         x.divisorRow[2], ",", x.divisorRow[3], ")");
    }

    inline const char* opName(DxvkRtTextureOperation op) {
      switch (op) {
      case DxvkRtTextureOperation::Disable:          return "Disable";
      case DxvkRtTextureOperation::SelectArg1:       return "SelectArg1";
      case DxvkRtTextureOperation::SelectArg2:       return "SelectArg2";
      case DxvkRtTextureOperation::Modulate:         return "Modulate";
      case DxvkRtTextureOperation::Modulate2x:       return "Modulate2x";
      case DxvkRtTextureOperation::Modulate4x:       return "Modulate4x";
      case DxvkRtTextureOperation::Add:              return "Add";
      case DxvkRtTextureOperation::Force_Modulate2x: return "Force_Modulate2x";
      default:                                       return "?";
      }
    }

    // The albedo expression this material will evaluate, written out. This is
    // the line that ends an argument about where a colour went: a material
    // whose colour was dropped reads "TEX * 1.0", and one whose tint survived
    // reads "TEX * tFactor(FF2200)".
    inline std::string albedoExpression(const LegacyMaterialData& m) {
      const char* a1 = argName(m.textureColorArg1Source);
      const char* a2 = argName(m.textureColorArg2Source);
      std::string expr;
      switch (m.textureColorOperation) {
      case DxvkRtTextureOperation::Disable:    expr = "<disabled>"; break;
      case DxvkRtTextureOperation::SelectArg1: expr = a1; break;
      case DxvkRtTextureOperation::SelectArg2: expr = a2; break;
      case DxvkRtTextureOperation::Add:        expr = str::format(a1, " + ", a2); break;
      case DxvkRtTextureOperation::Modulate2x: expr = str::format(a1, " * ", a2, " * 2"); break;
      case DxvkRtTextureOperation::Modulate4x: expr = str::format(a1, " * ", a2, " * 4"); break;
      default:                                 expr = str::format(a1, " * ", a2); break;
      }
      // The extra multi-stage TFACTOR modulate, which is a separate mechanism
      // from an arg source and is easy to miss when reading the two apart.
      if (m.isTextureFactorBlend) {
        expr += str::format(" * tFactor(", std::hex, m.tFactor & 0x00FFFFFF, std::dec, ")");
      }
      return expr;
    }

    // Identity of a material's *reconstruction shape*: which texture, which ops
    // and which argument sources. Deliberately excludes tFactor's value, which
    // varies per draw with the game's fog and time of day and would otherwise
    // report the same material hundreds of times.
    //
    // Still distinguishes one texture used in several contexts, because the ops
    // and arg sources differ there - which is the case the report exists for.
    inline XXH64_hash_t shapeKey(const LegacyMaterialData& m, const StageXform& x) {
      // Zero-initialised, and laid out so there is no padding at all: hashing a
      // struct with indeterminate padding bytes would key on uninitialised
      // memory. Two 8-byte hashes plus sixteen 1-byte fields fill exactly 32.
      // The explicit tail pad is part of that count and is zero-initialised, so
      // it is deterministic - the version without it was 24 bytes and adding a
      // ninth flag silently reintroduced implicit padding, which is what the
      // assert catches.
      struct Shape {
        XXH64_hash_t tex0;
        XXH64_hash_t tex1;
        uint8_t colorOp, colorArg1, colorArg2;
        uint8_t alphaOp, alphaArg1, alphaArg2;
        uint8_t tfBlend, vcBaked;
        // Included because the same texture drawn opaque and drawn additively
        // are different materials to Remix - the second one is claimed by the
        // emissive-blend override - and collapsing them would hide exactly the
        // draw this field was added to find.
        uint8_t blend;
        // Texcoord generation and transform. Keyed on, not just printed, because
        // one water texture is drawn by several MAxx layers that differ *only*
        // here - the MA02/MA10 reflection layer is the same texture as the
        // surface with a projective matrix on top. Collapsing them would report
        // one material and hide the layer whose UVs Remix drops.
        uint8_t xformCount, projected, tci, texgen;
        // The MA00/MA01/MA04/MA16 fog materials swap their alpha compare and Z
        // mode outright when the camera enters water (dKy_bg_MAxx_proc calls
        // mat_p->change()), so the same texture legitimately arrives as two
        // different materials. Both states are bounded, so this cannot multiply
        // the report the way a continuous value like tFactor would.
        //
        // atOp is forced to zero while the test is disabled. A disabled compare
        // decides nothing, and keying on it split one material into two lines
        // that printed identically - which is worse than not reporting it, since
        // the reader has no way to see why the two differ.
        uint8_t atEnabled, atOp;
        uint8_t pad[1];
      };
      static_assert(sizeof(Shape) == 2 * sizeof(XXH64_hash_t) + 16,
                    "Shape must have no implicit padding: it is hashed byte-wise");
      Shape shape {};
      shape.tex0 = m.getColorTexture().getImageHash();
      shape.tex1 = m.getColorTexture2().getImageHash();
      shape.colorOp = static_cast<uint8_t>(m.textureColorOperation);
      shape.colorArg1 = static_cast<uint8_t>(m.textureColorArg1Source);
      shape.colorArg2 = static_cast<uint8_t>(m.textureColorArg2Source);
      shape.alphaOp = static_cast<uint8_t>(m.textureAlphaOperation);
      shape.alphaArg1 = static_cast<uint8_t>(m.textureAlphaArg1Source);
      shape.alphaArg2 = static_cast<uint8_t>(m.textureAlphaArg2Source);
      shape.tfBlend = m.isTextureFactorBlend ? 1u : 0u;
      shape.vcBaked = m.isVertexColorBakedLighting ? 1u : 0u;
      shape.blend = static_cast<uint8_t>(blendClass(m.blendMode));
      shape.xformCount = x.elementCount;
      shape.projected = x.projected;
      shape.tci = static_cast<uint8_t>(x.tci);
      shape.texgen = static_cast<uint8_t>(x.texgen);
      shape.atEnabled = m.alphaTestEnabled ? 1u : 0u;
      shape.atOp = m.alphaTestEnabled ? static_cast<uint8_t>(m.alphaTestCompareOp) : 0u;
      return XXH3_64bits(&shape, sizeof(shape));
    }

    // Everything a reader would otherwise have to ask the owner about the run itself.
    //
    // A material report is only interpretable if you know how the runtime was configured
    // while it was written, and every one of these has a plausible wrong value that makes
    // the log say something false rather than nothing: water suppressed by a category that
    // was never populated, a legacy fallback tuned differently, the game feed not connected
    // so the markers below never fire. Printed once, at the first reported material, so it
    // sits at the top of the run's report.
    inline void emitContext() {
      static bool s_emitted = false;
      if (s_emitted) {
        return;
      }
      s_emitted = true;

      Logger::info(str::format(
        "dusklight.ctx envFeed=", DusklightEnv::enable(),
        " protocol=", DusklightEnv::protocol(),
        // Whether the two built-in water paths would do anything at all. Both only ever
        // act on draws in the AnimatedWater category, so a non-empty texture list is what
        // decides whether either is reachable - and it is empty by default, which means
        // "the water paths are off" no matter what the two enables say.
        " layeredWaterNormal=", OpaqueMaterialOptions::layeredWaterNormalEnable(),
        " animatedWaterTranslucent=", TranslucentMaterialOptions::animatedWaterEnable(),
        " animatedWaterTextures=", RtxOptions::animatedWaterTextures().size(),
        // What an unmatched draw becomes. Any water layer that finds no replacement lands
        // on these numbers, so they are the milky look's actual parameters.
        " legacyRoughness=", LegacyMaterialDefaults::roughnessConstant(),
        " legacyMetallic=", LegacyMaterialDefaults::metallicConstant(),
        " legacyAlbedoTex=", LegacyMaterialDefaults::useAlbedoTextureIfPresent(),
        " hideDashEffect=", DusklightGame::hideDashEffect(),
        // Capability, not a setting. A "proj=1" line means something different either side
        // of the build that implemented the projective divide - before it, the projection
        // was detected and discarded - so a log has to say which it came from rather than
        // leaving the reader to date it from the filename.
        " projectiveTexcoords=1"));
    }

    // Where in the run something happened. Emitted only when a tracked state changes, so
    // this is a handful of lines per session rather than one per frame, and it lands
    // interleaved with the matrep.rmx lines in the same file - which is the whole point.
    // A new material shape appearing between a dash=1 and the following dash=0 is
    // attributable to the dash without correlating two logs by wall clock.
    //
    // The initial values are latched without emitting, so connecting to a game already in
    // one of these states does not report a transition that never happened.
    inline void emitMarkers() {
      static bool s_primed = false;
      static bool s_dash = false;
      static bool s_camInWater = false;

      const bool dash = DusklightEnv::dash();
      const bool camInWater = DusklightEnv::camInWater();

      if (!s_primed) {
        s_primed = true;
        s_dash = dash;
        s_camInWater = camInWater;
        return;
      }

      if (dash == s_dash && camInWater == s_camInWater) {
        return;
      }

      s_dash = dash;
      s_camInWater = camInWater;
      Logger::info(str::format("dusklight.mark dash=", dash, " camInWater=", camInWater));
    }

    // True the first time this shape is seen.
    inline bool shouldEmit(XXH64_hash_t key) {
      static std::unordered_set<XXH64_hash_t> s_seen;
      static bool s_truncated = false;
      if (s_seen.count(key) != 0) {
        return false;
      }
      if (s_seen.size() >= DusklightMatrep::kMaxMaterials) {
        if (!s_truncated) {
          s_truncated = true;
          Logger::info(str::format("matrep.trunc side=remix cap=", DusklightMatrep::kMaxMaterials,
                                   " - further distinct materials not reported"));
        }
        return false;
      }
      s_seen.insert(key);
      return true;
    }

  } // namespace matrep

} // namespace dxvk
