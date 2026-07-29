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

// LSS Hair Test pass.
//
// A self-contained tech-demo pass that ray traces a hair-covered test sphere
// against its own acceleration structure (Linear Swept Sphere primitives from
// VK_NV_ray_tracing_linear_swept_spheres when available, DOTS triangles
// otherwise) and shades hits with the RTX Character Rendering SDK's strand
// hair BCSDFs, compositing the result over the final image with a depth test
// against the primary G-buffer depth.

#include "rtx/pass/hair_test/hair_test_bindings.slangh"
#include "rtx/pass/hair_test/hair_test_lss_geometry.slangh"
#include "rtx/concept/camera/camera.slangh"

// RTX Character Rendering SDK hair material library (submodules/rtxcr).
#include "rtxcr/HairChiangBSDF.hlsli"
#include "rtxcr/HairFarFieldBCSDF.hlsli"

// DOTS emits 4 triangles per curve segment (2 crossed quads), so a triangle
// primitive index maps back to its source segment by dividing by 4. Matches
// rtxcr::geometry::convertToDisjointOrthogonalTriangleStrips.
static const uint kDotsTrianglesPerSegment = 4;

struct HairSegment {
  float3 p0;
  float3 p1;
  float r0;
  float r1;
};

// Fetches a curve segment from the RTXCR LSS list layout buffers and lifts it
// into world space (the BLAS is built in sphere-local space; the TLAS instance
// only translates by spherePosition).
HairSegment fetchSegment(uint segmentIndex)
{
  const uint v0 = segmentIndex * 2;
  const uint v1 = v0 + 1;

  HairSegment segment;
  segment.p0 = float3(HairSegmentPositions[v0 * 3 + 0],
                      HairSegmentPositions[v0 * 3 + 1],
                      HairSegmentPositions[v0 * 3 + 2]) + hairCb.spherePosition;
  segment.p1 = float3(HairSegmentPositions[v1 * 3 + 0],
                      HairSegmentPositions[v1 * 3 + 1],
                      HairSegmentPositions[v1 * 3 + 2]) + hairCb.spherePosition;
  segment.r0 = HairSegmentRadii[v0];
  segment.r1 = HairSegmentRadii[v1];

  return segment;
}

uint segmentIndexFromPrimitive(uint primitiveIndex)
{
  return hairCb.geometryMode == HAIR_TEST_GEOMETRY_MODE_DOTS
    ? primitiveIndex / kDotsTrianglesPerSegment
    : primitiveIndex;
}

bool traceHairRay(float3 origin, float3 direction, float tMax, out float hitT, out uint segmentIndex)
{
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0f;
  ray.TMax = tMax;

  RayQuery<0> rayQuery = RayQuery<0>();
  rayQuery.TraceRayInline(hairTLAS, RAY_FLAG_FORCE_OPAQUE, 0xff, ray);

  while (rayQuery.Proceed())
  {
  }

  // Note: LSS hits may not report as triangle hits, so accept any committed
  // intersection type rather than testing for COMMITTED_TRIANGLE_HIT.
  if (rayQuery.CommittedStatus() == COMMITTED_NOTHING)
  {
    hitT = 0.0f;
    segmentIndex = 0;
    return false;
  }

  hitT = rayQuery.CommittedRayT();
  segmentIndex = segmentIndexFromPrimitive(rayQuery.CommittedPrimitiveIndex());

  return true;
}

bool traceHairOcclusion(float3 origin, float3 direction, float tMax)
{
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0f;
  ray.TMax = tMax;

  RayQuery<0> rayQuery = RayQuery<0>();
  rayQuery.TraceRayInline(hairTLAS, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, ray);

  while (rayQuery.Proceed())
  {
  }

  return rayQuery.CommittedStatus() != COMMITTED_NOTHING;
}

bool traceSceneOcclusion(float3 origin, float3 direction, float tMax)
{
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0f;
  ray.TMax = tMax;

  RayQuery<0> rayQuery = RayQuery<0>();
  rayQuery.TraceRayInline(topLevelAS, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xff, ray);

  while (rayQuery.Proceed())
  {
  }

  return rayQuery.CommittedStatus() != COMMITTED_NOTHING;
}

RTXCR_HairMaterialData createHairMaterialData()
{
  RTXCR_HairMaterialData hairMaterialData;
  hairMaterialData.baseColor = hairCb.baseColor;
  hairMaterialData.longitudinalRoughness = hairCb.longitudinalRoughness;
  hairMaterialData.azimuthalRoughness = hairCb.azimuthalRoughness;
  hairMaterialData.fresnelApproximation = 1;
  hairMaterialData.ior = hairCb.ior;
  hairMaterialData.eta = 1.0f / hairCb.ior;
  hairMaterialData.cuticleAngleInDegrees = hairCb.cuticleAngleDegrees;
  hairMaterialData.absorptionModel = hairCb.absorptionModel;
  hairMaterialData.melanin = hairCb.melanin;
  hairMaterialData.melaninRedness = hairCb.melaninRedness;

  return hairMaterialData;
}

// Shades a single hair hit with the selected RTXCR hair BCSDF: one directional
// key light (with optional hair self-shadowing and scene shadowing) plus a
// crude ambient term tinted by the fiber absorption, attenuated by a short
// occlusion probe inside the hair volume for cheap depth cues.
float3 shadeHairHit(float3 rayDirection, float3 hitPosition, HairSegment segment, float3 shadingNormal)
{
  const float3 viewVector = -rayDirection;
  const float3 vectorToLight = -hairCb.lightDirection;

  // Curve tangent along the strand; make the shading normal exactly
  // perpendicular so the hair tangent basis is orthonormal.
  const float3 tangentWorld = normalize(segment.p1 - segment.p0);
  float3 normalWorld = shadingNormal - dot(shadingNormal, tangentWorld) * tangentWorld;
  const float normalLength = length(normalWorld);
  normalWorld = normalLength > 1e-6f ? normalWorld / normalLength : viewVector;

  const float3 biTangentWorld = cross(normalWorld, tangentWorld);
  const float3x3 hairTangentBasis = float3x3(tangentWorld, biTangentWorld, normalWorld); // TBN

  const float3 viewVectorLocal = mul(hairTangentBasis, viewVector);
  const float3 lightVectorLocal = mul(hairTangentBasis, vectorToLight);

  RTXCR_HairInteractionSurface hairInteractionSurface;
  hairInteractionSurface.incidentRayDirection = viewVectorLocal;
  hairInteractionSurface.shadingNormal = float3(0.0f, 0.0f, 1.0f);
  hairInteractionSurface.tangent = float3(1.0f, 0.0f, 0.0f);

  const RTXCR_HairMaterialData hairMaterialData = createHairMaterialData();

  float3 hairBsdf = float3(0.0f, 0.0f, 0.0f);

  if (hairCb.bsdfModel == HAIR_TEST_BSDF_MODEL_CHIANG)
  {
    const RTXCR_HairMaterialInteraction hairMaterialInteraction =
      RTXCR_CreateHairMaterialInteraction(hairMaterialData, hairInteractionSurface);
    hairBsdf = RTXCR_HairChiangBsdfEval(hairMaterialInteraction, lightVectorLocal, viewVectorLocal);
  }
  else
  {
    const RTXCR_HairMaterialInteractionBcsdf hairMaterialInteractionBcsdf =
      RTXCR_CreateHairMaterialInteractionBcsdf(hairMaterialData,
                                               hairCb.diffuseReflectionTint,
                                               hairCb.diffuseReflectionWeight,
                                               hairCb.farFieldRoughness);

    float3 bsdfSpecular = float3(0.0f, 0.0f, 0.0f);
    float3 bsdfDiffuse = float3(0.0f, 0.0f, 0.0f);
    float pdf = 0.0f;
    RTXCR_HairFarFieldBcsdfEval(hairInteractionSurface,
                                hairMaterialInteractionBcsdf,
                                lightVectorLocal, viewVectorLocal,
                                bsdfSpecular, bsdfDiffuse, pdf);

    hairBsdf = pdf > 0.0f ? bsdfSpecular + bsdfDiffuse : float3(0.0f, 0.0f, 0.0f);
  }

  // Offset shadow/occlusion ray origins off the fiber surface to dodge
  // self-intersection with the segment that was just hit.
  const float rayOffset = max(segment.r0, segment.r1) * 2.0f + 1e-4f;
  const float3 occlusionOrigin = hitPosition + normalWorld * rayOffset;

  float shadowTerm = 1.0f;

  if (hairCb.enableHairShadows != 0 &&
      traceHairOcclusion(occlusionOrigin + vectorToLight * rayOffset, vectorToLight, hairCb.aoDistance * 4.0f))
  {
    shadowTerm *= 1.0f - hairCb.hairShadowIntensity;
  }

  if (hairCb.enableSceneShadows != 0 &&
      traceSceneOcclusion(occlusionOrigin, vectorToLight, 1e7f))
  {
    shadowTerm *= 0.05f;
  }

  const float3 directLight = hairBsdf * hairCb.lightColor * hairCb.lightIntensity * shadowTerm;

  // Fake multiple-scattering ambient: tint by a single-pass transmittance
  // through the fiber and darken points buried inside the hair volume.
  const float3 absorptionCoefficient = RTXCR_ComputeAbsorptionCoefficient(hairMaterialData);
  const float3 ambientTint = exp(-absorptionCoefficient);

  float ambientOcclusion = 1.0f;
  if (hairCb.enableAmbientOcclusion != 0 &&
      traceHairOcclusion(occlusionOrigin, normalWorld, hairCb.aoDistance))
  {
    ambientOcclusion = 0.35f;
  }

  const float3 ambientLight = hairCb.lightColor * hairCb.ambientIntensity * ambientTint * ambientOcclusion;

  const float3 radiance = directLight + ambientLight;

  // Guard against NaN/Inf sneaking into the final image from grazing-angle
  // BCSDF evaluations.
  return (any(isnan(radiance)) || any(isinf(radiance))) ? float3(0.0f, 0.0f, 0.0f) : radiance;
}

float3 shadeDebugMode(float3 rayDirection, float3 hitPosition, HairSegment segment, float3 shadingNormal)
{
  switch (hairCb.debugMode)
  {
  case HAIR_TEST_DEBUG_MODE_NORMALS:
    return shadingNormal * 0.5f + 0.5f;
  case HAIR_TEST_DEBUG_MODE_TANGENTS:
    return normalize(segment.p1 - segment.p0) * 0.5f + 0.5f;
  case HAIR_TEST_DEBUG_MODE_STRAND_U:
  {
    const float3 ba = segment.p1 - segment.p0;
    const float u = saturate(dot(hitPosition - segment.p0, ba) / max(dot(ba, ba), 1e-8f));
    return float3(u, u, u);
  }
  default:
    return shadeHairHit(rayDirection, hitPosition, segment, shadingNormal);
  }
}

// Device depth of a world position under the primary camera, comparable
// against the primary G-buffer depth (same convention the dust particle pass
// uses when depth-testing against m_primaryDepth).
float worldPositionToDeviceDepth(float3 worldPosition)
{
  const float4 viewPosition = mul(cb.camera.worldToView, float4(worldPosition, 1.0f));
  const float4 clipPosition = mul(cb.camera.viewToProjection, viewPosition);

  return clipPosition.w != 0.0f ? clipPosition.z / clipPosition.w : 0.0f;
}

static const float2 kSampleOffsets[4] = {
  float2(0.0f, 0.0f),
  float2(0.5f, 0.5f),
  float2(0.5f, 0.0f),
  float2(0.0f, 0.5f),
};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 ipos : SV_DispatchThreadID)
{
  if (any(ipos >= hairCb.outputResolution))
  {
    return;
  }

  const float3 cameraPosition = cameraGetWorldPosition(cb.camera);
  const uint sampleCount = clamp(hairCb.aaSamples, 1, 4);

  float3 accumulatedRadiance = float3(0.0f, 0.0f, 0.0f);
  uint hitCount = 0;

  for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
  {
    // Stratified subpixel positions: a single sample lands on the unjittered
    // pixel center, multiple samples land on 2x2 strata centers.
    const float2 subPixel = sampleCount > 1 ? kSampleOffsets[sampleIndex] : float2(0.25f, 0.25f);
    const float2 screenUV = (float2(ipos) + 0.25f + subPixel) * hairCb.outputResolutionInv;

    const CameraDirections cameraDirections = cameraScreenUVToDirection(cb.camera, screenUV, false);
    const float3 rayDirection = cameraDirections.worldDirection;

    float hitT;
    uint segmentIndex;
    if (!traceHairRay(cameraPosition, rayDirection, 1e7f, hitT, segmentIndex))
    {
      continue;
    }

    const float3 hitPosition = cameraPosition + rayDirection * hitT;

    // Depth test against the scene: reject hair hits behind primary geometry.
    const float sceneDepth = PrimaryDepth.SampleLevel(screenUV, 0);
    const float hairDepth = worldPositionToDeviceDepth(hitPosition);

    if (sceneDepth > 0.0f && sceneDepth <= hairDepth)
    {
      continue;
    }

    const HairSegment segment = fetchSegment(segmentIndex);
    const float3 shadingNormal = ComputeCurveSegmentNormal(
      cameraPosition, rayDirection, hitPosition,
      segment.p0, segment.p1, segment.r0, segment.r1);

    accumulatedRadiance += shadeDebugMode(rayDirection, hitPosition, segment, shadingNormal);
    ++hitCount;
  }

  if (hitCount == 0)
  {
    return;
  }

  const float coverage = float(hitCount) / float(sampleCount);
  const float3 hairRadiance = accumulatedRadiance / float(hitCount);

  const float4 background = HairOutput[ipos];
  HairOutput[ipos] = float4(lerp(background.rgb, hairRadiance, coverage), background.a);
}
