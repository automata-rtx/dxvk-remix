/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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
#include "rtx_hair_test.h"

#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_objects.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_context.h"
#include "rtx_scene_manager.h"
#include "rtx_options.h"
#include "rtx_imgui.h"
#include "../util/util_fast_cache.h"
#include "../util/util_once.h"

// RTX Character Rendering SDK Geometry Library (vendored): curve segment ->
// LSS / DOTS conversion.
//
// Note: the library builds a float vector from small integer masks in
// perpStark(), which MSVC reports as a lossy conversion and this project
// promotes to an error. Suppressed around the include rather than patched in
// place, so the vendored headers stay byte-identical to the SDK and can be
// updated without carrying local edits.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244) // conversion from 'uint32_t' to 'const float', possible loss of data
#endif
#include "rtxcr_geometry/CurveTessellation.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace dxvk {

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    // DOTS emits 4 triangles (12 vertices) per curve segment.
    constexpr uint32_t kDotsVerticesPerSegment = 4 * 3;

    // The RTXCR DOTS converter widens fibers by this factor to compensate the
    // volume lost when approximating a circular tube with crossed quads. The
    // hair test pre-divides radii fed to DOTS-based geometry so its silhouette
    // matches the Linear Swept Sphere silhouette the SDK targets.
    const float kDotsVolumeCompensationScale =
      1.0f / (std::sin(rtxcr::geometry::math::kPi / 4.0f) / (rtxcr::geometry::math::kPi / 4.0f));

    // Deterministic per-strand random stream.
    struct StrandRng {
      std::mt19937 generator;
      std::uniform_real_distribution<float> unitDistribution { 0.0f, 1.0f };

      explicit StrandRng(uint32_t seed) : generator(seed) { }

      float next() {
        return unitDistribution(generator);
      }
    };
  }

  RtxHairTest::RtxHairTest(DxvkDevice* device) : m_device(device) {
  }

  void RtxHairTest::prepareFrame(RtxContext* ctx) {
    if (!enable()) {
      releaseSurfaceHair();
      return;
    }

    ScopedCpuProfileZone();

    // Grow and submit hair for this frame's hair-tagged draws.
    submitSurfaceHairDraws(ctx);
  }

  void RtxHairTest::onDrawSubmitted(const DrawCallState& input) {
    // Note: called for every draw the scene manager accepts; must stay cheap.
    if (!enable() || m_submittingHairDraws) {
      return;
    }

    const XXH64_hash_t textureHash = input.getMaterialData().getColorTexture().getImageHash();

    if (textureHash == kEmptyHash || !lookupHash(RtxOptions::hairStrandTextures(), textureHash)) {
      return;
    }

    // Bound the queue so a pathological tagging choice (e.g. a texture shared
    // by hundreds of draws) cannot grow hair without limit. A single GX
    // character arrives as dozens of shape-packet draws (the wolf is ~54), so
    // the cap leaves room for a couple of tagged characters.
    constexpr size_t kMaxTaggedDrawsPerFrame = 128;
    if (m_taggedDrawQueue.size() >= kMaxTaggedDrawsPerFrame) {
      ONCE(Logger::warn("[Hair Test] More than 128 hair-tagged draws in a frame; ignoring the rest."));
      return;
    }

    m_taggedDrawQueue.push_back(input);
  }

  // Identity of a tagged mesh for hair caching: the rest-pose vertex position
  // hash. Aurora submits characters as rest-pose vertices with GPU-side
  // skinning precisely so these hashes are frame-stable (see dusklight's
  // gpu_skinning_and_platform_direction.md #4), and a GX character arrives as
  // many shape packets - each packet is its own mesh to Remix, with its own
  // hash and its own per-draw bone palette. The hash is the only key that is
  // both stable and unique per packet: a texture-based key collides across
  // same-sized packets of one character, which submits hair grown on one
  // packet (whose cluster bone indices only mean something against that
  // packet's palette) under every colliding packet's transform - fur floating
  // disconnected around the model.
  static XXH64_hash_t computeSurfaceHairCacheKey(const DrawCallState& source, XXH64_hash_t paramsHash) {
    return source.getGeometryData().hashes[HashComponents::VertexPosition] ^ paramsHash;
  }

  XXH64_hash_t RtxHairTest::computeSurfaceHairParamsHash() const {
    struct SurfaceParameters {
      int strandCount;
      float hairLength;
      float strandRadius;
      int segmentsPerStrand;
      float hairLengthJitter;
      float tipRadiusScale;
      float frizz;
      float curliness;
      float curlTurns;
      float gravityDroop;
      int scatterSeed;
      int attachmentMode;
      int evenScatterMode;
      float hybridRadiusScale;
      int hybridSeamStrands;
      int maskMirror;
      float occlusion;
    } parameters = {
      surfaceStrandCount(),
      surfaceHairLength(),
      surfaceStrandRadius(),
      segmentsPerStrand(),
      hairLengthJitter(),
      tipRadiusScale(),
      frizz(),
      curliness(),
      curlTurns(),
      gravityDroop(),
      scatterSeed(),
      surfaceAttachmentMode(),
      evenScatter() ? 1 : 0,
      hybridClusterRadiusScale(),
      hybridSeamStrandCount(),
      maskMirrorMode(),
      strandOcclusion(),
    };

    return XXH64(&parameters, sizeof(parameters), 0x48414952u);
  }

  std::atomic<bool> RtxHairTest::s_wantsSourceSnapshot { false };

  void RtxHairTest::reloadHairMasks() {
    // Masks are re-read from disk and all hair regrows against them.
    m_hairMasks.clear();
    m_surfaceHair.clear();
    m_surfaceStrandsLive = 0;
  }

  void RtxHairTest::releaseSurfaceHair() {
    m_surfaceHair.clear();
    m_surfaceStrandsLive = 0;
    m_taggedDrawQueue.clear();
    s_wantsSourceSnapshot.store(false, std::memory_order_relaxed);
  }

  void RtxHairTest::submitSurfaceHairDraws(RtxContext* ctx) {
    // Changing any strand parameter regrows all surface hair.
    const XXH64_hash_t paramsHash = computeSurfaceHairParamsHash();
    if (paramsHash != m_surfaceHairParamsHash) {
      m_surfaceHair.clear();
      m_surfaceStrandsLive = 0;
      m_surfaceHairParamsHash = paramsHash;
    }

    const uint32_t currentFrame = m_device->getCurrentFrameId();

    // Evict hair whose source mesh has not been drawn for a while (model
    // despawned or texture untagged) so its memory and strand budget return.
    constexpr uint32_t kEvictAfterFrames = 300;
    for (auto it = m_surfaceHair.begin(); it != m_surfaceHair.end();) {
      if (it->second.lastSeenFrame + kEvictAfterFrames < currentFrame) {
        m_surfaceStrandsLive -= std::min(m_surfaceStrandsLive, it->second.strandCount);
        it = m_surfaceHair.erase(it);
      } else {
        ++it;
      }
    }

    if (m_taggedDrawQueue.empty()) {
      s_wantsSourceSnapshot.store(false, std::memory_order_relaxed);
      return;
    }

    ScopedCpuProfileZone();

    // Set while any tagged mesh still needs growing, so the D3D9 capture
    // routes the next frame's tagged draws into dedicated snapshot buffers
    // this pass can safely read (see wantsSourceSnapshot()).
    bool wantSnapshots = false;

    // Pass 1: split this frame's tagged draws into meshes that already have
    // hair and new meshes, measuring the new ones for the area split below.
    struct NewMesh {
      size_t queueIndex;
      XXH64_hash_t cacheKey;
      float area;
    };
    std::vector<NewMesh> newMeshes;
    std::vector<size_t> readyDraws;
    float newMeshTotalArea = 0.0f;

    for (size_t queueIndex = 0; queueIndex < m_taggedDrawQueue.size(); ++queueIndex) {
      const DrawCallState& source = m_taggedDrawQueue[queueIndex];
      const XXH64_hash_t cacheKey = computeSurfaceHairCacheKey(source, paramsHash);

      auto it = m_surfaceHair.find(cacheKey);
      if (it != m_surfaceHair.end()) {
        it->second.lastSeenFrame = currentFrame;
        if (!it->second.buildFailed) {
          readyDraws.push_back(queueIndex);
        }
        continue;
      }

      // Only grow from draws whose data was snapshot-captured: the default
      // capture references ring memory that later draws in the frame rewrite,
      // so reading it here would scatter strands over another mesh's bytes.
      // Requesting snapshots makes the next frame's tagged draws buildable.
      if (!source.capturedForHairSnapshot) {
        wantSnapshots = true;
        continue;
      }

      // The same mesh can be drawn several times in one frame; measure once.
      bool alreadyQueued = false;
      for (const NewMesh& queued : newMeshes) {
        if (queued.cacheKey == cacheKey) {
          alreadyQueued = true;
          break;
        }
      }
      if (alreadyQueued) {
        continue;
      }

      const float area = measureSurfaceArea(source);
      if (!(area > 0.0f)) {
        // Unreadable or degenerate mesh: cache the failure so it is not
        // re-measured every frame.
        SurfaceHairEntry entry;
        entry.buildFailed = true;
        entry.lastSeenFrame = currentFrame;
        m_surfaceHair.emplace(cacheKey, std::move(entry));
        continue;
      }

      newMeshes.push_back({ queueIndex, cacheKey, area });
      newMeshTotalArea += area;
    }

    // Pass 2: grow new meshes largest-first, splitting the remaining strand
    // budget by surface area. Bounded per frame so a burst of tagged meshes
    // (a character's submeshes all arrive in one frame) grows over a few
    // frames instead of stalling one frame for seconds.
    if (!newMeshes.empty() && newMeshTotalArea > 0.0f) {
      std::sort(newMeshes.begin(), newMeshes.end(),
                [](const NewMesh& a, const NewMesh& b) { return a.area > b.area; });

      const uint32_t budget = static_cast<uint32_t>(std::max(surfaceStrandCount(), 1));
      uint32_t remaining = budget > m_surfaceStrandsLive ? budget - m_surfaceStrandsLive : 0u;
      const uint32_t distributable = remaining;

      constexpr uint32_t kMaxStrandsGrownPerFrame = 30000;
      uint32_t grownThisFrame = 0;

      for (const NewMesh& mesh : newMeshes) {
        if (remaining == 0) {
          ONCE(Logger::info("[Hair Test] Surface hair strand budget is exhausted; remaining tagged meshes stay bare. "
                            "Raise the total strand budget to cover them."));
          break;
        }
        if (grownThisFrame >= kMaxStrandsGrownPerFrame) {
          // Amortize: the remaining meshes grow over the following frames,
          // which need fresh snapshots.
          wantSnapshots = true;
          break;
        }

        const double areaShare = static_cast<double>(mesh.area) / static_cast<double>(newMeshTotalArea);
        const uint32_t share = static_cast<uint32_t>(static_cast<double>(distributable) * areaShare + 0.5);
        const uint32_t strandsForMesh = std::min(std::max(share, 1u), remaining);

        SurfaceHairEntry entry;
        entry.lastSeenFrame = currentFrame;
        if (!buildSurfaceHairGeometry(m_taggedDrawQueue[mesh.queueIndex], mesh.cacheKey, strandsForMesh, entry)) {
          entry.buildFailed = true;
        } else {
          m_surfaceStrandsLive += entry.strandCount;
          remaining -= std::min(remaining, entry.strandCount);
          grownThisFrame += entry.strandCount;
          readyDraws.push_back(mesh.queueIndex);
        }
        m_surfaceHair.emplace(mesh.cacheKey, std::move(entry));
      }
    }

    s_wantsSourceSnapshot.store(wantSnapshots, std::memory_order_relaxed);

    // Pass 3: submit hair draws for every tagged draw whose mesh has hair.
    // Guard: the hair draws carry the tagged texture themselves and must not
    // spawn hair recursively.
    m_submittingHairDraws = true;

    for (const size_t queueIndex : readyDraws) {
      const DrawCallState& source = m_taggedDrawQueue[queueIndex];
      const XXH64_hash_t cacheKey = computeSurfaceHairCacheKey(source, paramsHash);

      const auto it = m_surfaceHair.find(cacheKey);
      if (it != m_surfaceHair.end() && !it->second.buildFailed) {
        submitHairForEntry(ctx, source, it->second);
      }
    }

    m_submittingHairDraws = false;
    m_taggedDrawQueue.clear();
  }

  void RtxHairTest::submitHairForEntry(RtxContext* ctx, const DrawCallState& source, const SurfaceHairEntry& entry) {
    // Hybrid seam set: strands covering the areas outside the rigid
    // clusters' core regions, submitted as a skinned draw exactly like
    // attachment mode 1 - it carries the source's blend data and deforms
    // through Remix's skinning in its own BLAS, fully separate from the
    // static cluster BLASes.
    if (entry.hasSeamSet) {
      DrawCallState seamDraw = source;
      seamDraw.modifyGeometryData() = entry.geometry;
      seamDraw.modifyCategoryFlags().set(InstanceCategories::IgnoreOpacityMicromap);

      ctx->getSceneManager().submitDrawState(ctx, seamDraw, nullptr);
    }

    if (!entry.rigidClusters) {
      // Skinned mode: the hair draw is the source draw with its geometry
      // swapped for the grown strands. Material (the source's diffuse,
      // sampled at each strand's root UV), transforms, and the bone
      // matrices all carry over, so Remix's own skinning pipeline deforms
      // the hair with the character.
      DrawCallState hairDraw = source;
      hairDraw.modifyGeometryData() = entry.geometry;
      // Strands are solid; without this the source material's alpha test
      // makes every hair triangle an opacity micromap candidate, and tens of
      // millions of strand triangles overwhelm the OMM budget instantly.
      hairDraw.modifyCategoryFlags().set(InstanceCategories::IgnoreOpacityMicromap);

      ctx->getSceneManager().submitDrawState(ctx, hairDraw, nullptr);
      return;
    }

    // Rigid mode: each cluster is a static mesh carried by its dominant
    // bone. The per-frame transform composes the source draw's transform
    // with the bone's current palette matrix (the same matrices the game
    // submits for skinning this mesh), so the fur follows the animation
    // with zero per-frame geometry work - and, since each cluster is an
    // ordinary moving instance, Remix derives its motion vectors for free.
    const SkinningData& skinning = source.getSkinningState();

    for (const SurfaceHairCluster& cluster : entry.clusters) {
      Matrix4 boneMatrix;

      if (cluster.boneIndex != kNoBone) {
        if (cluster.boneIndex < skinning.pBoneMatrices.size()) {
          boneMatrix = skinning.pBoneMatrices[cluster.boneIndex];
        } else {
          ONCE(Logger::warn(str::format("[Hair Test] Cluster bone ", cluster.boneIndex,
                                        " is outside the draw's bone palette (", skinning.pBoneMatrices.size(),
                                        "); cluster follows the draw transform.")));
        }
      }

      DrawCallState hairDraw = source;
      hairDraw.modifyGeometryData() = cluster.geometry;
      hairDraw.modifyCategoryFlags().set(InstanceCategories::IgnoreOpacityMicromap);

      // The copied draw still carries the source's skinning state, and
      // geometry caching keys on its bone hash - left in place, every
      // animation frame would look like a new deformation and force a full
      // geometry re-process and BLAS rebuild of every cluster (the exact
      // per-frame cost rigid clusters exist to avoid). The clusters carry no
      // blend data, so the skinning state is dead weight either way.
      hairDraw.clearSkinningState();

      DrawCallTransforms& transforms = hairDraw.modifyTransformData();
      transforms.objectToWorld = source.getTransformData().objectToWorld * boneMatrix;
      transforms.objectToView = source.getTransformData().worldToView * transforms.objectToWorld;

      ctx->getSceneManager().submitDrawState(ctx, hairDraw, nullptr);
    }
  }

  // Total triangle area of a tagged mesh, used to split the strand budget
  // across meshes. Returns 0 for meshes hair cannot grow on (unreadable
  // buffers or an unsupported layout), mirroring buildSurfaceHairGeometry's
  // guards so a mesh that measures positive also builds.
  float RtxHairTest::measureSurfaceArea(const DrawCallState& input) {
    using namespace rtxcr::geometry;

    const RasterGeometry& source = input.getGeometryData();

    const VkFormat positionFormat = source.positionBuffer.vertexFormat();
    if (positionFormat != VK_FORMAT_R32G32B32_SFLOAT && positionFormat != VK_FORMAT_R32G32B32A32_SFLOAT) {
      Logger::warn(str::format("[Hair Test] Tagged mesh has unsupported position format ", positionFormat, "; no hair grown."));
      return 0.0f;
    }

    if (source.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && source.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) {
      Logger::warn(str::format("[Hair Test] Tagged mesh has unsupported topology ", source.topology, "; no hair grown."));
      return 0.0f;
    }

    // Snapshot-captured draws own their buffers: the bytes are written once on
    // the CPU at capture time and the GPU only ever reads them, so a
    // concurrent CPU read is always safe. isPendingGpuWrite() still reports
    // true while any consuming pass is in flight, because dxvk tracks every
    // storage-buffer descriptor as a write - read-only StructuredBuffer
    // inputs included (dxvk_context.cpp, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
    // - and the hair pass runs before the frame's geometry passes retire.
    // Left in place, that false positive blocked growth on every frame.
    // Non-snapshot sources keep the guard: for shared ring memory, in-flight
    // use really does mean the bytes may be rewritten under the read.
    const uint8_t* pPositions = static_cast<const uint8_t*>(source.positionBuffer.mapPtr(source.positionBuffer.offsetFromSlice()));
    if (pPositions == nullptr || (!input.capturedForHairSnapshot && source.positionBuffer.isPendingGpuWrite())) {
      Logger::warn("[Hair Test] Tagged mesh vertex data is not CPU-readable; no hair grown.");
      return 0.0f;
    }
    const uint32_t positionStride = source.positionBuffer.stride();

    const uint16_t* pIndices16 = nullptr;
    const uint32_t* pIndices32 = nullptr;
    uint32_t indexCount = 0;

    if (source.indexBuffer.defined() && source.indexCount > 0) {
      const void* pIndexData = source.indexBuffer.mapPtr(source.indexBuffer.offsetFromSlice());
      if (pIndexData == nullptr) {
        Logger::warn("[Hair Test] Tagged mesh index data is not CPU-readable; no hair grown.");
        return 0.0f;
      }
      if (source.indexBuffer.indexType() == VK_INDEX_TYPE_UINT16) {
        pIndices16 = static_cast<const uint16_t*>(pIndexData);
      } else {
        pIndices32 = static_cast<const uint32_t*>(pIndexData);
      }
      indexCount = source.indexCount;
    } else {
      indexCount = source.vertexCount;
    }

    const auto readIndex = [&](uint32_t i) -> uint32_t {
      if (pIndices16 != nullptr) {
        return pIndices16[i];
      }
      if (pIndices32 != nullptr) {
        return pIndices32[i];
      }
      return i;
    };

    const auto readPosition = [&](uint32_t vertexIndex) {
      math::float3 result;
      std::memcpy(&result, pPositions + static_cast<size_t>(vertexIndex) * positionStride, 3 * sizeof(float));
      return result;
    };

    const bool isStrip = source.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    const uint32_t triangleCount = isStrip
      ? (indexCount >= 3 ? indexCount - 2 : 0)
      : indexCount / 3;

    float totalArea = 0.0f;

    for (uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
      uint32_t i0, i1, i2;
      if (isStrip) {
        i0 = readIndex(triangleIndex);
        i1 = readIndex(triangleIndex + 1);
        i2 = readIndex(triangleIndex + 2);
      } else {
        i0 = readIndex(triangleIndex * 3);
        i1 = readIndex(triangleIndex * 3 + 1);
        i2 = readIndex(triangleIndex * 3 + 2);
      }

      if (i0 >= source.vertexCount || i1 >= source.vertexCount || i2 >= source.vertexCount ||
          i0 == i1 || i1 == i2 || i0 == i2) {
        continue;
      }

      const math::float3 p0 = readPosition(i0);
      const math::float3 p1 = readPosition(i1);
      const math::float3 p2 = readPosition(i2);
      const math::float3 crossProduct = math::cross(p1 - p0, p2 - p0);
      const float area = 0.5f * std::sqrt(math::dot(crossProduct, crossProduct));

      if (!(area > 1e-10f) || !std::isfinite(area)) {
        continue;
      }

      totalArea += area;
    }

    return totalArea;
  }

  bool RtxHairTest::buildSurfaceHairGeometry(const DrawCallState& input, XXH64_hash_t cacheKey, uint32_t strandCount, SurfaceHairEntry& entry) {
    using namespace rtxcr::geometry;

    const RasterGeometry& source = input.getGeometryData();

    // Only float3 positions and triangle lists/strips are supported; that
    // covers the fixed-function geometry this fork cares about.
    const VkFormat positionFormat = source.positionBuffer.vertexFormat();
    if (positionFormat != VK_FORMAT_R32G32B32_SFLOAT && positionFormat != VK_FORMAT_R32G32B32A32_SFLOAT) {
      Logger::warn(str::format("[Hair Test] Tagged mesh has unsupported position format ", positionFormat, "; no hair grown."));
      return false;
    }

    if (source.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && source.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) {
      Logger::warn(str::format("[Hair Test] Tagged mesh has unsupported topology ", source.topology, "; no hair grown."));
      return false;
    }

    // Same snapshot bypass as measureSurfaceArea: dxvk's conservative
    // storage-descriptor tracking makes isPendingGpuWrite() a false positive
    // for the draw-owned snapshot buffers, whose bytes are immutable after
    // the capture-time CPU write.
    const uint8_t* pPositions = static_cast<const uint8_t*>(source.positionBuffer.mapPtr(source.positionBuffer.offsetFromSlice()));
    if (pPositions == nullptr || (!input.capturedForHairSnapshot && source.positionBuffer.isPendingGpuWrite())) {
      Logger::warn("[Hair Test] Tagged mesh vertex data is not CPU-readable; no hair grown.");
      return false;
    }
    const uint32_t positionStride = source.positionBuffer.stride();

    const auto readPosition = [&](uint32_t vertexIndex) {
      math::float3 result;
      std::memcpy(&result, pPositions + static_cast<size_t>(vertexIndex) * positionStride, 3 * sizeof(float));
      return result;
    };

    // Optional per-vertex attributes.
    const uint8_t* pNormals = nullptr;
    uint32_t normalStride = 0;
    if (source.normalBuffer.defined() && source.normalBuffer.vertexFormat() != VK_FORMAT_R32_UINT) {
      pNormals = static_cast<const uint8_t*>(source.normalBuffer.mapPtr(source.normalBuffer.offsetFromSlice()));
      normalStride = source.normalBuffer.stride();
    }

    const uint8_t* pTexcoords = nullptr;
    uint32_t texcoordStride = 0;
    if (source.texcoordBuffer.defined()) {
      pTexcoords = static_cast<const uint8_t*>(source.texcoordBuffer.mapPtr(source.texcoordBuffer.offsetFromSlice()));
      texcoordStride = source.texcoordBuffer.stride();
    }

    const uint8_t* pBlendWeights = nullptr;
    uint32_t blendWeightStride = 0;
    if (source.blendWeightBuffer.defined()) {
      pBlendWeights = static_cast<const uint8_t*>(source.blendWeightBuffer.mapPtr(source.blendWeightBuffer.offsetFromSlice()));
      blendWeightStride = source.blendWeightBuffer.stride();
    }

    const uint8_t* pBlendIndices = nullptr;
    uint32_t blendIndicesStride = 0;
    if (source.blendIndicesBuffer.defined()) {
      pBlendIndices = static_cast<const uint8_t*>(source.blendIndicesBuffer.mapPtr(source.blendIndicesBuffer.offsetFromSlice()));
      blendIndicesStride = source.blendIndicesBuffer.stride();
    }

    // Resolve the triangle list (indexed or not, list or strip).
    const uint16_t* pIndices16 = nullptr;
    const uint32_t* pIndices32 = nullptr;
    uint32_t indexCount = 0;

    if (source.indexBuffer.defined() && source.indexCount > 0) {
      const void* pIndexData = source.indexBuffer.mapPtr(source.indexBuffer.offsetFromSlice());
      if (pIndexData == nullptr) {
        Logger::warn("[Hair Test] Tagged mesh index data is not CPU-readable; no hair grown.");
        return false;
      }
      if (source.indexBuffer.indexType() == VK_INDEX_TYPE_UINT16) {
        pIndices16 = static_cast<const uint16_t*>(pIndexData);
      } else {
        pIndices32 = static_cast<const uint32_t*>(pIndexData);
      }
      indexCount = source.indexCount;
    } else {
      indexCount = source.vertexCount;
    }

    const auto readIndex = [&](uint32_t i) -> uint32_t {
      if (pIndices16 != nullptr) {
        return pIndices16[i];
      }
      if (pIndices32 != nullptr) {
        return pIndices32[i];
      }
      return i;
    };

    struct SourceTriangle {
      uint32_t indices[3];
      float cumulativeArea;
    };
    std::vector<SourceTriangle> triangles;

    const bool isStrip = source.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    const uint32_t triangleCount = isStrip
      ? (indexCount >= 3 ? indexCount - 2 : 0)
      : indexCount / 3;
    triangles.reserve(triangleCount);

    float totalArea = 0.0f;

    for (uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
      uint32_t i0, i1, i2;
      if (isStrip) {
        i0 = readIndex(triangleIndex);
        i1 = readIndex(triangleIndex + 1);
        i2 = readIndex(triangleIndex + 2);
      } else {
        i0 = readIndex(triangleIndex * 3);
        i1 = readIndex(triangleIndex * 3 + 1);
        i2 = readIndex(triangleIndex * 3 + 2);
      }

      if (i0 >= source.vertexCount || i1 >= source.vertexCount || i2 >= source.vertexCount ||
          i0 == i1 || i1 == i2 || i0 == i2) {
        continue;
      }

      const math::float3 p0 = readPosition(i0);
      const math::float3 p1 = readPosition(i1);
      const math::float3 p2 = readPosition(i2);
      const math::float3 crossProduct = math::cross(p1 - p0, p2 - p0);
      const float area = 0.5f * std::sqrt(math::dot(crossProduct, crossProduct));

      if (!(area > 1e-10f) || !std::isfinite(area)) {
        continue;
      }

      totalArea += area;
      triangles.push_back({ { i0, i1, i2 }, totalArea });
    }

    if (triangles.empty()) {
      Logger::warn("[Hair Test] Tagged mesh has no usable triangles; no hair grown.");
      return false;
    }

    // Blend-stream availability, needed before scattering for the hybrid
    // partition and the dominant-bone decode.
    const bool sourceHasBlend = pBlendWeights != nullptr && source.numBonesPerVertex > 0;

    // Decodes the dominant (highest-weight) bone at a source vertex, using
    // the same conventions the skinning shader reads: numBones-1 stored
    // weights with an implicit last, and raw index bytes in memory order.
    const auto decodeDominantBone = [&](uint32_t vertexIndex) -> uint32_t {
      const uint32_t bonesPerVertex = std::min(source.numBonesPerVertex, 4u);

      if (bonesPerVertex == 0 || pBlendWeights == nullptr) {
        return kNoBone;
      }

      float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      float lastWeight = 1.0f;
      for (uint32_t i = 0; i + 1 < bonesPerVertex; ++i) {
        float weight;
        std::memcpy(&weight, pBlendWeights + static_cast<size_t>(vertexIndex) * blendWeightStride + i * sizeof(float), sizeof(float));
        weights[i] = weight;
        lastWeight -= weight;
      }
      weights[bonesPerVertex - 1] = lastWeight;

      uint32_t best = 0;
      for (uint32_t i = 1; i < bonesPerVertex; ++i) {
        if (weights[i] > weights[best]) {
          best = i;
        }
      }

      if (pBlendIndices == nullptr) {
        // No index buffer: weights address the bone palette in order.
        return best;
      }

      uint8_t boneIndices[4] = { 0, 0, 0, 0 };
      std::memcpy(boneIndices, pBlendIndices + static_cast<size_t>(vertexIndex) * blendIndicesStride, sizeof(boneIndices));

      return boneIndices[best];
    };

    // Attachment mode, with hybrid degrading to rigid on unskinned sources
    // (there are no bones to seam between).
    int attachmentMode = surfaceAttachmentMode();
    if (attachmentMode == 2 && !sourceHasBlend) {
      ONCE(Logger::info("[Hair Test] Hybrid attachment on an unskinned mesh behaves as rigid (no bones to seam)."));
      attachmentMode = 0;
    }

    // Optional artist scatter mask for this texture (see rtx_hair_mask.h):
    // an edited rest-pose copy of the mesh whose triangles replace the live
    // surface as the scatter domain. Roots bind back to the nearest live
    // vertex for bone, UV and fallback normal data.
    const XXH64_hash_t maskTextureHash = input.getMaterialData().getColorTexture().getImageHash();
    std::vector<HairMaskVec3> livePositions(source.vertexCount);
    for (uint32_t v = 0; v < source.vertexCount; ++v) {
      const math::float3 p = readPosition(v);
      livePositions[v] = { p.x, p.y, p.z };
    }

    HairPointGrid liveGrid;
    HairMaskMesh alignedMask;
    const HairMaskMesh* mask = nullptr;
    {
      auto maskIt = m_hairMasks.find(maskTextureHash);
      if (maskIt == m_hairMasks.end()) {
        const std::string maskPath = maskDirectory() + "/" + hashToString(maskTextureHash) + ".obj";
        maskIt = m_hairMasks.emplace(maskTextureHash, loadHairMaskObj(maskPath)).first;
        Logger::info(str::format("[Hair Test] Scatter mask ", maskPath, ": ", maskIt->second.status));
      }
      if (maskIt->second.loaded) {
        liveGrid.build(livePositions.data(), livePositions.size());
        // Aligned per mesh, on a copy: several meshes can share one tagged
        // texture and therefore one mask file (a character plus a small
        // extra piece), and one similarity transform cannot fit both. The
        // shared entry stays in file space; each build fits its own copy
        // against its own live vertices, and a mesh the mask does not
        // belong to (poor converged fit) scatters on its full surface
        // instead of wearing a misplaced mask.
        alignedMask = maskIt->second;
        alignHairMaskToLiveMesh(alignedMask, liveGrid, static_cast<HairMaskMirrorMode>(maskMirrorMode()));
        Logger::info(str::format("[Hair Test] Scatter mask: ", alignedMask.status));
        if (alignedMask.wellFitted) {
          mask = &alignedMask;
        } else {
          Logger::warn("[Hair Test] Scatter mask does not fit this mesh; falling back to the full surface.");
        }
        // The overlay shows one status line per mask file: keep the best
        // fit seen (the mesh the mask was authored for), or the latest
        // attempt while none has fitted yet. The shared entry's status
        // string stays load-only - each build copies it as the base of its
        // own alignment report - so only the numeric fields are mirrored.
        if (alignedMask.wellFitted || !maskIt->second.aligned) {
          maskIt->second.aligned = alignedMask.aligned;
          maskIt->second.mirrored = alignedMask.mirrored;
          maskIt->second.wellFitted = alignedMask.wellFitted;
          maskIt->second.alignmentName = alignedMask.alignmentName;
          maskIt->second.alignmentScale = alignedMask.alignmentScale;
          maskIt->second.medianResidual = alignedMask.medianResidual;
          maskIt->second.maxResidual = alignedMask.maxResidual;
          maskIt->second.liveRmsRadius = alignedMask.liveRmsRadius;
        }
      }
    }

    // Scatter domain: the mask's triangles when present, the live surface
    // otherwise, as one cumulative-area distribution.
    struct ScatterTri {
      uint32_t indices[3];
      float cumulativeArea;
    };
    std::vector<ScatterTri> scatterTris;
    float scatterArea = 0.0f;

    const auto scatterPosition = [&](uint32_t index) -> math::float3 {
      if (mask != nullptr) {
        const HairMaskVec3& p = mask->positions[index];
        return math::float3(p.x, p.y, p.z);
      }
      return readPosition(index);
    };

    if (mask != nullptr) {
      scatterTris.reserve(mask->triangles.size());
      for (const HairMaskMesh::Triangle& tri : mask->triangles) {
        const math::float3 p0 = scatterPosition(tri.v[0]);
        const math::float3 p1 = scatterPosition(tri.v[1]);
        const math::float3 p2 = scatterPosition(tri.v[2]);
        const math::float3 crossProduct = math::cross(p1 - p0, p2 - p0);
        const float area = 0.5f * std::sqrt(math::dot(crossProduct, crossProduct));
        if (!(area > 1e-10f) || !std::isfinite(area)) {
          continue;
        }
        scatterArea += area;
        scatterTris.push_back({ { tri.v[0], tri.v[1], tri.v[2] }, scatterArea });
      }
      if (scatterTris.empty()) {
        Logger::warn("[Hair Test] Scatter mask has no usable triangles; falling back to the full surface.");
        mask = nullptr;
      }
    }
    if (mask == nullptr) {
      scatterTris.reserve(triangles.size());
      for (const SourceTriangle& tri : triangles) {
        scatterTris.push_back({ { tri.indices[0], tri.indices[1], tri.indices[2] }, tri.cumulativeArea });
      }
      scatterArea = totalArea;
    }

    // Hybrid: per-bone influence regions from the live mesh. The region
    // radius is the RMS distance of the bone's vertices from their centroid,
    // so the rigid-core cutoff scales with how much surface each bone drives.
    struct BoneRegion {
      math::float3 centroid;
      float radius;
    };
    std::unordered_map<uint32_t, BoneRegion> boneRegions;
    if (attachmentMode == 2) {
      struct BoneAccum {
        double sum[3] = { 0.0, 0.0, 0.0 };
        double sumSq = 0.0;
        uint64_t count = 0;
      };
      std::unordered_map<uint32_t, BoneAccum> boneAccums;
      for (uint32_t v = 0; v < source.vertexCount; ++v) {
        BoneAccum& acc = boneAccums[decodeDominantBone(v)];
        const HairMaskVec3& p = livePositions[v];
        acc.sum[0] += p.x; acc.sum[1] += p.y; acc.sum[2] += p.z;
        acc.sumSq += static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y + static_cast<double>(p.z) * p.z;
        ++acc.count;
      }
      for (const auto& [bone, acc] : boneAccums) {
        const double inv = 1.0 / static_cast<double>(acc.count);
        const math::float3 centroid(static_cast<float>(acc.sum[0] * inv),
                                    static_cast<float>(acc.sum[1] * inv),
                                    static_cast<float>(acc.sum[2] * inv));
        const double variance = acc.sumSq * inv
          - (static_cast<double>(centroid.x) * centroid.x + static_cast<double>(centroid.y) * centroid.y + static_cast<double>(centroid.z) * centroid.z);
        boneRegions[bone] = { centroid, static_cast<float>(std::sqrt(std::max(variance, 0.0))) };
      }
    }

    const auto insideBoneCore = [&](const math::float3& root, uint32_t bone) -> bool {
      const auto it = boneRegions.find(bone);
      if (it == boneRegions.end()) {
        return true;
      }
      const math::float3 d = root - it->second.centroid;
      const float coreRadius = hybridClusterRadiusScale() * it->second.radius;
      return math::dot(d, d) <= coreRadius * coreRadius;
    };

    // Strand counts: `strands` primary (clusters, or the whole set outside
    // hybrid) plus the hybrid's separate seam set appended after them.
    const uint32_t strands = std::max(strandCount, 1u);
    const uint32_t seamStrands = attachmentMode == 2 ? static_cast<uint32_t>(std::max(hybridSeamStrandCount(), 0)) : 0u;
    const uint32_t totalStrands = strands + seamStrands;
    const uint32_t segmentsEach = static_cast<uint32_t>(std::clamp(segmentsPerStrand(), 1, 16));
    const uint32_t totalSegments = totalStrands * segmentsEach;

    std::vector<LineSegment> lineSegments;
    lineSegments.reserve(totalSegments);

    struct RootAttachment {
      uint32_t nearestVertex;
      // The smooth surface normal the strand grew from (the mask's authored
      // normal or the interpolated live normal) - also the strands' shading
      // normal, so fur shades with the pelt's curvature instead of the raw
      // normal of whichever vertex happened to be nearest.
      float normal[3];
    };
    std::vector<RootAttachment> rootAttachments;
    rootAttachments.reserve(totalStrands);

    StrandRng rng(static_cast<uint32_t>(scatterSeed()) ^ static_cast<uint32_t>(cacheKey));

    const math::float3 gravityDirection(0.0f, -1.0f, 0.0f);

    // Best-candidate scattering: placed roots go into a spacing grid and each
    // new root is the farthest-from-existing of several candidates.
    const bool useEvenScatter = evenScatter();
    HairPointGrid rootSpacingGrid;
    if (useEvenScatter) {
      const float meanSpacing = std::sqrt(std::max(scatterArea / static_cast<float>(totalStrands), 1e-12f));
      rootSpacingGrid.init(meanSpacing * 2.0f);
    }

    struct RootSample {
      uint32_t tri;
      float baryU, baryV, baryW;
      math::float3 root;
    };

    const auto drawRootSample = [&]() -> RootSample {
      RootSample s;
      const float areaRnd = rng.next() * scatterArea;
      const auto pick = std::lower_bound(scatterTris.begin(), scatterTris.end(), areaRnd,
        [](const ScatterTri& tri, float value) {
          return tri.cumulativeArea < value;
        });
      s.tri = static_cast<uint32_t>((pick != scatterTris.end() ? pick : scatterTris.end() - 1) - scatterTris.begin());

      const float baryRnd0 = rng.next();
      const float baryRnd1 = rng.next();
      const float sqrtRnd = std::sqrt(baryRnd0);
      s.baryU = 1.0f - sqrtRnd;
      s.baryV = baryRnd1 * sqrtRnd;
      s.baryW = 1.0f - s.baryU - s.baryV;

      const ScatterTri& tri = scatterTris[s.tri];
      s.root = scatterPosition(tri.indices[0]) * s.baryU
             + scatterPosition(tri.indices[1]) * s.baryV
             + scatterPosition(tri.indices[2]) * s.baryW;
      return s;
    };

    const auto drawSpacedSample = [&]() -> RootSample {
      if (!useEvenScatter || rootSpacingGrid.empty()) {
        return drawRootSample();
      }
      RootSample best {};
      float bestDistance = -1.0f;
      for (uint32_t candidate = 0; candidate < 4; ++candidate) {
        const RootSample s = drawRootSample();
        float d = 0.0f;
        rootSpacingGrid.nearest({ s.root.x, s.root.y, s.root.z }, &d);
        if (d > bestDistance) {
          bestDistance = d;
          best = s;
        }
      }
      return best;
    };

    // The live vertex a root binds to, providing its bone, UV and fallback
    // normal: nearest live vertex for mask scatter (the mask's triangles
    // reference mask vertices), the barycentric-dominant corner otherwise.
    const auto bindRoot = [&](const RootSample& s) -> uint32_t {
      if (mask != nullptr) {
        const int32_t nearestIndex = liveGrid.nearest({ s.root.x, s.root.y, s.root.z });
        return nearestIndex >= 0 ? static_cast<uint32_t>(nearestIndex) : 0u;
      }
      const ScatterTri& tri = scatterTris[s.tri];
      uint32_t nearestVertex = tri.indices[0];
      if (s.baryV > s.baryU && s.baryV >= s.baryW) {
        nearestVertex = tri.indices[1];
      } else if (s.baryW > s.baryU && s.baryW > s.baryV) {
        nearestVertex = tri.indices[2];
      }
      return nearestVertex;
    };

    for (uint32_t strandIndex = 0; strandIndex < totalStrands; ++strandIndex) {
      const bool isSeamStrand = strandIndex >= strands;

      // Root selection. In hybrid mode primary strands prefer roots inside
      // their bone's rigid core and seam strands prefer roots outside every
      // core; after bounded retries the last sample is accepted (primary
      // outliers ride their own bone's cluster, seam outliers stay skinned -
      // both remain correct, just less tidy).
      RootSample sample {};
      uint32_t attachVertex = 0;
      for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        sample = drawSpacedSample();
        attachVertex = bindRoot(sample);
        if (attachmentMode != 2) {
          break;
        }
        const bool inside = insideBoneCore(sample.root, decodeDominantBone(attachVertex));
        if (inside != isSeamStrand) {
          break;
        }
      }
      if (useEvenScatter) {
        rootSpacingGrid.insert({ sample.root.x, sample.root.y, sample.root.z });
      }

      const ScatterTri& triangle = scatterTris[sample.tri];
      const float baryU = sample.baryU;
      const float baryV = sample.baryV;
      const float baryW = sample.baryW;
      const math::float3 root = sample.root;

      // Surface normal. Mask scatter prefers the mask's own authored normals
      // (smooth-shaded exports give smoothly varying growth directions on
      // low-poly surfaces); without them, the bound live vertex's normal.
      // Live scatter interpolates live vertex normals, geometric fallback.
      math::float3 surfaceNormal;
      if (mask != nullptr) {
        if (!mask->normals.empty()) {
          const HairMaskVec3& n0 = mask->normals[triangle.indices[0]];
          const HairMaskVec3& n1 = mask->normals[triangle.indices[1]];
          const HairMaskVec3& n2 = mask->normals[triangle.indices[2]];
          surfaceNormal = math::float3(n0.x, n0.y, n0.z) * baryU
                        + math::float3(n1.x, n1.y, n1.z) * baryV
                        + math::float3(n2.x, n2.y, n2.z) * baryW;
        } else if (pNormals != nullptr) {
          std::memcpy(&surfaceNormal, pNormals + static_cast<size_t>(attachVertex) * normalStride, 3 * sizeof(float));
        } else {
          const math::float3 p0 = scatterPosition(triangle.indices[0]);
          const math::float3 p1 = scatterPosition(triangle.indices[1]);
          const math::float3 p2 = scatterPosition(triangle.indices[2]);
          surfaceNormal = math::cross(p1 - p0, p2 - p0);
        }
      } else if (pNormals != nullptr) {
        math::float3 n[3];
        for (uint32_t v = 0; v < 3; ++v) {
          std::memcpy(&n[v], pNormals + static_cast<size_t>(triangle.indices[v]) * normalStride, 3 * sizeof(float));
        }
        surfaceNormal = n[0] * baryU + n[1] * baryV + n[2] * baryW;
      } else {
        const math::float3 p0 = readPosition(triangle.indices[0]);
        const math::float3 p1 = readPosition(triangle.indices[1]);
        const math::float3 p2 = readPosition(triangle.indices[2]);
        surfaceNormal = math::cross(p1 - p0, p2 - p0);
      }
      const float normalLengthSq = math::dot(surfaceNormal, surfaceNormal);
      surfaceNormal = normalLengthSq > 1e-12f
        ? surfaceNormal * (1.0f / std::sqrt(normalLengthSq))
        : math::float3(0.0f, 1.0f, 0.0f);

      // Root UV: all strand vertices share it, so the source diffuse texture
      // colors the whole strand with the surface color under its root. Mask
      // scatter reads the bound live vertex's UV (the mask carries none).
      float rootUv[2] = { 0.0f, 0.0f };
      if (pTexcoords != nullptr) {
        if (mask != nullptr) {
          std::memcpy(rootUv, pTexcoords + static_cast<size_t>(attachVertex) * texcoordStride, 2 * sizeof(float));
        } else {
          float uv[3][2];
          for (uint32_t v = 0; v < 3; ++v) {
            std::memcpy(uv[v], pTexcoords + static_cast<size_t>(triangle.indices[v]) * texcoordStride, 2 * sizeof(float));
          }
          rootUv[0] = uv[0][0] * baryU + uv[1][0] * baryV + uv[2][0] * baryW;
          rootUv[1] = uv[0][1] * baryU + uv[1][1] * baryV + uv[2][1] * baryW;
        }
      }

      rootAttachments.push_back({ attachVertex, { surfaceNormal.x, surfaceNormal.y, surfaceNormal.z } });

      // Grow the strand along the jittered surface normal.
      const float jitterX = rng.next() * 2.0f - 1.0f;
      const float jitterY = rng.next() * 2.0f - 1.0f;
      const float jitterZ = rng.next() * 2.0f - 1.0f;

      const math::float3 randomOffset(jitterX, jitterY, jitterZ);
      const math::float3 jitteredNormal = surfaceNormal + randomOffset * frizz();
      const math::float3 growthDirection = math::dot(jitteredNormal, jitteredNormal) > 1e-6f
        ? math::normalize(jitteredNormal)
        : surfaceNormal;

      math::float3 curlAxisT, curlAxisB;
      math::buildFrame(growthDirection, curlAxisT, curlAxisB);

      const float strandLength = surfaceHairLength() * (1.0f - hairLengthJitter() * rng.next());
      const float curlAmplitude = curliness() * 0.25f * strandLength;
      const float curlPhase = rng.next() * math::kTwoPi;
      const float droopAmount = gravityDroop() * strandLength;

      const auto samplePoint = [&](float t) {
        const float curlAngle = curlPhase + curlTurns() * math::kTwoPi * t;
        return root
          + growthDirection * (strandLength * t)
          + gravityDirection * (droopAmount * t * t)
          + (curlAxisT * std::cos(curlAngle) + curlAxisB * std::sin(curlAngle)) * (curlAmplitude * t);
      };

      // Pre-divided by the DOTS volume compensation so the triangle
      // silhouette matches the intended strand radius.
      const auto sampleRadius = [&](float t) {
        return surfaceStrandRadius() * (1.0f + (tipRadiusScale() - 1.0f) * t) / kDotsVolumeCompensationScale;
      };

      for (uint32_t segmentIndex = 0; segmentIndex < segmentsEach; ++segmentIndex) {
        const float t0 = static_cast<float>(segmentIndex) / static_cast<float>(segmentsEach);
        const float t1 = static_cast<float>(segmentIndex + 1) / static_cast<float>(segmentsEach);

        const math::float3 sp0 = samplePoint(t0);
        const math::float3 sp1 = samplePoint(t1);

        LineSegment segment;
        segment.geometryIndex = 0;
        math::store3(segment.vertices[0].position, sp0);
        segment.vertices[0].radius = sampleRadius(t0);
        segment.vertices[0].texCoord[0] = rootUv[0];
        segment.vertices[0].texCoord[1] = rootUv[1];
        math::store3(segment.vertices[1].position, sp1);
        segment.vertices[1].radius = sampleRadius(t1);
        segment.vertices[1].texCoord[0] = rootUv[0];
        segment.vertices[1].texCoord[1] = rootUv[1];

        lineSegments.push_back(segment);
      }
    }

    // Interleaved hair vertex: position, normal (the root's surface normal,
    // so the strands shade like the surface they grow from), root UV, color.
    struct HairVertex {
      float position[3];
      float normal[3];
      float texcoord[2];
      uint32_t color;
    };
    static_assert(sizeof(HairVertex) == 36, "HairVertex layout is position/normal/uv/color");

    const bool hasBlendWeights = pBlendWeights != nullptr && source.numBonesPerVertex > 0;
    const bool hasBlendIndices = pBlendIndices != nullptr;
    const uint32_t weightsPerVertex = hasBlendWeights ? std::max<uint32_t>(source.numBonesPerVertex - 1, 1) : 0;
    const uint32_t hairWeightStride = weightsPerVertex * static_cast<uint32_t>(sizeof(float));

    // Host-visible buffers, matching the external mesh path.
    const auto allocBuffer = [this](size_t sizeInBytes, const char* name) -> Rc<DxvkBuffer> {
      DxvkBufferCreateInfo bufferInfo = {};
      bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      bufferInfo.size = align(sizeInBytes, CACHE_LINE_SIZE);

      return m_device->createBuffer(bufferInfo,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                    DxvkMemoryStats::Category::RTXBuffer, name);
    };

    // Assembles a RasterGeometry for a set of strands: gathers their curve
    // segments, tessellates to DOTS triangles via the SDK converter,
    // interleaves the vertices, and optionally attaches the source's blend
    // data (skinned mode only; rigid clusters need none).
    const auto assembleGeometry = [&](const std::vector<uint32_t>& strandList,
                                      bool includeBlendData,
                                      uint64_t hashSalt) -> RasterGeometry {
      const uint32_t clusterSegmentCount = static_cast<uint32_t>(strandList.size()) * segmentsEach;
      const uint32_t clusterVertexCount = clusterSegmentCount * kDotsVerticesPerSegment;

      std::vector<LineSegment> clusterSegments;
      clusterSegments.reserve(clusterSegmentCount);
      for (const uint32_t strandIndex : strandList) {
        for (uint32_t segmentIndex = 0; segmentIndex < segmentsEach; ++segmentIndex) {
          clusterSegments.push_back(lineSegments[static_cast<size_t>(strandIndex) * segmentsEach + segmentIndex]);
        }
      }

      std::vector<float> dotsPositionData(static_cast<size_t>(clusterVertexCount) * math::kFloatsPerPosition);
      std::vector<float> dotsTexcoordData(static_cast<size_t>(clusterVertexCount) * math::kFloatsPerTexCoord);

      convertToDisjointOrthogonalTriangleStrips(
        clusterSegments,
        clusterSegmentCount,
        nullptr,
        dotsPositionData.data(),
        nullptr,
        nullptr,
        dotsTexcoordData.data(),
        nullptr);

      std::vector<HairVertex> hairVertices(clusterVertexCount);
      std::vector<uint32_t> hairIndices(clusterVertexCount);
      std::vector<uint8_t> hairBlendWeights(includeBlendData && hasBlendWeights ? static_cast<size_t>(clusterVertexCount) * hairWeightStride : 0);
      std::vector<uint32_t> hairBlendIndices(includeBlendData && hasBlendIndices ? clusterVertexCount : 0);

      const uint32_t verticesPerStrand = segmentsEach * kDotsVerticesPerSegment;

      for (uint32_t vertexIndex = 0; vertexIndex < clusterVertexCount; ++vertexIndex) {
        HairVertex& vertex = hairVertices[vertexIndex];
        std::memcpy(vertex.position, &dotsPositionData[static_cast<size_t>(vertexIndex) * 3], 3 * sizeof(float));
        std::memcpy(vertex.texcoord, &dotsTexcoordData[static_cast<size_t>(vertexIndex) * 2], 2 * sizeof(float));

        const uint32_t localStrand = vertexIndex / verticesPerStrand;
        const RootAttachment& attachment = rootAttachments[strandList[localStrand]];
        const uint32_t attachmentVertex = attachment.nearestVertex;

        // Root-to-tip occlusion gradient in the vertex color: a real coat is
        // darkest where it is deepest, and the modulation reads as the
        // self-shadowing/ambient occlusion the path tracer cannot afford to
        // resolve between individual strands. The color multiplies the
        // strand's albedo through the source material's diffuse modulation.
        const uint32_t segmentOfVertex = (vertexIndex % verticesPerStrand) / kDotsVerticesPerSegment;
        const float strandT = (static_cast<float>(segmentOfVertex) + 0.5f) / static_cast<float>(segmentsEach);
        const float occlusionScale = 1.0f - strandOcclusion() * (1.0f - std::min(strandT, 1.0f));
        const auto occlusionByte = static_cast<uint32_t>(std::max(occlusionScale, 0.0f) * 255.0f + 0.5f);
        vertex.color = 0xFF000000u | (occlusionByte << 16) | (occlusionByte << 8) | occlusionByte;

        // Shading normal: the smooth surface normal the strand grew from.
        std::memcpy(vertex.normal, attachment.normal, 3 * sizeof(float));

        // Raw-copy the attachment vertex's blend data: the skinning shader's
        // conventions (numBones-1 weights, byte-packed indices) carry over
        // unchanged, so no format interpretation is needed.
        if (!hairBlendWeights.empty()) {
          std::memcpy(hairBlendWeights.data() + static_cast<size_t>(vertexIndex) * hairWeightStride,
                      pBlendWeights + static_cast<size_t>(attachmentVertex) * blendWeightStride,
                      hairWeightStride);
        }
        if (!hairBlendIndices.empty()) {
          std::memcpy(&hairBlendIndices[vertexIndex],
                      pBlendIndices + static_cast<size_t>(attachmentVertex) * blendIndicesStride,
                      sizeof(uint32_t));
        }

        hairIndices[vertexIndex] = vertexIndex;
      }

      Rc<DxvkBuffer> vertexBuffer = allocBuffer(hairVertices.size() * sizeof(HairVertex), "Surface Hair Vertices");
      DxvkBufferSlice vertexSlice { vertexBuffer };
      std::memcpy(vertexSlice.mapPtr(0), hairVertices.data(), hairVertices.size() * sizeof(HairVertex));

      Rc<DxvkBuffer> indexBuffer = allocBuffer(hairIndices.size() * sizeof(uint32_t), "Surface Hair Indices");
      DxvkBufferSlice indexSlice { indexBuffer };
      std::memcpy(indexSlice.mapPtr(0), hairIndices.data(), hairIndices.size() * sizeof(uint32_t));

      RasterGeometry geometry = {};
      geometry.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      geometry.cullMode = VK_CULL_MODE_NONE;
      geometry.frontFace = VK_FRONT_FACE_CLOCKWISE;
      geometry.vertexCount = clusterVertexCount;
      geometry.positionBuffer = RasterBuffer { vertexSlice, offsetof(HairVertex, position), sizeof(HairVertex), VK_FORMAT_R32G32B32_SFLOAT };
      geometry.normalBuffer = RasterBuffer { vertexSlice, offsetof(HairVertex, normal), sizeof(HairVertex), VK_FORMAT_R32G32B32_SFLOAT };
      geometry.texcoordBuffer = RasterBuffer { vertexSlice, offsetof(HairVertex, texcoord), sizeof(HairVertex), VK_FORMAT_R32G32_SFLOAT };
      geometry.color0Buffer = RasterBuffer { vertexSlice, offsetof(HairVertex, color), sizeof(HairVertex), VK_FORMAT_B8G8R8A8_UNORM };
      geometry.indexCount = clusterVertexCount;
      geometry.indexBuffer = RasterBuffer { indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32 };

      if (!hairBlendWeights.empty()) {
        Rc<DxvkBuffer> weightBuffer = allocBuffer(hairBlendWeights.size(), "Surface Hair Blend Weights");
        DxvkBufferSlice weightSlice { weightBuffer };
        std::memcpy(weightSlice.mapPtr(0), hairBlendWeights.data(), hairBlendWeights.size());
        geometry.blendWeightBuffer = RasterBuffer { weightSlice, 0, hairWeightStride, VK_FORMAT_R32_SFLOAT };
        geometry.numBonesPerVertex = source.numBonesPerVertex;
      }
      if (!hairBlendIndices.empty()) {
        Rc<DxvkBuffer> blendIndexBuffer = allocBuffer(hairBlendIndices.size() * sizeof(uint32_t), "Surface Hair Blend Indices");
        DxvkBufferSlice blendIndexSlice { blendIndexBuffer };
        std::memcpy(blendIndexSlice.mapPtr(0), hairBlendIndices.data(), hairBlendIndices.size() * sizeof(uint32_t));
        geometry.blendIndicesBuffer = RasterBuffer { blendIndexSlice, 0, sizeof(uint32_t), source.blendIndicesBuffer.vertexFormat() };
      }

      // Stable hashes derived from the source mesh + strand parameters (and
      // the cluster's bone in rigid mode): each piece of hair reads as one
      // persistent mesh across frames, and regrows (new BLAS) only when the
      // source mesh or parameters change.
      const auto deriveHash = [&](uint64_t salt) {
        struct HashKey {
          uint64_t salt;
          uint64_t base;
        } key = { salt, hashSalt };
        return XXH64(&key, sizeof(key), cacheKey);
      };
      geometry.hashes[HashComponents::Indices] = deriveHash(11);
      geometry.hashes[HashComponents::VertexPosition] = deriveHash(11);
      geometry.hashes[HashComponents::VertexTexcoord] = deriveHash(12);
      geometry.hashes[HashComponents::GeometryDescriptor] = deriveHash(13);
      geometry.hashes[HashComponents::VertexLayout] = deriveHash(14);
      geometry.hashes.precombine();

      return geometry;
    };

    if (attachmentMode == 1) {
      // Skinned mode: one geometry over all strands, carrying blend data.
      std::vector<uint32_t> allStrands(totalStrands);
      for (uint32_t strandIndex = 0; strandIndex < totalStrands; ++strandIndex) {
        allStrands[strandIndex] = strandIndex;
      }

      entry.geometry = assembleGeometry(allStrands, true, 0);
      entry.rigidClusters = false;
      entry.strandCount = totalStrands;

      Logger::info(str::format("[Hair Test] Grew ", totalStrands, " skinned strands on a tagged mesh",
                               hasBlendWeights ? " with skinning" : " without skinning"));
    } else {
      // Rigid (and the rigid half of hybrid): partition the primary strands
      // by their root's dominant bone; each partition becomes one static
      // cluster carried by that bone's transform.
      std::unordered_map<uint32_t, std::vector<uint32_t>> strandsByBone;
      for (uint32_t strandIndex = 0; strandIndex < strands; ++strandIndex) {
        strandsByBone[decodeDominantBone(rootAttachments[strandIndex].nearestVertex)].push_back(strandIndex);
      }

      entry.clusters.reserve(strandsByBone.size());
      for (const auto& [boneIndex, strandList] : strandsByBone) {
        SurfaceHairCluster cluster;
        cluster.boneIndex = boneIndex;
        cluster.strandCount = static_cast<uint32_t>(strandList.size());
        cluster.geometry = assembleGeometry(strandList, false, 0x1000ull + boneIndex);
        entry.clusters.push_back(std::move(cluster));
      }

      // Hybrid: the seam strands appended after the primary set become one
      // skinned geometry with its own BLAS, covering the areas outside the
      // clusters' core regions.
      if (attachmentMode == 2 && seamStrands > 0) {
        std::vector<uint32_t> seamList(seamStrands);
        for (uint32_t seamIndex = 0; seamIndex < seamStrands; ++seamIndex) {
          seamList[seamIndex] = strands + seamIndex;
        }

        entry.geometry = assembleGeometry(seamList, true, 0x2000ull);
        entry.hasSeamSet = true;
        entry.seamStrandCount = seamStrands;
      }

      entry.rigidClusters = true;
      entry.strandCount = totalStrands;

      Logger::info(str::format("[Hair Test] Grew ", strands, " strands in ", entry.clusters.size(),
                               " rigid bone clusters", entry.hasSeamSet ? str::format(" + ", seamStrands, " skinned seam strands") : "",
                               mask != nullptr ? " (mask scatter)" : "", " on a tagged mesh",
                               hasBlendWeights ? "" : " (source has no skinning; single rigid cluster)"));
    }

    return true;
  }

  void RtxHairTest::showImguiSettings() {
    ImGui::PushID("hairTest");

    ImGui::TextWrapped(
      "Strand fur grown across hair-tagged meshes, built from the RTX Character Rendering SDK's "
      "curve tessellation and path traced as ordinary scene geometry: it casts shadows, bounces "
      "light, reflects, and is lit by the scene's lights.");
    ImGui::Dummy({ 0, 2 });

    RemixGui::Checkbox("Enable Hair", &enableObject());

    ImGui::BeginDisabled(!enable());

    if (RemixGui::CollapsingHeader("Coverage", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      ImGui::TextWrapped(
        "Tag a model's texture with the 'Grow Hair Strands' category in the Game Setup tab and hair "
        "grows across every mesh drawn with it, colored by that texture and following the model's "
        "animation. The strand budget below is shared by all tagged meshes, split by surface area "
        "(a character is typically dozens of submeshes), and large batches grow over a few frames.");

      uint32_t totalClusters = 0;
      for (const auto& [meshHash, meshEntry] : m_surfaceHair) {
        totalClusters += static_cast<uint32_t>(meshEntry.clusters.size());
      }
      ImGui::Text("Hair-grown meshes: %u (%u bone clusters), strands: %u / %d budget",
                  static_cast<uint32_t>(m_surfaceHair.size()), totalClusters,
                  m_surfaceStrandsLive, surfaceStrandCount());

      RemixGui::DragInt("Total Strand Budget", &surfaceStrandCountObject(), 100.0f, 1, 2000000, "%d", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::Checkbox("Even Scatter (Best Candidate)", &evenScatterObject());

      RemixGui::Combo("Attachment", &surfaceAttachmentModeObject(), "Rigid Per-Bone Clusters\0Skinned (Exact Deformation)\0Hybrid (Rigid + Skinned Seams)\0");
      if (surfaceAttachmentMode() == 0) {
        ImGui::TextWrapped(
          "Each strand is parented to the bone with the highest skinning weight at its root and the "
          "cluster moves rigidly with that bone - no per-frame rebuild cost. Exact for single-influence "
          "meshes (this game's characters).");
      } else if (surfaceAttachmentMode() == 1) {
        ImGui::TextWrapped(
          "Strand roots inherit the mesh's blend weights and are skinned exactly like the surface. "
          "The hair BLAS rebuilds every frame the pose changes, which costs GPU time at high strand counts.");
      } else {
        ImGui::TextWrapped(
          "Rigid clusters cover each bone's core region (scaled by its influence radius); a separate "
          "skinned seam set with its own BLAS covers everything outside the cores. On single-influence "
          "meshes this renders identically to Rigid - its value is for blended, multi-influence content.");
        RemixGui::DragFloat("Cluster Core Radius Scale", &hybridClusterRadiusScaleObject(), 0.01f, 0.05f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragInt("Seam Strand Count", &hybridSeamStrandCountObject(), 100.0f, 0, 200000, "%d", ImGuiSliderFlags_AlwaysClamp);
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Strand Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      RemixGui::DragFloat("Hair Length", &surfaceHairLengthObject(), 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Strand Radius", &surfaceStrandRadiusObject(), 0.001f, 0.0001f, 100.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragInt("Segments Per Strand", &segmentsPerStrandObject(), 0.1f, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Length Jitter", &hairLengthJitterObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Tip Radius Scale", &tipRadiusScaleObject(), 0.01f, 0.05f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Frizz", &frizzObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Curliness", &curlinessObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Curl Turns", &curlTurnsObject(), 0.05f, 0.0f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Gravity Droop", &gravityDroopObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Root Occlusion", &strandOcclusionObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragInt("Scatter Seed", &scatterSeedObject(), 1.0f);

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Scatter Masks", 0)) {
      ImGui::Indent();

      ImGui::TextWrapped(
        "Optional per-texture scatter masks: a copy of the mesh with the no-fur faces deleted (eyes, "
        "accessories), exported as OBJ from a Remix capture - captures store skinned meshes in rest "
        "pose, so an edit that only deletes faces stays aligned automatically. Any export scale, axis "
        "convention or baked transform is solved by a similarity ICP at load (no manual rescaling "
        "needed); the residual below converges to ~0 for a correct mask, and a mask that cannot be "
        "fitted is ignored for that mesh.");

      for (const XXH64_hash_t maskHash : RtxOptions::hairStrandTextures()) {
        const std::string expectedPath = maskDirectory() + "/" + hashToString(maskHash) + ".obj";
        const auto maskIt = m_hairMasks.find(maskHash);
        if (maskIt == m_hairMasks.end()) {
          ImGui::Text("%s: not checked yet (grows without a mask until seen)", expectedPath.c_str());
        } else if (!maskIt->second.aligned) {
          ImGui::Text("%s: %s", expectedPath.c_str(), maskIt->second.status.c_str());
        } else {
          const std::string fit = std::string(maskIt->second.wellFitted ? "fitted" : "POOR FIT (mask ignored)")
            + " - seed " + maskIt->second.alignmentName
            + ", scale " + std::to_string(maskIt->second.alignmentScale)
            + (maskIt->second.mirrored ? ", mirrored" : "")
            + ", median residual " + std::to_string(maskIt->second.medianResidual)
            + " / max " + std::to_string(maskIt->second.maxResidual)
            + " (mesh RMS radius " + std::to_string(maskIt->second.liveRmsRadius) + ")";
          ImGui::Text("%s: %s; %s", expectedPath.c_str(), maskIt->second.status.c_str(), fit.c_str());
        }
      }
      RemixGui::Combo("Mask Handedness", &maskMirrorModeObject(),
                      "Auto Detect\0As Authored (Never Mirror)\0Mirrored (Force)\0");
      ImGui::TextWrapped(
        "If the fur pattern appears on the wrong side of a symmetric character, the export baked a reflection and "
        "the mesh is too symmetric for auto-detection - force Mirrored (or As Authored) here; changing it regrows.");
      if (RtxOptions::hairStrandTextures().empty()) {
        ImGui::Text("No hair-tagged textures yet.");
      }

      if (ImGui::Button("Reload Masks (regrows hair)")) {
        reloadHairMasks();
      }

      ImGui::Unindent();
    }

    ImGui::EndDisabled();

    ImGui::PopID();
  }
}
