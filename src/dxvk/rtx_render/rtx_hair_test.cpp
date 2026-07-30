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
#include "rtx_camera.h"
#include "rtx_scene_manager.h"
#include "rtx_asset_replacer.h"
#include "rtx_imgui.h"
#include "rtx_render/rtx_shader_manager.h"

#include <remix/remix_c.h>

#include "rtx/pass/common_binding_indices.h"

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

#include <rtx_shaders/hair_test.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace dxvk {

  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class HairTestShader : public ManagedShader {
      SHADER_SOURCE(HairTestShader, VK_SHADER_STAGE_COMPUTE_BIT, hair_test)

      BEGIN_PARAMETER()
        COMMON_RAYTRACING_BINDINGS

        CONSTANT_BUFFER(HAIR_TEST_BINDING_CONSTANTS)
        ACCELERATION_STRUCTURE(HAIR_TEST_BINDING_TLAS)
        STRUCTURED_BUFFER(HAIR_TEST_BINDING_SEGMENT_POSITIONS_INPUT)
        STRUCTURED_BUFFER(HAIR_TEST_BINDING_SEGMENT_RADII_INPUT)
        RW_TEXTURE2D(HAIR_TEST_BINDING_DEPTH_INPUT_OUTPUT)
        RW_TEXTURE2D(HAIR_TEST_BINDING_COMPOSITE_INPUT_OUTPUT)
        RW_TEXTURE2D(HAIR_TEST_BINDING_MOTION_VECTOR_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(HairTestShader);

    // DOTS emits 4 triangles (12 vertices) per curve segment.
    constexpr uint32_t kDotsVerticesPerSegment = 4 * 3;

    // The RTXCR DOTS converter widens fibers by this factor to compensate the
    // volume lost when approximating a circular tube with crossed quads. The
    // hair test pre-divides radii fed to DOTS-based geometry so its silhouette
    // matches the Linear Swept Sphere silhouette exactly instead.
    const float kDotsVolumeCompensationScale =
      1.0f / (std::sin(rtxcr::geometry::math::kPi / 4.0f) / (rtxcr::geometry::math::kPi / 4.0f));

    // Handles identifying the hair proxy mesh/material with the asset
    // replacer's external asset registry (arbitrary unique non-zero values).
    const remixapi_MeshHandle kProxyMeshHandle =
      reinterpret_cast<remixapi_MeshHandle>(static_cast<uintptr_t>(0x4861697254657374ull)); // 'HairTest'
    const remixapi_MaterialHandle kProxyMaterialHandle =
      reinterpret_cast<remixapi_MaterialHandle>(static_cast<uintptr_t>(0x4861697254657381ull));

    // Unpacks the DOTS converter's snorm8-packed normals (x|y<<8|z<<16).
    Vector3 unpackSnorm8Normal(uint32_t packed) {
      const auto channel = [&](uint32_t shift) {
        return static_cast<float>(static_cast<int8_t>((packed >> shift) & 0xff)) / 127.0f;
      };
      return Vector3(channel(0), channel(8), channel(16));
    }

    Rc<DxvkBuffer> createGeometryBuffer(const Rc<DxvkDevice>& device, VkDeviceSize size, bool shaderReadable, const char* name) {
      DxvkBufferCreateInfo info;
      info.size = size;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

      if (shaderReadable) {
        info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      }

      return device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::RTXAccelerationStructure, name);
    }

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

  bool RtxHairTest::isLssSupported(const DxvkDevice& device) {
    return device.extensions().nvRayTracingLinearSweptSpheres &&
           device.features().nvRayTracingLinearSweptSpheres.linearSweptSpheres;
  }

  RtxHairTest::ActiveGeometry RtxHairTest::resolveGeometryMode() const {
    const bool lssSupported = isLssSupported(*m_device);

    switch (static_cast<GeometryModeOption>(geometryMode())) {
    case GeometryModeOption::ForceLss:
      return lssSupported ? ActiveGeometry::Lss : ActiveGeometry::None;
    case GeometryModeOption::ForceDots:
      return ActiveGeometry::Dots;
    case GeometryModeOption::Automatic:
    default:
      return lssSupported ? ActiveGeometry::Lss : ActiveGeometry::Dots;
    }
  }

  XXH64_hash_t RtxHairTest::computeGenerationHash(ActiveGeometry desiredGeometry) const {
    struct GenerationParameters {
      int strandCount;
      int segmentsPerStrand;
      float hairLength;
      float hairLengthJitter;
      float strandRadius;
      float tipRadiusScale;
      float frizz;
      float curliness;
      float curlTurns;
      float gravityDroop;
      int scatterSeed;
      float sphereRadius;
      int geometry;
      int sceneProxy;
    } parameters = {
      strandCount(),
      segmentsPerStrand(),
      hairLength(),
      hairLengthJitter(),
      strandRadius(),
      tipRadiusScale(),
      frizz(),
      curliness(),
      curlTurns(),
      gravityDroop(),
      scatterSeed(),
      sphereRadius(),
      static_cast<int>(desiredGeometry),
      enableSceneProxy() ? 1 : 0,
    };

    return XXH64(&parameters, sizeof(parameters), 0);
  }

  void RtxHairTest::rebuildGeometryIfNeeded(RtxContext* ctx, ActiveGeometry desiredGeometry) {
    const XXH64_hash_t generationHash = computeGenerationHash(desiredGeometry);

    if (generationHash == m_generationHash && m_blas.ptr() != nullptr) {
      return;
    }

    ScopedGpuProfileZone(ctx, "Hair Test: Build Geometry");

    using namespace rtxcr::geometry;

    const uint32_t strands = static_cast<uint32_t>(std::max(strandCount(), 1));
    const uint32_t segmentsEach = static_cast<uint32_t>(std::clamp(segmentsPerStrand(), 1, 16));
    const uint32_t totalSegments = strands * segmentsEach;

    // Scatter strand roots evenly over the sphere with a Fibonacci spiral and
    // grow each strand outward along a jittered normal with droop and curl.
    // Strand geometry is generated in sphere-local space; the TLAS instance
    // translates it to rtx.hairTest.spherePosition, so moving the sphere does
    // not require regenerating or rebuilding the BLAS.
    std::vector<LineSegment> lineSegments;
    lineSegments.reserve(totalSegments);

    StrandRng rng(static_cast<uint32_t>(scatterSeed()));

    const float radius = sphereRadius();
    const float goldenAngle = math::kPi * (3.0f - std::sqrt(5.0f));
    const math::float3 gravityDirection(0.0f, -1.0f, 0.0f);

    for (uint32_t strandIndex = 0; strandIndex < strands; ++strandIndex) {
      const float y = 1.0f - 2.0f * (static_cast<float>(strandIndex) + 0.5f) / static_cast<float>(strands);
      const float ringRadius = std::sqrt(std::max(0.0f, 1.0f - y * y));
      const float phi = goldenAngle * static_cast<float>(strandIndex);

      const math::float3 normal(ringRadius * std::cos(phi), y, ringRadius * std::sin(phi));
      const math::float3 root = normal * radius;

      // Jitter the growth direction away from the surface normal.
      // Note: each random value is drawn into its own local first - the order
      // in which function arguments are evaluated is unspecified, so drawing
      // them inline would make the scatter depend on the compiler.
      const float jitterX = rng.next() * 2.0f - 1.0f;
      const float jitterY = rng.next() * 2.0f - 1.0f;
      const float jitterZ = rng.next() * 2.0f - 1.0f;
      const math::float3 randomOffset(jitterX, jitterY, jitterZ);

      // Note: at high frizz the jitter can cancel the normal almost exactly,
      // which would normalize to NaN and poison the acceleration structure
      // build, so fall back to growing straight out along the normal.
      const math::float3 jitteredNormal = normal + randomOffset * frizz();
      const math::float3 growthDirection = math::dot(jitteredNormal, jitteredNormal) > 1e-6f
        ? math::normalize(jitteredNormal)
        : normal;

      // Frame around the growth direction for the helical curl.
      math::float3 curlAxisT, curlAxisB;
      math::buildFrame(growthDirection, curlAxisT, curlAxisB);

      const float strandLength = hairLength() * (1.0f - hairLengthJitter() * rng.next());
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

      const auto sampleRadius = [&](float t) {
        return strandRadius() * (1.0f + (tipRadiusScale() - 1.0f) * t);
      };

      for (uint32_t segmentIndex = 0; segmentIndex < segmentsEach; ++segmentIndex) {
        const float t0 = static_cast<float>(segmentIndex) / static_cast<float>(segmentsEach);
        const float t1 = static_cast<float>(segmentIndex + 1) / static_cast<float>(segmentsEach);

        const math::float3 p0 = samplePoint(t0);
        const math::float3 p1 = samplePoint(t1);

        LineSegment segment;
        segment.geometryIndex = 0;
        math::store3(segment.vertices[0].position, p0);
        segment.vertices[0].radius = sampleRadius(t0);
        segment.vertices[0].texCoord[0] = t0;
        segment.vertices[0].texCoord[1] = 0.0f;
        math::store3(segment.vertices[1].position, p1);
        segment.vertices[1].radius = sampleRadius(t1);
        segment.vertices[1].texCoord[0] = t1;
        segment.vertices[1].texCoord[1] = 0.0f;

        lineSegments.push_back(segment);
      }
    }

    // Convert to the SDK's LSS list layout. These buffers are always built:
    // the LSS BLAS consumes them directly and the shading pass fetches segment
    // endpoints from them in both geometry modes.
    std::vector<float> positionData(2ull * totalSegments * math::kFloatsPerPosition);
    std::vector<float> radiusData(2ull * totalSegments);

    convertToLinearSweptSpheres(lineSegments, totalSegments, positionData.data(), radiusData.data());

    const Rc<DxvkDevice>& device = ctx->getDevice();

    const VkDeviceSize positionsSize = positionData.size() * sizeof(float);
    const VkDeviceSize radiiSize = radiusData.size() * sizeof(float);

    if (m_segmentPositions.ptr() == nullptr || m_segmentPositions->info().size < positionsSize) {
      m_segmentPositions = createGeometryBuffer(device, positionsSize, true, "Hair Test Segment Positions");
    }
    if (m_segmentRadii.ptr() == nullptr || m_segmentRadii->info().size < radiiSize) {
      m_segmentRadii = createGeometryBuffer(device, radiiSize, true, "Hair Test Segment Radii");
    }

    ctx->writeToBuffer(m_segmentPositions, 0, positionsSize, positionData.data());
    ctx->writeToBuffer(m_segmentRadii, 0, radiiSize, radiusData.data());

    uint32_t dotsVertexCount = 0;

    const bool wantProxy = enableSceneProxy();

    if (desiredGeometry == ActiveGeometry::Dots || wantProxy) {
      // DOTS-based geometry: tessellate the same segments into
      // camera-independent crossed triangle strips via the SDK converter,
      // used for the overlay's triangle fallback BLAS and/or the proxy mesh
      // that represents the hair in the path-traced scene.
      //
      // Pre-divide the radii by the converter's volume compensation so the
      // triangle silhouette matches the swept-sphere silhouette; the overlay
      // replaces proxy pixels in place, so the two must line up.
      for (auto& segment : lineSegments) {
        segment.vertices[0].radius /= kDotsVolumeCompensationScale;
        segment.vertices[1].radius /= kDotsVolumeCompensationScale;
      }

      dotsVertexCount = totalSegments * kDotsVerticesPerSegment;
      std::vector<float> dotsPositionData(static_cast<size_t>(dotsVertexCount) * math::kFloatsPerPosition);
      std::vector<uint32_t> dotsPackedNormalData(dotsVertexCount);
      std::vector<float> dotsTexcoordData(static_cast<size_t>(dotsVertexCount) * math::kFloatsPerTexCoord);

      convertToDisjointOrthogonalTriangleStrips(
        lineSegments,
        totalSegments,
        nullptr,
        dotsPositionData.data(),
        dotsPackedNormalData.data(),
        nullptr,
        dotsTexcoordData.data(),
        nullptr);

      if (desiredGeometry == ActiveGeometry::Dots) {
        const VkDeviceSize dotsSize = dotsPositionData.size() * sizeof(float);

        if (m_dotsVertices.ptr() == nullptr || m_dotsVertices->info().size < dotsSize) {
          m_dotsVertices = createGeometryBuffer(device, dotsSize, false, "Hair Test DOTS Vertices");
        }

        ctx->writeToBuffer(m_dotsVertices, 0, dotsSize, dotsPositionData.data());
      }

      if (wantProxy) {
        registerProxyMesh(ctx, generationHash, dotsPositionData, dotsPackedNormalData, dotsTexcoordData);
      }
    }

    if (!wantProxy && m_proxyMeshRegistered) {
      destroyProxyMesh(ctx);
    }

    buildBlas(ctx, desiredGeometry, totalSegments, desiredGeometry == ActiveGeometry::Dots ? dotsVertexCount : 0);

    m_generationHash = generationHash;
    m_activeGeometry = desiredGeometry;
    m_segmentCount = totalSegments;
  }

  // Registers the DOTS tessellation as an external mesh through the same path
  // the Remix API uses, so the hair becomes a first-class instance in the
  // path-traced scene: present in the scene TLAS with a real surface and
  // material, casting shadows, occluding, bouncing light and reflecting.
  void RtxHairTest::registerProxyMesh(RtxContext* ctx,
                                      XXH64_hash_t generationHash,
                                      const std::vector<float>& dotsPositions,
                                      const std::vector<uint32_t>& dotsPackedNormals,
                                      const std::vector<float>& dotsTexcoords) {
    const Rc<DxvkDevice>& device = ctx->getDevice();

    const uint32_t vertexCount = static_cast<uint32_t>(dotsPackedNormals.size());

    std::vector<remixapi_HardcodedVertex> vertices(vertexCount);
    std::vector<uint32_t> indices(vertexCount);

    for (uint32_t i = 0; i < vertexCount; ++i) {
      remixapi_HardcodedVertex& vertex = vertices[i];
      vertex = {};
      vertex.position[0] = dotsPositions[i * 3 + 0];
      vertex.position[1] = dotsPositions[i * 3 + 1];
      vertex.position[2] = dotsPositions[i * 3 + 2];

      const Vector3 normal = unpackSnorm8Normal(dotsPackedNormals[i]);
      vertex.normal[0] = normal.x;
      vertex.normal[1] = normal.y;
      vertex.normal[2] = normal.z;

      vertex.texcoord[0] = dotsTexcoords[i * 2 + 0];
      vertex.texcoord[1] = dotsTexcoords[i * 2 + 1];
      vertex.color = 0xFFFFFFFFu;

      indices[i] = i;
    }

    // Host-visible buffers, matching how the Remix API allocates external
    // mesh data (the BLAS build reads them through their device address).
    const auto allocBuffer = [&device](size_t sizeInBytes, const char* name) -> Rc<DxvkBuffer> {
      DxvkBufferCreateInfo bufferInfo = {};
      bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
      bufferInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      bufferInfo.size = align(sizeInBytes, CACHE_LINE_SIZE);

      return device->createBuffer(bufferInfo,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                  DxvkMemoryStats::Category::RTXBuffer, name);
    };

    Rc<DxvkBuffer> vertexBuffer = allocBuffer(vertices.size() * sizeof(remixapi_HardcodedVertex), "Hair Test Proxy Vertices");
    Rc<DxvkBuffer> indexBuffer = allocBuffer(indices.size() * sizeof(uint32_t), "Hair Test Proxy Indices");

    DxvkBufferSlice vertexSlice { vertexBuffer };
    std::memcpy(vertexSlice.mapPtr(0), vertices.data(), vertices.size() * sizeof(remixapi_HardcodedVertex));

    DxvkBufferSlice indexSlice { indexBuffer };
    std::memcpy(indexSlice.mapPtr(0), indices.data(), indices.size() * sizeof(uint32_t));

    RasterGeometry geometry = {};
    geometry.externalMaterial = kProxyMaterialHandle;
    geometry.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    geometry.cullMode = VK_CULL_MODE_NONE;
    geometry.frontFace = VK_FRONT_FACE_CLOCKWISE;
    geometry.vertexCount = vertexCount;
    geometry.positionBuffer = RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, position), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
    geometry.normalBuffer = RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, normal), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32B32_SFLOAT };
    geometry.texcoordBuffer = RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, texcoord), sizeof(remixapi_HardcodedVertex), VK_FORMAT_R32G32_SFLOAT };
    geometry.color0Buffer = RasterBuffer { vertexSlice, offsetof(remixapi_HardcodedVertex, color), sizeof(remixapi_HardcodedVertex), VK_FORMAT_B8G8R8A8_UNORM };
    geometry.indexCount = static_cast<uint32_t>(indices.size());
    geometry.indexBuffer = RasterBuffer { indexSlice, 0, sizeof(uint32_t), VK_INDEX_TYPE_UINT32 };

    // Stable, generation-derived hashes: the mesh identity follows the strand
    // parameters, so a regeneration reads as a new mesh (new BLAS) while
    // unchanged parameters keep all caches warm across frames.
    const auto deriveHash = [&](uint64_t salt) {
      return XXH64(&salt, sizeof(salt), generationHash);
    };
    geometry.hashes[HashComponents::Indices] = deriveHash(1);
    geometry.hashes[HashComponents::VertexPosition] = deriveHash(1);
    geometry.hashes[HashComponents::VertexTexcoord] = deriveHash(2);
    geometry.hashes[HashComponents::GeometryDescriptor] = deriveHash(3);
    geometry.hashes[HashComponents::VertexLayout] = deriveHash(4);
    geometry.hashes.precombine();

    if (m_proxyMeshRegistered) {
      ctx->getSceneManager().destroyExternalMesh(kProxyMeshHandle);
    }

    std::vector<RasterGeometry> submeshes;
    submeshes.push_back(std::move(geometry));
    ctx->getSceneManager().getAssetReplacer()->registerExternalMesh(kProxyMeshHandle, std::move(submeshes));

    m_proxyMeshRegistered = true;
  }

  void RtxHairTest::destroyProxyMesh(RtxContext* ctx) {
    if (m_proxyMeshRegistered) {
      ctx->getSceneManager().destroyExternalMesh(kProxyMeshHandle);
      m_proxyMeshRegistered = false;
    }
  }

  Vector3 RtxHairTest::computeProxyAlbedo() const {
    if (absorptionModel() == 0) {
      return baseColor();
    }

    // Melanin-driven absorption, mirroring the RTXCR material library's
    // RTXCR_AbsorptionCoefficientFromMelanin[Normalized] so the proxy's color
    // tracks the strand BCSDF's parameters.
    const Vector3 eumelaninSigmaA(0.506f, 0.841f, 1.653f);
    const Vector3 pheomelaninSigmaA(0.343f, 0.733f, 1.924f);

    float eumelanin, pheomelanin;
    if (absorptionModel() == 1) {
      const float melaninAmount = melanin() * melanin() * 2.4f;
      eumelanin = melaninAmount * (1.0f - melaninRedness());
      pheomelanin = melaninAmount * melaninRedness();
    } else {
      const float melaninQuantity = -std::log(std::max(1.0f - melanin(), 0.0001f));
      eumelanin = melaninQuantity * (1.0f - melaninRedness());
      pheomelanin = melaninQuantity * melaninRedness();
    }

    const Vector3 absorption = eumelanin * eumelaninSigmaA + pheomelanin * pheomelaninSigmaA;

    return Vector3(std::exp(-absorption.x), std::exp(-absorption.y), std::exp(-absorption.z));
  }

  void RtxHairTest::ensureProxyMaterial(RtxContext* ctx) {
    const Vector3 albedo = computeProxyAlbedo();

    struct MaterialParameters {
      Vector3 albedo;
      float roughness;
    } parameters = { albedo, proxyRoughness() };

    const XXH64_hash_t materialHash = XXH64(&parameters, sizeof(parameters), 0);

    if (materialHash == m_registeredProxyMaterialHash) {
      return;
    }

    OpaqueMaterialData opaqueMaterial = {};
    opaqueMaterial.setAlbedoConstant(albedo);
    opaqueMaterial.setOpacityConstant(1.0f);
    opaqueMaterial.setRoughnessConstant(proxyRoughness());
    opaqueMaterial.setMetallicConstant(0.0f);
    opaqueMaterial.setAnisotropyConstant(0.0f);
    opaqueMaterial.setEnableEmission(false);
    opaqueMaterial.setDisplaceIn(0.0f);
    opaqueMaterial.setUseLegacyAlphaState(false);

    ctx->getSceneManager().getAssetReplacer()->makeMaterialWithTexturePreload(
      *ctx, kProxyMaterialHandle, MaterialData { opaqueMaterial });

    m_registeredProxyMaterialHash = materialHash;
  }

  void RtxHairTest::prepareFrame(RtxContext* ctx) {
    if (!enable()) {
      destroyProxyMesh(ctx);
      return;
    }

    const ActiveGeometry desiredGeometry = resolveGeometryMode();

    if (desiredGeometry == ActiveGeometry::None) {
      destroyProxyMesh(ctx);
      return;
    }

    ScopedCpuProfileZone();

    rebuildGeometryIfNeeded(ctx, desiredGeometry);

    if (!enableSceneProxy() || !m_proxyMeshRegistered) {
      return;
    }

    ensureProxyMaterial(ctx);

    // Submit the proxy as this frame's draw so it enters the scene TLAS.
    const Vector3 position = spherePosition();
    Matrix4 objectToWorld;
    objectToWorld[3] = Vector4(position.x, position.y, position.z, 1.0f);

    auto state = std::make_unique<ExternalDrawState>();
    state->drawCall.modifyTransformData().objectToWorld = objectToWorld;
    state->drawCall.cameraType = CameraType::Main;
    state->mesh = kProxyMeshHandle;
    state->cameraType = CameraType::Main;
    state->doubleSided = true;

    ctx->getSceneManager().submitExternalDraw(ctx, std::move(state));
  }

  void RtxHairTest::buildBlas(RtxContext* ctx, ActiveGeometry geometryType, uint32_t segmentCount, uint32_t dotsVertexCount) {
    const Rc<DxvkDevice>& device = ctx->getDevice();

    VkAccelerationStructureGeometryLinearSweptSpheresDataNV lssData = {};
    VkAccelerationStructureGeometryKHR geometry = {};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    uint32_t primitiveCount = 0;

    if (geometryType == ActiveGeometry::Lss) {
      // Native Linear Swept Sphere primitives
      // (VK_NV_ray_tracing_linear_swept_spheres), fed straight from the RTXCR
      // LSS list layout: 2 vertices per segment, no index buffer.
      lssData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_LINEAR_SWEPT_SPHERES_DATA_NV;
      lssData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      lssData.vertexData.deviceAddress = m_segmentPositions->getDeviceAddress();
      lssData.vertexStride = 3 * sizeof(float);
      lssData.radiusFormat = VK_FORMAT_R32_SFLOAT;
      lssData.radiusData.deviceAddress = m_segmentRadii->getDeviceAddress();
      lssData.radiusStride = sizeof(float);
      lssData.indexType = VK_INDEX_TYPE_NONE_KHR;
      lssData.indexingMode = VK_RAY_TRACING_LSS_INDEXING_MODE_LIST_NV;
      lssData.endCapsMode = VK_RAY_TRACING_LSS_PRIMITIVE_END_CAPS_MODE_NONE_NV;

      geometry.pNext = &lssData;
      geometry.geometryType = VK_GEOMETRY_TYPE_LINEAR_SWEPT_SPHERES_NV;

      primitiveCount = segmentCount;
    } else {
      // DOTS triangle fallback: non-indexed triangle soup.
      geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      geometry.geometry.triangles.vertexData.deviceAddress = m_dotsVertices->getDeviceAddress();
      geometry.geometry.triangles.vertexStride = 3 * sizeof(float);
      geometry.geometry.triangles.maxVertex = dotsVertexCount > 0 ? dotsVertexCount - 1 : 0;
      geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

      primitiveCount = dotsVertexCount / 3;
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    device->vkd()->vkGetAccelerationStructureBuildSizesKHR(
      device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

    if (m_blas.ptr() == nullptr || m_blas->info().size < sizeInfo.accelerationStructureSize) {
      DxvkBufferCreateInfo blasBufferInfo = {};
      blasBufferInfo.size = sizeInfo.accelerationStructureSize;
      blasBufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      blasBufferInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      blasBufferInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

      m_blas = device->createAccelStructure(blasBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, "Hair Test BLAS");
    }

    const VkDeviceSize scratchAlignment =
      device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
    const VkDeviceSize scratchSize = align(sizeInfo.buildScratchSize + scratchAlignment, scratchAlignment);

    if (m_scratchBuffer.ptr() == nullptr || m_scratchBuffer->info().size < scratchSize) {
      DxvkBufferCreateInfo scratchInfo = {};
      scratchInfo.size = scratchSize;
      scratchInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      scratchInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      scratchInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

      m_scratchBuffer = device->createBuffer(scratchInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                             DxvkMemoryStats::Category::RTXAccelerationStructure, "Hair Test Scratch");
    }

    buildInfo.dstAccelerationStructure = m_blas->getAccelStructure();
    buildInfo.scratchData.deviceAddress = align(m_scratchBuffer->getDeviceAddress(), scratchAlignment);

    // Wait for the geometry uploads (writeToBuffer queues its barrier in the
    // context's pending set, which raw build commands don't flush), for any
    // prior traversal of the old BLAS contents, and for any prior build using
    // the shared scratch buffer before rebuilding in place.
    DxvkBarrierSet preBuildBarriers(DxvkCmdBuffer::ExecBuffer);
    preBuildBarriers.accessBuffer(
      m_segmentPositions->getSliceHandle(),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);
    preBuildBarriers.accessBuffer(
      m_segmentRadii->getSliceHandle(),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);
    if (geometryType == ActiveGeometry::Dots) {
      preBuildBarriers.accessBuffer(
        m_dotsVertices->getSliceHandle(),
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_SHADER_READ_BIT);
    }
    preBuildBarriers.accessBuffer(
      m_blas->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    preBuildBarriers.accessBuffer(
      m_scratchBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    preBuildBarriers.recordCommands(ctx->getCommandList());

    VkAccelerationStructureBuildRangeInfoKHR buildRange = {};
    buildRange.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildRange = &buildRange;

    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pBuildRange);

    // Make the BLAS build visible to the TLAS build that follows.
    DxvkBarrierSet barriers(DxvkCmdBuffer::ExecBuffer);
    barriers.accessBuffer(
      m_blas->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    barriers.recordCommands(ctx->getCommandList());

    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_blas);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
  }

  void RtxHairTest::buildTlas(RtxContext* ctx) {
    const Rc<DxvkDevice>& device = ctx->getDevice();

    // Single instance translating the sphere-local hair geometry to the test
    // sphere's world position. Rebuilt every frame so the sphere can be moved
    // freely without touching the BLAS.
    const Vector3 position = spherePosition();

    VkAccelerationStructureInstanceKHR instance = {};
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[0][3] = position.x;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[1][3] = position.y;
    instance.transform.matrix[2][2] = 1.0f;
    instance.transform.matrix[2][3] = position.z;
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = m_blas->getAccelDeviceAddress();

    if (m_instanceBuffer.ptr() == nullptr) {
      DxvkBufferCreateInfo info = {};
      info.size = sizeof(VkAccelerationStructureInstanceKHR);
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

      m_instanceBuffer = device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                              DxvkMemoryStats::Category::RTXAccelerationStructure, "Hair Test Instance");
    }

    ctx->writeToBuffer(m_instanceBuffer, 0, sizeof(instance), &instance);

    VkAccelerationStructureGeometryInstancesDataKHR instancesData = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR };
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = m_instanceBuffer->getDeviceAddress();

    VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    device->vkd()->vkGetAccelerationStructureBuildSizesKHR(
      device->handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &instanceCount, &sizeInfo);

    if (m_tlas.ptr() == nullptr || m_tlas->info().size < sizeInfo.accelerationStructureSize) {
      DxvkBufferCreateInfo tlasBufferInfo = {};
      tlasBufferInfo.size = sizeInfo.accelerationStructureSize;
      tlasBufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      tlasBufferInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      tlasBufferInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

      m_tlas = device->createAccelStructure(tlasBufferInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, "Hair Test TLAS");
    }

    const VkDeviceSize scratchAlignment =
      device->properties().khrDeviceAccelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment;
    const VkDeviceSize scratchSize = align(sizeInfo.buildScratchSize + scratchAlignment, scratchAlignment);

    if (m_scratchBuffer.ptr() == nullptr || m_scratchBuffer->info().size < scratchSize) {
      DxvkBufferCreateInfo scratchInfo = {};
      scratchInfo.size = scratchSize;
      scratchInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      scratchInfo.stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      scratchInfo.access = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

      m_scratchBuffer = device->createBuffer(scratchInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                             DxvkMemoryStats::Category::RTXAccelerationStructure, "Hair Test Scratch");
    }

    buildInfo.dstAccelerationStructure = m_tlas->getAccelStructure();
    buildInfo.scratchData.deviceAddress = align(m_scratchBuffer->getDeviceAddress(), scratchAlignment);

    // Wait for the instance upload, last frame's ray queries against the
    // TLAS, and any prior build using the shared scratch buffer before
    // rebuilding in place.
    DxvkBarrierSet preBuildBarriers(DxvkCmdBuffer::ExecBuffer);
    preBuildBarriers.accessBuffer(
      m_instanceBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_SHADER_READ_BIT);
    preBuildBarriers.accessBuffer(
      m_tlas->getSliceHandle(),
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    preBuildBarriers.accessBuffer(
      m_scratchBuffer->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    preBuildBarriers.recordCommands(ctx->getCommandList());

    VkAccelerationStructureBuildRangeInfoKHR buildRange = { instanceCount, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pBuildRange = &buildRange;

    ctx->getCommandList()->vkCmdBuildAccelerationStructuresKHR(1, &buildInfo, &pBuildRange);

    // Make the TLAS build visible to the ray queries in the shading pass.
    DxvkBarrierSet barriers(DxvkCmdBuffer::ExecBuffer);
    barriers.accessBuffer(
      m_tlas->getSliceHandle(),
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    barriers.recordCommands(ctx->getCommandList());

    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_tlas);
    ctx->getCommandList()->trackResource<DxvkAccess::Write>(m_scratchBuffer);
  }

  void RtxHairTest::dispatch(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput) {
    if (!enable()) {
      return;
    }

    // Geometry generation and scene-proxy submission happened in
    // prepareFrame at the start of the frame; nothing to trace if it bailed.
    if (m_activeGeometry == ActiveGeometry::None || m_segmentCount == 0 || m_blas.ptr() == nullptr) {
      return;
    }

    ScopedGpuProfileZone(ctx, "LSS Hair Test");

    buildTlas(ctx);

    const Resources::Resource& output = rtOutput.m_compositeOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D outputExtent = rtOutput.m_compositeOutputExtent;

    // Fill the pass constants.
    if (m_constants.ptr() == nullptr) {
      DxvkBufferCreateInfo info = {};
      info.size = sizeof(HairTestConstants);
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;

      m_constants = ctx->getDevice()->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                   DxvkMemoryStats::Category::RTXBuffer, "Hair Test Constants");
    }

    const Vector3 rawLightDirection = lightDirection();
    const float lightDirectionLength = std::max(length(rawLightDirection), 1e-6f);

    const Vector3 currentSpherePosition = spherePosition();
    const Vector3 sphereMotion = m_hasPreviousSpherePosition
      ? currentSpherePosition - m_previousSpherePosition
      : Vector3(0.0f, 0.0f, 0.0f);
    m_previousSpherePosition = currentSpherePosition;
    m_hasPreviousSpherePosition = true;

    HairTestConstants constants = {};
    constants.spherePosition = currentSpherePosition;
    constants.sphereRadius = sphereRadius();
    constants.sphereMotion = sphereMotion;
    constants.useSceneLighting = useSceneLighting() ? 1 : 0;
    constants.lightDirection = rawLightDirection / lightDirectionLength;
    constants.lightIntensity = lightIntensity();
    constants.lightColor = lightColor();
    constants.ambientIntensity = ambientIntensity();
    constants.baseColor = baseColor();
    constants.longitudinalRoughness = longitudinalRoughness();
    constants.diffuseReflectionTint = diffuseReflectionTint();
    constants.azimuthalRoughness = azimuthalRoughness();
    constants.outputResolution = uvec2 { outputExtent.width, outputExtent.height };
    constants.outputResolutionInv = vec2 { 1.0f / static_cast<float>(outputExtent.width), 1.0f / static_cast<float>(outputExtent.height) };
    constants.ior = ior();
    constants.cuticleAngleDegrees = cuticleAngle();
    constants.melanin = melanin();
    constants.melaninRedness = melaninRedness();
    constants.absorptionModel = static_cast<uint32_t>(std::clamp(absorptionModel(), 0, 2));
    constants.bsdfModel = static_cast<uint32_t>(std::clamp(bsdfModel(), 0, 1));
    constants.geometryMode = m_activeGeometry == ActiveGeometry::Dots ? HAIR_TEST_GEOMETRY_MODE_DOTS : HAIR_TEST_GEOMETRY_MODE_LSS;
    constants.debugMode = static_cast<uint32_t>(std::clamp(debugMode(), 0, 3));
    constants.farFieldRoughness = farFieldRoughness();
    constants.diffuseReflectionWeight = diffuseReflectionWeight();
    constants.hairShadowIntensity = hairShadowIntensity();
    constants.aoDistance = hairLength() * 1.5f;
    constants.enableHairShadows = enableHairShadows() ? 1 : 0;
    constants.enableSceneShadows = enableSceneShadows() ? 1 : 0;
    constants.enableAmbientOcclusion = enableAmbientOcclusion() ? 1 : 0;
    constants.aaSamples = static_cast<uint32_t>(std::clamp(aaSamples(), 1, 4));
    constants.sceneLightSamples = static_cast<uint32_t>(std::clamp(sceneLightSamples(), 1, 8));
    constants.rootStrandRadius = strandRadius();
    constants.sceneContainsHairProxy = (enableSceneProxy() && m_proxyMeshRegistered) ? 1 : 0;
    constants.pad1 = 0;

    ctx->writeToBuffer(m_constants, 0, sizeof(constants), &constants);

    ctx->bindCommonRayTracingResources(rtOutput);

    ctx->bindResourceBuffer(HAIR_TEST_BINDING_CONSTANTS, DxvkBufferSlice(m_constants));
    ctx->bindAccelerationStructure(HAIR_TEST_BINDING_TLAS, m_tlas);
    ctx->bindResourceBuffer(HAIR_TEST_BINDING_SEGMENT_POSITIONS_INPUT, DxvkBufferSlice(m_segmentPositions));
    ctx->bindResourceBuffer(HAIR_TEST_BINDING_SEGMENT_RADII_INPUT, DxvkBufferSlice(m_segmentRadii));
    ctx->bindResourceView(HAIR_TEST_BINDING_DEPTH_INPUT_OUTPUT, rtOutput.m_primaryDepth.view, nullptr);
    ctx->bindResourceView(HAIR_TEST_BINDING_COMPOSITE_INPUT_OUTPUT, output.view, nullptr);
    ctx->bindResourceView(HAIR_TEST_BINDING_MOTION_VECTOR_OUTPUT, rtOutput.m_primaryScreenSpaceMotionVector.view, nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, HairTestShader::getShader());

    const VkExtent3D workgroups = util::computeBlockCount(outputExtent, VkExtent3D { 8, 8, 1 });
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void RtxHairTest::showImguiSettings() {
    const bool lssSupported = isLssSupported(*m_device);

    ImGui::PushID("hairTest");

    ImGui::TextWrapped(
      "Tech demo: a hair-covered test sphere ray traced with the RTX Character Rendering SDK's "
      "Linear Swept Sphere hair system. Rendered into the scene at render resolution before "
      "upscaling, with hair depth and motion vectors feeding the upscaler.");
    ImGui::Dummy({ 0, 2 });

    ImGui::Text("Hardware LSS (VK_NV_ray_tracing_linear_swept_spheres): %s", lssSupported ? "supported" : "not supported");

    const char* activeGeometryName = "none";
    if (m_activeGeometry == ActiveGeometry::Lss) {
      activeGeometryName = "Linear Swept Spheres";
    } else if (m_activeGeometry == ActiveGeometry::Dots) {
      activeGeometryName = "DOTS triangles (fallback)";
    }
    ImGui::Text("Active geometry: %s (%u segments)", activeGeometryName, m_segmentCount);
    ImGui::Text("Scene proxy: %s", m_proxyMeshRegistered ? "in scene TLAS (casting shadows)" : "not in scene");
    ImGui::Dummy({ 0, 2 });

    RemixGui::Checkbox("Enable Hair Test", &enableObject());

    ImGui::BeginDisabled(!enable());

    RemixGui::Combo("Geometry Mode", &geometryModeObject(), "Automatic\0Force LSS\0Force DOTS\0");

    if (geometryMode() == static_cast<int>(GeometryModeOption::ForceLss) && !lssSupported) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "LSS is forced but not supported by this driver/GPU - nothing will render.");
    }

    if (RemixGui::CollapsingHeader("Scene Integration", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      RemixGui::Checkbox("Hair In Scene (Shadows / GI / Reflections)", &enableSceneProxyObject());
      RemixGui::Checkbox("Use Scene Lights", &useSceneLightingObject());
      ImGui::BeginDisabled(!useSceneLighting());
      RemixGui::DragInt("Light Samples Per Hit", &sceneLightSamplesObject(), 0.05f, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
      ImGui::EndDisabled();
      ImGui::BeginDisabled(!enableSceneProxy());
      RemixGui::DragFloat("Proxy Roughness", &proxyRoughnessObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::EndDisabled();

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Placement", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      if (ImGui::Button("Place In Front Of Camera")) {
        const RtCamera& camera = m_device->getCommon()->getSceneManager().getCamera();
        const float distance = (sphereRadius() + hairLength()) * 3.0f;
        const Vector3 newPosition = camera.getPosition() + camera.getDirection() * distance;
        spherePositionObject().setDeferred(newPosition);
      }

      RemixGui::DragFloat3("Sphere Position", &spherePositionObject(), 0.5f);
      RemixGui::DragFloat("Sphere Radius", &sphereRadiusObject(), 0.1f, 0.01f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Hair Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      RemixGui::DragInt("Strand Count", &strandCountObject(), 100.0f, 1, 150000, "%d", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragInt("Segments Per Strand", &segmentsPerStrandObject(), 0.1f, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Hair Length", &hairLengthObject(), 0.05f, 0.01f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Length Jitter", &hairLengthJitterObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Strand Radius", &strandRadiusObject(), 0.005f, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Tip Radius Scale", &tipRadiusScaleObject(), 0.01f, 0.05f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Frizz", &frizzObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Curliness", &curlinessObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Curl Turns", &curlTurnsObject(), 0.05f, 0.0f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Gravity Droop", &gravityDroopObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragInt("Scatter Seed", &scatterSeedObject(), 1.0f);

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Hair Material", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      RemixGui::Combo("BCSDF Model", &bsdfModelObject(), "Chiang BCSDF\0Far-Field BCSDF\0");
      RemixGui::Combo("Absorption Model", &absorptionModelObject(), "Color\0Physics (Melanin)\0Physics (Normalized)\0");

      if (absorptionModel() == 0) {
        RemixGui::ColorEdit3("Base Color", &baseColorObject());
      } else {
        RemixGui::DragFloat("Melanin", &melaninObject(), 0.005f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Melanin Redness", &melaninRednessObject(), 0.005f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      }

      if (bsdfModel() == 0) {
        RemixGui::DragFloat("Longitudinal Roughness", &longitudinalRoughnessObject(), 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Azimuthal Roughness", &azimuthalRoughnessObject(), 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      } else {
        RemixGui::DragFloat("Roughness", &farFieldRoughnessObject(), 0.01f, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        RemixGui::DragFloat("Diffuse Weight", &diffuseReflectionWeightObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (diffuseReflectionWeight() > 0.0f) {
          RemixGui::ColorEdit3("Diffuse Tint", &diffuseReflectionTintObject());
        }
      }

      RemixGui::DragFloat("IOR", &iorObject(), 0.005f, 1.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::DragFloat("Cuticle Angle (deg)", &cuticleAngleObject(), 0.05f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();

      if (useSceneLighting()) {
        ImGui::TextWrapped("Hair is lit by the scene's lights. The manual key light below is only used as a fallback when the scene has none.");
      }

      ImGui::BeginDisabled(useSceneLighting());
      RemixGui::DragFloat3("Light Direction", &lightDirectionObject(), 0.01f);
      RemixGui::ColorEdit3("Light Color", &lightColorObject());
      RemixGui::DragFloat("Light Intensity", &lightIntensityObject(), 0.05f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::EndDisabled();
      RemixGui::DragFloat("Ambient Intensity", &ambientIntensityObject(), 0.01f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      RemixGui::Checkbox("Hair Self-Shadows", &enableHairShadowsObject());
      ImGui::BeginDisabled(!enableHairShadows());
      RemixGui::DragFloat("Self-Shadow Intensity", &hairShadowIntensityObject(), 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      ImGui::EndDisabled();
      RemixGui::Checkbox("Scene Shadows", &enableSceneShadowsObject());
      RemixGui::Checkbox("Hair Ambient Occlusion", &enableAmbientOcclusionObject());

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Debug / Quality")) {
      ImGui::Indent();

      RemixGui::Combo("Display", &debugModeObject(), "Lit\0Normals\0Tangents\0Strand U\0");
      RemixGui::DragInt("Rays Per Pixel", &aaSamplesObject(), 0.05f, 1, 4, "%d", ImGuiSliderFlags_AlwaysClamp);

      ImGui::Unindent();
    }

    ImGui::EndDisabled();

    ImGui::PopID();
  }
}
