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

      // How many distinct missing indices get a line before the log goes quiet.
      //
      // Read this together with what it cannot do. The game creates its materials a few per
      // frame (dusklight-ao remix_bridge.cpp, kTexRepCreationsPerFrame), so a pack larger
      // than this cap will spend the whole cap on indices that are merely not created YET,
      // and nothing is re-armed afterwards - there is no pack-change hook in this runtime,
      // and the only fork-visible signal that a pack was rebuilt is the game-pushed readout
      // rtx.dusklight.env.texrepCreated going backwards, which nothing here reads. So these
      // lines are once per run, and a creation that genuinely failed later in that run can
      // go unnamed by them.
      //
      // That is why the warning below points at texrep.rmx rather than claiming to show
      // persistence itself: `missing=` there is a per-frame count with no cap, so a pack
      // index that is still missing long after loading shows up as a missing= that never
      // reaches zero, and `late=` counts the opposite outcome. rtx.dusklight.texrep.report.
      constexpr size_t kMaxReportedMissing = 32;

      // Guards only the diagnostic state below. The substitution itself reads m_extMaterials,
      // which is written on the CS thread by CreateMaterial's lambda and read here on the same
      // thread; this mutex does not extend to that and is not claimed to.
      std::mutex s_statsMutex;
      Stats s_stats;
      Stats s_lastFrame;
      // Handles whose outcome this code treats as decided, so that awaitingReplacement stops
      // holding the preserve path open for them. Two ways in, and they are not equally solid:
      //
      //   resident - the replacement texture loaded and was applied. Terminal, observed.
      //   missing  - accessExternalMaterial found no material on THIS call. Treated as
      //              terminal on the first miss, which is a decision rather than an
      //              observation: the game registers its materials a few per frame
      //              (kTexRepCreationsPerFrame), so a draw first seen mid-drain can miss and
      //              then have its material appear a few frames later.
      //
      // The second one is a deliberate trade, and the cost runs the other way from the
      // obvious fix. Not settling a miss keeps the draw off the preserve path every frame
      // until a material appears - and for an index whose entry was skipped (.png, which the
      // game logs) or whose creation failed, that is never, so preservation would be disabled
      // for the rest of the run with nothing on screen to show for it. Settling it costs the
      // other case: an instance that latched the game's texture during the drain keeps it for
      // its lifetime, because the preserve path skips determineMaterialData entirely.
      //
      // Regression signature of that case, and why `late=` exists: a surface that only
      // sharpens after leaving the room and coming back. `late=` on texrep.rmx counts handles
      // that became resident AFTER having missed, so a nonzero late= says the window was hit
      // this run - it is the measurement that would justify replacing this trade with a
      // drain-gated one (settle a miss only once the game reports created+skipped == entries,
      // rtx.dusklight.env.texrep*). Nothing here has measured it yet.
      //
      // Note the pending branch below deliberately does NOT settle: a texture still loading is
      // transient on a timescale this code can see, and it re-resolves on the next frame.
      // Both sets are bounded by the pack size.
      std::unordered_set<uint64_t> s_settled;
      std::unordered_set<uint64_t> s_resident;
      // Cumulative count of handles that resolved after having been settled by a miss - see
      // the trade above. Cumulative rather than per-frame because it is a "did this happen at
      // all this run" question, the same way resident= is.
      uint32_t s_lateResolved = 0;
      std::unordered_set<uint64_t> s_reportedMissing;
      bool s_reportedMissingTruncated = false;

      uint32_t indexFromAmbientG(float ambientG) {
        // Aurora writes a small non-negative integer. Reject anything that is not one rather
        // than truncating garbage into a plausible handle - a NaN or a negative here means the
        // channel is carrying something else and the correct answer is "no replacement".
        //
        // The upper bound is kMaxIndex (2^24), not kIndexMask (2^48). The rounded value is
        // converted to uint32_t below, and [conv.fpint] leaves that conversion UNDEFINED for
        // anything the destination type cannot represent, so testing against the handle mask
        // admitted a range the conversion could not take. Latent rather than reachable: the
        // only producer in the tree writes static_cast<float>(index) from a pack table
        // (aurora-ao lib/dx9/dx9_internal.hpp), and unset D3D9 material state is all zeros,
        // which the >= 1.0f test already rejects.
        if (!(ambientG >= 1.0f) || ambientG > static_cast<float>(kMaxIndex)) {
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
        // Bounded and self-describing, the same shape dusklightWater::shouldLogOnce uses:
        // check membership first, then the cap, so the truncation notice fires on a
        // suppressed NEW index rather than on a repeat of one already reported.
        //
        // The text no longer says "the game did not create it": the game registers its
        // materials a few per frame at startup, so most of these are "not created YET" and
        // the old wording read as a pack failure during ordinary loading.
        //
        // It also does not claim to show persistence. Each handle is named here at most once
        // per run, so this line cannot distinguish "missing for one frame" from "missing all
        // session"; texrep.rmx can, and is named rather than described so a reader can grep
        // for it.
        if (s_reportedMissing.count(handle) == 0) {
          if (s_reportedMissing.size() < kMaxReportedMissing) {
            s_reportedMissing.insert(handle);
            Logger::warn(str::format("texrep: no material registered (yet) for index ",
                                     static_cast<uint32_t>(handle & kIndexMask),
                                     " - the game creates them over several frames, so this is expected"
                                     " during loading. Reported once per index; whether it is still"
                                     " missing later is the missing= field on texrep.rmx"
                                     " (rtx.dusklight.texrep.report)"));
          } else if (!s_reportedMissingTruncated) {
            s_reportedMissingTruncated = true;
            Logger::warn(str::format("texrep.trunc cap=", kMaxReportedMissing,
                                     " - further missing-material indices not reported"));
          }
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
        // A handle already settled but never resident was settled by a miss, so this is a
        // material that arrived after this code had written the index off - the window
        // described beside s_settled. Counted on the first residency only.
        const bool firstResidency = s_resident.insert(handle).second;
        if (firstResidency && s_settled.count(handle) != 0) {
          ++s_lateResolved;
        }
        s_settled.insert(handle);
      }
      return &albedo;
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
      uint32_t lateCount = 0;
      {
        std::lock_guard lock { s_statsMutex };
        snapshot = s_lastFrame;
        residentCount = s_resident.size();
        lateCount = s_lateResolved;
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
        " late=", lateCount,
        "  (per frame, except resident and late which are cumulative;"
        " late = indices whose material arrived after this runtime had already written them off,"
        " so a draw first seen before that can be showing the game's own texture)"));
    }
  }
}
