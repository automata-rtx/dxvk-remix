# Dusklight hair — strand fur for Remix scenes

Status: working feature on `Fixed-Function-dev` lineage. This documents the
whole fur system: surface hair on hair-tagged meshes, the data-lifetime
rules that were expensive to learn, the strand disk cache, artist scatter
masks, shading, and the attachment modes. It is written so the system could
later be re-implemented as an upstream Remix PR (§8) — the modules are
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
  There is no separate hair shading pass and the main path tracer is
  untouched.

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
   otherwise. The bound vertex supplies the root UV (strand color), the
   dominant bone (decoded with the skinning shader's exact conventions:
   `numBones-1` stored weights + implicit last, raw index bytes), and the
   fallback normal.
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

5. **Shading bake**: every strand vertex carries the *smooth* surface
   normal its root grew from (the mask's authored normal when present), so
   fur shades with the pelt's curvature instead of individual tube facets,
   and a root-to-tip darkening gradient in the vertex colors
   (`strandOcclusion`) stands in for the strand-to-strand self-shadowing
   the path tracer cannot afford to resolve — a real coat is darkest where
   it is deepest. The strands otherwise shade with the source mesh's
   converted legacy material: there is **no hair BCSDF in the main
   integrator** (an anisotropic fiber response would be an upstream-scale
   material-system change; see §8).

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

## 6. Attachment modes (`surfaceAttachmentMode`)

- **0 — Rigid per-bone clusters** (default): strands grouped by their root's
  dominant bone; each group is a static mesh whose per-frame transform is
  `draw.objectToWorld × pBoneMatrices[bone]`. No per-frame BLAS or skinning
  cost; motion vectors come free from the moving instance. The submitted
  cluster draws **clear the skinning state**
  (`DrawCallState::clearSkinningState`) — geometry caching keys on the bone
  hash, and a stale animated hash forces a full re-process + BLAS rebuild
  every frame.
- **1 — Skinned**: one geometry carrying the source's blend data, deformed
  by Remix's GPU skinning; per-frame BLAS update cost.
- **2 — Hybrid**: rigid clusters cover each bone's *core region* (roots
  within `hybridClusterRadiusScale ×` the bone's influence radius — the RMS
  distance of its vertices from their centroid, so the cutoff scales per
  bone); `hybridSeamStrandCount` additional strands scatter into the areas
  outside every core and form a **separate skinned seam set with its own
  BLAS**. Only the small seam set pays per-frame skinning.

Reality check for this game: aurora's characters are single-influence
(weight 1.0 per vertex; GX blends *inside* the palette matrices), so rigid
clusters are already mathematically exact and mode 2 renders identically to
mode 0. Hybrid exists for multi-influence content (`J3DSkinDeform` actors,
future GXSetSkinning models with up to 4 weights).

## 7. Validation harness

Development happens in a Linux container; nothing here requires a GPU:

- `scratchpad/tu/mask_check.cpp`: compiles the **real**
  `rtx_hair_mask.{h,cpp}` (std-only) and validates OBJ forms, grid
  nearest-neighbor against brute force, and alignment recovery of known
  transforms to ~0 residual.
- `scratchpad/tu/surface_check.cpp`: compiles the **verbatim body** of
  `buildSurfaceHairGeometry` against stubs and a synthetic two-bone mesh;
  validates skinned/rigid/unskinned builds, raw blend-data copies,
  mask-confined scatter (all strands land on the masked half and bind to
  its bone), and the hybrid partition (cluster counts, seam set size, seam
  blend data, distinct hashes).
- The surface harness also validates the shading bake (root vertices
  strictly darker than tips) and round-trips the strand disk cache
  bit-identically for skinned entries (blend buffers included) and rigid
  cluster entries, plus rejection of corrupt cache files.
- MSVC builds run in CI on every push of `claude/**`.

## 8. Notes toward an upstream PR

If this gets re-implemented for upstream Remix:

- `rtx_hair_mask.{h,cpp}` is already dependency-free and lifts as-is.
- The snapshot protocol (§2) is the part upstream must own differently:
  either an official "CPU-stable geometry copy" capture flag, or growth at
  capture time instead of frame prep.
- The per-texture tag + per-texture mask file convention generalizes; a
  per-material USD attribute would be the Remix-native shape.
- The attachment modes only assume what Remix already provides: per-vertex
  blend streams, a per-draw bone palette, and instance transforms. Nothing
  is game-specific except the guarantees in §3, which upstream would state
  as requirements ("stable rest-pose geometry, consistent draw
  granularity") rather than implement.

## 9. Options reference (all `rtx.hairTest.*`)

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
| `scatterSeed` | 1337 | scatter/variation seed |
| `surfaceAttachmentMode` | 0 | 0 rigid clusters, 1 skinned, 2 hybrid |
| `hybridClusterRadiusScale` | 0.75 | rigid-core cutoff as a fraction of each bone's influence radius |
| `hybridSeamStrandCount` | 15000 | extra strands for the hybrid's skinned seam set |
| `evenScatter` | true | best-candidate scattering |
| `maskDirectory` | `hair_masks` | scatter mask directory, relative to the game exe |
| `maskMirrorMode` | 0 | mask handedness: 0 auto-detect, 1 as authored, 2 force mirrored |
| `strandCacheDirectory` | `hair_cache` | strand disk cache directory; empty disables |
