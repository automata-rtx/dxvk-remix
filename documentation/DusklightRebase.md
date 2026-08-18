# Rebase surface: every upstream file this fork touches

Read this before starting a rebase onto upstream dxvk-remix, rather than
discovering the list a conflict at a time. It covers **the whole fork**, not just
the atmosphere.

**What this list is worth:** each file below either names `dusklight` in the tree
or was identified from the change that owns it. It was **not** produced by
diffing an upstream tag, so read it as "where our changes are visible", not as
proof nothing else moved — still read the conflict list. Line numbers are
deliberately omitted; they rot.

---

## New files — zero upstream churn

`rtx_dusklight_atmosphere.{h,cpp}`, `rtx_dusklight_grade.{h,cpp}`,
`rtx_dusklight_emissive.h`, `rtx_dusklight_texrep.{h,cpp}`,
`rtx_dusklight_water.h`, `rtx_dusklight_{env,game}.h`,
`rtx_dusklight_drawmeta.h`, `src/d3d9/d3d9_rtx_matrep.h`, `rtx_agx.{h,cpp}`,
`rtx_gt7.{h,cpp}`, `shaders/rtx/pass/{dusklight,tonemap}/*`,
`shaders/rtx/pass/bloom/bloom_dusklight_*`, and
`shaders/rtx/pass/tonemap/reference/` — kept verbatim; **fix the port, never the
reference.**

Shaders are auto-discovered by `compile_shaders.py`'s `os.walk`, so no build-file
edit; `.cpp`/`.h` need a `meson.build` line (`src/d3d9/meson.build` for the d3d9
half), which the two header-only files skip.

**`shaders/rtx/algorithm/volume_lighting.slangh` is the one fork file that is
neither new nor upstream's** — it shadows the RTXDI submodule's copy of the same
relative path. Its own header, `volume_lighting.slangh:24-51`, states the
mechanism, the `-I` ordering it depends on, the RTXDI pin, and exactly what a
rebase must re-check. Read it there.

---

## Upstream files touched

Each row is one guarded hook unless it says otherwise.

**Atmosphere — fog, sky, sky light**

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp` | fog state selection + `prepareSceneData` | `DusklightAtmosphere::active()` |
| `rtx_global_volumetrics.cpp` `getVolumeArgs` | early branch to the Dusklight derivation; `volumetricFogAnisotropy` from our medium | same |
| `rtx_global_volumetrics.cpp` `getVolumeArgs` | one **unguarded** `fillVolumeRampArgs` — writing the "off" state is the point | none |
| `rtx_composite.cpp` | fills `DusklightCompositeArgs`; **skips `rtx.fogColorScale` on the legacy depth fog** | args `active()`; skip **`fogColorOverridden()`**, the narrower question |
| `composite.comp.slang` | `applyFog` range split (mode 0) / top-up residual (modes 1 **and 2**, same branch by design); the same top-up on the stochastic alpha blend; three helpers by `sampleDomeLightTexture` | `cb.dusklightArgs.enable` |
| `volume_args.h`, `composite_args.h`, `froxel.slangh` | ramp fields, one args struct, `previousFroxelMaxDistance` | additive; zero is upstream behaviour |

**Effect lights** — almost all game-side (`dusklight-ao/docs/effect-lights.md`).

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_light_manager.cpp` | **two** unguarded corrections, both about the RTXDI buffer index on API lights: `addExternalLight` preserves it across an overwrite, `prepareSceneData` range-checks it. Mechanism at `rtx_light_manager.cpp:824-828`. **A rebase that re-applies one and not the other gets the worse half of each** | none |
| `rtx_dusklight_{game,env}.h` | the `effectLight*` block under the `// Effect lights.` header (`game.h:305`) and the `effLights*` readouts (`env.h:331`). **Do not write a count here** — the invariants script prints the live one | additive |
| `dxvk_imgui.cpp` | the **Effect Lights** section of the **Lights** tab | own block |

**Materials** (`aurora-ao/docs/dx9/remix-material-interface.md` §9–§10)

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_instance_manager.cpp` | the emissive patch, one site; `CheckRtInstanceSize`'s constant needs updating whenever `RtSurface` grows | `rtx.dusklight.emissive.enable` |
| `rtx_materials.h` | ramp endpoints on `RtSurface` (`data15.w`, `textureFlags` 15–16) | additive; size guarded |
| `rtx_materials.cpp` | ramp/emissive/side-channel members in `hashStructByMemory` **and** `computeIdentityHash` | must sum to `sizeof(T)` exactly |
| `surface.h`, `opaque_surface_material_interaction.slangh` | GPU fields; `albedo = mix(rampLo, rampHi, albedo)` | `rtx.dusklight.rampMaterials` |
| `d3d9_rtx_utils.cpp` | `isVertexColorBakedLighting` per draw from `Specular.r` | falls back to the option |
| `d3d9_rtx.cpp` | one matrep call at the tail of `processTextures` | `rtx.dusklight.matrep` |

**API assets, made capturable and replaceable.** These two change upstream
*behaviour* rather than adding a branch, so a rebase re-applies intent, not an
`if`. **Check them first.**

| File | Change |
| :-- | :-- |
| `rtx_remix_api.cpp` | API mesh hashes derived from the submitted vertex/index data; upstream's `hack_getNextGeomHash` removed |
| `rtx_scene_manager.cpp` `submitExternalDraw` | consults `getReplacementMaterial` before using the supplied material |

**HD texture packs** (`aurora-ao/docs/dx9/texture-replacements.md`)

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp` | `determineMaterialData` tail call **after** `as<OpaqueMaterialData>()`, albedo only — the conversion sets the sampler override and ignore-alpha flag, so it must not become a merge; plus one `usePreservePath` term and two `onFrameEnd` counters | `rtx.dusklight.texrep.enable` |
| `d3d9_device.cpp` `BindTexture` | the rasterized/HUD site, and **the only place the fork touches this file** — dropping it loses the HUD half silently while the world half keeps working. Re-check with `git diff origin/main...HEAD -- src/d3d9/d3d9_device.cpp`: exactly two hunks, the include and `BindTexture` | `texrep.applyToRaster` |

**Water** (`remix-material-interface.md` §11) shares rows with texture packs and
with the ramp — which is why it was *rebased* rather than merged: it was built
against a side band that had since been claimed, and the conflict's obvious
resolution deletes texture packs. Read `DusklightSideChannels.md` before
resolving a conflict here.

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp` | the water branch, **after** the replacement lookup and **before** `as<OpaqueMaterialData>()` — load-bearing both ways; replacement coercion; two bounded logs | `water.enable`, `water.applyToReplacements` |
| `rtx_instance_manager.cpp` | hides the projected overlay; texcoords from options | `hideProjectedLayer`, `animateTexcoords` |
| `rtx_materials.{h,cpp}` | `texcoordElementCount`, `isTexcoordProjected` **in existing padding so the struct does not grow**; projective encoding in `writeGPUData` | additive |
| `rtx_draw_call_tracker.cpp` | `_pad0` becomes `texcoordProjection`, same size | additive |
| `surface.h`, `surface_interaction.slangh` | projective divisor in the eye-origin words | `textureFlags` 21–22 |
| `rtx_opacity_micromap_manager.cpp`, `rtx_types.h`, `d3d9_rtx*` | plumbing | additive |

**Overlay, bloom, plumbing**

| File | Change | Guard |
| :-- | :-- | :-- |
| `dxvk_imgui.{cpp,h}` | the F1 Dusklight overlay and its **eight** tabs — `DusklightOverlay.md` | own functions, one call site each |
| `rtx_bloom.{h,cpp}` + `bloom.h` | the Dusklight bloom mode | `rtx.bloom.dusklight` |
| `rtx_context.{h,cpp}` | `dispatchDusklightGrade`; bloom stage ordering | `DusklightGrade` enable |
| `dxvk_objects.h`, `dxvk_device.cpp` | the modules exposed as `metaDusklight*` | additive |

**Tone mapping and auto exposure** (`ToneMappingExposureNotes.md`)

> **A different shape from every block above, and a rebase should expect that.**
> The Dusklight work sits in `dusklight_*` files behind single guarded hooks.
> This does not: it **rewrites** two upstream passes and edits eight upstream
> shaders, and almost none of the filenames say `dusklight` — the name-matching
> that produced the rest of this list would have found none of it.

`rtx_auto_exposure.{h,cpp}` and `auto_exposure*.comp.slang` are **rewritten**
(removed options stay registered as deprecated no-ops).
`rtx_tone_mapping.{h,cpp}` and `rtx_local_tone_mapping.{h,cpp}` take a
`tonemapOperator` enum in place of `finalizeWithACES`, migrated on load.
`rtx_context.cpp` passes `resetHistory` to auto exposure on a camera cut, which
upstream never did. `tonemapping.h`, `local_tonemapping.h`,
`tonemapping_apply_tonemapping.comp.slang`, `local_tonemapping.slangh`,
`luminance.comp.slang` and `final_combine.comp.slang` carry the operator args,
the dispatch and `localTonemapRuler()` — and the missing `suppressBlackLevelClamp`
fix. `ThirdPartyLicenses.txt` gains three MIT notices.

**Three things to check first here:**
1. `ToneMappingApplyToneMappingArgs` is **exactly** at the 128-byte push-constant
   limit, guarded by three `static_assert`s. If upstream adds a field, move the
   operator arg blocks into a uniform buffer — do not shrink them.
2. The operator enum is mirrored in `rtx_agx.h` and `tonemapping.h`, tied by
   `static_assert`.
3. `rtx.autoExposure`'s deprecated options must stay registered, or existing
   `rtx.conf` files log unknown-option noise.

**`RtxOptions.md` is generated.** It will conflict; the resolution is to
regenerate it by running the DLL on Windows, never to merge it by hand.

---

**Rule for every guarded hook:** one branch, no reformatting of surrounding code,
and the guarded path calls into our module rather than inlining logic — so a
conflict resolves by re-applying a single `if` rather than re-deriving intent.
The two API rows are the deliberate exception.

**Game side** (`dusklight-ao`): everything in `src/dusk/remix_*.{cpp,hpp}` and
`effect_lights.{cpp,hpp}`. `d_kankyo.cpp` churn is one capture call following the
existing `dKy_celestial_orbit_z_ratio` pattern; `d_particle.cpp` carries one
guarded call at the tail of `dPa_simpleEcallBack::set`. Aurora's half of the
material transport rebases against aurora, not against Remix.
