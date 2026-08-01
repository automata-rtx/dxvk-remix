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

#include <atomic>
#include <unordered_map>
#include <vector>

#include "../dxvk_include.h"
#include "../util/xxHash/xxhash.h"

#include "rtx_hair_mask.h"
#include "rtx_types.h"
#include "rtx_option.h"

namespace dxvk {

  class DxvkDevice;
  class RtxContext;

  // Strand fur for hair-tagged meshes, built from the RTX Character Rendering
  // SDK's curve tessellation (Disjoint Orthogonal Triangle Strips) and
  // submitted as ordinary scene geometry: the strands are path traced and
  // denoised with the scene, cast shadows, bounce light and reflect, and are
  // colored by the source mesh's diffuse at each strand's root UV. Growth
  // runs once in bind pose per mesh and is cached; the strands follow the
  // model's animation through the attachment modes (rigid per-bone clusters,
  // exact skinning, or a hybrid of both). The main path tracer is untouched.
  class RtxHairTest {
  public:
    explicit RtxHairTest(DxvkDevice* device);
    ~RtxHairTest() = default;

    RtxHairTest(const RtxHairTest&) = delete;
    RtxHairTest(RtxHairTest&&) noexcept = delete;
    RtxHairTest& operator=(const RtxHairTest&) = delete;
    RtxHairTest& operator=(RtxHairTest&&) noexcept = delete;

    void showImguiSettings();

    // Frame-start work: grows hair for newly seen tagged meshes (cached
    // afterwards) and submits this frame's hair draws. Call from injectRTX
    // before the scene data is finalized, alongside other procedural draw
    // submission.
    void prepareFrame(RtxContext* ctx);

    // Called by the scene manager for every game draw it accepts. Draws whose
    // color texture is tagged with the rtx.hairStrandTextures category are
    // recorded so prepareFrame can grow and submit hair for them.
    void onDrawSubmitted(const DrawCallState& input);

    // True while surface hair needs CPU-readable copies of tagged meshes'
    // geometry. Read by the D3D9 capture (app thread) to route tagged draws'
    // vertex and index data into dedicated snapshot buffers - the default
    // capture references ring memory (the UP buffer / RT staging) that later
    // draws rewrite before the hair pass reads it at frame preparation.
    static bool wantsSourceSnapshot() {
      return s_wantsSourceSnapshot.load(std::memory_order_relaxed);
    }

    RTX_OPTION("rtx.hairTest", bool, enable, false,
               "Enables strand fur for hair-tagged meshes (rtx.hairStrandTextures, Game Setup tab). The strands are "
               "built with the RTX Character Rendering SDK's curve tessellation and path traced as ordinary scene "
               "geometry - shadows, GI and reflections included. The main path tracer is untouched.");

    // Surface hair: strands grown across meshes whose color texture is tagged
    // with the rtx.hairStrandTextures category (Game Setup tab). The strands
    // are generated once in bind pose, textured by the source's diffuse at
    // each strand's root UV, and follow the model's animation through the
    // attachment mode below (rigid per-bone clusters or exact skinning).
    RTX_OPTION_ARGS("rtx.hairTest", int, surfaceAttachmentMode, 0,
                    "How surface hair follows its mesh.\n"
                    "0: Rigid per-bone clusters (default) - strands are grouped by their root's dominant bone and each group is a "
                    "static mesh carried by that bone's transform. Costs nothing per frame after creation (no BLAS rebuilds, no "
                    "skinning). Exact for single-influence meshes.\n"
                    "1: Skinned - strands carry the source mesh's blend weights and deform exactly with it, at per-frame skinning "
                    "and BLAS update cost.\n"
                    "2: Hybrid - rigid clusters cover each bone's core region; strands falling outside every core go into a "
                    "separate skinned seam set with its own BLAS. See hybridClusterRadiusScale / hybridSeamStrandCount.",
                    args.minValue = 0, args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.hairTest", float, hybridClusterRadiusScale, 0.75f,
                    "Hybrid attachment: a strand joins its bone's rigid cluster only when its root lies within this fraction of "
                    "the bone's influence-region radius (the RMS distance of the bone's vertices from their centroid), so the "
                    "cutoff scales per bone. Roots outside every core region go to the skinned seam set.",
                    args.minValue = 0.05f, args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.hairTest", int, hybridSeamStrandCount, 15000,
                    "Hybrid attachment: number of additional strands generated for the skinned seam set covering the areas "
                    "outside the rigid clusters' core regions. These are on top of the total strand budget and live in their "
                    "own BLAS, updated per frame by GPU skinning.",
                    args.minValue = 0, args.maxValue = 200000);
    RTX_OPTION("rtx.hairTest", bool, evenScatter, true,
               "Best-candidate (Mitchell's) scattering for surface hair: each strand root is chosen from several candidates, "
               "keeping the one farthest from already-placed roots, for an even coat without clumps. One-time cost at growth.");
    RTX_OPTION("rtx.hairTest", std::string, maskDirectory, "hair_masks",
               "Directory (relative to the game executable, like rtx.conf) searched for hair scatter masks: an OBJ per hair-"
               "tagged texture named <texture hash hex>.obj. A mask is an edited copy of the mesh - taken from an RTX Remix "
               "capture, which exports skinned meshes in rest pose - with the faces that should not grow fur deleted. When "
               "present, strand roots scatter over the mask instead of the full surface and bind to the nearest live vertex "
               "for bone, UV and fallback normal data.");
    RTX_OPTION_ARGS("rtx.hairTest", int, maskMirrorMode, 0,
                    "Scatter mask handedness. Mask exports can bake a reflection (a handedness-flipping axis convention or "
                    "negative scale), and on a bilaterally symmetric character the reflected mask fits the MIRROR pose almost "
                    "perfectly - the authored cut then lands on the wrong side.\n"
                    "0: Auto (default) - both handednesses are fitted and the closer one wins; an unmirrored export is "
                    "bit-exact and cannot lose, so this is safe, but a PERFECTLY symmetric mesh ties and the tie prefers "
                    "as-authored.\n"
                    "1: As Authored - never mirror.\n"
                    "2: Mirrored - always mirror; use when Auto guessed wrong on a perfectly symmetric mesh.",
                    args.minValue = 0, args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.hairTest", int, surfaceStrandCount, 60000,
                    "Total strand budget shared by all hair-tagged meshes, distributed across them by surface area. "
                    "A tagged texture is often used by many submeshes (a character is typically split into dozens), so a "
                    "per-mesh count would multiply out of control - the budget is the hard ceiling on strands alive at once.\n"
                    "Memory scales with the budget: a strand costs 36 bytes per vertex at 12 vertices per segment, so "
                    "500k strands at 3 segments is ~650 MB of vertex data.",
                    args.minValue = 1, args.maxValue = 2000000);
    RTX_OPTION_ARGS("rtx.hairTest", float, surfaceHairLength, 2.0f,
                    "Strand length for surface hair, in world units.",
                    args.minValue = 0.001f);
    RTX_OPTION_ARGS("rtx.hairTest", float, surfaceStrandRadius, 0.02f,
                    "Strand root radius for surface hair, in world units.",
                    args.minValue = 0.0001f);

    // Strand shape. These mirror the curve parameters the RTXCR SDK sample
    // feeds its tessellation.
    RTX_OPTION_ARGS("rtx.hairTest", int, segmentsPerStrand, 4,
                    "Linear swept sphere segments per strand. More segments give smoother curls and droop.",
                    args.minValue = 1, args.maxValue = 16);
    RTX_OPTION_ARGS("rtx.hairTest", float, hairLengthJitter, 0.3f,
                    "Random per-strand length variation, 0..1 fraction of the hair length.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, tipRadiusScale, 0.4f,
                    "Fiber radius at the tip relative to the root, tapering the strand.",
                    args.minValue = 0.05f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, frizz, 0.3f,
                    "Random deviation of each strand's growth direction away from the surface normal, 0..1.",
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
    RTX_OPTION("rtx.hairTest", std::string, strandCacheDirectory, "hair_cache",
               "Directory (relative to the game executable, like rtx.conf) where generated strand geometry is cached "
               "on disk. A mesh whose strands were grown in a previous run with identical parameters (and an identical "
               "scatter mask) loads its strands from here instead of regrowing them - and needs no geometry snapshots "
               "at all. Files are keyed by mesh + parameters + mask content, so stale files are simply never read "
               "again; delete the directory to reclaim the space. Empty disables the cache.");
    // Fiber shading (RTX Character Rendering hair BCSDF). These drive the hair
    // material the strand draws use; see rtx/concept/surface_material/hair_bcsdf.slangh.
    RTX_OPTION("rtx.hairTest", bool, enableFiberBcsdf, true,
               "Shade strands with the RTX Character Rendering hair BCSDF (R / TT / TRT fiber lobes with absorption) "
               "instead of the standard opaque model. This is what gives hair its directional sheen, its coloured "
               "secondary highlight and its glow when backlit; a coat shaded as ordinary geometry reads as plastic. "
               "Turning this off makes strands shade like any other surface (and stores a surface normal instead of "
               "the fiber tangent), which regrows the strands.");
    RTX_OPTION_ARGS("rtx.hairTest", int, fiberBsdfModel, 0,
                    "Fiber model. 0: Far-field BCSDF (default) - analytic, low noise, and provides the evaluation "
                    "probabilities light sampling needs. 1: Chiang near-field BSDF - reference quality per fiber, "
                    "noisier, and without an evaluation pdf it cannot take part in multiple importance sampling.",
                    args.minValue = 0, args.maxValue = 1);
    RTX_OPTION_ARGS("rtx.hairTest", int, fiberAbsorptionModel, 0,
                    "Where the fiber's absorption (its colour) comes from. 0: the strand's own base colour, sampled "
                    "from the source texture at the root - the natural choice for game characters. 1: melanin "
                    "(physical). 2: melanin, normalized.",
                    args.minValue = 0, args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberMelanin, 0.8f,
                    "Melanin concentration, 0 (white) to 1 (black). Used by the melanin absorption models.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberMelaninRedness, 0.05f,
                    "Pheomelanin fraction: higher values shift the fibers toward red tones.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberRoughness, 0.25f,
                    "Fiber roughness for the far-field model. Low values give a tight, wet-looking sheen; high values "
                    "spread the highlight into a soft coat.",
                    args.minValue = 0.01f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberLongitudinalRoughness, 0.4f,
                    "Roughness along the fiber (beta_m), Chiang model only.",
                    args.minValue = 0.01f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberAzimuthalRoughness, 0.6f,
                    "Roughness around the fiber's cross-section (beta_n), Chiang model only.",
                    args.minValue = 0.01f, args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberIor, 1.55f,
                    "Index of refraction of the fiber. 1.55 is keratin.",
                    args.minValue = 1.0f, args.maxValue = 2.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberCuticleAngle, 3.0f,
                    "Cuticle scale tilt in degrees. This is what separates the primary (white) and secondary "
                    "(coloured) highlights - the visual signature of real hair.",
                    args.minValue = 0.0f, args.maxValue = 10.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberPrimaryHighlightScale, 1.0f,
                    "Artistic scale on the R lobe, the sharp white sheen running along the coat.",
                    args.minValue = 0.0f, args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberDiffuseWeight, 0.0f,
                    "Weight of the far-field model's artificial diffuse lobe. 0 keeps the fiber physically based; "
                    "raising it fills in dense fur that would otherwise need many light bounces to brighten.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);
    RTX_OPTION("rtx.hairTest", Vector3, fiberDiffuseTint, Vector3(1.0f, 1.0f, 1.0f),
               "Tint of the far-field model's artificial diffuse lobe.");
    RTX_OPTION_ARGS("rtx.hairTest", float, fiberDenoiserRoughness, 0.4f,
                    "Perceptual roughness hair reports to the denoiser. The fiber model has no single roughness; this "
                    "only controls how aggressively the specular signal is filtered and demodulated.",
                    args.minValue = 0.01f, args.maxValue = 1.0f);

    RTX_OPTION_ARGS("rtx.hairTest", float, strandOcclusion, 0.45f,
                    "Root-to-tip darkening baked into the strand vertex colors: 0 leaves the whole strand at the surface "
                    "color, 1 fades the roots to black. A real coat is darkest where it is deepest; the gradient stands in "
                    "for the strand-to-strand self-shadowing the path tracer cannot afford to resolve individually, and is "
                    "the main thing separating 'fur' from 'colored spikes' under uniform lighting.",
                    args.minValue = 0.0f, args.maxValue = 1.0f);

  private:
    DxvkDevice* m_device;

    // Surface hair: bind-pose strand geometry grown across hair-tagged meshes,
    // cached per source mesh and re-submitted each frame with the source
    // draw's material and transforms.
    //
    // Rigid mode partitions the strands by their root's dominant bone; each
    // cluster is a static mesh whose per-frame transform is the source draw's
    // transform composed with that bone's current palette matrix, so the fur
    // follows the animation without any per-frame BLAS or skinning work.
    static constexpr uint32_t kNoBone = 0xFFFFFFFFu;

    struct SurfaceHairCluster {
      RasterGeometry geometry;
      // Raw palette index into the source draw's bone matrices, or kNoBone to
      // follow the draw transform alone (unskinned sources).
      uint32_t boneIndex = kNoBone;
      uint32_t strandCount = 0;
    };

    struct SurfaceHairEntry {
      // Skinned mode: one geometry carrying the source's blend data.
      RasterGeometry geometry;
      uint32_t strandCount = 0;
      // Rigid mode: static per-bone clusters.
      std::vector<SurfaceHairCluster> clusters;
      bool rigidClusters = false;
      // Hybrid mode: the skinned seam set (stored in `geometry`) exists
      // alongside the rigid clusters.
      bool hasSeamSet = false;
      uint32_t seamStrandCount = 0;
      // Set when the source mesh could not be read (unmappable buffers or an
      // unsupported layout); the entry is kept to avoid retrying every frame.
      bool buildFailed = false;
      // Last frame the source mesh was drawn; stale entries are evicted so
      // their memory and strand budget return.
      uint32_t lastSeenFrame = 0;
    };

    void submitSurfaceHairDraws(RtxContext* ctx);
    void submitHairForEntry(RtxContext* ctx, const DrawCallState& source, const SurfaceHairEntry& entry);
    void releaseSurfaceHair();
    void reloadHairMasks();
    static float measureSurfaceArea(const DrawCallState& input);
    bool buildSurfaceHairGeometry(const DrawCallState& input, XXH64_hash_t cacheKey, uint32_t strandCount, SurfaceHairEntry& entry);
    XXH64_hash_t computeSurfaceHairParamsHash() const;

    // Disk cache for generated strands: identical mesh + parameters + mask
    // content across runs loads the previous run's geometry instead of
    // regrowing it (and needs no geometry snapshots at all).
    const HairMaskMesh& ensureMaskLoaded(XXH64_hash_t textureHash);
    XXH64_hash_t computeSurfaceHairDiskKey(const DrawCallState& source, XXH64_hash_t cacheKey);
    bool loadCachedSurfaceHair(XXH64_hash_t diskKey, SurfaceHairEntry& entry);
    void saveCachedSurfaceHair(XXH64_hash_t diskKey, const SurfaceHairEntry& entry) const;

    std::unordered_map<XXH64_hash_t, SurfaceHairEntry> m_surfaceHair;
    // Loaded (or failed-to-load, to avoid retrying every build) hair masks,
    // keyed by the tagged texture hash. See rtx_hair_mask.h.
    std::unordered_map<XXH64_hash_t, HairMaskMesh> m_hairMasks;
    std::vector<DrawCallState> m_taggedDrawQueue;
    bool m_submittingHairDraws = false;
    XXH64_hash_t m_surfaceHairParamsHash = 0;
    // Written at frame preparation (CS thread), read by the D3D9 capture (app
    // thread) - see wantsSourceSnapshot().
    static std::atomic<bool> s_wantsSourceSnapshot;
    // Strands currently alive across all surface hair entries, counted against
    // the surfaceStrandCount budget.
    uint32_t m_surfaceStrandsLive = 0;
  };
}
