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
#include "rtx_imgui.h"
#include "rtx_render/rtx_shader_manager.h"

#include "rtx/pass/common_binding_indices.h"

// RTX Character Rendering SDK Geometry Library (vendored): curve segment ->
// LSS / DOTS conversion.
#include "rtxcr_geometry/CurveTessellation.h"

#include <rtx_shaders/hair_test.h>

#include <algorithm>
#include <cmath>
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
        SAMPLER2D(HAIR_TEST_BINDING_DEPTH_INPUT)
        RW_TEXTURE2D(HAIR_TEST_BINDING_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(HairTestShader);

    // DOTS emits 4 triangles (12 vertices) per curve segment.
    constexpr uint32_t kDotsVerticesPerSegment = 4 * 3;

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
      const float y = 1.0f - 2.0f * (strandIndex + 0.5f) / static_cast<float>(strands);
      const float ringRadius = std::sqrt(std::max(0.0f, 1.0f - y * y));
      const float phi = goldenAngle * static_cast<float>(strandIndex);

      const math::float3 normal(ringRadius * std::cos(phi), y, ringRadius * std::sin(phi));
      const math::float3 root = normal * radius;

      // Jitter the growth direction away from the surface normal.
      const math::float3 randomOffset(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f);
      const math::float3 growthDirection = math::normalize(normal + randomOffset * frizz());

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

    if (desiredGeometry == ActiveGeometry::Dots) {
      // DOTS fallback: tessellate the same segments into camera-independent
      // crossed triangle strips via the SDK converter. Only positions are
      // needed; normals and tangents are reconstructed analytically at hit
      // time from the segment buffers.
      dotsVertexCount = totalSegments * kDotsVerticesPerSegment;
      std::vector<float> dotsPositionData(static_cast<size_t>(dotsVertexCount) * math::kFloatsPerPosition);

      convertToDisjointOrthogonalTriangleStrips(
        lineSegments,
        totalSegments,
        nullptr,
        dotsPositionData.data(),
        nullptr,
        nullptr,
        nullptr,
        nullptr);

      const VkDeviceSize dotsSize = dotsPositionData.size() * sizeof(float);

      if (m_dotsVertices.ptr() == nullptr || m_dotsVertices->info().size < dotsSize) {
        m_dotsVertices = createGeometryBuffer(device, dotsSize, false, "Hair Test DOTS Vertices");
      }

      ctx->writeToBuffer(m_dotsVertices, 0, dotsSize, dotsPositionData.data());
    }

    buildBlas(ctx, desiredGeometry, totalSegments, dotsVertexCount);

    m_generationHash = generationHash;
    m_activeGeometry = desiredGeometry;
    m_segmentCount = totalSegments;
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

    const ActiveGeometry desiredGeometry = resolveGeometryMode();

    if (desiredGeometry == ActiveGeometry::None) {
      // LSS was forced but the driver doesn't support it; nothing to trace.
      return;
    }

    ScopedGpuProfileZone(ctx, "LSS Hair Test");

    rebuildGeometryIfNeeded(ctx, desiredGeometry);

    if (m_segmentCount == 0 || m_blas.ptr() == nullptr) {
      return;
    }

    buildTlas(ctx);

    const Resources::Resource& output = rtOutput.m_finalOutput.resource(Resources::AccessType::ReadWrite);
    const VkExtent3D outputExtent = output.image->info().extent;

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

    HairTestConstants constants = {};
    constants.spherePosition = spherePosition();
    constants.sphereRadius = sphereRadius();
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

    ctx->writeToBuffer(m_constants, 0, sizeof(constants), &constants);

    ctx->bindCommonRayTracingResources(rtOutput);

    Rc<DxvkSampler> linearSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    ctx->bindResourceBuffer(HAIR_TEST_BINDING_CONSTANTS, DxvkBufferSlice(m_constants));
    ctx->bindAccelerationStructure(HAIR_TEST_BINDING_TLAS, m_tlas);
    ctx->bindResourceBuffer(HAIR_TEST_BINDING_SEGMENT_POSITIONS_INPUT, DxvkBufferSlice(m_segmentPositions));
    ctx->bindResourceBuffer(HAIR_TEST_BINDING_SEGMENT_RADII_INPUT, DxvkBufferSlice(m_segmentRadii));
    ctx->bindResourceView(HAIR_TEST_BINDING_DEPTH_INPUT, rtOutput.m_primaryDepth.view, nullptr);
    ctx->bindResourceSampler(HAIR_TEST_BINDING_DEPTH_INPUT, linearSampler);
    ctx->bindResourceView(HAIR_TEST_BINDING_OUTPUT, output.view, nullptr);

    ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, HairTestShader::getShader());

    const VkExtent3D workgroups = util::computeBlockCount(outputExtent, VkExtent3D { 8, 8, 1 });
    ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
  }

  void RtxHairTest::showImguiSettings() {
    const bool lssSupported = isLssSupported(*m_device);

    ImGui::PushID("hairTest");

    ImGui::TextWrapped(
      "Tech demo: a hair-covered test sphere ray traced with the RTX Character Rendering SDK's "
      "Linear Swept Sphere hair system, composited over the final image.");
    ImGui::Dummy({ 0, 2 });

    ImGui::Text("Hardware LSS (VK_NV_ray_tracing_linear_swept_spheres): %s", lssSupported ? "supported" : "not supported");

    const char* activeGeometryName = "none";
    if (m_activeGeometry == ActiveGeometry::Lss) {
      activeGeometryName = "Linear Swept Spheres";
    } else if (m_activeGeometry == ActiveGeometry::Dots) {
      activeGeometryName = "DOTS triangles (fallback)";
    }
    ImGui::Text("Active geometry: %s (%u segments)", activeGeometryName, m_segmentCount);
    ImGui::Dummy({ 0, 2 });

    RemixGui::Checkbox("Enable Hair Test", &enableObject());

    ImGui::BeginDisabled(!enable());

    RemixGui::Combo("Geometry Mode", &geometryModeObject(), "Automatic\0Force LSS\0Force DOTS\0");

    if (geometryMode() == static_cast<int>(GeometryModeOption::ForceLss) && !lssSupported) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "LSS is forced but not supported by this driver/GPU - nothing will render.");
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

      RemixGui::DragFloat3("Light Direction", &lightDirectionObject(), 0.01f);
      RemixGui::ColorEdit3("Light Color", &lightColorObject());
      RemixGui::DragFloat("Light Intensity", &lightIntensityObject(), 0.05f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
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
