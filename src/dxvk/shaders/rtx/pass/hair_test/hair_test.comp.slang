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
// Tech-demo pass that ray traces a hair-covered test sphere against its own
// acceleration structure (Linear Swept Sphere primitives from
// VK_NV_ray_tracing_linear_swept_spheres when available, DOTS triangles
// otherwise) and shades hits with the RTX Character Rendering SDK's strand
// hair BCSDFs. The hair is rendered into the composite output at render
// resolution before upscaling, depth-tested against the primary G-buffer, and
// writes its own depth and motion vectors for covered pixels so DLSS/TAAU and
// later passes treat it as part of the scene rather than an overlay.

#include "rtx/pass/hair_test/hair_test_bindings.slangh"
#include "rtx/pass/hair_test/hair_test_lss_geometry.slangh"
#include "rtx/concept/camera/camera.slangh"
#include "rtx/utility/noise.slangh"

// Polymorphic light decode/sampling for lighting hair hits from the scene's
// actual light pool (the `lights` buffer from the common bindings).
#include "rtx/concept/light/light.slangh"

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

// Per-hit shading context: the hair tangent frame, local-space view vector and
// material, built once per hit and reused for every light sample.
struct HairShadingContext
{
  float3 hitPosition;
  float3 normalWorld;
  float3 tangentWorld;
  float3x3 tangentBasis; // TBN rows: tangent, bitangent, normal
  float3 viewVectorLocal;
  float rayOffset;
  RTXCR_HairMaterialData materialData;
  RTXCR_HairInteractionSurface surface;
};

HairShadingContext createHairShadingContext(float3 rayDirection, float3 hitPosition, HairSegment segment, float3 shadingNormal)
{
  HairShadingContext ctx;
  ctx.hitPosition = hitPosition;

  const float3 viewVector = -rayDirection;

  // Curve tangent along the strand; make the shading normal exactly
  // perpendicular so the hair tangent basis is orthonormal.
  ctx.tangentWorld = normalize(segment.p1 - segment.p0);
  float3 normalWorld = shadingNormal - dot(shadingNormal, ctx.tangentWorld) * ctx.tangentWorld;
  const float normalLength = length(normalWorld);
  ctx.normalWorld = normalLength > 1e-6f ? normalWorld / normalLength : viewVector;

  const float3 biTangentWorld = cross(ctx.normalWorld, ctx.tangentWorld);
  ctx.tangentBasis = float3x3(ctx.tangentWorld, biTangentWorld, ctx.normalWorld);

  ctx.viewVectorLocal = mul(ctx.tangentBasis, viewVector);

  ctx.surface.incidentRayDirection = ctx.viewVectorLocal;
  ctx.surface.shadingNormal = float3(0.0f, 0.0f, 1.0f);
  ctx.surface.tangent = float3(1.0f, 0.0f, 0.0f);

  ctx.materialData = createHairMaterialData();

  // Offset shadow/occlusion ray origins off the fiber surface to dodge
  // self-intersection with the segment that was just hit (and with the proxy
  // mesh representing this hair in the scene TLAS).
  ctx.rayOffset = max(segment.r0, segment.r1) * 2.0f + 1e-4f;

  return ctx;
}

// Evaluates the selected RTXCR hair BCSDF for a world-space light direction.
float3 evalHairBcsdf(HairShadingContext ctx, float3 vectorToLightWorld)
{
  const float3 lightVectorLocal = mul(ctx.tangentBasis, vectorToLightWorld);

  if (hairCb.bsdfModel == HAIR_TEST_BSDF_MODEL_CHIANG)
  {
    const RTXCR_HairMaterialInteraction hairMaterialInteraction =
      RTXCR_CreateHairMaterialInteraction(ctx.materialData, ctx.surface);
    return RTXCR_HairChiangBsdfEval(hairMaterialInteraction, lightVectorLocal, ctx.viewVectorLocal);
  }

  const RTXCR_HairMaterialInteractionBcsdf hairMaterialInteractionBcsdf =
    RTXCR_CreateHairMaterialInteractionBcsdf(ctx.materialData,
                                             hairCb.diffuseReflectionTint,
                                             hairCb.diffuseReflectionWeight,
                                             hairCb.farFieldRoughness);

  float3 bsdfSpecular = float3(0.0f, 0.0f, 0.0f);
  float3 bsdfDiffuse = float3(0.0f, 0.0f, 0.0f);
  float pdf = 0.0f;
  RTXCR_HairFarFieldBcsdfEval(ctx.surface,
                              hairMaterialInteractionBcsdf,
                              lightVectorLocal, ctx.viewVectorLocal,
                              bsdfSpecular, bsdfDiffuse, pdf);

  return pdf > 0.0f ? bsdfSpecular + bsdfDiffuse : float3(0.0f, 0.0f, 0.0f);
}

// Occlusion toward a light sample. When the hair's proxy mesh is present in
// the scene TLAS, a single scene ray covers both world shadows and hair
// self-shadowing; otherwise the dedicated hair TLAS provides self-shadowing.
float lightVisibility(HairShadingContext ctx, float3 vectorToLight, float maxDistance)
{
  const float3 origin = ctx.hitPosition + ctx.normalWorld * ctx.rayOffset + vectorToLight * ctx.rayOffset;
  const float rayLength = max(maxDistance - 2.0f * ctx.rayOffset, 0.0f);

  bool occluded = false;

  if (hairCb.enableSceneShadows != 0)
  {
    occluded = traceSceneOcclusion(origin, vectorToLight, rayLength);
  }

  if (!occluded && hairCb.enableHairShadows != 0 &&
      !(hairCb.enableSceneShadows != 0 && hairCb.sceneContainsHairProxy != 0))
  {
    occluded = traceHairOcclusion(origin, vectorToLight, min(rayLength, hairCb.aoDistance * 4.0f));
  }

  return occluded ? 1.0f - hairCb.hairShadowIntensity : 1.0f;
}

// Direct lighting for a hair hit. When scene lighting is enabled and the
// scene has lights, hair is lit by uniformly sampling the Remix light pool
// (next event estimation with the hair BCSDF); otherwise the manual test key
// light is used.
float3 computeDirectLight(HairShadingContext ctx, inout RNG randomState)
{
  if (hairCb.useSceneLighting != 0)
  {
    const uint lightCount = cb.lightRanges[lightTypeCount - 1].offset + cb.lightRanges[lightTypeCount - 1].count;

    if (lightCount > 0)
    {
      MinimalSurfaceInteraction minimalSurfaceInteraction = (MinimalSurfaceInteraction)0;
      minimalSurfaceInteraction.position = ctx.hitPosition;
      minimalSurfaceInteraction.geometryNormal = ctx.normalWorld;
      minimalSurfaceInteraction.geometryTangent = ctx.tangentWorld;

      const uint sampleCount = clamp(hairCb.sceneLightSamples, 1, 8);
      float3 totalRadiance = float3(0.0f, 0.0f, 0.0f);

      for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
      {
        // Uniformly select a light from the pool and sample a point on it.
        const float lightRnd = getNextSampleBlueNoise(randomState);
        const uint lightIndex = min(uint(lightRnd * float(lightCount)), lightCount - 1);
        const float2 sampleCoordinates = float2(getNextSampleBlueNoise(randomState), getNextSampleBlueNoise(randomState));

        const LightSample lightSample =
          memoryPolymorphicLightSampleArea(lights[lightIndex], sampleCoordinates, minimalSurfaceInteraction);

        if (lightSample.solidAnglePdf <= 0.0f)
        {
          continue;
        }

        const float3 toLight = lightSample.position - ctx.hitPosition;
        const float lightDistance = length(toLight);

        if (lightDistance <= ctx.rayOffset * 4.0f)
        {
          continue;
        }

        const float3 vectorToLight = toLight / lightDistance;

        const float3 hairBsdf = evalHairBcsdf(ctx, vectorToLight);

        if (all(hairBsdf <= 0.0f))
        {
          continue;
        }

        const float visibility = lightVisibility(ctx, vectorToLight, lightDistance);

        // Standard NEE estimator: bcsdf * radiance * inverseSelectionPdf / solidAnglePdf.
        const float sampleThroughput = float(lightCount) / (lightSample.solidAnglePdf * float(sampleCount));

        totalRadiance += hairBsdf * lightSample.radiance * sampleThroughput * visibility;
      }

      return totalRadiance;
    }
  }

  // Manual test key light fallback (also used when the scene has no lights).
  const float3 vectorToLight = -hairCb.lightDirection;
  const float3 hairBsdf = evalHairBcsdf(ctx, vectorToLight);
  const float visibility = lightVisibility(ctx, vectorToLight, 1e7f);

  return hairBsdf * hairCb.lightColor * hairCb.lightIntensity * visibility;
}

// Shades a single hair hit with the selected RTXCR hair BCSDF: direct light
// from the scene's light pool (or the manual key light), plus a crude ambient
// term tinted by the fiber absorption and attenuated by a short occlusion
// probe inside the hair volume for cheap multiple-scattering depth cues.
float3 shadeHairHit(float3 rayDirection, float3 hitPosition, HairSegment segment, float3 shadingNormal, inout RNG randomState)
{
  const HairShadingContext ctx = createHairShadingContext(rayDirection, hitPosition, segment, shadingNormal);

  const float3 directLight = computeDirectLight(ctx, randomState);

  // Fake multiple-scattering ambient: tint by a single-pass transmittance
  // through the fiber and darken points buried inside the hair volume.
  const float3 absorptionCoefficient = RTXCR_ComputeAbsorptionCoefficient(ctx.materialData);
  const float3 ambientTint = exp(-absorptionCoefficient);

  float ambientOcclusion = 1.0f;
  if (hairCb.enableAmbientOcclusion != 0 &&
      traceHairOcclusion(ctx.hitPosition + ctx.normalWorld * ctx.rayOffset, ctx.normalWorld, hairCb.aoDistance))
  {
    ambientOcclusion = 0.35f;
  }

  const float3 ambientLight = hairCb.lightColor * hairCb.ambientIntensity * ambientTint * ambientOcclusion;

  const float3 radiance = directLight + ambientLight;

  // Guard against NaN/Inf sneaking into the final image from grazing-angle
  // BCSDF evaluations.
  return (any(isnan(radiance)) || any(isinf(radiance))) ? float3(0.0f, 0.0f, 0.0f) : radiance;
}

float3 shadeDebugMode(float3 rayDirection, float3 hitPosition, HairSegment segment, float3 shadingNormal, inout RNG randomState)
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
    return shadeHairHit(rayDirection, hitPosition, segment, shadingNormal, randomState);
  }
}

// Device depth of a world position under the primary camera, matching the
// convention the geometry resolver uses when writing the primary G-buffer
// depth (jittered projection, z/w).
float worldPositionToDeviceDepth(float3 worldPosition)
{
  const float4 clipPosition = mul(cb.camera.worldToProjectionJittered, float4(worldPosition, 1.0f));

  return clipPosition.w != 0.0f ? clipPosition.z / clipPosition.w : 0.0f;
}

// Screen-space motion vector for a hair hit, matching the geometry resolver's
// convention: (previous NDC - current NDC) * (0.5, -0.5) * resolution. The
// hair is static in sphere-local space, so its world motion is exactly the
// test sphere's motion since the previous frame.
float2 computeHairMotionVector(uint2 pixelCoordinate, float3 hitPosition)
{
  const float3 prevWorldPosition = hitPosition - hairCb.sphereMotion;
  const float4 prevClip = mul(cb.camera.prevWorldToProjection, float4(prevWorldPosition, 1.0f));
  const float2 prevNDC = prevClip.w != 0.0f ? prevClip.xy / prevClip.w : float2(0.0f, 0.0f);

  const float2 ndc = cameraPixelCoordinateToNDC(cb.camera, ivec2(pixelCoordinate));
  const float2 motionVectorNDC = prevNDC - ndc;
  const float2 motionVectorUVOffset = motionVectorNDC * float2(0.5f, -0.5f);

  return motionVectorUVOffset * cb.camera.resolution;
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

  RNG randomState = createRNG(uvec2(ipos), cb.frameIdx, 0);

  // Scene depth at this pixel, read once; hair behind primary geometry is
  // rejected, hair in front of it takes the pixel over.
  const float sceneDepth = PrimaryDepth[ipos];

  float3 accumulatedRadiance = float3(0.0f, 0.0f, 0.0f);
  uint hitCount = 0;
  float closestHairDepth = 0.0f;
  float closestHitT = 1e30f;
  float3 closestHitPosition = float3(0.0f, 0.0f, 0.0f);

  for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
  {
    // Stratified subpixel positions: a single sample lands on the pixel
    // center, multiple samples land on 2x2 strata centers. The camera jitter
    // is applied on top (via the jittered projection) so temporal upscalers
    // can resolve sub-pixel strand detail across frames, matching how the
    // primary G-buffer rays are generated.
    const float2 subPixel = sampleCount > 1 ? kSampleOffsets[sampleIndex] : float2(0.25f, 0.25f);
    const float2 screenUV = (float2(ipos) + 0.25f + subPixel) * hairCb.outputResolutionInv;

    const CameraDirections cameraDirections = cameraScreenUVToDirection(cb.camera, screenUV, true);
    const float3 rayDirection = cameraDirections.worldDirection;

    float hitT;
    uint segmentIndex;
    if (!traceHairRay(cameraPosition, rayDirection, 1e7f, hitT, segmentIndex))
    {
      continue;
    }

    const float3 hitPosition = cameraPosition + rayDirection * hitT;

    // Depth test against the scene with a small world-space tolerance: the
    // hair's proxy mesh in the scene TLAS writes G-buffer depth at nearly the
    // same position as the swept-sphere hit, and must not occlude it. Only
    // geometry clearly in front of the strand (beyond the tolerance) rejects
    // the hit.
    const float hairDepth = worldPositionToDeviceDepth(hitPosition);
    const float depthTolerance = 4.0f * hairCb.rootStrandRadius + hitT * 1e-3f;
    const float pulledHairDepth = worldPositionToDeviceDepth(hitPosition - rayDirection * depthTolerance);

    if (sceneDepth > 0.0f && sceneDepth < pulledHairDepth)
    {
      continue;
    }

    const HairSegment segment = fetchSegment(segmentIndex);
    const float3 shadingNormal = ComputeCurveSegmentNormal(
      cameraPosition, rayDirection, hitPosition,
      segment.p0, segment.p1, segment.r0, segment.r1);

    accumulatedRadiance += shadeDebugMode(rayDirection, hitPosition, segment, shadingNormal, randomState);

    if (hitT < closestHitT)
    {
      closestHitT = hitT;
      closestHitPosition = hitPosition;
      closestHairDepth = hairDepth;
    }

    ++hitCount;
  }

  if (hitCount == 0)
  {
    return;
  }

  const float coverage = float(hitCount) / float(sampleCount);
  const float3 hairRadiance = accumulatedRadiance / float(hitCount);

  const float4 background = CompositeOutput[ipos];
  CompositeOutput[ipos] = float4(lerp(background.rgb, hairRadiance, coverage), background.a);

  // Pixels the hair majority-covers join the G-buffer: their depth and motion
  // vectors now describe the hair so upscalers reproject and later passes
  // depth-test against it as scene geometry.
  if (hitCount * 2 >= sampleCount)
  {
    PrimaryDepth[ipos] = closestHairDepth;
    ScreenSpaceMotionVector[ipos] = computeHairMotionVector(ipos, closestHitPosition);
  }
}
