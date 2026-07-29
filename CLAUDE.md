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

| Repo | Role | Its docs |
| :-- | :-- | :-- |
| `automata-rtx/dxvk-remix` | **this repo** — the Remix fork | `documentation/Dusklight*.md` |
| `automata-rtx/dusklight-ao` | the game | `docs/kankyo-remix.md` ← the overall entry point |
| `automata-rtx/aurora-ao` | GX→D3D9 backend, `extern/aurora` in dusklight | `docs/dx9/` |

## Branches — ALL THREE repos use the same structure

- **`Fixed-Function-dev` — the working branch. ALL development commits land
  here, in every one of the three repos.** This repo has no `Fixed-Function`
  integration branch and does not need one.
- `main` is the upstream tracker plus one merged PR. Do not develop on it.

**Standing authorization — this file is the authority for it.** A remote
session is often configured to push to a generated branch name like
`claude/<something>-<hash>`. That is fine, but **every such push must also be
mirrored to `Fixed-Function-dev`, in the same turn**:

```
git push -u origin <session-branch>
git push origin HEAD:Fixed-Function-dev
```

**REVOKED by the owner on 2026-07-29 — the mirroring instruction above no
longer applies.** The owner merges to `Fixed-Function-dev` themselves, at
milestones they choose. Push only the session branch:

```
git push -u origin <session-branch>        # yes
git push origin HEAD:Fixed-Function-dev    # NO - the owner does this
```

If a session branch is about to be deleted and its work is not yet merged,
**say so and stop** rather than mirroring it.

> **The containment check survives, and matters more now.** On 2026-07-28 this
> repo's `Fixed-Function-dev` was found **19 commits behind** its `claude/*`
> branch, immediately before that branch was to be deleted. Deleting it would
> have destroyed the entire atmosphere, overlay, warp and clock work. Before
> anyone deletes a branch:
> `git rev-list --count origin/Fixed-Function-dev..origin/<branch>` must be `0`.
>
> With auto-mirror off, a non-zero count is the **normal** state between
> milestones rather than an anomaly — so this check is no longer a formality,
> it is the only thing standing between a routine cleanup and lost work.

## The one coupling that has cost evenings

**The game and this DLL are a single protocol.** The game pushes
`rtx.dusklight.env.protocol`; this fork compares it against `kRequiredProtocol`
in `showDusklightRemixTab` (`src/dxvk/imgui/dxvk_imgui.cpp`).
**Protocol is at 6.** Build both sides from the same commit point, and bump
both in the same commit. Skew in either direction has cost an evening twice.
The Dusklight tab reports which side is old — read it before debugging
anything else.

## The Dusklight surface in this repo

| File | What it holds |
| :-- | :-- |
| `src/dxvk/rtx_render/rtx_dusklight_game.h` | `rtx.dusklight.game.*` — settings the **game** reads back every frame through the `getRtxOptionValue` export. Nothing here is read by Remix itself. |
| `src/dxvk/rtx_render/rtx_dusklight_env.h` | `rtx.dusklight.env.*` — readouts the game **writes** via `remixapi_SetConfigVariable`. All `NoSave`. |
| `src/dxvk/rtx_render/rtx_dusklight_atmosphere.{h,cpp}` | one medium driving fog, sky and sky-light; Hillaire physical sky |
| `src/dxvk/rtx_render/rtx_dusklight_grade.{h,cpp}` | the ambient grade stage |
| `src/dxvk/imgui/dxvk_imgui.cpp` | the F1 Dusklight overlay: `showDusklightOverlay` → `showDusklightWindow` → the three tabs |

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

## CI

`.github/workflows/build.yml`, three Windows configs. `claude/**` is in the
push triggers, so a branch gets built without opening a PR. The x86 bridge
steps were removed on 2026-07-28 — Dusklight is 64-bit and loads `d3d9.dll`
directly, so it never used the bridge.

## Rebasing onto upstream

The Dusklight changes are deliberately concentrated in the `rtx_dusklight_*`
files so that most of them do not conflict. The parts that **do** touch
upstream files, and therefore are the rebase surface, are listed in
`documentation/DusklightAtmosphere.md` §11. Read that before starting a rebase
rather than discovering the list a conflict at a time.
