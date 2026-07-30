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
#pragma once

#include <vector>

#include "../dxvk_include.h"
#include "../util/xxHash/xxhash.h"

#include "rtx_resources.h"
#include "rtx_option.h"
#include "rtx/pass/hair_test/hair_test_binding_indices.h"

namespace dxvk {

  class DxvkDevice;
  class RtxContext;

  // Tech-demo integration of the RTX Character Rendering SDK's strand hair
  // rendering, isolated from the rest of the SDK: a test sphere covered in
  // procedurally scattered hair strands, represented as the SDK's Linear Swept
  // Sphere (LSS) curve segments. When the driver exposes
  // VK_NV_ray_tracing_linear_swept_spheres the strands are ray traced as
  // native LSS primitives; otherwise they fall back to the SDK's Disjoint
  // Orthogonal Triangle Strips (DOTS) tessellation of the same segments.
  // The pass builds its own BLAS/TLAS and renders into the Remix scene at
  // render resolution: hair is traced into the composite output after
  // denoising and before upscaling, depth-tested against the primary
  // G-buffer, and writes hair depth and motion vectors for covered pixels so
  // upscalers and downstream passes treat it as scene geometry. The main path
  // tracer itself is untouched.
  class RtxHairTest {
  public:
    explicit RtxHairTest(DxvkDevice* device);
    ~RtxHairTest() = default;

    RtxHairTest(const RtxHairTest&) = delete;
    RtxHairTest(RtxHairTest&&) noexcept = delete;
    RtxHairTest& operator=(const RtxHairTest&) = delete;
    RtxHairTest& operator=(RtxHairTest&&) noexcept = delete;

    void showImguiSettings();

    // Frame-start work: (re)generates the strand geometry when parameters
    // change, and registers + submits the hair's proxy mesh into the scene as
    // a regular draw so it enters the scene TLAS (casting shadows, occluding,
    // and appearing in GI/reflections). Call from injectRTX before the scene
    // data is finalized, alongside other procedural draw submission.
    void prepareFrame(RtxContext* ctx);

    // Runs the strand shading pass. Call after the composite pass and before
    // upscaling, while the scene image, primary depth and motion vectors are
    // at render resolution.
    void dispatch(RtxContext* ctx, const Resources::RaytracingOutput& rtOutput);

    static bool isLssSupported(const DxvkDevice& device);

    enum class GeometryModeOption : int {
      Automatic = 0, // LSS when supported, DOTS otherwise
      ForceLss = 1,
      ForceDots = 2,
    };

    RTX_OPTION("rtx.hairTest", bool, enable, false,
               "Enables the LSS hair rendering test: a hair-covered sphere ray traced against its own acceleration structure "
               "and shaded with the RTX Character Rendering SDK hair BCSDFs.\n"
               "The hair is rendered into the scene at render resolution before upscaling, depth-tested against the primary G-buffer, "
               "and contributes depth and motion vectors so upscalers treat it as scene geometry.\n"
               "This is a tech demo for evaluating Linear Swept Sphere hair in Remix; it does not modify the main path tracer.");
    RTX_OPTION("rtx.hairTest", int, geometryMode, 0,
               "Curve representation used for the hair acceleration structure. 0: Automatic (native Linear Swept Spheres when the driver "
               "supports VK_NV_ray_tracing_linear_swept_spheres, otherwise DOTS triangles), 1: Force LSS, 2: Force DOTS.\n"
               "Forcing LSS on hardware without support renders nothing.");

    // Scene integration: the hair exists in the path-traced scene through a
    // proxy mesh (the SDK's DOTS tessellation of the same strands) submitted
    // as a regular opaque draw, and hair shading samples the scene's lights.
    RTX_OPTION("rtx.hairTest", bool, enableSceneProxy, true,
               "Submits the hair strands as real opaque geometry in the path-traced scene (a DOTS triangle tessellation of the same curves).\n"
               "This makes the hair cast shadows onto the scene and itself, occlude other objects, bounce light, and appear in reflections. "
               "The strand shading pass then refines the hair's primary-visible pixels with the swept-sphere silhouette and hair BCSDF.");
    RTX_OPTION("rtx.hairTest", bool, useSceneLighting, true,
               "Lights the hair with the Remix scene's actual light pool: each hair hit samples scene lights and evaluates the hair BCSDF "
               "against them (next event estimation), with shadow rays against the scene.\n"
               "When disabled, or when the scene has no lights, the manual rtx.hairTest.light* test key light is used instead.");
    RTX_OPTION_ARGS("rtx.hairTest", int, sceneLightSamples, 2,
                    "Scene light samples per hair hit. More samples reduce noise with many or large lights at proportional cost.",
                    args.minValue = 1, args.maxValue = 8);
    RTX_OPTION_ARGS("rtx.hairTest", float, proxyRoughness, 0.55f,
                    "Roughness of the hair proxy mesh's opaque material, used where the path tracer shades the proxy directly "
                    "(GI bounces, reflections, and any hair pixels the strand pass does not cover).",
                    args.minValue = 0.0f, args.maxValue = 1.0f);

    // Strand scattering / growth. These mirror the curve parameters the RTXCR
    // SDK sample feeds its tessellation, generated procedurally over a sphere.
    RTX_OPTION_ARGS("rtx.hairTest", int, strandCount, 20000,
                    "Number of hair strands scattered over the test sphere (evenly distributed).",
                    args.minValue = 1, args.maxValue = 150000);
    RTX_OPTION_ARGS("rtx.hairTest", int, segmentsPerStrand, 4,
                    "Linear swept sphere segments per strand. More segments give smoother curls and droop.",
                    args.minValue = 1, args.maxValue = 16);
    RTX_OPTION_ARGS("rtx.hairTest", float, hairLength, 8.0f,
                    "Hair strand length in world units.",
                    args.minValue = 0.01f);
    RTX_OPTION_ARGS("rtx.hairTest", float, hairLengthJitter, 0.3f,
                    "Random per-strand length variation, 0..1 fraction of the hair length.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, strandRadius, 0.05f,
                    "Hair fiber radius at the root in world units (the swept sphere radius).",
                    args.minValue = 0.001f);
    RTX_OPTION_ARGS("rtx.hairTest", float, tipRadiusScale, 0.4f,
                    "Fiber radius at the tip relative to the root, tapering the strand.",
                    args.minValue = 0.05f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, frizz, 0.3f,
                    "Random deviation of each strand's growth direction away from the sphere normal, 0..1.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, curliness, 0.15f,
                    "Amplitude of the helical curl applied along each strand, 0..1.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, curlTurns, 2.0f,
                    "Number of helical turns over a strand's length when curliness is non-zero.",
                    args.minValue = 0.0f, args.maxValue = 16.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, gravityDroop, 0.15f,
                    "How strongly strands bend downward along their length, 0..1.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION("rtx.hairTest", int, scatterSeed, 1337,
               "Random seed for strand scattering and per-strand variation.");

    // Test sphere placement.
    RTX_OPTION_ARGS("rtx.hairTest", float, sphereRadius, 25.0f,
                    "Radius of the test sphere the hair grows from, in world units.",
                    args.minValue = 0.01f);
    RTX_OPTION("rtx.hairTest", Vector3, spherePosition, Vector3(0.0f, 0.0f, 0.0f),
               "World-space position of the test sphere's center.");

    // Hair fiber material. Defaults follow the RTXCR SDK pathtracer sample's
    // human hair setup (physics-based melanin absorption, brown hair).
    RTX_OPTION("rtx.hairTest", int, bsdfModel, 1,
               "Hair BCSDF used for shading. 0: Chiang near-field BCSDF, 1: Far-Field BCSDF (the SDK sample's default; less noisy).");
    RTX_OPTION("rtx.hairTest", int, absorptionModel, 1,
               "How the fiber absorption coefficient is derived. 0: from base color, 1: from melanin (physics), 2: from melanin (normalized).");
    RTX_OPTION("rtx.hairTest", Vector3, baseColor, Vector3(0.227f, 0.130f, 0.035f),
               "Hair base color; only used when rtx.hairTest.absorptionModel is 0 (color mode).");
    RTX_OPTION_ARGS("rtx.hairTest", float, melanin, 0.805f,
                    "Melanin concentration, 0 (white/blonde) to 1 (black). Used by the physics absorption models.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, melaninRedness, 0.05f,
                    "Pheomelanin fraction: higher values shift the hair toward red tones.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, longitudinalRoughness, 0.4f,
                    "Roughness along the fiber (beta_m).",
                    args.minValue = 0.01f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, azimuthalRoughness, 0.6f,
                    "Roughness around the fiber cross-section (beta_n).",
                    args.minValue = 0.01f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, ior, 1.55f,
                    "Index of refraction of the hair fiber.",
                    args.minValue = 1.0f, args.maxValue = 2.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, cuticleAngle, 3.0f,
                    "Cuticle scale tilt in degrees; shifts the primary and secondary specular highlights apart.",
                    args.minValue = 0.0f, args.maxValue = 10.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, farFieldRoughness, 0.25f,
                    "Roughness used by the Far-Field BCSDF.",
                    args.minValue = 0.01f, args.maxValue = 1.0f);
    RTX_OPTION("rtx.hairTest", Vector3, diffuseReflectionTint, Vector3(1.0f, 1.0f, 1.0f),
               "Tint of the Far-Field BCSDF's artificial diffuse lobe.");
    RTX_OPTION_ARGS("rtx.hairTest", float, diffuseReflectionWeight, 0.0f,
                    "Weight of the Far-Field BCSDF's artificial diffuse lobe; 0 disables it (physically based).",
                    args.minValue = 0.0f, args.maxValue = 1.0f);

    // Test lighting, independent from the scene's lights so the demo works in
    // any scene state.
    RTX_OPTION("rtx.hairTest", Vector3, lightDirection, Vector3(-0.5f, -1.0f, -0.35f),
               "Direction the test key light travels (normalized internally).");
    RTX_OPTION("rtx.hairTest", Vector3, lightColor, Vector3(1.0f, 1.0f, 1.0f),
               "Color of the test key light.");
    RTX_OPTION_ARGS("rtx.hairTest", float, lightIntensity, 3.0f,
                    "Radiance of the test key light (pre-tonemap linear HDR).",
                    args.minValue = 0.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, ambientIntensity, 0.2f,
                    "Fake ambient/multiple-scattering intensity, tinted by the fiber absorption.",
                    args.minValue = 0.0f);
    RTX_OPTION("rtx.hairTest", bool, enableHairShadows, true,
               "Traces a self-shadow ray through the hair volume toward the key light.");
    RTX_OPTION_ARGS("rtx.hairTest", float, hairShadowIntensity, 0.75f,
                    "How dark hair self-shadowing gets, 0 (off) to 1 (black).",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION("rtx.hairTest", bool, enableSceneShadows, true,
               "Shadows light samples against the scene's geometry (main TLAS). When the hair proxy is in the scene, "
               "this single ray also covers hair self-shadowing.");
    RTX_OPTION("rtx.hairTest", bool, enableAmbientOcclusion, true,
               "Darkens the ambient term for points buried inside the hair volume using a short occlusion probe.");

    // Debug/quality.
    RTX_OPTION("rtx.hairTest", int, debugMode, 0,
               "Debug visualization. 0: Lit, 1: Normals, 2: Tangents, 3: Strand parameter (root to tip).");
    RTX_OPTION_ARGS("rtx.hairTest", int, aaSamples, 2,
                    "Rays per pixel (1, 2 or 4). More samples smooth strand edges at proportional cost.",
                    args.minValue = 1, args.maxValue = 4);

  private:
    enum class ActiveGeometry {
      None,
      Lss,
      Dots,
    };

    // Rebuilds strand geometry + BLAS when generation parameters change.
    void rebuildGeometryIfNeeded(RtxContext* ctx, ActiveGeometry desiredGeometry);
    // Registers the DOTS proxy tessellation as an external mesh in the scene.
    void registerProxyMesh(RtxContext* ctx,
                           XXH64_hash_t generationHash,
                           const std::vector<float>& dotsPositions,
                           const std::vector<uint32_t>& dotsPackedNormals,
                           const std::vector<float>& dotsTexcoords);
    void destroyProxyMesh(RtxContext* ctx);
    // Registers/updates the proxy's opaque material from the hair parameters.
    void ensureProxyMaterial(RtxContext* ctx);
    Vector3 computeProxyAlbedo() const;
    void buildBlas(RtxContext* ctx, ActiveGeometry geometryType, uint32_t segmentCount, uint32_t dotsVertexCount);
    void buildTlas(RtxContext* ctx);
    XXH64_hash_t computeGenerationHash(ActiveGeometry desiredGeometry) const;

    ActiveGeometry resolveGeometryMode() const;

    DxvkDevice* m_device;

    // RTXCR LSS list layout: 2 vertices per segment; positions xyzxyz..., radii rrr...
    // Feeds both the LSS BLAS build and the shading pass's segment fetch.
    Rc<DxvkBuffer> m_segmentPositions;
    Rc<DxvkBuffer> m_segmentRadii;
    // DOTS fallback: non-indexed triangle soup positions from the RTXCR geometry library.
    Rc<DxvkBuffer> m_dotsVertices;

    Rc<DxvkBuffer> m_instanceBuffer;
    Rc<DxvkBuffer> m_scratchBuffer;
    Rc<DxvkAccelStructure> m_blas;
    Rc<DxvkAccelStructure> m_tlas;
    Rc<DxvkBuffer> m_constants;

    XXH64_hash_t m_generationHash = 0;
    ActiveGeometry m_activeGeometry = ActiveGeometry::None;
    uint32_t m_segmentCount = 0;

    // Sphere position from the previous frame, for hair motion vectors while
    // the sphere is being moved.
    Vector3 m_previousSpherePosition = Vector3(0.0f, 0.0f, 0.0f);
    bool m_hasPreviousSpherePosition = false;

    // Scene proxy state: the external mesh/material registered with the asset
    // replacer so the hair exists in the path-traced scene.
    bool m_proxyMeshRegistered = false;
    XXH64_hash_t m_registeredProxyMaterialHash = 0;
  };
}
