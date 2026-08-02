# Dusklight hair — strand fur for Remix scenes

Status: working feature on `Fixed-Function-dev` lineage. This documents the
whole fur system: surface hair on hair-tagged meshes, the data-lifetime
rules that were expensive to learn, the strand disk cache, artist scatter
masks, shading, and the attachment modes. It is written so the system could
later be re-implemented as an upstream Remix PR (§9) — the modules are
deliberately layered for that.

(The hair-covered test sphere this system was bootstrapped on — its own
BLAS/TLAS and a BCSDF shading pass — was removed once surface fur shipped;
`git log src/dxvk/shaders/rtx/pass/hair_test` finds it if it is ever wanted
again.)

## 1. What it is

Fur grown across game meshes whose color texture carries the
`rtx.hairStrandTextures` tag (Game Setup tab), built from the RTX Character
Rendering SDK's curve representation:

- Strands are chains of curve segments tessellated with the SDK's DOTS
  scheme (4 triangles / 12 vertices per segment, radii pre-divided by
  `(π/4)/sin(π/4)` so the silhouette matches the SDK's Linear Swept Sphere
  reference).
- Strand geometry goes through the regular draw path — it is ordinary scene
  geometry, path traced and denoised, casting shadows and appearing in GI
  and reflections, colored by the source diffuse at each strand's root UV.
  There is no separate hair shading pass.
- Strands shade with the SDK's **hair BCSDF** (R / TT / TRT fiber lobes with
  absorption), carried on the opaque material behind a flag so hair keeps
  NEE, RTXDI/ReSTIR, shadows and denoising — see §6.

Files:

| File | Role |
| :-- | :-- |
| `src/dxvk/rtx_render/rtx_hair_test.{h,cpp}` | the system: growth, caching (RAM + disk), attachment modes, shading bake, overlay UI |
| `src/dxvk/rtx_render/rtx_hair_mask.{h,cpp}` | artist scatter masks: OBJ load, similarity-ICP alignment, nearest-vertex grid. **Std-only, no dxvk types** — unit-testable standalone |
| `src/dxvk/rtx_render/rtxcr_geometry/` | vendored RTXCR geometry lib (DOTS conversion), byte-identical to the SDK |
| `submodules/rtxcr` | RTXCR SDK submodule (pinned v1.2.0) |

## 2. The data-lifetime rule (why snapshots exist)

**Never read a captured draw's vertex/index buffers at frame preparation.**
Remix's D3D9 capture references ring memory: UP draws live in a 1 MB ring
whose backing is renamed when it fills, and captured indices live in the RT
staging ring, recycled once hashing completes. Remix's own pipeline consumes
these promptly; the hair pass runs at end-of-frame prep, after every later
draw has rewritten the rings. Reading them there produced fur grown across
*other draws' bytes* — shaped wrong while still moving with the character,
since transforms are captured by value. The `isPendingGpuWrite()` guard
("vertex data is not CPU-readable") is the same hazard caught in the act.

The fix is the snapshot protocol:

- `RtxHairTest::wantsSourceSnapshot()` (static, atomic) is true while any
  tagged mesh still needs growing.
- While it is up, `D3D9Rtx` (app thread, at capture time — the only moment
  the data is provably live) copies tagged draws' vertex streams and all
  draws' index data into **dedicated buffers owned by the draw**, and marks
  the draw `capturedForHairSnapshot`.
- Hair only ever grows from marked draws. An unmarked tagged draw just
  raises the flag; growth happens a frame later. Once everything is grown
  the flag drops and no copies are made.
- Snapshot bytes are identical to what Remix hashes, so cache keys are
  unaffected.
- **The strand disk cache short-circuits all of this.** Built strands are
  serialized under `rtx.hairTest.strandCacheDirectory` (default
  `hair_cache`, next to `rtx.conf`), keyed by mesh identity ^ parameter
  hash ^ mask content hash with a format version. A later run with the
  identical mesh, parameters and mask loads the file — checked *before*
  the snapshot gate, so a fully cached startup never copies source
  geometry at all. Any change misses the key and regrows; stale files are
  never read again (delete the directory to reclaim space).
- **Marked draws bypass the `isPendingGpuWrite()` guard.** dxvk tracks every
  storage-buffer descriptor as a *write* — read-only `StructuredBuffer`
  inputs included — so a snapshot buffer consumed by the frame's
  interleave/skinning passes reports "pending GPU write" until the command
  list retires, which is after the hair pass runs. For draw-owned snapshot
  buffers that is a false positive (bytes are CPU-written once at capture,
  then only read); left in place it blocked all growth. The guard still
  applies to unmarked sources, where in-flight use genuinely means the ring
  bytes may be rewritten.

## 3. Mesh identity (the aurora batcher contract)

Surface hair caches per mesh under `VertexPosition hash ^ params hash`. This
works because the game side guarantees:

- rest-pose vertices (stable bytes frame over frame),
- one draw per character material run — aurora's DX9 draw batcher merges a
  GX character's shape packets into a single indexed draw against a virtual
  256-entry world palette (matching Remix's `SkinningArgs::bones[256]`),
  with palette entries allocated per (slot, load-generation) so the merged
  bytes are deterministic under animation — and
- **one coherent coordinate space across the merged mesh.** J3D stores
  single-joint ("full weight") shapes' vertices in the joint's local frame
  and enveloped shapes' vertices in model/bind space; raw merged bytes
  therefore showed a character's full-weight parts (Wolf Link's head and
  tail) as spikes collapsed at the model origin — captures exported that
  explosion and hair scattered across it, while rendering looked fine
  because each part's matrix undoes its own storage space. The game now
  annotates each position-matrix load with the storage→rest transform
  (aurora `GXSetPosMtxRest`); the DX9 backend rewrites vertices into bind
  pose and compensates the world matrices, so the bytes Remix hashes,
  skins, captures — and grows hair on — are one bind-pose mesh.

Accessories with their own material (eye decals, a chained paw) break the
batch and stay separate meshes — which is also what keeps them furless even
without a mask. See aurora `docs/dx9/progress.md` §3.19–3.20.

## 4. Growth pipeline

Per tagged mesh, once (results cached until parameters change, the mesh
changes, or eviction after ~300 unseen frames):

1. **Scatter domain**: the artist mask's triangles when present (§5), else
   the live surface. One cumulative-area distribution; roots are uniform per
   area. With **even scatter** on (default), each root is the
   farthest-from-existing of 4 candidates (Mitchell's best-candidate),
   tracked in a spacing hash grid — an even coat with no clumps, one-time
   cost.
2. **Binding**: every root binds to a live-mesh vertex — the nearest vertex
   (grid lookup) for mask scatter, the barycentric-dominant corner
   otherwise — which supplies the root UV (strand color) and the fallback
   normal. Separately, the root's **skinning** is resolved as the
   barycentric blend of its triangle's three corner bindings (§7); binding
   it to the single bound vertex instead is what used to tear the coat open
   at joints.
3. **Growth**: strands extend along the interpolated surface normal (the
   mask's own smooth-shaded normals when it has them — author them smooth in
   Blender for low-poly meshes) with jitter/frizz/curl/droop.
4. **Assembly**: strand subsets become `RasterGeometry` pieces (36-byte
   interleaved vertices, DOTS triangles, stable derived hashes) submitted as
   copies of the source draw with swapped geometry — material, transforms
   and skinning carry over. All hair draws set
   `InstanceCategories::IgnoreOpacityMicromap` (strands are solid; an
   alpha-tested source material once turned 39M strand triangles into a
   6.2 GB micromap request).

5. **Shading bake**: the normal slot carries the **fiber tangent** (the
   strand direction) when fiber shading is on — the hair BCSDF is built
   around the tangent, not a normal — and the root's *smooth* surface
   normal when it is off. A root-to-tip darkening gradient in the vertex
   colors (`strandOcclusion`) stands in for the strand-to-strand
   self-shadowing the path tracer cannot afford to resolve; a real coat is
   darkest where it is deepest. Which of the two the bake stores is part of
   the parameter hash, so toggling it regrows and re-caches.

Budget: `surfaceStrandCount` is a **total** across all tagged meshes, split
by surface area, grown amortized (~30k strands/frame), with a live
`strands / budget` readout in the overlay. Disk-cached meshes load outside
the amortization (a file read instead of growth).

## 5. Artist scatter masks

Purpose: art-directed fur placement — no fur on eyes, inner mouth,
accessories — plus smooth authored normals for growth direction.

Authoring loop:

1. Take a Remix capture with the character on screen. For skinned meshes the
   capturer exports the **rest-pose input geometry** plus skeleton
   (`rtx_game_capturer.cpp`), so the captured mesh is byte-compatible with
   the live draw's vertex data. With the game side's rest-space annotations
   (§3) the export is the character's coherent bind pose; a capture showing
   parts collapsed at the origin means the game build predates them.
2. Import the capture USD in Blender, duplicate the character mesh, delete
   the faces that should not grow fur. **Only delete — never move
   vertices.** Blender stores mesh data in rest pose (the armature is a
   modifier), so a destructive edit stays in the right space.
3. Shade smooth, export OBJ to `hair_masks/<TEXHASH>.obj` next to
   `rtx.conf`, where `<TEXHASH>` is the tagged texture's hash exactly as the
   texture UI shows it (16 uppercase hex digits). The overlay's Scatter
   Masks panel lists the expected filename per tagged texture, the load
   status, and a Reload button (regrows without restarting). **Export
   settings do not matter**: any axis preset, any uniform viewer/import
   scale (working on a capture scaled to 0.01 is fine), and applied object
   transforms are all solved by the alignment - what still matters is never
   moving individual vertices.

Runtime (`rtx_hair_mask.{h,cpp}`):

- Minimal OBJ parser (`v`/`vn`/`f`, quads/n-gons fan-triangulated, negative
  indices).
- **Similarity ICP alignment**: solves the full rotation + uniform scale +
  translation. Seeds = the as-exported placement, 24 proper axis
  permutations, and 4 principal-axis (PCA) frame alignments - the PCA seeds
  are what catch arbitrary baked rotations - each under three scale
  hypotheses (RMS-radius ratio, 1.0, and the median per-principal-axis
  extent ratio, which survives a cut along one axis). Every seed is refined
  by trimmed ICP re-solving Horn's closed-form absolute orientation from
  nearest-live-vertex correspondences. Because mask vertices are unmoved
  copies of live vertices, a correct export converges to ~zero residual
  under any settings; median/max residual, the recovered scale and the mesh
  RMS radius are reported in the status line.
- **Both handednesses are fitted** (`rtx.hairTest.maskMirrorMode`, default
  Auto). Exports can bake a reflection - a handedness-flipping axis
  convention or negative scale - and on a bilaterally symmetric character
  the reflected mask fits the MIRROR pose almost perfectly with a proper
  rotation, which grew the authored cut on the wrong side of Wolf Link.
  Auto fits the mask as-loaded and mirrored and keeps the strictly closer
  one: a genuine unmirrored export is bit-exact and cannot lose. A
  PERFECTLY symmetric mesh ties (geometry cannot decide), the tie prefers
  as-authored, and the overlay's Mask Handedness combo forces either way;
  the status line says "mirrored" when the flip was applied.
- **Fitted per mesh, on a copy.** Several meshes can share one tagged
  texture (Wolf Link plus a small second piece); each build fits its own
  copy of the mask against its own live vertices. A mesh the mask does not
  belong to converges poorly, is flagged **POOR FIT**, and scatters on its
  full surface instead of wearing a misplaced mask - same fallback as a
  missing/broken mask file. The known limitation: a mask missing a large
  fraction of its mesh combined with a wrong scale can defeat the fit; the
  residual number and POOR FIT flag make that loud rather than silent.

## 6. Fiber shading (the hair BCSDF)

Strands shade with the RTX Character Rendering SDK's hair BCSDF rather than
the opaque GGX + Lambert model — the difference between hair and geometry
that merely has strand shapes. The fiber response has three lobes:

- **R** — light reflecting off the fiber surface: the sharp, near-white
  sheen that runs along a coat.
- **TT** — light passing through the fiber: why a backlit animal glows at
  the silhouette.
- **TRT** — light reflected inside the fiber: the *coloured* secondary
  highlight, offset from the white one by the cuticle scale tilt.

Absorption is integrated along the path through each fiber, so colour
deepens with the distance light travels inside it. By default absorption
comes from the strand's own base colour (the source texture at the root),
so fur inherits the character's colours; melanin models are also available.

**Hair rides the opaque surface material interaction**, behind
`OPAQUE_SURFACE_MATERIAL_FLAG_IS_HAIR`, instead of being a fourth
polymorphic surface material type. This is deliberate and load-bearing:

- The material type field is **2 bits and fully allocated** (the fourth
  encoding is the subsurface extension). Widening it would shift every
  material flag, the TLAS instance custom index and the GBuffer type bits.
- Every BSDF *evaluation* entry point — RTXDI/ReSTIR target PDFs,
  `RAB_CalculateBRDF`, NEE, the NEE-cache MIS weights, ReSTIR GI final
  shading — is hard-typed to `OpaqueSurfaceMaterialInteraction`, and
  non-opaque types get **no direct lighting** (`integrator_direct`) and
  **no shadow attenuation** (`visibility`) at all. Riding the opaque
  interaction keeps hair inside light sampling, shadows and the denoiser,
  which is what makes fur converge instead of boiling.

Consequences worth knowing:

- The fiber tangent reaches the shader in the interpolated normal
  attribute. `SurfaceInteraction::interpolatedVertexNormal` exists (unbent,
  in every build configuration) precisely for this: the ordinary
  `geometryNormal` is bent toward the triangle normal, which for strand
  geometry would blend the tangent with the quad's face normal and destroy
  it.
- Transport is free: hair is never emissive, so the GBuffer's emissive
  words and the polymorphic form's unused `idata0` carry the octahedral
  tangent.
- Hair is **exempt from the hemisphere rejection** in NEE and the RTXDI
  BRDF hook — a fiber legitimately scatters light arriving from behind it.
- Sampled lobes reuse the opaque lobe identifiers, so the denoiser's
  diffuse/specular routing keeps working: the R lobe drives the specular
  signal, the transmission lobes the diffuse one.
- The far-field model is the default because it provides the evaluation pdf
  MIS needs. Chiang's near-field model is selectable and is the reference
  per-fiber response, but without an evaluation pdf it cannot join MIS and
  is noticeably noisier.

Implementation: `rtx/concept/surface_material/hair_bcsdf.slangh` (frame,
evaluation, sampling) and the hair branches in
`opaque_surface_material_interaction.slangh`.

## 7. Attachment: weight-signature clusters (`clusterWeightBuckets`)

Strands are partitioned by their root's **quantized skinning weights**. Each
partition is a static mesh whose per-frame transform is

```
draw.objectToWorld × ( Σ wᵢ · pBoneMatrices[bᵢ] )   normalized by Σ wᵢ
```

Because every strand in a cluster shares one weight signature, that single
blended matrix is *exactly* the skinning matrix linear blend skinning would
hand each of its vertices. So this is exact skinning at rigid-cluster cost:
no per-frame BLAS work, no skinning dispatch, and motion vectors free from
the moving instance.

The submitted cluster draws **clear the skinning state**
(`DrawCallState::clearSkinningState`) and carry **no blend streams**. Both
matter: `processGeometryInfo` compares `getSkinningState().boneHash` against
`lastBoneHash`, so a hair draw that kept either would take `kUpdateBVH` every
animation frame and re-interleave, re-skin and refit every hair vertex.

`clusterWeightBuckets` (default 8) is the number of steps weights round to.
It is the whole trade:

- Position error scales with the weight error, so **each doubling of the
  bucket count halves the error** (measured; see §8).
- Every distinct signature is one instance and one BLAS, so cluster count
  scales with it too — roughly 300 clusters for 200k strands over a 40-bone
  skeleton at 8 buckets.
- Signatures per mesh are capped at `kMaxClustersPerMesh` (1024); exceeding
  it halves the bucket count and regroups. Clusters under
  `kMinStrandsPerCluster` (32) fold into their dominant bone.
- **1 means dominant-bone grouping** — one bone per cluster, which is what
  this system did before signatures existed.

### Where a root's weights come from — and why this tore the coat

A root's signature is the **barycentric blend of its triangle's three corner
bindings**, not the binding of one bound vertex. That distinction is the
whole fix, and it is easy to get wrong twice:

> **Single-influence per *vertex* is not rigid per *triangle*.** This game's
> matrix-palette characters really do carry one bone per vertex (aurora's
> pn-matrix path writes weight 1.0 and a single index). But a triangle whose
> corners sit on *different* bones is interpolated across its face — the skin
> stretches smoothly over the joint, which is exactly why the body looks
> continuous there. A strand bound to one corner's bone is rigidly attached
> to one end of a surface that is stretching, so it walks off the body.
> Blending by barycentric weight is precisely what the surface itself does.

Bald patches therefore appeared in bands along bone boundaries — neck,
shoulder, jaw, haunch — while the skin beneath stayed whole. Measured
against exact LBS, snapping to one bone is **16× worse** than 16 buckets;
the residual is a large fraction of a strand length, which is the size of
the visible gap.

Mask scatter needs one extra step, because a root then sits on a *mask*
triangle that carries no skinning at all. The build makes a vertex →
incident-triangle table for the live mesh and takes the incident triangle
whose surface passes closest to the root (Ericson's closest-point test),
then uses that point's barycentrics. Both paths end in the same place: up to
four influences, strongest kept, normalized.

Models that *are* genuinely enveloped reach the same machinery from the
other direction: `dusk::gpu_skin` inverts `J3DSkinDeform`'s per-joint skin
lists into a per-position influence table (up to
`GX_AURORA_MAX_SKIN_INFLUENCES` = 4) and emits `GXSetSkinning`, so aurora
writes real multi-bone weights per vertex. Those blend into the corner
bindings before the barycentric step.

> The overlay reports total clusters **and how many span multiple bones**. On
> a skinned character that second number must be non-zero: it counts the
> clusters straddling a joint, which are the ones that used to tear. A mesh
> reporting 0 blended clusters is either unskinned or has no triangle
> crossing a bone boundary.

## 8. Validation harness

Development happens in a Linux container; nothing here requires a GPU:

- `scratchpad/tu/mask_check.cpp`: compiles the **real**
  `rtx_hair_mask.{h,cpp}` (std-only) and validates OBJ forms, grid
  nearest-neighbor against brute force, and alignment recovery of known
  transforms to ~0 residual.
- `scratchpad/tu/surface_check.cpp`: compiles the **verbatim body** of
  `buildSurfaceHairGeometry` against stubs and a synthetic multi-bone mesh
  (`extract_bodies.py` lifts the bodies out of the source, so the harness
  cannot drift from what ships); validates signature grouping (correct bone
  pairs and weights, no blend streams on a cluster, distinct hashes, every
  strand accounted for), dominant-bone collapse at 1 bucket, the unskinned
  single-cluster case, and mask-confined scatter.
- The surface harness also validates the shading bake (root vertices
  strictly darker than tips) and round-trips the strand disk cache
  bit-identically including each cluster's weight signature, plus rejection
  of corrupt files and of the superseded v1 cache layout.
- The surface harness's decisive case is a mesh whose vertices are all
  **single-influence** (weight 1.0, as aurora's pn-matrix path writes) but
  whose triangles straddle bones: it must produce blended clusters. Before
  the barycentric fix it produced none, and that is exactly the shape of the
  bug that reached the screen.
- `scratchpad/tu/cluster_check.cpp`: compiles the **verbatim**
  `quantizeBinding` lambda and drives it against a reference LBS
  implementation over random skeletons. Confirms the properties the fix
  rests on: signatures are canonical under influence reordering, weights
  stay bucket multiples, single-influence roots are bit-exact at every
  bucket count, error halves per bucket doubling (mean 12.26 → 0.77 from 1
  to 16 buckets), and signature counts stay under the cluster cap.
- MSVC builds run in CI on every push of `claude/**`.

## 9. Notes toward an upstream PR

If this gets re-implemented for upstream Remix:

- `rtx_hair_mask.{h,cpp}` is already dependency-free and lifts as-is.
- The snapshot protocol (§2) is the part upstream must own differently:
  either an official "CPU-stable geometry copy" capture flag, or growth at
  capture time instead of frame prep.
- The per-texture tag + per-texture mask file convention generalizes; a
  per-material USD attribute would be the Remix-native shape.
- The fiber BCSDF (§6) is the part with a real upstream design question:
  riding the opaque interaction behind a flag is the right trade *here*
  (the alternative is a fourth material type plus a polymorphic BSDF
  evaluation API that does not exist yet), but upstream would more likely
  want that polymorphic eval API introduced first, at which point hair
  becomes a clean fourth type.
- The attachment modes only assume what Remix already provides: per-vertex
  blend streams, a per-draw bone palette, and instance transforms. Nothing
  is game-specific except the guarantees in §3, which upstream would state
  as requirements ("stable rest-pose geometry, consistent draw
  granularity") rather than implement.

## 10. Options reference (all `rtx.hairTest.*`)

| Option | Default | Meaning |
| :-- | :-- | :-- |
| `enable` | false | master switch for strand fur |
| `surfaceStrandCount` | 60000 | total strand budget across all tagged meshes, area-split (max 2M; ~36 B × 12 verts × segments per strand) |
| `surfaceHairLength` / `surfaceStrandRadius` | 2.0 / 0.02 | strand shape in world units |
| `segmentsPerStrand` | 4 | curve segments per strand (geometry cost scales linearly) |
| `hairLengthJitter` | 0.3 | per-strand length variation 0..1 |
| `tipRadiusScale` | 0.4 | tip radius relative to root |
| `frizz` / `curliness` / `curlTurns` | 0.3 / 0.15 / 2.0 | growth direction jitter and helical curl |
| `gravityDroop` | 0.15 | downward bend along the strand |
| `strandOcclusion` | 0.45 | root-to-tip darkening baked into vertex colors |
| `enableFiberBcsdf` | true | shade strands with the hair BCSDF (regrows: changes what the normal slot stores) |
| `fiberBsdfModel` | 0 | 0 far-field (has an eval pdf, MIS-capable), 1 Chiang near-field |
| `fiberAbsorptionModel` | 0 | 0 from the strand's base color, 1 melanin, 2 melanin normalized |
| `fiberMelanin` / `fiberMelaninRedness` | 0.8 / 0.05 | melanin absorption inputs |
| `fiberRoughness` | 0.25 | far-field fiber roughness (highlight tightness) |
| `fiberLongitudinalRoughness` / `fiberAzimuthalRoughness` | 0.4 / 0.6 | Chiang beta_m / beta_n |
| `fiberCuticleAngle` | 3.0 | cuticle tilt in degrees; separates the white and coloured highlights |
| `fiberIor` | 1.55 | fiber index of refraction (keratin) |
| `fiberPrimaryHighlightScale` | 1.0 | artistic scale on the R lobe |
| `fiberDiffuseWeight` / `fiberDiffuseTint` | 0.0 / white | artificial fill lobe for dense fur |
| `fiberDenoiserRoughness` | 0.4 | perceptual roughness hair reports for denoising/demodulation |
| `scatterSeed` | 1337 | scatter/variation seed |
| `clusterWeightBuckets` | 8 | steps skinning weights quantize to when grouping strands into clusters; higher tracks joints more closely at more clusters, 1 is dominant-bone grouping (regrows) |
| `evenScatter` | true | best-candidate scattering |
| `maskDirectory` | `hair_masks` | scatter mask directory, relative to the game exe |
| `maskMirrorMode` | 0 | mask handedness: 0 auto-detect, 1 as authored, 2 force mirrored |
| `strandCacheDirectory` | `hair_cache` | strand disk cache directory; empty disables |
