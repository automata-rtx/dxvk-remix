# Claude session notes — dxvk-remix (automata-rtx fork)

**Interactive approval prompts are broken for the owner** — they always resolve
as "no approval given". Read other public repos via `raw.githubusercontent.com`
rather than `add_repo`, use a background watcher rather than `send_later`, and
ask questions in plain chat.

## What this fork is for

A fork of NVIDIA's RTX Remix runtime carrying changes that serve **one game**:
Dusklight, a Twilight Princess port rendering through a GX→D3D9 fixed-function
backend. Nothing here is general-purpose Remix work unless it says so.
`automata-rtx/dusklight-ao` is the game (start at `docs/kankyo-remix.md`);
`automata-rtx/aurora-ao` is the GX→D3D9 backend (`docs/dx9/README.md`), vendored
there as `extern/aurora`.

| This repo's documents | Holds |
| :-- | :-- |
| `documentation/DusklightAtmosphere.md` | one medium driving fog, sky and sky-light; the knobs and what they mean |
| `documentation/DusklightOverlay.md` | the option wire, its traps, the tab map, the **protocol registry** |
| `documentation/DusklightRebase.md` | every upstream file this fork touches, and what a rebase must re-check |
| `documentation/DusklightSideChannels.md` | the `D3DMATERIAL9` allocation table (CI-parsed) |
| `documentation/ToneMappingExposureNotes.md` | tone mapping / auto exposure; five of six defects are **upstream's**, written for carry-back |
| `shaders/rtx/pass/tonemap/reference/README.md` | provenance and licence for the third-party GT7 reference source — fix the port, never the reference |

Material and colour questions are aurora's:
`aurora-ao/docs/dx9/remix-material-interface.md`.

## What the D3D9 renderer is for

**The raw D3D9 image is never shown to a player.** It exists so Remix's
DX9→Vulkan translation picks the scene up for free. **Remix's renderer is the
product; D3D9 is the feed** — so where D3D9 cannot carry something faithfully,
implement it in Remix rather than contorting D3D9. All three repos are ours.
"Raw D3D9 stays correct" is not a design goal.

**Two exceptions must still rasterize:** the **HUD** (Remix rasterizes UI draws)
and **alpha** (Remix reads the stage's alpha for opacity and the alpha test).
Full statement: `aurora-ao/docs/dx9/remix-material-interface.md` §0.

## Five rules, each learned expensively

1. **Translate, don't tag.** A texture tag is wrong somewhere by construction; a
   per-draw translation of the game's own state is right everywhere.
2. **A question we would have to ask the owner is a defect in the logging.** They
   play, they send a log, we know. Bounded, capped, self-describing.
3. **Do not write inference as finding.** "Unknown" beats confident and wrong.
4. **A fix that cannot be observed is a guess.** Instrument first; state the
   regression signature.
5. **Say what was verified.** "Compiles", "CI green" and "tested in game" are
   three different claims.

## Branches

**`Fixed-Function-dev` is the working branch, in all three repos**; `main` is the
upstream tracker. **Push only your session branch** — the owner merges. Before
anyone deletes a branch,
`git rev-list --count origin/Fixed-Function-dev..origin/<branch>` must be `0`. A
non-zero count is the normal state between milestones, which is why this is not a
formality: it once stood between a routine cleanup and 19 commits of lost work.

## Protocol

The game and this DLL are one protocol: the game pushes
`rtx.dusklight.env.protocol`, this fork compares it against `kRequiredProtocol`
(`src/dxvk/imgui/dxvk_imgui.cpp:2613`, file scope, read by the status strip above
the tab bar). **Protocol is at 18.** Build both sides from the same commit point
and bump both in one commit; the Dusklight tab reports which side is old.

**12 is skipped and is not free** — it was taken by
`claude/kasumi-naming-correction-w3e204`, and 13–17 were taken beside it while it
sat. Part of that branch has since landed: the corrected haze blend, gated behind
`rtx.dusklight.atmosphere.kasumiBlendMode` and **still never run in game**; the
`env.*` band alpha it also carried did not. So 12 stays a hole rather than a
merge to go and do, and the next branch to need a number takes **19**. No script
can see an unmerged branch, so check the live `claude/*` branches yourself.

## The Dusklight surface in this repo

| File | Holds |
| :-- | :-- |
| `rtx_dusklight_game.h` | `rtx.dusklight.game.*` — settings the **game** reads back via `getRtxOptionValue`; Remix reads none of them |
| `rtx_dusklight_env.h` | `rtx.dusklight.env.*` — readouts the game **writes** via `remixapi_SetConfigVariable`. All `NoSave` |
| `rtx_dusklight_atmosphere.{h,cpp}` | one medium driving fog, sky and sky-light; Hillaire physical sky |
| `rtx_dusklight_emissive.h` | self-illumination (a rule, not a score) and the two-colour ramp |
| `rtx_dusklight_water.h` | water; three facts decoded from `D3DMATERIAL9::Power` |
| `rtx_dusklight_texrep.{h,cpp}` | HD texture packs — substituted at both consumption sites, never through D3D9 |
| `rtx_dusklight_drawmeta.{h,cpp}` | the per-draw flag word |
| `rtx_dusklight_catrep.h` | the texture-category report |
| `rtx_dusklight_grade.{h,cpp}` | the ambient grade stage |
| `rtx_agx.{h,cpp}`, `rtx_gt7.{h,cpp}` | AgX presets, `TonemapOperator`, GT7. **Not Dusklight-specific** |
| `shaders/rtx/algorithm/volume_lighting.slangh` | the one file that **shadows** an RTXDI submodule file; its header states the mechanism and what a rebase must check |
| `src/d3d9/d3d9_rtx_matrep.h` | the material translation report (`rtx.dusklight.matrep`) |

**A new per-draw fact is a new bit, not a new `D3DMATERIAL9` channel** —
`rtx_dusklight_drawmeta.h:27-50` argues it in full, including why the channel
scarcity was never the real constraint. The allocation table is
`DusklightSideChannels.md`, CI-checked both ways and against
`computeIdentityHash`.

## Transport traps

Full versions in `DusklightOverlay.md` §1.1.

- Anything **action-triggering must be `RtxOptionFlags::NoSave`** — a persisted
  commit counter fires on next launch.
- Act on a commit counter **changing**, and **latch the first value seen without
  acting**, or connecting to a Remix that outlived a game restart triggers it.
- **An empty string never crosses.** `parseOptionValue` returns false for
  `size() == 0` (`src/util/config/config.cpp:1190`), so the old value survives —
  a readout that never changes is not evidence the push is dead.
- **ImGui here is 1.88**: no `SeparatorText`; `BeginDisabled`/`EndDisabled` are
  available. `dxvk_imgui.cpp` does **not** include `<cmath>` — do not rely on a
  transitive one across three compilers.
- The overlay only saves because of the `RtxOptionLayerTarget` scope at
  `dxvk_imgui.cpp:2780-2794`; that comment says what it silently did for months.

## This runtime charges per draw, not per pixel

A draw too small for its own BLAS is merged into a shared bucket but **still
contributes its own geometry entry and its own surface**, and the bucket rebuilds
whenever any of its geometry moves (`rtx_accel_manager.cpp`). A thousand
single-quad draws cost roughly a thousand times what the same quads cost in one
draw, and no shading work changes it. Two corollaries, each mistaken for
something else:

- **`rtx.particleTextures` is not a performance control.** It picks a TLAS and a
  resolve path (`rtx_instance_manager.cpp`), not per-draw scene-management cost.
  A dense effect that does not respond to it is draw-bound.
- **Opacity micromaps working is not evidence they are helping** — they cut
  per-pixel any-hit work, not BLAS rebuild or surface upload.

Measured game-side: aurora logs `dx9.draws frames=… mean=… peak=…`; a peak in the
thousands is the signature.

## Verification

```
python3 scripts/check_dusklight_invariants.py     # after any merge, before any push
```

It names its own checks. The `Invariants` workflow runs it on every push and PR
unfiltered by path, because doc-only commits are exactly when these drift.

**A clean `git merge` is not a correct merge.** No script can see whether a
"tested in game" claim survived the change under it, whether prose still
describes reality, or whether an unmerged branch is about to take the same
protocol number or side channel.

**`RtxOptions.md` is generated by running the DLL on Windows — never hand-edit
it, and never restate its drift counts from memory.** The script prints the live
count; that is the figure to quote. A name absent from it is not evidence the
option does not exist — the headers are the authority.

**Shader source is ASCII.** `compile_shaders.py` opens shaders with the platform
default encoding — cp1252 on the runner — so a kana, or one of the kanji whose
bytes cp1252 rejects, ends the Windows build before a shader is compiled, naming
a Python file rather than yours. The em dashes already in there decode, which is
exactly why this looks permitted; `shader-encoding` is the local one-liner.

**Three tripwires fire only in a Windows CI build, and each prints its own
remedy:** `CheckRtInstanceSize` (`rtx_instance_manager.cpp:159-163`),
`hashStructByMemory` (`rtx_materials.cpp`, `d3d9_rtx_matrep.h` — the invariants
script now catches this one locally, naming the missing field, which MSVC does
not), and the 128-byte push-constant budget (`rtx_tone_mapping.cpp:43-46`).

CI is one Windows x86_64 `release` config with `claude/**` in the push triggers.
A red build is not automatically your code — read the log first.

## The game's names are romanized Japanese

This fork's own code is `camelCase` English (`AGENTS.md`) and stays that way. But
every *game-side* symbol these documents quote is romanized Japanese preserved by
the decompilation, and reading one as English gets you the wrong file — `kankyo`
is 環境, *environment*; `wether` is the game's own spelling of weather, not a typo
to fix. **Read the name before inventing a classification; it usually already
says what the thing is.** Search both romanizations (the tree mixes kunrei-shiki
and Hepburn for one word, so an empty grep proves nothing) and
`export LC_ALL=C.UTF-8` first, or `grep -P` on kana silently matches nothing.
Glossary, grep rules and the worked water-material examples:
`dusklight-ao/docs/japanese-naming-remix.md`.
