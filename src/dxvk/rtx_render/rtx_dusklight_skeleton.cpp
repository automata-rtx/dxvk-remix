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
#include "rtx_dusklight_skeleton.h"

#include "../../util/log/log.h"
#include "../../util/util_once.h"
#include "../../util/util_string.h"
#include "../../util/xxHash/xxhash.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace dxvk {
  namespace dusklightSkeleton {
    static std::mutex s_mutex;
    static std::unordered_map<uint64_t, Skeleton> s_skeletons;
    static uint32_t s_declared = 0;
    static uint32_t s_rejected = 0;
    static Stats s_stats;
    static Stats s_lastFrame;
    // Model instance -> the frame its replacement was last instantiated. See claimGroupDraw.
    static std::unordered_map<uint64_t, uint32_t> s_groupClaims;
    // Model key -> whether a body replacement is authored for it. See noteGroupReplacement.
    static std::unordered_map<uint64_t, bool> s_groupReplacements;

    // The latch is per thread on purpose: aurora writes it and D3D9Rtx reads it on the game's
    // thread, and a Remix worker submitting anything of its own must not pick up a character's
    // identity from a neighbour.
    static thread_local DrawBinding t_current;
    static const DrawBinding s_invalidBinding;

    namespace {

      // J3D hands over a row-major 3x4 (its Mtx), which is the same layout D3D9 and this codebase
      // call a 4x3 affine. Widened to a full 4x4 with the implicit last column.
      Matrix4 mtx3x4ToMatrix4(const float* m) {
        Matrix4 out;
        for (uint32_t row = 0; row < 3; ++row) {
          out[0][row] = m[row * 4 + 0];
          out[1][row] = m[row * 4 + 1];
          out[2][row] = m[row * 4 + 2];
          out[3][row] = m[row * 4 + 3];
        }
        out[0][3] = 0.0f;
        out[1][3] = 0.0f;
        out[2][3] = 0.0f;
        out[3][3] = 1.0f;
        return out;
      }

      // USD identifies a joint by its path from the root, and Blender rebuilds the bone hierarchy
      // from those paths rather than from any separate parent array - so a wrong path is a wrong
      // armature even when the parents are right. Built here rather than shipped so the two cannot
      // drift apart.
      //
      // Two things this guards against, both of which produce a silently broken skeleton rather
      // than an error: a name containing '/' would invent a joint level that does not exist, and a
      // cycle or forward reference in the parent array would loop forever.
      bool buildJointPaths(Skeleton& skeleton) {
        skeleton.jointPaths.resize(skeleton.jointCount);
        for (uint32_t i = 0; i < skeleton.jointCount; ++i) {
          std::string& name = skeleton.jointNames[i];
          if (name.empty()) {
            name = str::format("joint", i);
          }
          std::replace(name.begin(), name.end(), '/', '_');
          std::replace(name.begin(), name.end(), '.', '_');

          const int32_t parent = skeleton.parents[i];
          if (parent < 0) {
            skeleton.jointPaths[i] = name;
            continue;
          }
          // A parent must already have a path. J3D emits its joints parents-first, so this holds;
          // rejecting rather than tolerating a violation keeps a malformed tree from becoming a
          // plausible-looking wrong armature.
          if (static_cast<uint32_t>(parent) >= i) {
            return false;
          }
          skeleton.jointPaths[i] = skeleton.jointPaths[parent] + "/" + name;
        }
        return true;
      }

      // restTransforms are joint-local, bindTransforms are model-space. Deriving one from the
      // other here means a game that ships only the easy one cannot make them disagree.
      void buildRestTransforms(Skeleton& skeleton) {
        skeleton.restTransforms.resize(skeleton.jointCount);
        for (uint32_t i = 0; i < skeleton.jointCount; ++i) {
          const int32_t parent = skeleton.parents[i];
          if (parent < 0) {
            skeleton.restTransforms[i] = skeleton.bindTransforms[i];
          } else {
            skeleton.restTransforms[i] =
              inverse(skeleton.bindTransforms[parent]) * skeleton.bindTransforms[i];
          }
        }
      }
    }

    XXH64_hash_t groupMeshHash(uint64_t modelKey) {
      // Salted so a group hash cannot accidentally equal a per-draw geometry hash, which would let
      // a replacement authored for one bind to the other.
      constexpr uint64_t kGroupSalt = 0xD05C11'A7'5CE1E70Full;
      const uint64_t salted = modelKey ^ kGroupSalt;
      return XXH64(&salted, sizeof(salted), kGroupSalt);
    }

    void noteGroupReplacement(uint64_t modelKey, bool exists) {
      std::lock_guard lock { s_mutex };
      s_groupReplacements[modelKey] = exists;
    }

    bool hasGroupReplacement(uint64_t modelKey) {
      if (!DusklightSkeleton::enable() || !DusklightSkeleton::replaceBodies()) {
        return false;
      }
      std::lock_guard lock { s_mutex };
      const auto found = s_groupReplacements.find(modelKey);
      return found != s_groupReplacements.end() && found->second;
    }

    bool claimGroupDraw(const DrawBinding& binding, uint32_t frameId) {
      if (!binding.isValid()) {
        return false;
      }
      // Keyed on the model instance, not the model: two Bokoblins on screen are two bodies and
      // each needs its own claim, or the second one vanishes.
      const uint64_t key = binding.modelKey ^ (binding.instanceKey * 0x9E3779B97F4A7C15ull);

      std::lock_guard lock { s_mutex };
      auto& lastFrame = s_groupClaims[key];
      if (lastFrame == frameId) {
        ++s_stats.drawsSuppressed;
        return false;
      }
      lastFrame = frameId;
      ++s_stats.groupsReplaced;

      // The map is bounded by how many character instances a session sees, which is small, but it
      // is never pruned - so drop it wholesale if it ever grows past anything plausible rather
      // than leak for a long session. Losing the claims costs one frame of double-drawn bodies.
      if (s_groupClaims.size() > 4096) {
        s_groupClaims.clear();
      }
      return true;
    }

    const DrawBinding& currentDrawBinding() {
      if (!DusklightSkeleton::enable()) {
        return s_invalidBinding;
      }
      return t_current;
    }

    bool findSkeleton(uint64_t modelKey, Skeleton& skeletonOut) {
      std::lock_guard lock { s_mutex };
      const auto found = s_skeletons.find(modelKey);
      if (found == s_skeletons.end()) {
        return false;
      }
      skeletonOut = found->second;
      return true;
    }

    const Stats& stats() {
      return s_lastFrame;
    }

    void noteDrawBinding(bool bound) {
      // Deliberately not under s_mutex: this runs per draw on the submitting thread, and a
      // diagnostic counter is not worth serialising the draw path for. The cost is that a frame
      // straddling two submitting threads can lose a count, which is acceptable for a number
      // whose only job is to say "roughly how much of the scene is bound".
      if (bound) {
        ++s_stats.boundDraws;
      } else {
        ++s_stats.unboundDraws;
      }
    }

    namespace {
      // Caller must hold s_mutex.
      void reportLocked(const char* trigger) {
        Logger::info(str::format(
          "skeleton.rmx enable=", DusklightSkeleton::enable() ? 1 : 0,
          " merge=", DusklightSkeleton::mergeCaptures() ? 1 : 0,
          " declared=", s_skeletons.size(),
          " rejected=", s_rejected,
          " boundDraws=", s_lastFrame.boundDraws,
          " unboundDraws=", s_lastFrame.unboundDraws,
          " replaceBodies=", DusklightSkeleton::replaceBodies() ? 1 : 0,
          " bodiesReplaced=", s_lastFrame.groupsReplaced,
          " drawsSuppressed=", s_lastFrame.drawsSuppressed,
          " trigger=", trigger,
          "  (declared and rejected are cumulative; the draw counts are per frame."
          " boundDraws=0 means the game published nothing - check the game log for"
          " 'dx9.skeleton model identity export')"));

        // One line per model, capped, so a reader without the source can tell a model that declared
        // 60 joints from one that declared 3.
        //
        // groupHash is the one number an artist actually needs and cannot derive: it is a salted
        // hash of the model key, so nothing outside this file can compute it. It names the merged
        // mesh in a capture, and it is what a body replacement must be authored against.
        uint32_t printed = 0;
        for (const auto& [key, skeleton] : s_skeletons) {
          if (printed >= 16) {
            Logger::info(str::format("skeleton.rmx   ... ", s_skeletons.size() - printed,
                                     " more model(s) not listed"));
            break;
          }
          Logger::info(str::format(
            "skeleton.rmx   model=0x", std::hex, key,
            " groupHash=0x", groupMeshHash(key), std::dec,
            " joints=", skeleton.jointCount,
            " root=", skeleton.jointCount > 0 ? skeleton.jointNames[0] : std::string("-")));
          ++printed;
        }
      }

      // One automatic report per session, so a log answers "is this alive, and what do I author
      // against" without the owner having to know an option exists. Two triggers, because the
      // interesting cases are opposite: the first frame that binds a character shows the feature
      // working, and a fixed deadline shows it NOT working - which is the case a
      // fires-only-on-success report can never surface, and the case that sent the owner to ask.
      bool s_autoReported = false;
      uint32_t s_frameCount = 0;
      constexpr uint32_t kAutoReportDeadlineFrames = 600;
    }

    void onFrameEnd() {
      std::lock_guard lock { s_mutex };
      s_lastFrame = s_stats;
      s_lastFrame.skeletonsDeclared = s_declared;
      s_lastFrame.skeletonsRejected = s_rejected;

      ++s_frameCount;
      if (!s_autoReported && DusklightSkeleton::enable() &&
          (s_lastFrame.boundDraws > 0 || s_frameCount >= kAutoReportDeadlineFrames)) {
        s_autoReported = true;
        reportLocked(s_lastFrame.boundDraws > 0 ? "firstBoundDraw" : "deadline");
      }

      s_stats.boundDraws = 0;
      s_stats.unboundDraws = 0;
      s_stats.groupsReplaced = 0;
      s_stats.drawsSuppressed = 0;
    }

    void reportIfRequested() {
      if (!DusklightSkeleton::report()) {
        return;
      }
      DusklightSkeleton::reportObject().setDeferred(false);

      std::lock_guard lock { s_mutex };
      reportLocked("requested");
    }
  }
}

extern "C" {

  uint32_t dusklight_DeclareSkeleton(
    uint64_t modelKey,
    uint32_t jointCount,
    const int32_t* parents,
    const float* bindTransforms,
    const char* packedNames) {
    using namespace dxvk;
    using namespace dxvk::dusklightSkeleton;

    if (modelKey == 0 || jointCount == 0 || jointCount > kMaxJoints ||
        parents == nullptr || bindTransforms == nullptr || packedNames == nullptr) {
      std::lock_guard lock { s_mutex };
      ++s_rejected;
      ONCE(Logger::warn(str::format(
        "skeleton: rejected a declaration (model=0x", std::hex, modelKey, std::dec,
        " joints=", jointCount, "); those draws keep stock capture behaviour")));
      return 0;
    }

    Skeleton skeleton;
    skeleton.modelKey = modelKey;
    skeleton.jointCount = jointCount;
    skeleton.parents.assign(parents, parents + jointCount);
    skeleton.bindTransforms.resize(jointCount);
    skeleton.jointNames.resize(jointCount);

    const char* name = packedNames;
    for (uint32_t i = 0; i < jointCount; ++i) {
      skeleton.bindTransforms[i] = mtx3x4ToMatrix4(bindTransforms + i * 12);
      skeleton.jointNames[i] = name;
      // Walking a packed name block means trusting the caller to have terminated every one of
      // them. That is the game, in the same commit, so the risk is a bug rather than hostile
      // input - but the cost of being wrong is a read past the end, so the count is the authority
      // and the walk simply stops advancing once names run out.
      name += skeleton.jointNames[i].size() + 1;
    }

    if (!buildJointPaths(skeleton)) {
      std::lock_guard lock { s_mutex };
      ++s_rejected;
      ONCE(Logger::warn(str::format(
        "skeleton: model 0x", std::hex, modelKey, std::dec,
        " has a joint whose parent is not declared before it; rejected")));
      return 0;
    }
    buildRestTransforms(skeleton);

    {
      std::lock_guard lock { s_mutex };
      s_skeletons[modelKey] = std::move(skeleton);
      ++s_declared;
    }
    return 1;
  }

  void dusklight_SetDrawSkeleton(
    uint64_t modelKey,
    uint64_t instanceKey,
    uint32_t jointCount,
    uint32_t blendIndexCount,
    const uint16_t* blendIndexToJoint) {
    using namespace dxvk::dusklightSkeleton;

    t_current = DrawBinding {};
    if (modelKey == 0 || instanceKey == 0 || jointCount == 0) {
      return;
    }

    t_current.modelKey = modelKey;
    t_current.instanceKey = instanceKey;
    t_current.jointCount = static_cast<uint16_t>(std::min<uint32_t>(jointCount, kMaxJoints));
    t_current.blendIndexToJoint.fill(kNoJoint);

    const uint32_t count = std::min<uint32_t>(blendIndexCount, kMaxBlendIndices);
    if (blendIndexToJoint != nullptr) {
      for (uint32_t i = 0; i < count; ++i) {
        t_current.blendIndexToJoint[i] = blendIndexToJoint[i];
      }
      t_current.blendIndexCount = static_cast<uint16_t>(count);
    }
  }

  void dusklight_ClearDrawSkeleton() {
    dxvk::dusklightSkeleton::t_current = dxvk::dusklightSkeleton::DrawBinding {};
  }
}
