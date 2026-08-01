# Dusklight hair — LSS strand hair for Remix scenes

Status: working feature on `Fixed-Function-dev` lineage. This documents the
whole hair system: the sphere tech demo, surface hair on hair-tagged meshes,
the data-lifetime rules that were expensive to learn, artist scatter masks,
and the attachment modes. It is written so the system could later be
re-implemented as an upstream Remix PR (§8) — the modules are deliberately
layered for that.

## 1. What it is

Strand hair rendered inside the path-traced Remix scene, built from the RTX
Character Rendering SDK's curve representation:

- Strands are chains of Linear Swept Sphere (LSS) segments. On drivers with
  `VK_NV_ray_tracing_linear_swept_spheres` (RTX 50 series) they trace as
  native LSS primitives; otherwise the SDK's DOTS tessellation (4 triangles /
  12 vertices per segment, radii pre-divided by `(π/4)/sin(π/4)` so the
  silhouette matches) represents the same segments.
- Two consumers:
  1. **The test sphere** (Hair Test tab): its own BLAS/TLAS, shaded by the
     SDK's Chiang / far-field BCSDFs against the scene light pool, composited
     before upscaling with depth + motion vectors. A DOTS proxy of the same
     strands always lives in the scene TLAS so hair shadows, occludes, and
     appears in GI.
  2. **Surface hair**: fur grown across game meshes whose color texture
     carries the `rtx.hairStrandTextures` tag (Game Setup tab). Strand
     geometry goes through the regular draw path — it is ordinary scene
     geometry, path traced and denoised, colored by the source diffuse at
     each strand's root UV.

Files:

| File | Role |
| :-- | :-- |
| `src/dxvk/rtx_render/rtx_hair_test.{h,cpp}` | the system: sphere demo + surface hair growth, caching, attachment modes, overlay UI |
| `src/dxvk/rtx_render/rtx_hair_mask.{h,cpp}` | artist scatter masks: OBJ load, alignment auto-fit, nearest-vertex grid. **Std-only, no dxvk types** — unit-testable standalone |
| `src/dxvk/rtx_render/rtxcr_geometry/` | vendored RTXCR geometry lib (LSS/DOTS conversion), byte-identical to the SDK |
| `submodules/rtxcr` | RTXCR material lib (hair BCSDFs), pinned v1.2.0 |
| `src/dxvk/shaders/rtx/pass/hair_test/` | the sphere demo's shading pass |

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
   Blender for low-poly meshes) with jitter/frizz/curl/droop, identical
   construction to the sphere demo.
4. **Assembly**: strand subsets become `RasterGeometry` pieces (36-byte
   interleaved vertices, DOTS triangles, stable derived hashes) submitted as
   copies of the source draw with swapped geometry — material, transforms
   and skinning carry over. All hair draws set
   `InstanceCategories::IgnoreOpacityMicromap` (strands are solid; an
   alpha-tested source material once turned 39M strand triangles into a
   6.2 GB micromap request).

Budget: `surfaceStrandCount` is a **total** across all tagged meshes, split
by surface area, grown amortized (~30k strands/frame), with a live
`strands / budget` readout in the overlay.

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
   status, and a Reload button (regrows without restarting).

Runtime (`rtx_hair_mask.{h,cpp}`):

- Minimal OBJ parser (`v`/`vn`/`f`, quads/n-gons fan-triangulated, negative
  indices).
- **Alignment auto-fit**: tries identity plus the common exporter axis swaps
  (±90°/180° frame rotations), a centroid translation, then ICP-style
  translation refinement against nearest live vertices. Because mask
  vertices are unmoved copies of live vertices, a correct export fits with
  ~zero residual; the median/max residual is reported in the status line, so
  a wrong export setting is a visible number, not mystery fur.
- Missing/broken mask → falls back to full-surface scatter with a status
  note.

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
| `enable` | false | master switch (sphere demo + surface hair) |
| `surfaceStrandCount` | 60000 | total strand budget across all tagged meshes, area-split |
| `surfaceAttachmentMode` | 0 | 0 rigid clusters, 1 skinned, 2 hybrid |
| `hybridClusterRadiusScale` | 0.75 | rigid-core cutoff as a fraction of each bone's influence radius |
| `hybridSeamStrandCount` | 15000 | extra strands for the hybrid's skinned seam set |
| `evenScatter` | true | best-candidate scattering |
| `maskDirectory` | `hair_masks` | scatter mask directory, relative to the game exe |
| `surfaceHairLength` / `surfaceStrandRadius` | 2.0 / 0.02 | strand shape (plus the shared jitter/frizz/curl/droop set) |

Sphere-demo options (strand shape, BCSDF material, placement) are documented
inline in `rtx_hair_test.h`.
