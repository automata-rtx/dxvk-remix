/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_dusklight_texrep.h"

#include "rtx_asset_replacer.h"
#include "rtx_material_data.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace dxvk {
  namespace dusklightTexRep {
    namespace {
      // Every substituted texture is held at full resolution. Rasterized draws produce no
      // sampler feedback at all, and a path-traced surface that was just substituted has no
      // feedback history for the new image either, so without an explicit request the streamer
      // has nothing to size it from. 16 covers 65536x65536; the request is clamped by the
      // texture's own mip count.
      constexpr uint32_t kFullMipRequest = 16;

      // Guards only the diagnostic state below. The substitution itself reads m_extMaterials,
      // which is written on the CS thread by CreateMaterial's lambda and read here on the same
      // thread; this mutex does not extend to that and is not claimed to.
      std::mutex s_statsMutex;
      Stats s_stats;
      Stats s_lastFrame;
      // Handles whose outcome is decided: either the texture became resident, or there is no
      // material for it and there never will be without the game creating one. Both are
      // terminal, and both must release the preserve path - a handle that is merely absent
      // would otherwise disable instance preservation for that draw forever, which is a silent
      // performance regression rather than a visible one. Bounded by the pack size.
      std::unordered_set<uint64_t> s_settled;
      std::unordered_set<uint64_t> s_resident;
      std::unordered_set<uint64_t> s_reportedMissing;

      uint32_t indexFromAmbientG(float ambientG) {
        // Aurora writes a small non-negative integer. Reject anything that is not one rather
        // than truncating garbage into a plausible handle - a NaN or a negative here means the
        // channel is carrying something else and the correct answer is "no replacement".
        if (!(ambientG >= 1.0f) || ambientG >= static_cast<float>(kIndexMask)) {
          return 0;
        }
        const float rounded = std::floor(ambientG + 0.5f);
        if (std::fabs(ambientG - rounded) > 0.001f) {
          return 0;
        }
        return static_cast<uint32_t>(rounded);
      }
    }

    uint64_t handleFromLegacyMaterial(const D3DMATERIAL9& material) {
      const uint32_t index = indexFromAmbientG(material.Ambient.g);
      return index == 0 ? 0ull : (kHandleBase | static_cast<uint64_t>(index));
    }

    uint64_t handleFromLegacyMaterial(const LegacyMaterialData& material) {
      return handleFromLegacyMaterial(material.getLegacyMaterial());
    }

    uint64_t handleForRasterStage(const D3DMATERIAL9& material, uint32_t stateSampler) {
      const uint64_t handle = handleFromLegacyMaterial(material);
      if (handle == 0) {
        return 0;
      }
      // Ambient.b names the stage Ambient.g describes. Anything that is not a small
      // non-negative integer means the channel is not carrying aurora's contract.
      const float stageF = material.Ambient.b;
      if (!(stageF >= 0.0f) || stageF > 15.0f) {
        return 0;
      }
      return static_cast<uint32_t>(stageF + 0.5f) == stateSampler ? handle : 0;
    }

    const TextureRef* resolveAlbedo(AssetReplacer* replacer, uint64_t handle, bool forRaster) {
      if (!DusklightTexRep::enable() || handle == 0 || replacer == nullptr) {
        return nullptr;
      }
      if (forRaster && !DusklightTexRep::applyToRaster()) {
        return nullptr;
      }

      {
        std::lock_guard lock { s_statsMutex };
        ++s_stats.handlesSeen;
      }

      const MaterialData* material =
        replacer->accessExternalMaterial(reinterpret_cast<remixapi_MaterialHandle>(handle));
      if (material == nullptr || material->getType() != MaterialDataType::Opaque) {
        std::lock_guard lock { s_statsMutex };
        ++s_stats.missing;
        s_settled.insert(handle);
        if (s_reportedMissing.size() < 32 && s_reportedMissing.insert(handle).second) {
          Logger::warn(str::format("texrep: no material registered for index ",
                                   static_cast<uint32_t>(handle & kIndexMask),
                                   " (the game did not create it, or creation failed)"));
        }
        return nullptr;
      }

      // getOpaqueMaterialData() is non-const on MaterialData; the external material is owned by
      // the replacer and we only read from it, so cast away the const of the lookup rather than
      // copying the whole MaterialData per draw.
      auto& opaque = const_cast<MaterialData*>(material)->getOpaqueMaterialData();
      TextureRef& albedo = opaque.getAlbedoOpacityTexture();

      if (DusklightTexRep::forceFullMips()) {
        albedo.tryRequestMips(kFullMipRequest);
      }

      // isValid() alone is not enough: a TextureRef whose load has not landed yet resolves to a
      // null image view, and binding that gives the shader the zero-cleared bindless dummy,
      // which renders black. Falling back to the game's own texture for a frame or two is the
      // strictly better failure.
      if (!albedo.isValid() || albedo.isImageEmpty()) {
        std::lock_guard lock { s_statsMutex };
        ++s_stats.pending;
        return nullptr;
      }

      {
        std::lock_guard lock { s_statsMutex };
        ++s_stats.applied;
        if (forRaster) {
          ++s_stats.appliedRaster;
        }
        s_resident.insert(handle);
        s_settled.insert(handle);
      }
      return &albedo;
    }

    CaptureAlbedo resolveAlbedoForCapture(AssetReplacer* replacer,
                                          const LegacyMaterialData& legacy,
                                          const TextureRef** textureOut) {
      if (textureOut != nullptr) {
        *textureOut = nullptr;
      }
      if (!DusklightTexRep::enable() || !DusklightTexRep::captureReplaced() || replacer == nullptr) {
        return CaptureAlbedo::NoReplacement;
      }
      const uint64_t handle = handleFromLegacyMaterial(legacy);
      if (handle == 0) {
        return CaptureAlbedo::NoReplacement;
      }

      const MaterialData* material =
        replacer->accessExternalMaterial(reinterpret_cast<remixapi_MaterialHandle>(handle));
      if (material == nullptr || material->getType() != MaterialDataType::Opaque) {
        return CaptureAlbedo::Missing;
      }

      auto& opaque = const_cast<MaterialData*>(material)->getOpaqueMaterialData();
      TextureRef& albedo = opaque.getAlbedoOpacityTexture();
      albedo.tryRequestMips(kFullMipRequest);

      if (!albedo.isValid() || albedo.isImageEmpty()) {
        return CaptureAlbedo::NotResident;
      }
      // m_currentMip_begin is the first mip level present in the streamed image, so anything other
      // than zero means the top of the chain is still missing. A TextureRef built straight from an
      // image view rather than a managed texture has no streaming state and is always complete.
      const Rc<ManagedTexture>& managed = albedo.getManagedTexture();
      if (managed.ptr() != nullptr && managed->m_currentMip_begin != 0) {
        return CaptureAlbedo::NotResident;
      }

      if (textureOut != nullptr) {
        *textureOut = &albedo;
      }
      return CaptureAlbedo::Substituted;
    }

    void applyAlbedo(AssetReplacer* replacer, const LegacyMaterialData& legacy, MaterialData& renderMaterialData) {
      if (renderMaterialData.getType() != MaterialDataType::Opaque) {
        return;
      }
      const uint64_t handle = handleFromLegacyMaterial(legacy);
      if (const TextureRef* replacement = resolveAlbedo(replacer, handle, false)) {
        renderMaterialData.getOpaqueMaterialData().setAlbedoOpacityTexture(*replacement);
      }
    }

    bool awaitingReplacement(const LegacyMaterialData& legacy) {
      if (!DusklightTexRep::enable()) {
        return false;
      }
      const uint64_t handle = handleFromLegacyMaterial(legacy);
      if (handle == 0) {
        return false;
      }
      std::lock_guard lock { s_statsMutex };
      // Unsettled means the material this instance latched used the game's texture and the
      // outcome could still change, so it has to be re-evaluated. Once the handle is settled -
      // resident, or known to have no material - this goes quiet and preservation resumes.
      return s_settled.find(handle) == s_settled.end();
    }

    const Stats& stats() {
      return s_lastFrame;
    }

    void onFrameEnd() {
      std::lock_guard lock { s_statsMutex };
      s_lastFrame = s_stats;
      s_stats = Stats {};
    }

    void reportIfRequested() {
      if (!DusklightTexRep::report()) {
        return;
      }
      DusklightTexRep::reportObject().setDeferred(false);

      Stats snapshot;
      size_t residentCount = 0;
      {
        std::lock_guard lock { s_statsMutex };
        snapshot = s_lastFrame;
        residentCount = s_resident.size();
      }
      Logger::info(str::format(
        "texrep.rmx enable=", DusklightTexRep::enable() ? 1 : 0,
        " applyToRaster=", DusklightTexRep::applyToRaster() ? 1 : 0,
        " forceFullMips=", DusklightTexRep::forceFullMips() ? 1 : 0,
        " handles=", snapshot.handlesSeen,
        " applied=", snapshot.applied,
        " raster=", snapshot.appliedRaster,
        " pending=", snapshot.pending,
        " missing=", snapshot.missing,
        " resident=", residentCount,
        "  (per frame, except resident which is cumulative)"));
    }
  }
}
