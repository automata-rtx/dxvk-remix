/*
 * Copyright (c) 2024-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

#include "CurveTessellationMath.h"

#ifndef RTXCR_CURVE_POLYTUBE_ORDER
#define RTXCR_CURVE_POLYTUBE_ORDER 3
#endif

namespace rtxcr::geometry
{
    struct Vertex
    {
        float position[3] = {};
        float radius = 0.0f;
        float texCoord[2] = {};
    };

    struct LineSegment
    {
        unsigned int geometryIndex = 0;
        Vertex vertices[2] = {};
    };

    /// Convert curve line segments into Linear Swept Spheres (LSS) list format.
    ///
    /// This function writes 2 vertices per input segment (start and end).
    ///
    /// Buffer layout:
    /// - positionData: tightly packed xyzxyz... (3 floats per vertex), length >= (2 * endSegmentIndex) * 3.
    /// - radiusData:   tightly packed rrr... (1 float per vertex), length >= (2 * endSegmentIndex).
    ///
    /// [IN]  lineSegments            Input segments.
    /// [IN]  indexSize               Number of segments to convert (starting at lineSegmentIndexOffset).
    /// [OUT] positionData            Output position buffer
    /// [OUT] radiusData              Output radius buffer
    /// [IN]  lineSegmentIndexOffset  [Optional] Starting segment index into lineSegments and output buffers.
    ///
    /// Returns:
    /// - The final segment index after writing (lineSegmentIndexOffset + indexSize).
    ///   This is useful as a write cursor when packing multiple geometries into shared buffers.
    static uint32_t convertToLinearSweptSpheres(
        const std::vector<rtxcr::geometry::LineSegment>& lineSegments,
        const uint32_t indexSize,
        float* positionData,
        float* radiusData,
        const uint32_t lineSegmentIndexOffset = 0)
    {
        uint32_t lineSegmentIndex = lineSegmentIndexOffset;
        for (uint32_t index = 0; index < indexSize; ++index)
        {
            const rtxcr::geometry::LineSegment& line = lineSegments[lineSegmentIndex];
            if (positionData)
            {
                math::store3(&positionData[(2 * lineSegmentIndex    ) * math::kFloatsPerPosition], math::float3(line.vertices[0].position));
                math::store3(&positionData[(2 * lineSegmentIndex + 1) * math::kFloatsPerPosition], math::float3(line.vertices[1].position));
            }

            if (radiusData)
            {
                radiusData[2 * lineSegmentIndex    ] = std::max(line.vertices[0].radius, 0.001f);
                radiusData[2 * lineSegmentIndex + 1] = std::max(line.vertices[1].radius, 0.001f);
            }

            ++lineSegmentIndex;
        }

        return lineSegmentIndex;
    }

    /// Converts line segments into Disjoint Orthogonal Triangle Strips (DOTS) representation.
    ///
    /// DOTS output:
    /// - Emits 6 vertices per face, multiple faces per segment.
    /// 
    /// [IN]  lineSegments            Input segments.
    /// [IN]  indexSize               Number of segments to convert.
    /// [OUT] indexData               Index buffer output
    /// [OUT] positionData            Vertex positions (xyzxyz...)
    /// [OUT] normalData              Normals (packed/unpacked as documented)
    /// [OUT] tangentData             Tangents (packed/unpacked)
    /// [OUT] texCoord1Data           [Optional] UVs (uvuv...), may be nullptr.
    /// [OUT] radiusData              Radii per vertex, may be nullptr.
    /// [IN]  lineSegmentIndexOffset  [Optional] Starting segment index for this batch.
    /// 
    /// Returns:
    /// - The final segment index after writing (lineSegmentIndexOffset + indexSize).
    ///   This is useful as a write cursor when packing multiple geometries into shared buffers.
    static uint32_t convertToDisjointOrthogonalTriangleStrips(
        const std::vector<rtxcr::geometry::LineSegment>& lineSegments,
        const uint32_t indexSize,
        uint32_t* indexData,
        float* positionData,
        uint32_t* normalData,
        uint32_t* tangentData,
        float* texcoord1Data,
        float* radiusData,
        const uint32_t lineSegmentIndexOffset = 0)
    {
        using namespace math;

        // 4 triangles (3 vertices each)
        constexpr uint32_t kNumVerticesPerSegment = 4 * 3;

        uint32_t lineSegmentIndex = lineSegmentIndexOffset;

        for (uint32_t index = 0; index < indexSize; ++index)
        {
            const rtxcr::geometry::LineSegment& line = lineSegments[lineSegmentIndex];
            const float3 p0(line.vertices[0].position);
            const float3 p1(line.vertices[1].position);

            // Build the initial frame
            float3 fwd, s, t;
            fwd = normalize(p1 - p0);
            math::buildFrame(fwd, s, t);

            const float3 v[2] = { s, t };

            for (uint32_t face = 0; face < 2; ++face)
            {
                const uint32_t baseIndex = lineSegmentIndex * kNumVerticesPerSegment + face * 6;
                if (indexData)
                {
                    const uint32_t baseGeometryIndex = index * kNumVerticesPerSegment + face * 6;
                    indexData[baseIndex    ] = baseGeometryIndex;
                    indexData[baseIndex + 1] = baseGeometryIndex + 1;
                    indexData[baseIndex + 2] = baseGeometryIndex + 2;
                    indexData[baseIndex + 3] = baseGeometryIndex + 3;
                    indexData[baseIndex + 4] = baseGeometryIndex + 4;
                    indexData[baseIndex + 5] = baseGeometryIndex + 5;
                }

                // Necessary to make up for lost volume of PolyTube approximation of a circular tube
                const float dotsVolumeCompensationScale = 1.0f / (std::sin(kPi / 4.0f) / (kPi / 4.0f));
                if (positionData)
                {
                    store3(&positionData[(baseIndex    ) * kFloatsPerPosition], p0 + v[face] * line.vertices[0].radius * dotsVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 1) * kFloatsPerPosition], p1 - v[face] * line.vertices[1].radius * dotsVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 2) * kFloatsPerPosition], p1 + v[face] * line.vertices[1].radius * dotsVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 3) * kFloatsPerPosition], p0 + v[face] * line.vertices[0].radius * dotsVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 4) * kFloatsPerPosition], p0 - v[face] * line.vertices[0].radius * dotsVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 5) * kFloatsPerPosition], p1 - v[face] * line.vertices[1].radius * dotsVolumeCompensationScale);
                }

                if (normalData)
                {
                    const uint32_t n[2] = { vectorToSnorm8(-v[face]), vectorToSnorm8(v[face]) };
                    normalData[baseIndex    ] = n[1];
                    normalData[baseIndex + 1] = n[0];
                    normalData[baseIndex + 2] = n[1];
                    normalData[baseIndex + 3] = n[1];
                    normalData[baseIndex + 4] = n[0];
                    normalData[baseIndex + 5] = n[0];
                }

                if (tangentData)
                {
                    const uint32_t tangent = vectorToSnorm8(fwd);
                    tangentData[baseIndex    ] = tangent;
                    tangentData[baseIndex + 1] = tangent;
                    tangentData[baseIndex + 2] = tangent;
                    tangentData[baseIndex + 3] = tangent;
                    tangentData[baseIndex + 4] = tangent;
                    tangentData[baseIndex + 5] = tangent;
                }

                if (texcoord1Data)
                {
                    store2(&texcoord1Data[(baseIndex    ) * kFloatsPerTexCoord], float2(line.vertices[0].texCoord));
                    store2(&texcoord1Data[(baseIndex + 1) * kFloatsPerTexCoord], float2(line.vertices[1].texCoord));
                    store2(&texcoord1Data[(baseIndex + 2) * kFloatsPerTexCoord], float2(line.vertices[1].texCoord));
                    store2(&texcoord1Data[(baseIndex + 3) * kFloatsPerTexCoord], float2(line.vertices[0].texCoord));
                    store2(&texcoord1Data[(baseIndex + 4) * kFloatsPerTexCoord], float2(line.vertices[0].texCoord));
                    store2(&texcoord1Data[(baseIndex + 5) * kFloatsPerTexCoord], float2(line.vertices[1].texCoord));
                }

                if (radiusData)
                {
                    radiusData[baseIndex    ] = line.vertices[0].radius * dotsVolumeCompensationScale;
                    radiusData[baseIndex + 1] = line.vertices[1].radius * dotsVolumeCompensationScale;
                    radiusData[baseIndex + 2] = line.vertices[1].radius * dotsVolumeCompensationScale;
                    radiusData[baseIndex + 3] = line.vertices[0].radius * dotsVolumeCompensationScale;
                    radiusData[baseIndex + 4] = line.vertices[0].radius * dotsVolumeCompensationScale;
                    radiusData[baseIndex + 5] = line.vertices[1].radius * dotsVolumeCompensationScale;
                }
            }

            ++lineSegmentIndex;
        }

        return lineSegmentIndex;
    }

    /// Convert curve line segments into triangle PolyTube representation.
    ///
    /// PolyTubes approximate a tube around the segment by sampling points on a unit circle and sweeping them along
    /// the segment direction. The number of radial samples is controlled by the implementation (kNumVerticesOnCircle).
    ///
    /// Buffer layout:
    /// - positionData:  xyzxyz... (3 floats per vertex)
    /// - texcoord1Data: uvuv...   (2 floats per vertex)
    /// - radiusData:    rrr...    (1 float per vertex)
    /// - normalData / tangentData: packed snorm8 xyz in low 24 bits (see vectorToSnorm8)
    ///
    /// All output buffers are indexed by vertex index.
    ///
    /// [IN]  lineSegments            Input segments.
    /// [IN]  indexSize               Number of segments to convert (starting at lineSegmentIndexOffset).
    /// [OUT] indexData               Output index buffer
    /// [OUT] positionData            Output position buffer
    /// [OUT] normalData              Output packed normal buffer
    /// [OUT] tangentData             Output packed tangent buffer
    /// [OUT] texcoord1Data           [Optional] Output UV1 buffer, or nullptr to skip writing UV1.
    /// [OUT] radiusData              Output radius buffer
    /// [IN]  lineSegmentIndexOffset  [Optional] Starting segment index into lineSegments and output buffers.
    ///
    /// Returns:
    /// - The final segment index after writing (lineSegmentIndexOffset + indexSize).
    ///   This is useful as a write cursor when packing multiple geometries into shared buffers.
    static uint32_t convertToTrianglePolyTubes(
        const std::vector<rtxcr::geometry::LineSegment>& lineSegments,
        const uint32_t indexSize,
        uint32_t* indexData,
        float* positionData,
        uint32_t* normalData,
        uint32_t* tangentData,
        float* texcoord1Data,
        float* radiusData,
        const uint32_t lineSegmentIndexOffset = 0)
    {
        using namespace math;

        const uint32_t numVerticesPerSegment = RTXCR_CURVE_POLYTUBE_ORDER * 2 * 3;
        uint32_t lineSegmentIndex = lineSegmentIndexOffset;

        for (uint32_t index = 0; index < indexSize; ++index)
        {
            const rtxcr::geometry::LineSegment& line = lineSegments[lineSegmentIndex];
            const float3 p0(line.vertices[0].position);
            const float3 p1(line.vertices[1].position);

            // Build the initial frame
            float3 fwd, s, t;
            fwd = normalize(p1 - p0);
            buildFrame(fwd, s, t);

            for (uint32_t face = 0; face < RTXCR_CURVE_POLYTUBE_ORDER; ++face)
            {
                const uint32_t baseIndex = lineSegmentIndex * numVerticesPerSegment + face * 6;
                const uint32_t baseGeometryIndex = index * numVerticesPerSegment + face * 6;

                if (indexData)
                {
                    indexData[baseIndex    ] = baseGeometryIndex;
                    indexData[baseIndex + 1] = baseGeometryIndex + 1;
                    indexData[baseIndex + 2] = baseGeometryIndex + 2;
                    indexData[baseIndex + 3] = baseGeometryIndex + 3;
                    indexData[baseIndex + 4] = baseGeometryIndex + 4;
                    indexData[baseIndex + 5] = baseGeometryIndex + 5;
                }

                const float angleRadians1 = kTwoPi * face / RTXCR_CURVE_POLYTUBE_ORDER;
                const float angleRadians2 = kTwoPi * (face + 1.0f) / RTXCR_CURVE_POLYTUBE_ORDER;

                const float3 v1 = getUnitCircleCoords(s, t, angleRadians1);
                const float3 v2 = getUnitCircleCoords(s, t, angleRadians2);

                // Necessary to make up for lost volume of PolyTube approximation of a circular tube
                const float polyTubesVolumeCompensationScale = 1.0f / (std::sin(kPi / RTXCR_CURVE_POLYTUBE_ORDER) / (kPi / RTXCR_CURVE_POLYTUBE_ORDER));

                if (positionData)
                {
                    store3(&positionData[(baseIndex    ) * kFloatsPerPosition], p0 + v1 * line.vertices[0].radius * polyTubesVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 1) * kFloatsPerPosition], p1 + v2 * line.vertices[1].radius * polyTubesVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 2) * kFloatsPerPosition], p1 + v1 * line.vertices[1].radius * polyTubesVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 3) * kFloatsPerPosition], p0 + v1 * line.vertices[0].radius * polyTubesVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 4) * kFloatsPerPosition], p0 + v2 * line.vertices[0].radius * polyTubesVolumeCompensationScale);
                    store3(&positionData[(baseIndex + 5) * kFloatsPerPosition], p1 + v2 * line.vertices[1].radius * polyTubesVolumeCompensationScale);
                }

                if (normalData)
                {
                    const uint32_t n1 = vectorToSnorm8(v1);
                    const uint32_t n2 = vectorToSnorm8(v2);
                    normalData[baseIndex    ] = n1;
                    normalData[baseIndex + 1] = n2;
                    normalData[baseIndex + 2] = n1;
                    normalData[baseIndex + 3] = n1;
                    normalData[baseIndex + 4] = n2;
                    normalData[baseIndex + 5] = n2;
                }

                if (tangentData)
                {
                    const uint32_t tangent = vectorToSnorm8(fwd);
                    tangentData[baseIndex    ] = tangent;
                    tangentData[baseIndex + 1] = tangent;
                    tangentData[baseIndex + 2] = tangent;
                    tangentData[baseIndex + 3] = tangent;
                    tangentData[baseIndex + 4] = tangent;
                    tangentData[baseIndex + 5] = tangent;
                }

                if (texcoord1Data)
                {
                    store2(&texcoord1Data[(baseIndex)*kFloatsPerTexCoord], float2(line.vertices[0].texCoord));
                    store2(&texcoord1Data[(baseIndex + 1) * kFloatsPerTexCoord], float2(line.vertices[1].texCoord));
                    store2(&texcoord1Data[(baseIndex + 2) * kFloatsPerTexCoord], float2(line.vertices[1].texCoord));
                    store2(&texcoord1Data[(baseIndex + 3) * kFloatsPerTexCoord], float2(line.vertices[0].texCoord));
                    store2(&texcoord1Data[(baseIndex + 4) * kFloatsPerTexCoord], float2(line.vertices[0].texCoord));
                    store2(&texcoord1Data[(baseIndex + 5) * kFloatsPerTexCoord], float2(line.vertices[1].texCoord));
                }

                if (radiusData)
                {
                    radiusData[baseIndex    ] = line.vertices[0].radius * polyTubesVolumeCompensationScale;
                    radiusData[baseIndex + 1] = line.vertices[1].radius * polyTubesVolumeCompensationScale;
                    radiusData[baseIndex + 2] = line.vertices[1].radius * polyTubesVolumeCompensationScale;
                    radiusData[baseIndex + 3] = line.vertices[0].radius * polyTubesVolumeCompensationScale;
                    radiusData[baseIndex + 4] = line.vertices[0].radius * polyTubesVolumeCompensationScale;
                    radiusData[baseIndex + 5] = line.vertices[1].radius * polyTubesVolumeCompensationScale;
                }
            }

            ++lineSegmentIndex;
        }

        return lineSegmentIndex;
    }
}
