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
#include "rtx_hair_mask.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dxvk {

  namespace {

    inline HairMaskVec3 sub(const HairMaskVec3& a, const HairMaskVec3& b) {
      return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    inline float lengthSq(const HairMaskVec3& v) {
      return v.x * v.x + v.y * v.y + v.z * v.z;
    }

    // Axis-permutation candidates the auto-fit tries: identity plus the
    // 90/180-degree frame swaps common across exporters (Blender's Z-up
    // internal frame vs Y-up export conventions, in both directions).
    struct AxisCandidate {
      const char* name;
      // Row-major 3x3; applied as m * v.
      float m[9];
    };

    constexpr AxisCandidate kAxisCandidates[] = {
      { "identity",  { 1, 0, 0,   0, 1, 0,   0, 0, 1 } },
      { "rotX+90",   { 1, 0, 0,   0, 0, -1,  0, 1, 0 } },
      { "rotX-90",   { 1, 0, 0,   0, 0, 1,   0, -1, 0 } },
      { "rotX180",   { 1, 0, 0,   0, -1, 0,  0, 0, -1 } },
      { "rotY180",   { -1, 0, 0,  0, 1, 0,   0, 0, -1 } },
      { "rotZ180",   { -1, 0, 0,  0, -1, 0,  0, 0, 1 } },
    };

    inline HairMaskVec3 applyAxis(const AxisCandidate& c, const HairMaskVec3& v) {
      return {
        c.m[0] * v.x + c.m[1] * v.y + c.m[2] * v.z,
        c.m[3] * v.x + c.m[4] * v.y + c.m[5] * v.z,
        c.m[6] * v.x + c.m[7] * v.y + c.m[8] * v.z,
      };
    }

    // Resolves an OBJ index (1-based, negative counts from the end) to a
    // 0-based index, or returns false when out of range.
    inline bool resolveObjIndex(long raw, size_t count, uint32_t& out) {
      if (raw > 0 && static_cast<size_t>(raw) <= count) {
        out = static_cast<uint32_t>(raw - 1);
        return true;
      }
      if (raw < 0 && static_cast<size_t>(-raw) <= count) {
        out = static_cast<uint32_t>(count + raw);
        return true;
      }
      return false;
    }

  } // namespace

  // -------------------------------------------------------------------------
  // HairPointGrid
  // -------------------------------------------------------------------------

  void HairPointGrid::init(float cellSize) {
    m_points.clear();
    m_cells.clear();
    m_cellSize = cellSize > 1e-6f ? cellSize : 1e-6f;
  }

  void HairPointGrid::build(const HairMaskVec3* points, size_t count) {
    // Cell size from the bounds: ~64 cells along the largest extent keeps
    // buckets small for meshes in the tens of thousands of vertices.
    HairMaskVec3 lo { 3.4e38f, 3.4e38f, 3.4e38f };
    HairMaskVec3 hi { -3.4e38f, -3.4e38f, -3.4e38f };
    for (size_t i = 0; i < count; ++i) {
      lo.x = std::min(lo.x, points[i].x); hi.x = std::max(hi.x, points[i].x);
      lo.y = std::min(lo.y, points[i].y); hi.y = std::max(hi.y, points[i].y);
      lo.z = std::min(lo.z, points[i].z); hi.z = std::max(hi.z, points[i].z);
    }
    const float extent = count > 0
      ? std::max(hi.x - lo.x, std::max(hi.y - lo.y, hi.z - lo.z))
      : 1.0f;

    init(extent > 1e-6f ? extent / 64.0f : 1.0f);
    m_points.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      insert(points[i]);
    }
  }

  void HairPointGrid::insert(const HairMaskVec3& p) {
    const auto index = static_cast<uint32_t>(m_points.size());
    m_points.push_back(p);
    int64_t x, y, z;
    cellCoords(p, x, y, z);
    m_cells[cellKey(x, y, z)].push_back(index);
  }

  int32_t HairPointGrid::nearest(const HairMaskVec3& p, float* distanceOut) const {
    if (m_points.empty()) {
      return -1;
    }

    int64_t cx, cy, cz;
    cellCoords(p, cx, cy, cz);

    int32_t best = -1;
    float bestSq = 3.4e38f;

    // Expanding shells; once a hit exists, one further ring guarantees the
    // true nearest (a point in ring r is at least (r-1)*cell away).
    constexpr int kMaxRing = 8;
    for (int ring = 0; ring <= kMaxRing; ++ring) {
      if (best >= 0) {
        const float safe = static_cast<float>(ring - 1) * m_cellSize;
        if (safe > 0.0f && bestSq <= safe * safe) {
          break;
        }
      }
      for (int64_t dx = -ring; dx <= ring; ++dx) {
        for (int64_t dy = -ring; dy <= ring; ++dy) {
          for (int64_t dz = -ring; dz <= ring; ++dz) {
            if (std::max(std::llabs(dx), std::max(std::llabs(dy), std::llabs(dz))) != ring) {
              continue; // shell surface only
            }
            const auto it = m_cells.find(cellKey(cx + dx, cy + dy, cz + dz));
            if (it == m_cells.end()) {
              continue;
            }
            for (const uint32_t index : it->second) {
              const float dSq = lengthSq(sub(m_points[index], p));
              if (dSq < bestSq) {
                bestSq = dSq;
                best = static_cast<int32_t>(index);
              }
            }
          }
        }
      }
    }

    if (best < 0) {
      // Point far outside the populated cells: fall back to a full scan
      // (rare; correctness over speed).
      for (size_t i = 0; i < m_points.size(); ++i) {
        const float dSq = lengthSq(sub(m_points[i], p));
        if (dSq < bestSq) {
          bestSq = dSq;
          best = static_cast<int32_t>(i);
        }
      }
    }

    if (distanceOut != nullptr) {
      *distanceOut = std::sqrt(bestSq);
    }
    return best;
  }

  HairMaskVec3 HairPointGrid::centroid() const {
    HairMaskVec3 c {};
    if (m_points.empty()) {
      return c;
    }
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (const HairMaskVec3& p : m_points) {
      sx += p.x; sy += p.y; sz += p.z;
    }
    const double inv = 1.0 / static_cast<double>(m_points.size());
    c.x = static_cast<float>(sx * inv);
    c.y = static_cast<float>(sy * inv);
    c.z = static_cast<float>(sz * inv);
    return c;
  }

  uint64_t HairPointGrid::cellKey(int64_t x, int64_t y, int64_t z) const {
    // Mix the three signed coordinates into one key. Collisions only cost
    // extra distance checks.
    const uint64_t ux = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull;
    const uint64_t uy = static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full;
    const uint64_t uz = static_cast<uint64_t>(z) * 0x165667B19E3779F9ull;
    return ux ^ uy ^ uz;
  }

  void HairPointGrid::cellCoords(const HairMaskVec3& p, int64_t& x, int64_t& y, int64_t& z) const {
    x = static_cast<int64_t>(std::floor(p.x / m_cellSize));
    y = static_cast<int64_t>(std::floor(p.y / m_cellSize));
    z = static_cast<int64_t>(std::floor(p.z / m_cellSize));
  }

  // -------------------------------------------------------------------------
  // OBJ loading
  // -------------------------------------------------------------------------

  HairMaskMesh loadHairMaskObj(const std::string& path) {
    HairMaskMesh mask;

    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
      mask.status = "no mask file (" + path + ")";
      return mask;
    }

    std::vector<HairMaskVec3> objNormals;
    // Per-position accumulated corner normals, averaged after the parse.
    std::vector<HairMaskVec3> accumulated;
    std::vector<uint32_t> accumulatedCount;
    bool sawNormalIndex = false;

    char line[1024];
    size_t lineNumber = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
      ++lineNumber;
      if (line[0] == 'v' && line[1] == ' ') {
        HairMaskVec3 v;
        if (std::sscanf(line + 2, "%f %f %f", &v.x, &v.y, &v.z) == 3) {
          mask.positions.push_back(v);
        }
      } else if (line[0] == 'v' && line[1] == 'n' && line[2] == ' ') {
        HairMaskVec3 n;
        if (std::sscanf(line + 3, "%f %f %f", &n.x, &n.y, &n.z) == 3) {
          objNormals.push_back(n);
        }
      } else if (line[0] == 'f' && line[1] == ' ') {
        // Face corners: v, v/vt, v//vn or v/vt/vn. Fan-triangulate n-gons.
        uint32_t corners[64];
        long cornerNormals[64];
        uint32_t cornerCount = 0;

        const char* s = line + 2;
        while (*s != '\0' && cornerCount < 64) {
          while (*s == ' ' || *s == '\t') {
            ++s;
          }
          if (*s == '\0' || *s == '\r' || *s == '\n') {
            break;
          }
          char* end = nullptr;
          const long vIndex = std::strtol(s, &end, 10);
          if (end == s) {
            break;
          }
          s = end;
          long nIndex = 0;
          bool hasNormal = false;
          if (*s == '/') {
            ++s;
            if (*s != '/') {
              std::strtol(s, &end, 10); // texcoord index, unused
              s = end;
            }
            if (*s == '/') {
              ++s;
              nIndex = std::strtol(s, &end, 10);
              hasNormal = end != s;
              s = end;
            }
          }

          uint32_t resolved = 0;
          if (!resolveObjIndex(vIndex, mask.positions.size(), resolved)) {
            std::fclose(file);
            mask.positions.clear();
            mask.triangles.clear();
            mask.status = "bad vertex index on line " + std::to_string(lineNumber);
            return mask;
          }
          corners[cornerCount] = resolved;
          cornerNormals[cornerCount] = hasNormal ? nIndex : 0;
          sawNormalIndex = sawNormalIndex || hasNormal;
          ++cornerCount;
        }

        for (uint32_t c = 2; c < cornerCount; ++c) {
          mask.triangles.push_back({ { corners[0], corners[c - 1], corners[c] } });
        }

        // Accumulate corner normals onto positions for a per-position average.
        if (sawNormalIndex && !objNormals.empty()) {
          if (accumulated.size() < mask.positions.size()) {
            accumulated.resize(mask.positions.size());
            accumulatedCount.resize(mask.positions.size());
          }
          for (uint32_t c = 0; c < cornerCount; ++c) {
            uint32_t nResolved = 0;
            if (cornerNormals[c] != 0 && resolveObjIndex(cornerNormals[c], objNormals.size(), nResolved)) {
              HairMaskVec3& a = accumulated[corners[c]];
              const HairMaskVec3& n = objNormals[nResolved];
              a.x += n.x; a.y += n.y; a.z += n.z;
              ++accumulatedCount[corners[c]];
            }
          }
        }
      }
      // Everything else (vt, o, g, s, usemtl, mtllib, comments) is ignored.
    }
    std::fclose(file);

    if (mask.positions.empty() || mask.triangles.empty()) {
      mask.positions.clear();
      mask.triangles.clear();
      mask.status = "no usable geometry in " + path;
      return mask;
    }

    if (sawNormalIndex && !accumulated.empty()) {
      mask.normals.resize(mask.positions.size());
      accumulated.resize(mask.positions.size());
      for (size_t i = 0; i < mask.normals.size(); ++i) {
        const HairMaskVec3& a = accumulated[i];
        const float len = std::sqrt(lengthSq(a));
        mask.normals[i] = len > 1e-6f
          ? HairMaskVec3 { a.x / len, a.y / len, a.z / len }
          : HairMaskVec3 { 0.0f, 1.0f, 0.0f };
      }
    }

    mask.loaded = true;
    mask.status = std::to_string(mask.triangles.size()) + " triangles, "
                + std::to_string(mask.positions.size()) + " vertices"
                + (mask.normals.empty() ? ", no normals" : "");
    return mask;
  }

  // -------------------------------------------------------------------------
  // Alignment
  // -------------------------------------------------------------------------

  void alignHairMaskToLiveMesh(HairMaskMesh& mask, const HairPointGrid& liveGrid) {
    if (!mask.loaded || liveGrid.empty()) {
      return;
    }

    // Sample a subset of mask vertices for scoring.
    const size_t sampleCount = std::min<size_t>(mask.positions.size(), 512);
    const size_t sampleStep = std::max<size_t>(mask.positions.size() / sampleCount, 1);

    const HairMaskVec3 liveCentroid = liveGrid.centroid();

    int bestCandidate = -1;
    float bestMedian = 3.4e38f;
    HairMaskVec3 bestDelta {};

    std::vector<float> distances;
    distances.reserve(sampleCount);

    for (size_t candidate = 0; candidate < sizeof(kAxisCandidates) / sizeof(kAxisCandidates[0]); ++candidate) {
      const AxisCandidate& axis = kAxisCandidates[candidate];

      // Centroid of the rotated mask, for the translation term.
      double sx = 0.0, sy = 0.0, sz = 0.0;
      for (const HairMaskVec3& p : mask.positions) {
        const HairMaskVec3 r = applyAxis(axis, p);
        sx += r.x; sy += r.y; sz += r.z;
      }
      const double inv = 1.0 / static_cast<double>(mask.positions.size());
      const HairMaskVec3 delta {
        liveCentroid.x - static_cast<float>(sx * inv),
        liveCentroid.y - static_cast<float>(sy * inv),
        liveCentroid.z - static_cast<float>(sz * inv),
      };

      distances.clear();
      for (size_t i = 0; i < mask.positions.size(); i += sampleStep) {
        HairMaskVec3 r = applyAxis(axis, mask.positions[i]);
        r.x += delta.x; r.y += delta.y; r.z += delta.z;
        float d = 0.0f;
        liveGrid.nearest(r, &d);
        distances.push_back(d);
      }
      if (distances.empty()) {
        continue;
      }

      std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());
      const float median = distances[distances.size() / 2];

      if (median < bestMedian) {
        bestMedian = median;
        bestCandidate = static_cast<int>(candidate);
        bestDelta = delta;
      }
    }

    if (bestCandidate < 0) {
      mask.status += "; alignment failed";
      return;
    }

    // Apply the winning fit to all positions and (rotation only) normals.
    const AxisCandidate& axis = kAxisCandidates[bestCandidate];
    for (HairMaskVec3& p : mask.positions) {
      p = applyAxis(axis, p);
      p.x += bestDelta.x; p.y += bestDelta.y; p.z += bestDelta.z;
    }
    for (HairMaskVec3& n : mask.normals) {
      n = applyAxis(axis, n);
    }

    // Translation refinement. The centroid fit above is off by however much
    // the mask's deleted faces shift its centroid away from the full mesh's
    // (an eyes-only cut barely moves it; cutting a large region does).
    // Nearest-neighbor correspondences pull that residual out: once mask
    // points sit within a vertex spacing of their source vertices, the mean
    // offset to the nearest live vertex IS the remaining translation error.
    for (int iteration = 0; iteration < 3; ++iteration) {
      double ox = 0.0, oy = 0.0, oz = 0.0;
      size_t used = 0;
      for (size_t i = 0; i < mask.positions.size(); i += sampleStep) {
        const int32_t nearestIndex = liveGrid.nearest(mask.positions[i]);
        if (nearestIndex < 0) {
          continue;
        }
        const HairMaskVec3 offset = sub(liveGrid.points()[nearestIndex], mask.positions[i]);
        ox += offset.x; oy += offset.y; oz += offset.z;
        ++used;
      }
      if (used == 0) {
        break;
      }
      const HairMaskVec3 shift {
        static_cast<float>(ox / static_cast<double>(used)),
        static_cast<float>(oy / static_cast<double>(used)),
        static_cast<float>(oz / static_cast<double>(used)),
      };
      for (HairMaskVec3& p : mask.positions) {
        p.x += shift.x; p.y += shift.y; p.z += shift.z;
      }
      if (lengthSq(shift) < 1e-12f) {
        break;
      }
    }

    // Full residual statistics with the fit applied.
    float maxResidual = 0.0f;
    double residualSum = 0.0;
    distances.clear();
    for (size_t i = 0; i < mask.positions.size(); i += sampleStep) {
      float d = 0.0f;
      liveGrid.nearest(mask.positions[i], &d);
      distances.push_back(d);
      maxResidual = std::max(maxResidual, d);
      residualSum += d;
    }
    std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());

    mask.aligned = true;
    mask.alignmentName = axis.name;
    mask.medianResidual = distances.empty() ? -1.0f : distances[distances.size() / 2];
    mask.maxResidual = maxResidual;
    mask.status += "; aligned (" + std::string(axis.name) + "), median residual "
                 + std::to_string(mask.medianResidual) + ", max " + std::to_string(mask.maxResidual);
  }

} // namespace dxvk
