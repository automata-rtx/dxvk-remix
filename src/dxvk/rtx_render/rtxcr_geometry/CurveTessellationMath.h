/*
 * Copyright (c) 2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace rtxcr::geometry::math
{
    constexpr float epsilon = 1e-6f;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;

    constexpr uint32_t kFloatsPerPosition = 3;

    constexpr uint32_t kFloatsPerTexCoord = 2;

    struct float2
    {
        float2() : x(0.0f), y(0.0f) {}
        float2(const float v0, const float v1) : x(v0), y(v1) {}
        float2(const float v[2]) : x(v[0]), y(v[1]) {}
        float x, y;
    };

    inline void store2(float p[2], const float2& v) { p[0] = v.x; p[1] = v.y; }

    struct float3
    {
        float3() : x(0.0f), y(0.0f), z(0.0f) {}
        float3(const float v0, const float v1, const float v2) : x(v0), y(v1), z(v2) {}
        float3(const float v[3]) : x(v[0]), y(v[1]), z(v[2]) {}
        float x, y, z;
    };

    inline void store3(float p[3], const float3& v) { p[0] = v.x; p[1] = v.y; p[2] = v.z; }

    inline float3 add(const float3& a, const float3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline float3 mul(const float3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

    inline float3 abs3(const float3& a) { return { std::fabs(a.x), std::fabs(a.y), std::fabs(a.z) }; }

    inline float dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    inline float3 cross(const float3& a, const float3& b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    inline float3 cross(const float* a, const float* b)
    {
        return {
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        };
    }

    inline float3 normalize(const float3& v)
    {
        float ls = dot(v, v);

#if RTXCR_GEOM_ENABLE_VALIDATION
        if (ls <= 1e-20f)
        {
            return { 0.0f, 0.0f, 0.0f };
        }
#endif

        float invLen = 1.0f / std::sqrt(ls);
        return { v.x * invLen, v.y * invLen, v.z * invLen };
    }

    inline float3 operator-(const float3& v)
    {
        return { -v.x, -v.y, -v.z };
    }

    inline float3 operator+(const float3& v0, const float3& v1)
    {
        return { v0.x + v1.x, v0.y + v1.y, v0.z + v1.z };
    }

    inline float3 operator-(const float3& v0, const float3& v1)
    {
        return { v0.x - v1.x, v0.y - v1.y, v0.z - v1.z };
    }

    inline float3 operator*(const float3& v, const float s)
    {
        return { v.x * s, v.y * s, v.z * s };
    }

    inline float3 operator*(const float s, const float3& v)
    {
        return { v.x * s, v.y * s, v.z * s };
    }

    // Equality test with epsilon
    inline bool isNear(float a, float b, float eps = epsilon)
    {
        return (std::fabs(b - a) < eps);
    }

    inline uint32_t vectorToSnorm8(const float3& v)
    {
        const float scale = 127.0f / sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        const int x = int(v.x * scale);
        const int y = int(v.y * scale);
        const int z = int(v.z * scale);
        return (x & 0xff) | ((y & 0xff) << 8) | ((z & 0xff) << 16);
    }

    // Generate a vector that is orthogonal to the input vector
    // This can be used to invent a tangent frame for meshes that don't have real tangents/bitangents.
    inline float3 perpStark(const float3& u)
    {
        float3 a = abs3(u);
        uint32_t uyx = (a.x - a.y) < 0 ? 1 : 0;
        uint32_t uzx = (a.x - a.z) < 0 ? 1 : 0;
        uint32_t uzy = (a.y - a.z) < 0 ? 1 : 0;
        uint32_t xm = uyx & uzx;
        uint32_t ym = (1 ^ xm) & uzy;
        uint32_t zm = 1 ^ (xm | ym); // 1 ^ (xm & ym)
        return normalize(cross(float3(u), float3(xm, ym, zm)));
    }

    // Build a local frame from a unit normal vector.
    inline void buildFrame(const float3& n, float3& t, float3& b)
    {
        t = perpStark(n);
        b = cross(n, t);
    }

    inline float3 getUnitCircleCoords(const float3& xAxis, const float3& yAxis, const float angleRadians)
    {
        // Reduce angle to [0, 2PI)
        float unused = 0.0f;
        float unitCircleFraction = std::modf(angleRadians / kTwoPi, &unused);
        if (unitCircleFraction < 0.0f)
        {
            unitCircleFraction = 1.0f - unitCircleFraction;
        }

        const float adjustedAngleRadians = unitCircleFraction * kTwoPi;
        return std::cos(adjustedAngleRadians) * xAxis + std::sin(adjustedAngleRadians) * yAxis;
    }
}