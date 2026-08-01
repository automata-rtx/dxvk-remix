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

    // Axis-permutation candidates that seed the ICP: identity plus the
    // 90/180-degree frame swaps common across exporters (Blender's Z-up
    // internal frame vs Y-up export conventions, in both directions),
    // followed by every remaining proper signed axis permutation - 24 seed
    // rotations in total, so any axis-preset combination starts the ICP
    // inside its convergence basin. The named six come first so common
    // exports report a readable name.
    struct AxisCandidate {
      const char* name;
      // Row-major 3x3; applied as m * v.
      float m[9];
    };

    constexpr AxisCandidate kNamedAxisCandidates[] = {
      { "identity",  { 1, 0, 0,   0, 1, 0,   0, 0, 1 } },
      { "rotX+90",   { 1, 0, 0,   0, 0, -1,  0, 1, 0 } },
      { "rotX-90",   { 1, 0, 0,   0, 0, 1,   0, -1, 0 } },
      { "rotX180",   { 1, 0, 0,   0, -1, 0,  0, 0, -1 } },
      { "rotY180",   { -1, 0, 0,  0, 1, 0,   0, 0, -1 } },
      { "rotZ180",   { -1, 0, 0,  0, -1, 0,  0, 0, 1 } },
    };

    std::vector<AxisCandidate> buildAxisCandidates() {
      std::vector<AxisCandidate> candidates(std::begin(kNamedAxisCandidates), std::end(kNamedAxisCandidates));
      // Generate all signed permutation matrices with determinant +1 and
      // append the ones the named list does not already carry.
      const int perms[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };
      for (const auto& perm : perms) {
        for (int signBits = 0; signBits < 8; ++signBits) {
          float m[9] = {};
          for (int row = 0; row < 3; ++row) {
            m[row * 3 + perm[row]] = (signBits >> row & 1) != 0 ? -1.0f : 1.0f;
          }
          const float det = m[0] * (m[4] * m[8] - m[5] * m[7])
                          - m[1] * (m[3] * m[8] - m[5] * m[6])
                          + m[2] * (m[3] * m[7] - m[4] * m[6]);
          if (det < 0.5f) {
            continue; // reflections would mirror the mask
          }
          bool duplicate = false;
          for (const AxisCandidate& existing : candidates) {
            bool same = true;
            for (int k = 0; k < 9; ++k) {
              same = same && existing.m[k] == m[k];
            }
            duplicate = duplicate || same;
          }
          if (!duplicate) {
            AxisCandidate c { "axis swap", {} };
            std::memcpy(c.m, m, sizeof(m));
            candidates.push_back(c);
          }
        }
      }
      return candidates;
    }

    inline HairMaskVec3 applyAxis(const AxisCandidate& c, const HairMaskVec3& v) {
      return {
        c.m[0] * v.x + c.m[1] * v.y + c.m[2] * v.z,
        c.m[3] * v.x + c.m[4] * v.y + c.m[5] * v.z,
        c.m[6] * v.x + c.m[7] * v.y + c.m[8] * v.z,
      };
    }

    // Similarity transform p' = s * R * p + t (R row-major, applied to
    // column vectors). Double precision: the ICP solves it from sums over
    // hundreds of points whose coordinates can span very different scales.
    struct Similarity {
      double r[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
      double s = 1.0;
      double t[3] = { 0, 0, 0 };

      HairMaskVec3 apply(const HairMaskVec3& p) const {
        return {
          static_cast<float>(s * (r[0] * p.x + r[1] * p.y + r[2] * p.z) + t[0]),
          static_cast<float>(s * (r[3] * p.x + r[4] * p.y + r[5] * p.z) + t[1]),
          static_cast<float>(s * (r[6] * p.x + r[7] * p.y + r[8] * p.z) + t[2]),
        };
      }

      // Rotation only - normals ignore scale and translation.
      HairMaskVec3 rotate(const HairMaskVec3& v) const {
        return {
          static_cast<float>(r[0] * v.x + r[1] * v.y + r[2] * v.z),
          static_cast<float>(r[3] * v.x + r[4] * v.y + r[5] * v.z),
          static_cast<float>(r[6] * v.x + r[7] * v.y + r[8] * v.z),
        };
      }
    };

    // Jacobi eigen decomposition of a symmetric 3x3 matrix. Eigenvalues and
    // matching eigenvectors (columns of v) are returned sorted descending.
    void jacobiEigen(const double a[3][3], double eigenvalues[3], double v[3][3]) {
      double m[3][3];
      std::memcpy(m, a, sizeof(m));
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          v[r][c] = r == c ? 1.0 : 0.0;
        }
      }
      for (int sweep = 0; sweep < 32; ++sweep) {
        // Largest off-diagonal element.
        int p = 0, q = 1;
        double largest = std::fabs(m[0][1]);
        if (std::fabs(m[0][2]) > largest) { largest = std::fabs(m[0][2]); p = 0; q = 2; }
        if (std::fabs(m[1][2]) > largest) { largest = std::fabs(m[1][2]); p = 1; q = 2; }
        if (largest < 1e-14) {
          break;
        }
        const double theta = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
        const double sign = theta >= 0.0 ? 1.0 : -1.0;
        const double tTan = sign / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double cRot = 1.0 / std::sqrt(tTan * tTan + 1.0);
        const double sRot = tTan * cRot;
        for (int k = 0; k < 3; ++k) {
          const double mkp = m[k][p], mkq = m[k][q];
          m[k][p] = cRot * mkp - sRot * mkq;
          m[k][q] = sRot * mkp + cRot * mkq;
        }
        for (int k = 0; k < 3; ++k) {
          const double mpk = m[p][k], mqk = m[q][k];
          m[p][k] = cRot * mpk - sRot * mqk;
          m[q][k] = sRot * mpk + cRot * mqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = cRot * vkp - sRot * vkq;
          v[k][q] = sRot * vkp + cRot * vkq;
        }
      }
      int order[3] = { 0, 1, 2 };
      const double lambda[3] = { m[0][0], m[1][1], m[2][2] };
      // Sort indices by eigenvalue, descending (3 elements: manual).
      if (lambda[order[0]] < lambda[order[1]]) std::swap(order[0], order[1]);
      if (lambda[order[1]] < lambda[order[2]]) std::swap(order[1], order[2]);
      if (lambda[order[0]] < lambda[order[1]]) std::swap(order[0], order[1]);
      double sortedV[3][3];
      for (int c = 0; c < 3; ++c) {
        eigenvalues[c] = lambda[order[c]];
        for (int r = 0; r < 3; ++r) {
          sortedV[r][c] = v[r][order[c]];
        }
      }
      std::memcpy(v, sortedV, sizeof(sortedV));
    }

    // Principal-axis frame of a point set: columns are the eigenvectors of
    // the covariance (eigenvalues returned alongside, descending), made
    // right-handed. Handles arbitrary baked rotations the fixed
    // axis-permutation seeds cannot: aligning the mask's frame to the live
    // mesh's frame is (up to per-axis sign) the true rotation.
    void principalFrame(const HairMaskVec3* points, size_t count, const HairMaskVec3& centroid, double frame[3][3],
                        double eigenvaluesOut[3]) {
      double cov[3][3] = {};
      for (size_t i = 0; i < count; ++i) {
        const double d[3] = { points[i].x - centroid.x, points[i].y - centroid.y, points[i].z - centroid.z };
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 3; ++c) {
            cov[r][c] += d[r] * d[c];
          }
        }
      }
      jacobiEigen(cov, eigenvaluesOut, frame);
      // Right-handed: flip the last column if needed.
      const double det =
          frame[0][0] * (frame[1][1] * frame[2][2] - frame[1][2] * frame[2][1])
        - frame[0][1] * (frame[1][0] * frame[2][2] - frame[1][2] * frame[2][0])
        + frame[0][2] * (frame[1][0] * frame[2][1] - frame[1][1] * frame[2][0]);
      if (det < 0.0) {
        for (int r = 0; r < 3; ++r) {
          frame[r][2] = -frame[r][2];
        }
      }
    }

    // Horn's closed-form absolute orientation: the similarity transform
    // mapping src points onto dst points (least squares). The optimal
    // rotation is the dominant eigenvector of Horn's 4x4 quaternion matrix,
    // found by power iteration; uniform scale and translation follow in
    // closed form. Returns false on degenerate input.
    bool solveSimilarity(const std::vector<HairMaskVec3>& src, const std::vector<HairMaskVec3>& dst, Similarity& out) {
      const size_t count = src.size();
      if (count < 4 || dst.size() != count) {
        return false;
      }

      double srcCentroid[3] = {}, dstCentroid[3] = {};
      for (size_t i = 0; i < count; ++i) {
        srcCentroid[0] += src[i].x; srcCentroid[1] += src[i].y; srcCentroid[2] += src[i].z;
        dstCentroid[0] += dst[i].x; dstCentroid[1] += dst[i].y; dstCentroid[2] += dst[i].z;
      }
      const double inv = 1.0 / static_cast<double>(count);
      for (int k = 0; k < 3; ++k) {
        srcCentroid[k] *= inv;
        dstCentroid[k] *= inv;
      }

      // Covariance sums S[a][b] = sum(srcCentered[a] * dstCentered[b]).
      double S[3][3] = {};
      double srcSq = 0.0;
      for (size_t i = 0; i < count; ++i) {
        const double p[3] = { src[i].x - srcCentroid[0], src[i].y - srcCentroid[1], src[i].z - srcCentroid[2] };
        const double q[3] = { dst[i].x - dstCentroid[0], dst[i].y - dstCentroid[1], dst[i].z - dstCentroid[2] };
        for (int a = 0; a < 3; ++a) {
          for (int b = 0; b < 3; ++b) {
            S[a][b] += p[a] * q[b];
          }
        }
        srcSq += p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
      }
      if (srcSq < 1e-12) {
        return false;
      }

      const double N[4][4] = {
        { S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1],           S[2][0] - S[0][2],           S[0][1] - S[1][0] },
        { S[1][2] - S[2][1],           S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0],           S[2][0] + S[0][2] },
        { S[2][0] - S[0][2],           S[0][1] + S[1][0],           -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
        { S[0][1] - S[1][0],           S[2][0] + S[0][2],           S[1][2] + S[2][1],           -S[0][0] - S[1][1] + S[2][2] },
      };

      // Shifted power iteration: adding a bound on the spectral radius makes
      // the most positive eigenvalue dominant in magnitude.
      double shift = 0.0;
      for (int row = 0; row < 4; ++row) {
        double rowSum = 0.0;
        for (int col = 0; col < 4; ++col) {
          rowSum += std::fabs(N[row][col]);
        }
        shift = std::max(shift, rowSum);
      }

      double q[4] = { 1.0, 0.0, 0.0, 0.0 };
      for (int iteration = 0; iteration < 100; ++iteration) {
        double next[4];
        for (int row = 0; row < 4; ++row) {
          next[row] = shift * q[row];
          for (int col = 0; col < 4; ++col) {
            next[row] += N[row][col] * q[col];
          }
        }
        const double len = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]);
        if (len < 1e-30) {
          // Started orthogonal to the dominant eigenvector; reseed.
          q[0] = 0.5; q[1] = 0.5; q[2] = 0.5; q[3] = 0.5;
          continue;
        }
        for (int k = 0; k < 4; ++k) {
          q[k] = next[k] / len;
        }
      }

      const double w = q[0], x = q[1], y = q[2], z = q[3];
      out.r[0] = 1.0 - 2.0 * (y * y + z * z);
      out.r[1] = 2.0 * (x * y - w * z);
      out.r[2] = 2.0 * (x * z + w * y);
      out.r[3] = 2.0 * (x * y + w * z);
      out.r[4] = 1.0 - 2.0 * (x * x + z * z);
      out.r[5] = 2.0 * (y * z - w * x);
      out.r[6] = 2.0 * (x * z - w * y);
      out.r[7] = 2.0 * (y * z + w * x);
      out.r[8] = 1.0 - 2.0 * (x * x + y * y);

      // Uniform scale: sum(dstCentered . (R * srcCentered)) / sum|srcCentered|^2.
      double dotSum = 0.0;
      for (size_t i = 0; i < count; ++i) {
        const double p[3] = { src[i].x - srcCentroid[0], src[i].y - srcCentroid[1], src[i].z - srcCentroid[2] };
        const double rp[3] = {
          out.r[0] * p[0] + out.r[1] * p[1] + out.r[2] * p[2],
          out.r[3] * p[0] + out.r[4] * p[1] + out.r[5] * p[2],
          out.r[6] * p[0] + out.r[7] * p[1] + out.r[8] * p[2],
        };
        dotSum += rp[0] * (dst[i].x - dstCentroid[0]) + rp[1] * (dst[i].y - dstCentroid[1]) + rp[2] * (dst[i].z - dstCentroid[2]);
      }
      out.s = dotSum / srcSq;
      if (!(out.s > 1e-12) || !std::isfinite(out.s)) {
        return false;
      }

      const double rc[3] = {
        out.r[0] * srcCentroid[0] + out.r[1] * srcCentroid[1] + out.r[2] * srcCentroid[2],
        out.r[3] * srcCentroid[0] + out.r[4] * srcCentroid[1] + out.r[5] * srcCentroid[2],
        out.r[6] * srcCentroid[0] + out.r[7] * srcCentroid[1] + out.r[8] * srcCentroid[2],
      };
      for (int k = 0; k < 3; ++k) {
        out.t[k] = dstCentroid[k] - out.s * rc[k];
      }
      return std::isfinite(out.t[0]) && std::isfinite(out.t[1]) && std::isfinite(out.t[2]);
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

    // Size references. The RMS radius ratio estimates the export's uniform
    // scale (a capture edited at 0.01x viewer scale exports 100x too small);
    // the live RMS radius is also the yardstick residuals are judged by.
    const HairMaskVec3 liveCentroid = liveGrid.centroid();
    double liveRmsSq = 0.0;
    for (const HairMaskVec3& p : liveGrid.points()) {
      liveRmsSq += lengthSq(sub(p, liveCentroid));
    }
    const float liveRms = static_cast<float>(std::sqrt(liveRmsSq / static_cast<double>(liveGrid.size())));

    double mx = 0.0, my = 0.0, mz = 0.0;
    for (const HairMaskVec3& p : mask.positions) {
      mx += p.x; my += p.y; mz += p.z;
    }
    const double invCount = 1.0 / static_cast<double>(mask.positions.size());
    const HairMaskVec3 maskCentroid {
      static_cast<float>(mx * invCount), static_cast<float>(my * invCount), static_cast<float>(mz * invCount)
    };
    double maskRmsSq = 0.0;
    for (const HairMaskVec3& p : mask.positions) {
      maskRmsSq += lengthSq(sub(p, maskCentroid));
    }
    const double maskRms = std::sqrt(maskRmsSq * invCount);
    if (!(maskRms > 1e-12) || !(liveRms > 1e-12f)) {
      mask.status += "; alignment failed (degenerate geometry)";
      return;
    }
    const double initialScale = static_cast<double>(liveRms) / maskRms;

    // Sample a subset of mask vertices for correspondences and scoring.
    const size_t sampleCount = std::min<size_t>(mask.positions.size(), 512);
    const size_t sampleStep = std::max<size_t>(mask.positions.size() / sampleCount, 1);
    std::vector<HairMaskVec3> samples;
    samples.reserve(sampleCount);
    for (size_t i = 0; i < mask.positions.size(); i += sampleStep) {
      samples.push_back(mask.positions[i]);
    }

    // Bounds of the live points: queries far outside cannot have a nearby
    // vertex, and answering them through the grid is its worst case (ring
    // walks ending in a full scan). The distance to the box is a correct
    // lower bound and a fine score for what is a hopeless placement anyway.
    HairMaskVec3 liveLo { 3.4e38f, 3.4e38f, 3.4e38f };
    HairMaskVec3 liveHi { -3.4e38f, -3.4e38f, -3.4e38f };
    for (const HairMaskVec3& p : liveGrid.points()) {
      liveLo.x = std::min(liveLo.x, p.x); liveHi.x = std::max(liveHi.x, p.x);
      liveLo.y = std::min(liveLo.y, p.y); liveHi.y = std::max(liveHi.y, p.y);
      liveLo.z = std::min(liveLo.z, p.z); liveHi.z = std::max(liveHi.z, p.z);
    }
    const float boundsMargin = 0.05f * liveRms;
    const auto nearestBounded = [&](const HairMaskVec3& p, float* distanceOut) -> int32_t {
      const float cx = std::min(std::max(p.x, liveLo.x), liveHi.x);
      const float cy = std::min(std::max(p.y, liveLo.y), liveHi.y);
      const float cz = std::min(std::max(p.z, liveLo.z), liveHi.z);
      const float dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
      const float outsideSq = dx * dx + dy * dy + dz * dz;
      if (outsideSq > boundsMargin * boundsMargin) {
        *distanceOut = std::sqrt(outsideSq);
        return -1;
      }
      return liveGrid.nearest(p, distanceOut);
    };

    std::vector<float> distances;
    distances.reserve(samples.size());
    const auto scoreMedian = [&](const Similarity& transform) -> float {
      distances.clear();
      for (const HairMaskVec3& sample : samples) {
        float d = 0.0f;
        nearestBounded(transform.apply(sample), &d);
        distances.push_back(d);
      }
      std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());
      return distances[distances.size() / 2];
    };

    // Principal frames of both point sets, shared by the scale hypotheses
    // and the PCA rotation seeds below.
    double maskFrame[3][3], liveFrame[3][3];
    double maskEigenvalues[3], liveEigenvalues[3];
    principalFrame(mask.positions.data(), mask.positions.size(), maskCentroid, maskFrame, maskEigenvalues);
    principalFrame(liveGrid.points().data(), liveGrid.size(), liveCentroid, liveFrame, liveEigenvalues);

    // Stage 1: score every seed rotation under each scale hypothesis with a
    // centroid translation; keep the best few as ICP starting points.
    // Three scales, because no single estimate survives every authoring
    // accident: the RMS-radius ratio is unbiased only when the mask covers
    // most of the mesh (a large cut shrinks the mask's radius and inflates
    // the ratio); 1.0 is exactly right whenever the export kept the
    // capture's units; and the median of the per-principal-axis extent
    // ratios is robust to a cut along ONE axis, since the other two axes
    // still measure the true scale.
    static const std::vector<AxisCandidate> axisCandidates = buildAxisCandidates();

    double scaleHypotheses[3] = { initialScale, 1.0, initialScale };
    if (maskEigenvalues[0] > 1e-12 && maskEigenvalues[1] > 1e-12 && maskEigenvalues[2] > 1e-12) {
      double ratios[3];
      for (int k = 0; k < 3; ++k) {
        ratios[k] = std::sqrt(std::max(liveEigenvalues[k], 0.0) / maskEigenvalues[k]);
      }
      if (ratios[0] > ratios[1]) std::swap(ratios[0], ratios[1]);
      if (ratios[1] > ratios[2]) std::swap(ratios[1], ratios[2]);
      if (ratios[0] > ratios[1]) std::swap(ratios[0], ratios[1]);
      if (std::isfinite(ratios[1]) && ratios[1] > 1e-12) {
        scaleHypotheses[2] = ratios[1];
      }
    }
    int scaleCount = 0;
    double uniqueScales[3];
    for (const double hypothesis : scaleHypotheses) {
      bool duplicate = false;
      for (int k = 0; k < scaleCount; ++k) {
        duplicate = duplicate || std::fabs(hypothesis - uniqueScales[k]) < 0.01 * uniqueScales[k];
      }
      if (!duplicate) {
        uniqueScales[scaleCount++] = hypothesis;
      }
    }

    struct Seed {
      Similarity transform;
      float median;
      const char* name;
    };
    std::vector<Seed> seeds;

    // The as-exported placement first: an export with correct settings puts
    // every mask vertex bit-exactly on its live source vertex, and no
    // centroid adjustment must then perturb it - the centroid seeds below
    // are biased by however much the mask's deleted faces move its centroid
    // off the full mesh's.
    {
      Similarity identity;
      seeds.push_back({ identity, scoreMedian(identity), "as exported" });
    }

    for (const AxisCandidate& axis : axisCandidates) {
      for (int scaleIndex = 0; scaleIndex < scaleCount; ++scaleIndex) {
        Similarity transform;
        for (int k = 0; k < 9; ++k) {
          transform.r[k] = axis.m[k];
        }
        transform.s = uniqueScales[scaleIndex];
        const HairMaskVec3 rotatedCentroid = applyAxis(axis, maskCentroid);
        transform.t[0] = liveCentroid.x - transform.s * rotatedCentroid.x;
        transform.t[1] = liveCentroid.y - transform.s * rotatedCentroid.y;
        transform.t[2] = liveCentroid.z - transform.s * rotatedCentroid.z;
        seeds.push_back({ transform, scoreMedian(transform), axis.name });
      }
    }

    // PCA seeds: rotate the mask's principal frame onto the live mesh's.
    // This is what catches arbitrary rotations baked into an export (an
    // applied object transform) that no axis permutation can represent. The
    // per-axis sign ambiguity gives four proper-rotation candidates.
    {
      for (int signBits = 0; signBits < 4; ++signBits) {
        const double sign0 = (signBits & 1) != 0 ? -1.0 : 1.0;
        const double sign1 = (signBits & 2) != 0 ? -1.0 : 1.0;
        const double sign2 = sign0 * sign1; // keep det = +1
        const double signs[3] = { sign0, sign1, sign2 };

        for (int scaleIndex = 0; scaleIndex < scaleCount; ++scaleIndex) {
          Similarity transform;
          // R = liveFrame * diag(signs) * maskFrame^T.
          for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
              double sum = 0.0;
              for (int k = 0; k < 3; ++k) {
                sum += liveFrame[r][k] * signs[k] * maskFrame[c][k];
              }
              transform.r[r * 3 + c] = sum;
            }
          }
          transform.s = uniqueScales[scaleIndex];
          const double rc[3] = {
            transform.r[0] * maskCentroid.x + transform.r[1] * maskCentroid.y + transform.r[2] * maskCentroid.z,
            transform.r[3] * maskCentroid.x + transform.r[4] * maskCentroid.y + transform.r[5] * maskCentroid.z,
            transform.r[6] * maskCentroid.x + transform.r[7] * maskCentroid.y + transform.r[8] * maskCentroid.z,
          };
          transform.t[0] = liveCentroid.x - transform.s * rc[0];
          transform.t[1] = liveCentroid.y - transform.s * rc[1];
          transform.t[2] = liveCentroid.z - transform.s * rc[2];
          seeds.push_back({ transform, scoreMedian(transform), "pca" });
        }
      }
    }

    std::sort(seeds.begin(), seeds.end(), [](const Seed& a, const Seed& b) { return a.median < b.median; });

    // Stage 2: similarity ICP from the best seeds. Each iteration pairs
    // sampled mask vertices with their nearest live vertex, trims the worst
    // fifth (edited-away regions, early mismatches), and re-solves the FULL
    // transform from the original sample positions - absolute re-solves
    // cannot accumulate drift. Mask vertices are unmoved copies of live
    // vertices, so a mask that belongs to this mesh converges to ~zero
    // residual regardless of the export's scale, axes or baked transforms.
    Similarity best = seeds[0].transform;
    float bestMedian = seeds[0].median;
    const char* bestName = seeds[0].name;

    // Refine EVERY seed: the pre-ICP score cannot rank reliably - a
    // wrong-scale placement that collapses the mask inside the surface
    // cloud scores better than a correct-scale start still offset by the
    // cut's centroid bias, yet only the latter converges to the true fit.
    // Alignment runs once per mask load, so the extra polish is cheap.
    const size_t startCount = seeds.size();
    struct Correspondence {
      float distance;
      uint32_t sample;
      int32_t live;
    };
    std::vector<Correspondence> pairs;
    std::vector<HairMaskVec3> pairSrc, pairDst;

    const auto runIcp = [&](Similarity current, float currentMedian) {
      const float startMedian = currentMedian;
      for (int iteration = 0; iteration < 20; ++iteration) {
        // Abandon starts that are going nowhere: a genuine fit collapses
        // geometrically within a few iterations, and most seeds are junk
        // whose full refinement would dominate the alignment's cost.
        if (iteration == 3 && currentMedian > 0.9f * startMedian) {
          break;
        }
        pairs.clear();
        for (uint32_t i = 0; i < samples.size(); ++i) {
          float d = 0.0f;
          const int32_t nearestIndex = nearestBounded(current.apply(samples[i]), &d);
          if (nearestIndex >= 0) {
            pairs.push_back({ d, i, nearestIndex });
          }
        }
        // Trimmed correspondences: keep the best 80%.
        const size_t keep = std::max<size_t>(pairs.size() * 4 / 5, 8);
        if (pairs.size() > keep) {
          std::nth_element(pairs.begin(), pairs.begin() + keep, pairs.end(),
                           [](const Correspondence& a, const Correspondence& b) { return a.distance < b.distance; });
          pairs.resize(keep);
        }
        if (pairs.size() < 8) {
          break;
        }

        pairSrc.clear();
        pairDst.clear();
        for (const Correspondence& pair : pairs) {
          pairSrc.push_back(samples[pair.sample]);
          pairDst.push_back(liveGrid.points()[pair.live]);
        }

        Similarity solved;
        if (!solveSimilarity(pairSrc, pairDst, solved)) {
          break;
        }
        const float solvedMedian = scoreMedian(solved);
        if (solvedMedian >= currentMedian - 1e-7f * liveRms) {
          if (solvedMedian < currentMedian) {
            current = solved;
            currentMedian = solvedMedian;
          }
          break; // converged
        }
        current = solved;
        currentMedian = solvedMedian;
        if (currentMedian <= 1e-5f * liveRms) {
          break; // residual at float noise already
        }
      }
      return std::make_pair(current, currentMedian);
    };

    for (size_t start = 0; start < startCount; ++start) {
      const auto [refined, refinedMedian] = runIcp(seeds[start].transform, seeds[start].median);
      if (refinedMedian < bestMedian) {
        best = refined;
        bestMedian = refinedMedian;
        bestName = seeds[start].name;
      }
      if (bestMedian <= 1e-5f * liveRms) {
        break; // no better fit exists
      }
    }

    // Apply the winning fit to all positions and (rotation only) normals.
    for (HairMaskVec3& p : mask.positions) {
      p = best.apply(p);
    }
    for (HairMaskVec3& n : mask.normals) {
      n = best.rotate(n);
      const float len = std::sqrt(lengthSq(n));
      if (len > 1e-6f) {
        n.x /= len; n.y /= len; n.z /= len;
      }
    }

    // Full residual statistics with the fit applied.
    float maxResidual = 0.0f;
    distances.clear();
    for (size_t i = 0; i < mask.positions.size(); i += sampleStep) {
      float d = 0.0f;
      liveGrid.nearest(mask.positions[i], &d);
      distances.push_back(d);
      maxResidual = std::max(maxResidual, d);
    }
    std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());

    mask.aligned = true;
    mask.alignmentName = bestName;
    mask.alignmentScale = static_cast<float>(best.s);
    mask.medianResidual = distances.empty() ? -1.0f : distances[distances.size() / 2];
    mask.maxResidual = maxResidual;
    mask.liveRmsRadius = liveRms;
    // A mask that belongs to this mesh converges to ~zero; anything beyond a
    // small fraction of the mesh size means it is the wrong mesh or a broken
    // export, and scattering on it would misplace fur. The spread ratio
    // guards the residual: a mask collapsed deep inside (or blown far
    // beyond) the surface cloud can sit near many vertices without covering
    // anything.
    const double transformedSpread = best.s * maskRms;
    const bool spreadSane = transformedSpread > 0.1 * liveRms && transformedSpread < 10.0 * liveRms;
    mask.wellFitted = spreadSane && mask.medianResidual >= 0.0f && mask.medianResidual <= 0.02f * liveRms;
    mask.status += "; aligned (seed " + std::string(bestName)
                 + ", scale " + std::to_string(mask.alignmentScale)
                 + "), median residual " + std::to_string(mask.medianResidual)
                 + ", max " + std::to_string(mask.maxResidual)
                 + " (mesh RMS radius " + std::to_string(liveRms) + ")"
                 + (mask.wellFitted ? "" : "; POOR FIT - mask ignored for this mesh");
  }

} // namespace dxvk
