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
#pragma once

// Dusklight character skeletons, Remix half.
//
// The problem this exists to solve. The game draws a character as many draws - J3D issues one
// GXCallDisplayList per matrix group (J3DShape::drawFast), groups split per material and per
// matrix palette - and Remix gives every draw its own BLAS, its own geometry hash and, in a
// capture, its own USD mesh and its own skeleton. Worse, the skeleton is invented:
// game_exporter.cpp's generateSkeleton names joints "root", "root/joint1", ... in a flat list and
// places them at the weighted centroid of the vertices they influence, with no rotation. So a
// forty-packet character arrives as forty meshes and forty unrelated stick figures, and neither
// pulling a body out nor replacing one is practical.
//
// None of that can be fixed from inside Remix, because two facts only the game has are missing:
//
//   1. A global joint index space. On the matrix-palette path aurora compacts the palette per
//      draw (D3DCAPS9::MaxVertexBlendMatrixIndex is 8), and a GX position-matrix slot is reused
//      between packets anyway - slot 3 is a different joint in the next group. So two draws'
//      blend index 0 are unrelated, and merging them would be nonsense.
//   2. The joint tree itself: names, parents, and bind transforms. J3DJointTree has all three
//      (getJointName, the child/younger links, getInvJointMtx) and none of them enter D3D9.
//
// So the game supplies both, and this file receives them.
//
// Who calls what, and why it is aurora rather than the game that makes the per-draw call:
// the game knows which joint it loaded into which GX slot, and aurora knows which GX slot each
// D3D9 blend index ended up meaning after its compaction. Neither half is enough. The game hands
// its slot->joint table to aurora through GXSetModelIdentity, aurora composes the two and calls
// dusklight_SetDrawSkeleton below once per draw, so this side sees one finished
// blendIndex -> global joint table and has nothing to reconstruct.
//
// A note on what this does NOT change: nothing here alters what is rendered. The bindings are
// consumed by the capture path only. The runtime still draws exactly the draws the game issues,
// with exactly the hashes it had before, so texture tagging, rtx.conf categories, USD bindings
// and existing per-draw replacements are all untouched.
//
// Game side: dusklight-ao/src/dusk/remix_skeleton.{cpp,hpp}
// Aurora side: aurora-ao/lib/dx9/dx9_draw.cpp (publish_draw_skeleton)
// Design and the replacement caveat: dusklight-ao/docs/remix-open-issues.md issue 15.

#include "rtx_option.h"
#include "../../util/util_matrix.h"
#include "../../util/xxHash/xxhash.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dxvk {

  struct DusklightSkeleton {
    RTX_OPTION("rtx.dusklight.skeleton", bool, enable, true,
               "Use the character skeletons and model identity the game publishes.\n"
               "With this on, a captured character is merged into one mesh per model instance carrying the game's own\n"
               "joint tree, instead of one mesh and one invented skeleton per draw call; and every draw of that\n"
               "character reports one shared identity to the Geometry Hash debug view, so a body paints as one colour\n"
               "rather than a patchwork. That identity is a debug-view field only - what is rendered, how geometry is\n"
               "cached, and which textures are tagged are all untouched. Turning it off restores stock Remix behaviour\n"
               "exactly.");
    RTX_OPTION("rtx.dusklight.skeleton", bool, mergeCaptures, true,
               "Merge a model instance's draws into one captured mesh.\n"
               "Requires the skeletons above. Materials survive as USD GeomSubsets of the merged mesh, so a character\n"
               "arrives in Blender as one object with one armature rather than dozens. Turn it off to keep one mesh\n"
               "per draw while still getting the real skeleton - useful for telling a merge fault from a skeleton one.");
    RTX_OPTION("rtx.dusklight.skeleton", bool, replaceBodies, true,
               "Let one replacement stand in for a whole character at runtime.\n"
               "A capture names a merged character mesh after its model, and with this on the runtime looks that same\n"
               "name up when any of the character's draws arrives. The first draw of the character each frame\n"
               "instantiates the replacement; the rest are dropped, because otherwise the new body and the original\n"
               "would both be drawn. With no replacement authored for a character nothing changes at all - the draws\n"
               "take exactly the path they always did.");
    RTX_OPTION_FLAG("rtx.dusklight.skeleton", bool, report, false, RtxOptionFlags::NoSave,
                    "Log a bounded skeleton.* summary of what the game has declared. Clears itself after one report.");
  };

  namespace dusklightSkeleton {
    // GX has 10 position matrices and D3D9 fixed-function reaches 8 blend indices; 16 is headroom
    // that keeps the per-draw binding a fixed size, which is what lets it ride on DrawCallState
    // without an allocation per draw.
    constexpr uint32_t kMaxBlendIndices = 16;
    // Refuse anything larger rather than truncating a skeleton into a plausible-looking wrong one.
    constexpr uint32_t kMaxJoints = 1024;
    // Sentinel for a blend index this draw does not use, or one the game could not resolve to a
    // single joint (a weighted envelope reached through the matrix-palette path, which is a blend
    // rather than a joint - see the game side for why those are left alone).
    constexpr uint16_t kNoJoint = 0xFFFF;

    // What the game declared for one model asset. Immutable once declared; re-declaring the same
    // key replaces it, which is what makes a mid-session model reload harmless.
    struct Skeleton {
      uint64_t modelKey = 0;
      uint32_t jointCount = 0;
      // Parent index per joint, -1 for a root. The game derives these by walking J3DJointTree's
      // child/younger links, because J3DJoint stores no parent pointer.
      std::vector<int32_t> parents;
      // Model-space transform of each joint in the bind pose - the inverse of J3D's
      // getInvJointMtx. USD wants this as skel:bindTransforms.
      std::vector<Matrix4> bindTransforms;
      // Joint-local transform, i.e. bindTransforms[i] relative to its parent. USD's
      // skel:restTransforms. Derived here rather than shipped, so the two cannot disagree.
      std::vector<Matrix4> restTransforms;
      // The joint's own name, and its full slash-delimited path from the root. USD identifies
      // joints by path, and Blender builds its bone hierarchy from exactly that.
      std::vector<std::string> jointNames;
      std::vector<std::string> jointPaths;
    };

    // What one draw contributes. Rides on DrawCallState, so it is a fixed-size POD.
    struct DrawBinding {
      uint64_t modelKey = 0;
      uint64_t instanceKey = 0;
      uint16_t jointCount = 0;
      uint16_t blendIndexCount = 0;
      std::array<uint16_t, kMaxBlendIndices> blendIndexToJoint = {};

      bool isValid() const {
        return modelKey != 0 && instanceKey != 0 && jointCount != 0;
      }
    };

    // The binding latched for the draw currently being submitted, or an invalid one. Thread local
    // because the latch is written by aurora on the game's thread and read on the same thread by
    // D3D9Rtx, while Remix's own worker threads must not see a neighbour's draw.
    const DrawBinding& currentDrawBinding();

    // Looked up during capture, on a different thread from the declaration, so this takes the
    // registry lock and returns a copy rather than a pointer into a container that can rehash.
    bool findSkeleton(uint64_t modelKey, Skeleton& skeletonOut);

    // The hash a whole character is known by: the name a capture gives its merged mesh, and the
    // name a replacement for that character has to be authored against.
    //
    // Derived from the model key ALONE, and that constraint is the point rather than a
    // simplification. A replacement is looked up per draw at runtime, where only the model key is
    // known - the set of draws the character will turn out to consist of this frame is not known
    // until the frame is over. Any hash that depends on that set can be computed by the capture and
    // never by the runtime, so a replacement authored against it could not bind to anything.
    //
    // The first revision of the merge did exactly that: it mixed in the ordered member mesh hashes
    // to keep two instances showing different packets from colliding. It produced stable, correct,
    // completely unusable hashes. Collisions are handled by the merge refusing the second instance
    // instead - see mergeGroup.
    XXH64_hash_t groupMeshHash(uint64_t modelKey);

    // Whether a body replacement is actually authored for this model.
    //
    // Asked by the skinning path, which must not raise its bone count for a character nobody is
    // replacing. Raising it unconditionally would look harmless and quietly cost real performance:
    // RtSurface bakes a single-bone draw's matrix into the transform and skips the skinning pass
    // entirely (rtx_types.cpp, `minBoneIndex + 1 == numBones`), and a raised count defeats that
    // test for every rigid packet of every character in the game.
    //
    // SceneManager records the answer the first time it resolves a draw of that model, so the very
    // first frame a character appears answers "no" and skins as it always did. Self-correcting, and
    // one frame of a body being posed by the original skeleton is not something anyone can see.
    void noteGroupReplacement(uint64_t modelKey, bool exists);
    bool hasGroupReplacement(uint64_t modelKey);

    // True for the first draw of a given character in a given frame, false for its siblings.
    //
    // This is what turns "forty draws" into "one body": the claiming draw instantiates the
    // replacement, and the rest are dropped by the caller. Dropping them is not optional - a
    // replacement stands in for the whole character, so leaving the siblings in would draw the new
    // body and the original one through each other.
    //
    // Which draw claims is whichever the game issues first, and J3D's order is deterministic, so
    // in practice it is the same packet every frame.
    bool claimGroupDraw(const DrawBinding& binding, uint32_t frameId);

    // Called once per submitted draw so the report can say how much of a frame is actually bound.
    // A scene where boundDraws stays zero while the game insists it is publishing is the first
    // thing to check, and it separates "the game is not calling" from "the merge is not merging".
    void noteDrawBinding(bool bound);

    // Counters for the overlay and the report.
    struct Stats {
      uint32_t skeletonsDeclared = 0;
      uint32_t skeletonsRejected = 0;
      uint32_t boundDraws = 0;
      uint32_t unboundDraws = 0;
      uint32_t groupsReplaced = 0;
      uint32_t drawsSuppressed = 0;
    };
    const Stats& stats();
    void onFrameEnd();
    void reportIfRequested();
  }
}

extern "C" {
  // Declare one model asset's skeleton. Idempotent; declaring the same modelKey again replaces it.
  //
  // `parents` has jointCount entries, -1 for a root. `bindTransforms` has jointCount * 12 floats,
  // a row-major 3x4 model-space matrix per joint (J3D's Mtx layout, so the game can hand over what
  // it already has). `packedNames` is jointCount NUL-terminated strings back to back.
  //
  // Returns 1 on success, 0 if the arguments are malformed or jointCount exceeds kMaxJoints. A
  // rejected skeleton is not a fatal condition: its draws simply fall back to stock capture
  // behaviour, which is what happened for every model before this existed.
  __declspec(dllexport) uint32_t dusklight_DeclareSkeleton(
    uint64_t modelKey,
    uint32_t jointCount,
    const int32_t* parents,
    const float* bindTransforms,
    const char* packedNames);

  // Latch the binding for the draw about to be submitted. `blendIndexToJoint` has
  // blendIndexCount entries, each a global joint index or kNoJoint.
  __declspec(dllexport) void dusklight_SetDrawSkeleton(
    uint64_t modelKey,
    uint64_t instanceKey,
    uint32_t jointCount,
    uint32_t blendIndexCount,
    const uint16_t* blendIndexToJoint);

  // Drop the latch. Must be called after the draw, or the next unrelated draw inherits this
  // model's identity and would be merged into the character - the loudest possible failure, and
  // the reason the game's hook is scoped rather than fire-and-forget.
  __declspec(dllexport) void dusklight_ClearDrawSkeleton();
}
