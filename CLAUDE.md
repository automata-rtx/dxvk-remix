# Claude session notes — dxvk-remix (automata-rtx fork)

## Owner's environment: interactive approval prompts are BROKEN

Any tool call that pops an interactive approval/authorization prompt for the
owner is bugged across ALL of their Claude Code sessions — the prompt always
resolves as "no approval given" (e.g. MCP calls returning
`MCP error -32003: MCP tool call requires approval`, or the `add_repo`
authorization flow looping back to "there was no approval").

**Never rely on a tool that requires interactive approval.** Route around it:

- Reading other public repos: fetch files via `raw.githubusercontent.com`
  instead of the `add_repo` flow.
- Scheduling / reminders (`send_later` etc.): use a background `Monitor` /
  background Bash watcher instead.
- Questions for the owner: ask in plain chat text, not interactive pickers.

## What this fork is for

This is a fork of NVIDIA's RTX Remix runtime, carrying changes that exist to
serve **one game**: Dusklight, a Twilight Princess port rendering through a
GX→D3D9 fixed-function backend. Nothing here is general-purpose Remix work
unless it says so.

**Start at `documentation/DusklightAtmosphere.md` (the rendering) and
`documentation/DusklightOverlay.md` (the control plane).** Between them they
carry the design, the measurements, and a §14 "Facts that were expensive to
learn" that exists because several of them were wrong in an earlier draft.

`documentation/ToneMappingExposureNotes.md` covers the tone mapping and auto
exposure work, which is **not** Dusklight-specific — five of the six defects it
records are upstream dxvk-remix bugs and are written up so they can be carried
back on their own.

| Repo | Role | Its docs |
| :-- | :-- | :-- |
| `automata-rtx/dxvk-remix` | **this repo** — the Remix fork | `documentation/Dusklight*.md` |
| `automata-rtx/dusklight-ao` | the game | `docs/kankyo-remix.md` ← the overall entry point; `docs/japanese-naming.md` for reading its symbol names |
| `automata-rtx/aurora-ao` | GX→D3D9 backend, `extern/aurora` in dusklight | `docs/dx9/` |

## The game's symbols are named in Japanese; this fork's are not

**This fork's code is `camelCase` English** (`AGENTS.md`, Naming Conventions),
and that does not change. But every *game-side* symbol these documents quote is
romanized Japanese, preserved by the decompilation from the original Japanese
team — and reading one as English gets you the wrong file:

- `kankyo` = 環境, **environment** — the system this whole fork's atmosphere
  work is driven by. `dKy_`, `dKyw_`, `dKyr_` are all it.
- `kumo` = 雲 cloud, `kasumi` = 霞 horizon haze, `moya` = 靄 mist — the sky and
  haze fields the atmosphere reads.
- `kytag01`…`kytag17` = *kankyo tag*, invisible per-area override actors.
- `vrbox` is the game's word for the skybox dome; `wether` is its own spelling
  of **weather**, not a typo, and neither is `dKyd_lightSchejule`.

**When you go reading game code, search in both romanizations.** The game tree
mixes kunrei-shiki (`si`, `tu`, `ti`, `sya`) with Hepburn (`shi`, `tsu`, `chi`,
`sha`) *for the same word*, so one spelling finds half a feature and an empty
grep is not evidence of absence. Full reference and glossary:
`dusklight-ao/docs/japanese-naming.md`.

**And `export LC_ALL=C.UTF-8` before grepping it for Japanese.** Nearly 500 game
files carry literal kana/kanji — the original team's debug-panel labels, which
are what settled the `kasumi` near/far question above. Under the default `POSIX`
locale, `grep -P` on a kana/kanji class silently matches nothing.

## Branches — ALL THREE repos use the same structure

- **`Fixed-Function-dev` — the working branch. ALL development commits land
  here, in every one of the three repos.** This repo has no `Fixed-Function`
  integration branch and does not need one.
- `main` is the upstream tracker plus one merged PR. Do not develop on it.

**Push only your session branch.** The owner merges to `Fixed-Function-dev`
themselves, at milestones they choose. (An auto-mirror rule existed until
2026-07-29 and was revoked.)

```
git push -u origin <session-branch>        # yes
git push origin HEAD:Fixed-Function-dev    # NO - the owner does this
```

If a session branch is about to be deleted and its work is not yet merged,
**say so and stop** rather than mirroring it.

> **The containment check.** On 2026-07-28 this repo's `Fixed-Function-dev` was
> found **19 commits behind** its `claude/*` branch, immediately before that
> branch was to be deleted — which would have destroyed the entire atmosphere,
> overlay, warp and clock work. Before anyone deletes a branch:
> `git rev-list --count origin/Fixed-Function-dev..origin/<branch>` must be `0`.
> A non-zero count is the **normal** state between milestones, so this is not a
> formality; it is the only thing between a routine cleanup and lost work.

## What the D3D9 renderer is for — read this before proposing a fix

**The raw fixed-function D3D9 image is never shown to a player.** It exists so
Remix's DX9→Vulkan translation picks the scene up automatically — geometry,
transforms, textures, most of a frame, for free. **Remix's renderer is the
product; D3D9 is the feed.**

So:

- **Fixed-function limits are not the ceiling.** Where the D3D9 stream cannot
  carry something faithfully enough to reach Remix, implement it **in Remix** —
  Remix API or a fork change — rather than contorting D3D9 to approximate it.
  All three repos are ours.
- **"Raw D3D9 stays correct" is not a design goal.** It is occasionally a handy
  safety property, never a reason to reject an approach. Documents written
  before 2026-08-04 sometimes treat it as a requirement; they are wrong and are
  being corrected as they are touched.

**Two exceptions still have to rasterize correctly:** the **HUD** (Remix
rasterizes UI draws rather than path-tracing them) and **alpha** (Remix reads
the stage's alpha to build opacity and the alpha test).

Full statement: `aurora-ao/docs/dx9/remix-material-interface.md` §0.

## The game's names are Japanese, and they are load-bearing

Twilight Princess is a Japanese production and this decompilation preserves the
original team's naming, so a material, actor or function name is usually a
*romanised Japanese word describing what the thing is*. Read it before inventing
a classification — the answer is very often already in the name.

Worked examples from the water work, all of which changed a decision:

| Name | Reading | What it meant |
| :-- | :-- | :-- |
| `cc_MA06_nami_v_x` | nami — wave | a wave pass, **not** interchangeable with the murk pass beside it |
| `cc_MA06_mizugiwa_v_x` | mizugiwa — water's edge | the shoreline |
| `cc_MA06_NigoriWater_v_x` | nigori — turbidity | the murky body |
| `cc_MA09_mera_v` | mera — shimmer | the shimmer pass |
| `ce_MA03_WaterKasan_v_x` | kasan (加算) — **addition** | an additively blended pass — and every material carrying it measured `SRC_ALPHA,ONE` |
| `cd_MA03_Funsui_v` | funsui — fountain | a fountain, an object rather than a lake layer |
| `cc_MA02_IndirectWater_v` | (indirect texturing) | the warp the game uses to fake refraction |

Two lessons worth carrying into unrelated features:

- **`kasan` is the case to remember.** The blend state was measured a session
  before anyone read the name, and the name had said it all along. Reading the
  vocabulary first would have saved the measurement.
- **A numeric tag is usually coarser than the name.** `MA06` alone covers the
  waves, the shoreline and the murk; a control that cut on the tag was built,
  recommended, and would have deleted two of the three. The suffix is where the
  distinction lives.

When adding a classifier over these names, prefer matching `_word` and `Word`
(the convention lowercases after the tag and capitalises inside a compound) over
a bare substring, so `minami` is not read as `nami` — and make "unrecognised"
mean "leave it alone".

## How this project works — read before proposing a fix

Five rules. They exist because each was learned the expensive way, and following
them is worth more than any individual fix.

### 1. Translate, don't tag

Every Remix project the world over works by hashing textures and hand-authoring
replacements, because the game is a closed box. **All three of our repos are
ours.** We can read the game's intent at the source and hand it to the renderer
directly.

So the default answer to "how do we make Remix understand X" is *translate the
game state*, not *tag the asset*. Tagging gives one answer per texture; this
game reuses textures across contexts constantly, so a tag is wrong somewhere
almost by construction. Translation is per-draw and is right everywhere.

### 2. A question we would have to ask the owner is a defect in the logging

The owner should not be the diagnostic instrument. Asking them to describe a
colour, count an artifact, or judge whether something looks "too dark" produces
answers that are honest and unusable — and it wastes a scarce test window.

**The target loop is: they play, they send a log, we know.** If a question
cannot be answered from a log, the correct response is to add the log line, not
to ask the question. Design instrumentation before designing the fix.

Corollary: **logs must be bounded and self-describing.** One line per distinct
thing, capped, with a truncation notice when the cap is hit, and enum names
spelled out so a reader without the source can follow. A log nobody can read is
the same as no log; a log that fills a disk is worse.

### 3. Do not write inference as finding

This project has three times recorded a plausible mechanism as a verified cause.
One of those shipped and turned out to be a no-op, and the documents kept saying
"FIXED" for a week.

State what you read, cite where, and mark inference as inference. A document
that says "unknown" is more valuable than one that says something confident and
wrong, because the second one stops the next person looking.

### 4. A fix that cannot be observed is a guess

Before shipping a change to a system with no instrumentation, add the
instrumentation. A change that alters behaviour *and* reports on itself is
fine — bundling saves a test window — but a change that alters behaviour and
stays silent cannot be evaluated except by looking at pixels, which is how this
project lost three rounds.

Say plainly what the regression signature of a change is, so it can be
recognised rather than discovered.

### 5. Say what was verified and what was not

"Compiles" and "is correct" are different claims. So are "CI green" and "tested
in game". Every doc entry and every hand-off should make clear which one it is.
There is no penalty here for saying a thing is untested; there is a real cost to
implying it was tested.

## The one coupling that has cost evenings

**The game and this DLL are a single protocol.** The game pushes
`rtx.dusklight.env.protocol`; this fork compares it against `kRequiredProtocol`
in `showDusklightRemixTab` (`src/dxvk/imgui/dxvk_imgui.cpp`).
**Protocol is at 11.** Build both sides from the same commit point, and bump
both in the same commit. Skew in either direction has cost an evening twice.
The Dusklight tab reports which side is old — read it before debugging
anything else.

## Effect lights — almost none of it is in this repo

Since 2026-08-06 the game's fires, lava and glows get real sphere lights, placed
at the **origin of the JPA effect that draws them** rather than at the position
of whatever point light the game registered. That distinction is the whole
system: a GameCube point light casts no shadow, so its position was free to be
wrong, and a path tracer casts a real shadow from exactly where the light is.
The old mirror (`rtx.dusklight.game.localLights`) still exists, now defaulting
**off**, purely so the two can be A/B'd.

**The decision layer is in the game** — `dusklight-ao/src/dusk/effect_lights.cpp`
reads the emitter table and the game's light registries, and the bridge submits
the result through the Remix API. Nothing in this repo classifies anything. What
*is* here is three things, and they are easy to change without realising they
belong to this system:

- the 20 `rtx.dusklight.game.effectLight*` options and the 11
  `rtx.dusklight.env.effLights*` readouts;
- the **Effect Lights** section of the Game tab in `dxvk_imgui.cpp`, including
  the warning shown when both light systems are on;
- both `rtx_light_manager.cpp` fork changes below — they exist *for* this system
  and upstream has no reason to want them.

Design, citations and what is and is not verified: **`dusklight-ao/docs/effect-lights.md`**.

Beware a name collision: upstream Remix has its own unrelated `rtx.effectLight*`
/ `rtx.lightConverter` feature, and its "Effect Light" section in the Lighting
tab. Grepping this repo for `effectLight` hits both.

## The Dusklight surface in this repo

| File | What it holds |
| :-- | :-- |
| `src/dxvk/rtx_render/rtx_dusklight_game.h` | `rtx.dusklight.game.*` — settings the **game** reads back every frame through the `getRtxOptionValue` export. Nothing here is read by Remix itself. |
| `src/dxvk/rtx_render/rtx_dusklight_env.h` | `rtx.dusklight.env.*` — readouts the game **writes** via `remixapi_SetConfigVariable`. All `NoSave`. |
| `src/dxvk/rtx_render/rtx_dusklight_atmosphere.{h,cpp}` | one medium driving fog, sky and sky-light; Hillaire physical sky |
| `src/dxvk/rtx_render/rtx_dusklight_grade.{h,cpp}` | the ambient grade stage |
| `src/dxvk/rtx_render/rtx_dusklight_emissive.h` | `rtx.dusklight.emissive.*` — self-illumination. **A rule, not a score**: self-lit (no TEV colour stage reads the rasterized channel) AND a colour of its own (authored in GX constants, not the vertex stream and not a bare texture pass-through) AND that colour reading as a glow (saturated **or** near-white-hot). Aurora ships the three facts in `D3DMATERIAL9::Specular.{a,b}` and `Emissive.rgb`; `Emissive.a` still carries the old evidence score but **nothing decides on it** — three revisions cut on it and all three missed the lava, which scores 0.00. Applied at one site in `rtx_instance_manager.cpp`. **Note the trap around the emissive colour: by default the shader re-applies the albedo's texture op to it** — `RtSurface::emissiveSource` (`textureFlags` bits 19–20) is what selects out of that. Also holds `rtx.dusklight.rampMaterials`, the two-colour ramp, which shares the same `D3DMATERIAL9` transport: the fork evaluates the GX combiner `a*(1-c) + b*c` from both endpoints rather than squeezing it into one D3D9 texture op. *Stock* Remix cannot express that lerp; this fork can |
| `src/dxvk/rtx_render/rtx_light_manager.cpp` | mostly upstream, but `addExternalLight` carries one fork change: it preserves the light's buffer index across an overwrite, the way the game-light path a few lines above already did. Without it every update to an API light drops its RTXDI temporal history for a frame, which upstream barely notices (its API lights are static scene lights) and this fork very much does (a flame's radiance animates, so its light updates constantly and would never accumulate any reuse at all). `prepareSceneData` carries a second, related correction: a buffer index is only meaningful if it was assigned during the previous frame, and a light that left `m_linearizedLights` entirely - which an API light does whenever `DrawLightInstance` is not called for it - is never reset by the loop's else branch. Trusting that stale index wrote past the end of the mapping buffer or mapped one light's temporal history onto another; it is now range-checked |
| `src/dxvk/rtx_render/rtx_dusklight_texrep.{h,cpp}` | `rtx.dusklight.texrep.*` — HD texture packs. The game loads its pack through `remixapi_CreateMaterial` (used purely as a file loader) and tags each draw with a 1-based index in `D3DMATERIAL9::Ambient.g`, with the stage it refers to in `Ambient.b`; this substitutes the loaded albedo at the **two** places a draw can consume a texture — `determineMaterialData` for ray-traced draws, `D3D9DeviceEx::BindTexture` for rasterized ones. Both are needed: UI draws never reach material resolution. **The pack never travels through D3D9**, so the game's own textures stay what Remix hashes — tagging, `rtx.conf` categories and USD bindings are unaffected by installing or changing a pack. **Tested good 2026-08-06.** Note `d3d9_device.cpp` `BindTexture` is the *only* place this fork touches that file: a rebase that drops it loses the HUD half silently while the world half keeps working |
| `src/dxvk/rtx_render/rtx_dusklight_water.h` | `rtx.dusklight.water.*` — water. The game recognises its own water by J3D material name (`dKy_bg_MAxx_proc`), which GX never carries, and marks each draw; aurora packs **all three facts into `D3DMATERIAL9::Power`** as `tag * 100 + layer * 10 + role`, and this decodes them. A `SURFACE` draw becomes a `TranslucentMaterialData` instead of falling through to `as<OpaqueMaterialData>()` — that fall-through is what made every water layer an opaque white sheet. A `PROJECTED` draw (MA02/MA10, a camera-projected fake reflection) is hidden. **One field for three facts on purpose**: the side band had two left and taking both would have left nothing. **The transport was rebased on 2026-08-11** — it was in `Ambient.g`/`.b`/`.a`, which HD texture packs already owned, and merging that as written would have deleted texture packs silently. Decimal packing, not bit fields, because `power=921` is legible in a log as MA09 / waves / surface. **Untested in game on this transport** |
| `src/dxvk/rtx_render/rtx_agx.{h,cpp}` | AgX look presets, the shared `TonemapOperator` enum, and the `finalizeWithACES` → operator migration. **Not Dusklight-specific** |
| `src/dxvk/rtx_render/rtx_gt7.{h,cpp}` | GT7 setup, a transcription of Polyphony's `initializeAsSDR()`. The reference `.cpp` is kept verbatim at `shaders/rtx/pass/tonemap/reference/` — fix the port, never the reference. **Not Dusklight-specific** |
| `src/dxvk/imgui/dxvk_imgui.cpp` | the F1 Dusklight overlay: `showDusklightOverlay` → `showDusklightWindow` → the three tabs |
| `src/d3d9/d3d9_rtx_matrep.h` | the material translation report (`rtx.dusklight.matrep`), one guarded call at the tail of `D3D9Rtx::processTextures` |

**API-submitted assets are capturable and replaceable** as of 2026-08-04, which
upstream they are not — **CI-green, not yet exercised by an actual capture in
game.** Two changes made it so: API mesh hashes are derived from
the submitted vertex/index data instead of a creation-order counter
(`rtx_remix_api.cpp` — upstream's `hack_getNextGeomHash`), and
`submitExternalDraw` consults `getReplacementMaterial` before using the supplied
material. Without the first, a capture wrote a hash that changed next launch;
without the second, external draws bypassed the replacer entirely because they
supply their material directly and so never reach `determineMaterialData`.

**Materials are not documented here.** How a captured D3D9 draw becomes a
material in this runtime — what survives the capture path and what silently
resolves to white — lives in `aurora-ao/docs/dx9/remix-material-interface.md`,
because the encoding side is aurora's. It is the system most often reasoned
about incorrectly on this project; read it before changing anything in
`d3d9_rtx.cpp`, `d3d9_rtx_utils.cpp`, or the emissive patch in
`rtx_instance_manager.cpp`. §9 covers self-illumination, including why the
thresholds are options rather than constants; §10 covers the two-colour ramp.

**`RtxOptions.md` is stale for `rtx.dusklight.*` — read the headers instead.**
It is generated by *running* the DLL with
`DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1`, so a Linux container cannot
refresh it and every session that adds an option leaves it further behind.
Recounted **2026-08-11**, after the effect-light branch and `Fixed-Function-dev`
merged, across all eight `RTX_OPTION*` surfaces: **166** declared against
**133** in the file, so it is **missing 35** — the 20 `game.effectLight*`, the
11 `env.effLights*` readouts, plus `catrepCommit`, `game.lanternInfiniteOil`,
and the two `atmosphere.kasumi*` haze-blend options added the same day — and
lists exactly **one** that no longer exists, `emissive.intensity`, renamed
to `brightness`. The declarations in `rtx_dusklight_*.h` are the authority; a
name absent from `RtxOptions.md` is not evidence it does not exist. Regenerate
on the next Windows run.

> **This paragraph was wrong for a day, and how it got that way is the point.**
> Before that merge it said "missing 31" and "lists 10 that no longer exist (the
> whole `texrep` family, **removed**…)". The `texrep` family was never removed —
> it lived only on `Fixed-Function-dev`, which the branch could not see, and the
> file had been regenerated from a build that *did* carry it. The merge brought
> both together and made all four numbers false **without touching this line**,
> so git reported nothing. Its line 15 still carries a caveat saying the
> `texrep` rows describe an unmerged branch; that is now false too, and since
> the file is generated rather than hand-edited it stays until the next
> regeneration. This is the "merges that succeed and are still wrong" section,
> happening to the file that documents it.

**Transport rules that are easy to get wrong** (full versions in
`documentation/DusklightOverlay.md` §1.1):

- Anything **action-triggering must be `RtxOptionFlags::NoSave`.** A persisted
  commit counter fires on next launch.
- Act on a commit counter **changing**, and **latch the first value seen
  without acting** — otherwise connecting to a Remix that outlived a game
  restart triggers the action.
- An empty string never crosses: `parseOptionValue(const std::string&,
  std::string&)` returns **false** when `value.size() == 0`
  (`src/util/config/config.cpp:1190`), so the old value survives. A readout
  that never changes is not evidence the push is dead.
- **ImGui here is 1.88.** No `SeparatorText` (arrived 1.89).
  `BeginDisabled`/`EndDisabled` are available.
- `dxvk_imgui.cpp` does **not** include `<cmath>`. Do not rely on a transitive
  one across three compilers.

## This runtime charges per draw, not per pixel

Established 2026-08-07 while chasing unusable frame rates in Dusklight's rain
and snow, and worth knowing before anyone reaches for a shading explanation
again.

A draw call too small to deserve its own BLAS is merged into a shared bucket —
but it **still contributes its own `VkAccelerationStructureGeometryKHR` and its
own surface**, and the bucket's BLAS is rebuilt whenever any of its geometry
moves (`rtx_accel_manager.cpp`: `buildInfo.geometryCount =
bucket->geometries.size()`, and the bucket's `originalInstances`). So a
thousand single-quad draws cost roughly a thousand times what the same
thousand quads cost inside one draw, and no amount of shading work changes it.
The game was emitting one `GXBegin`/`GXEnd` per particle quad; batching them
game-side fixed it (tested 2026-08-08).

**Two corollaries that were each mistaken for something else:**

- **`rtx.particleTextures` is not a performance control.** It sets
  `m_isUnordered` and `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR`
  (`rtx_instance_manager.cpp`) — which TLAS a draw lands in and how the resolve
  loop treats it. It does nothing about per-draw scene-management cost. **If a
  dense effect does not respond to it, the cost is draw count.**
- **Opacity micromaps working is not evidence they are helping.** They reduce
  per-pixel any-hit work, which is not the bottleneck when the bottleneck is
  BLAS rebuild and surface upload.

The measurement lives game-side: aurora logs `dx9.draws frames=600 mean=… peak=…`
once every 600 frames. A `peak` in the thousands is the signature.
`aurora-ao/docs/dx9/progress.md` §3.32,
`dusklight-ao/docs/remix-open-issues.md` issue 13.

**Not yet exercised:** `createBillboards` only runs for instances already in the
unordered TLAS, and `rtx.useIntersectionBillboardsOnPrimaryRays` is **false** by
default, so the intersection-billboard path does nothing on primary rays today.
Batched particle draws are the first geometry that path has had anything useful
to chew on — an optimisation to try, not a fix that is owed.

## Merges that succeed and are still wrong

**A clean `git merge` is not a correct merge.** Several Dusklight features are
developed on parallel branches that edit the same documents, and git only
compares *lines* — it cannot see that two branches have made the same sentence
false, or that a conflict's obvious resolution is the wrong one.

Two instances, both real:

- **The protocol double-bump.** Two branches independently took protocol 6 → 7,
  each editing `kRequiredProtocol` and the registry line. Git *did* conflict —
  which made it worse: both sides said `7`, so keeping either looks correct and
  ships two features claiming one version. The conflict was flagged; the
  **resolution** was the trap.
- **The side-channel map.** A feature claimed `D3DMATERIAL9::Ambient.g` and
  `.b`, updating three of the four places that describe the struct. The
  canonical table merged cleanly and kept advertising both as spare, so the next
  feature to want a channel would have taken one already in use — surfacing as a
  material bug nowhere near either change.

**So, after any merge — and before pushing one:**

```
python3 scripts/check_dusklight_invariants.py
```

It checks `kRequiredProtocol` against the prose and the version registry
(including **duplicate protocol numbers**, which is the double-bump), every
`D3DMATERIAL9` channel the fork reads against
`documentation/DusklightSideChannels.md`, `RtxOptions.md` coverage of every
declared `rtx.dusklight.*` option, and leftover conflict markers. The
`Invariants` workflow runs it on **every** push and PR, unfiltered by path or
branch — doc-only commits and `Fixed-Function-dev` merges are exactly when these
drift, and `build.yml` covers neither.

`RtxOptions.md` drift is reported as a **warning, not a failure**: it is
generated by running the runtime on Windows, and a check that blocks a merge on
something the author cannot do from their checkout gets disabled the first time
it is inconvenient.

**What it cannot check, and therefore what a human still has to:**

- whether a "tested in game" claim survived the change underneath it
- whether a document's *prose* still describes reality, as opposed to its
  numbers agreeing with the code
- whether two in-flight branches are about to take the same spare side channel
  or protocol number — nothing can see an unmerged branch, so **check the other
  live `claude/*` branches before taking either**

**As of 2026-08-11 exactly one side channel is left: `Ambient.a`.** Water took
`Power` — all three of its facts packed into that one field
(`tag * 100 + layer * 10 + role`), specifically so `Ambient.a` would survive for
something else. `claude/dusklight-remix-transparency-e7l766` has an unmerged
claim on it, and after that there is nothing: the feature after next has to pack
into an existing field or move to a different transport.

The allocation table is `documentation/DusklightSideChannels.md` and it is
CI-checked **both ways** — a channel read with no row fails, and a row nothing
reads fails. **A third check requires every allocated channel to be in
`LegacyMaterialData::computeIdentityHash`**, because a channel outside that hash
lets two draws differing only in it collide, and the preserve path then serves a
stale material with nothing logged. `Power` was outside it until 2026-08-11 on
the stated grounds that nothing read it.

**GX FIFO subcommand `0x0053` is water's.** Four branches had each taken it; the
`else if` dispatch in aurora's `command_processor.cpp` merges both arms without
conflict and the loser desyncs the FIFO. `include/dolphin/gx/GXAurora.h` now
carries a registry comment reserving `0x0054`–`0x0057` for the other three, and
aurora's `check_invariants.py` fails on a duplicate or an unregistered number.

Full account: `aurora-ao/docs/dx9/in-flight-allocation.md`.

**When auditing documentation after a merge, re-derive the file list from the
diff, not from memory.** On the merge that prompted all of this, every gap found
on the thorough pass was in a document nobody had edited — precisely the set
recall does not surface. `documentation/DusklightAtmosphere.md` §11, the rebase
surface, is the highest-consequence one to keep current.

## Shader source must be ASCII — and `§` is not evidence otherwise

**Cost one CI round on 2026-08-11.** All three Windows configs died 60 seconds in,
before a single `.cpp` was compiled:

```
compile_shaders.py:492 in parseShaderVariants ->  for line in file:
UnicodeDecodeError: 'charmap' codec can't decode byte 0x8d in position 184
```

`scripts-common/compile_shaders.py` opens every shader with a bare
`open(inputFile, "r")`, so on Windows it decodes as **cp1252**. The commit had put
the game's own kanji (前 / 奥) in a `.slang` comment; `前` is UTF-8
`E5 89 8D`, and `0x8D` is **undefined** in cp1252.

**The trap is that the tree looks like it already allows non-ASCII.** These files
are full of `§` — and `§` is `0xA7`, a *perfectly valid* cp1252 byte, so it decodes
(as a different character nobody ever looks at) and has never failed. `§`
surviving says nothing about CJK, which does not.

So: **anything under `src/dxvk/shaders/` stays ASCII.** Romanize the Japanese in
the comment and cite `dusklight-ao/docs/japanese-naming.md` for the kanji. This
matters more than it used to, because the naming work means sessions now routinely
quote the game's labels — and this is the one place that quoting is fatal.

Not a Linux-visible failure: Python defaults to UTF-8 there, so the container
compiles the same file happily. Check with:

```
grep -rnP '[^\x00-\x7F]' src/dxvk/shaders/ | grep -vP '[\xa0-\xff]$'   # CJK etc.
grep -rnP '[^\x00-\x7F]' src/dxvk/shaders/                            # everything
```

The proper fix is `encoding='utf-8'` in `compile_shaders.py` — deliberately **not**
done, because that file is upstream's and the rebase surface is worth more than the
convenience. If upstream ever fixes it, this section retires.

## Two tripwires that only fire in CI

Both are deliberate guards, not bugs, and both have cost a CI round. Neither
shows up in a Linux container — the first is release/debugoptimized-only, the
second needs the exact MSVC layout.

- **`CheckRtInstanceSize`** (`rtx_instance_manager.cpp`). Any field added to
  `RtSurface` grows `RtInstance` and trips it. The fix it asks for is: confirm
  `copyInstanceDataFrom` carries the new members (it assigns `surface`
  wholesale, so normally yes), then update the constant to the size named in the
  error — the compiler prints it as `CheckRtInstanceSize<newSize>`.
- **`hashStructByMemory`** (`rtx_materials.cpp`, `d3d9_rtx_matrep.h`). Requires
  the listed members to sum to `sizeof(T)` exactly. Adding a field usually needs
  the trailing `padding[N]` adjusted. **This one is checkable locally** — copy
  the struct into a standalone file and compile it with a matching
  `static_assert` before pushing.
- **Push constant budget** (`rtx_tone_mapping.cpp`, `rtx_local_tone_mapping.cpp`).
  `MaxPushConstantSize` is 128 and `ToneMappingApplyToneMappingArgs` is now
  **exactly 128**. Three `static_assert`s guard it. If one fires, move the
  operator argument blocks into a uniform buffer — only one operator runs per
  dispatch, so they are currently paying for each other's space. Do not shrink
  an operator's parameters to squeeze past it. **Checkable locally** the same way
  as `hashStructByMemory`.

## CI

`.github/workflows/build.yml`, three Windows configs. `claude/**` is in the
push triggers, so a branch gets built without opening a PR.

**A red build is not automatically your code.** On 2026-08-06 two of the three
configs failed with `Failed to resolve action download info: Service Unavailable`
— GitHub infrastructure, dying before checkout, while the third config passed on
the identical commit. Read the log before debugging. Re-running failed jobs needs
the MCP `actions_run_trigger` tool; a plain REST POST 403s on a read-scoped
token. The x86 bridge
steps were removed on 2026-07-28 — Dusklight is 64-bit and loads `d3d9.dll`
directly, so it never used the bridge.

## Rebasing onto upstream

The Dusklight changes are deliberately concentrated in the `rtx_dusklight_*`
files so that most of them do not conflict. The parts that **do** touch
upstream files, and therefore are the rebase surface, are listed in
`documentation/DusklightAtmosphere.md` §11. Read that before starting a rebase
rather than discovering the list a conflict at a time.
