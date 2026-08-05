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
#include "../util/util_string.h"

#include <unordered_set>

namespace dxvk {

  struct DusklightMatrep {
    RTX_OPTION_FLAG("rtx.dusklight", bool, matrep, false, RtxOptionFlags::NoSave,
                    "Log one material translation report line per distinct reconstructed material.\n"
                    "Pairs with aurora's matrep.* lines to show what a GameCube material became on the way through "
                    "D3D9 fixed-function. Off by default; NoSave so it never persists into a config. "
                    "Bounded at 1024 distinct materials per run.");
    // Matches aurora's own cap so the two logs truncate at comparable points.
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
    inline XXH64_hash_t shapeKey(const LegacyMaterialData& m) {
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
        uint8_t pad[7];
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
      return XXH3_64bits(&shape, sizeof(shape));
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
