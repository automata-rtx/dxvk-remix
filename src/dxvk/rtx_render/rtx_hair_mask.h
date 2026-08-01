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

// Artist-authored hair scatter masks.
//
// A hair mask is a copy of a hair-tagged mesh with the faces that should not
// grow fur removed (eyes, accessories, paw pads, inner mouth). The authoring
// loop rides RTX Remix's own capture pipeline: for skinned meshes the game
// capturer exports the REST-POSE input geometry (rtx_game_capturer.cpp,
// captureMeshPositions uses the pre-skinning buffer), so a mesh copied from a
// capture, edited destructively (faces deleted, vertices never moved) and
// exported sits in exactly the coordinate space of the live draw's vertex
// data. The mask then replaces the scatter surface: strand roots distribute
// over the mask's triangles and bind to the nearest live-mesh vertex for
// bone, UV and (fallback) normal data.
//
// Exporters disagree about axes, captures recenter positions, and authoring
// round-trips bake in scene scale (a capture viewed at 0.01x in Blender
// exports 100x too small), so the loader never trusts the file's frame:
// alignHairMaskToLiveMesh solves the full similarity transform - rotation,
// uniform scale and translation - by seeding axis-permutation candidates
// with an RMS-radius scale estimate and refining with ICP (Horn's
// closed-form absolute orientation on nearest-vertex correspondences).
// Because mask vertices are unmoved copies of live vertices, a correct
// export converges to ~zero residual under ANY export settings; the residual
// is reported so a broken export shows up as a number in the overlay
// instead of misplaced fur, and a fit that stays poor is flagged so the
// caller can ignore the mask rather than scatter into the wrong place.
//
// This module deliberately depends only on the C++ standard library (no dxvk
// types, no logging) so it can be unit-tested standalone and lifted into
// other codebases; callers log the returned status strings.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxvk {

  struct HairMaskVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
  };

  struct HairMaskMesh {
    struct Triangle {
      uint32_t v[3];
    };

    std::vector<HairMaskVec3> positions;
    // Per-position averaged normals, normalized; empty when the OBJ carried
    // no vn data (callers then fall back to the live mesh's normals).
    std::vector<HairMaskVec3> normals;
    std::vector<Triangle> triangles;

    // Load / alignment diagnostics for the caller's log and overlay.
    bool loaded = false;
    bool aligned = false;
    // True when the winning fit mirrored the mask (the export baked a
    // reflection - a handedness-flipping axis convention or negative
    // scale). Detected by fitting both handednesses; see
    // alignHairMaskToLiveMesh.
    bool mirrored = false;
    // True when the converged fit lands mask vertices on live vertices
    // (median residual within a small fraction of the live mesh's RMS
    // radius). A false value means the mask does not belong to this mesh -
    // callers should scatter on the full surface instead.
    bool wellFitted = false;
    std::string status;
    const char* alignmentName = "";
    float alignmentScale = 1.0f;
    float medianResidual = -1.0f;
    float maxResidual = -1.0f;
    // RMS distance of live vertices from their centroid, the size reference
    // the residuals should be read against.
    float liveRmsRadius = 0.0f;
  };

  // Uniform hash grid over a point set. Used both to bind mask-scattered
  // strand roots to their nearest live-mesh vertex and, in insert mode, to
  // keep best-candidate scattering evenly spaced.
  class HairPointGrid {
  public:
    // Fixed-cell mode for incremental insertion (scatter spacing queries).
    void init(float cellSize);
    // Builds over a full point set; cell size derived from the bounds.
    void build(const HairMaskVec3* points, size_t count);

    void insert(const HairMaskVec3& p);

    // Index of the nearest inserted point, or -1 when empty.
    // distanceOut receives the distance when non-null.
    int32_t nearest(const HairMaskVec3& p, float* distanceOut = nullptr) const;

    bool empty() const { return m_points.empty(); }
    size_t size() const { return m_points.size(); }
    const std::vector<HairMaskVec3>& points() const { return m_points; }
    HairMaskVec3 centroid() const;

  private:
    uint64_t cellKey(int64_t x, int64_t y, int64_t z) const;
    void cellCoords(const HairMaskVec3& p, int64_t& x, int64_t& y, int64_t& z) const;

    std::vector<HairMaskVec3> m_points;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_cells;
    float m_cellSize = 1.0f;
  };

  // Parses a Wavefront OBJ (v / vn / f; quads and n-gons fan-triangulated;
  // negative indices supported; all other statements ignored). On failure
  // returns an unloaded mask whose status says why.
  HairMaskMesh loadHairMaskObj(const std::string& path);

  // Handedness handling for the alignment. Exports can bake a reflection
  // (an axis convention or negative scale that flips handedness), and on a
  // bilaterally symmetric character a reflected mask fits the mirror pose
  // almost perfectly with a proper rotation - fur then grows on the wrong
  // side. Auto fits both handednesses and keeps the strictly better one
  // (a genuine unmirrored export is bit-exact and cannot lose; ties from a
  // perfectly symmetric mesh prefer as-authored). The forced modes exist
  // for that tie, where geometry alone cannot decide.
  enum class HairMaskMirrorMode : int {
    Auto = 0,
    AsAuthored = 1,
    Mirrored = 2,
  };

  // Fits the mask onto the live mesh by solving the full similarity
  // transform (rotation, uniform scale, translation): axis-permutation and
  // principal-axis candidates under several scale hypotheses seed an ICP
  // that solves Horn's absolute orientation on nearest-live-vertex
  // correspondences until it converges, in both handednesses per
  // mirrorMode. The best transform is applied to positions (rotation only
  // to normals) and the residual statistics recorded; wellFitted reports
  // whether the converged fit is close enough to trust. The live grid must
  // be built over the live mesh's rest-pose vertex positions.
  void alignHairMaskToLiveMesh(HairMaskMesh& mask, const HairPointGrid& liveGrid,
                               HairMaskMirrorMode mirrorMode = HairMaskMirrorMode::Auto);

} // namespace dxvk
