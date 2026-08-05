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
| `src/dxvk/rtx_render/rtx_dusklight_emissive.h` | `rtx.dusklight.emissive.*` — self-illumination. **A rule, not a score**: self-lit (no TEV colour stage reads the rasterized channel) AND a colour of its own (authored in GX constants, not the vertex stream and not a bare texture pass-through) AND that colour reading as a glow (saturated **or** near-white-hot). Aurora ships the three facts in `D3DMATERIAL9::Specular.{a,b}` and `Emissive.rgb`; `Emissive.a` still carries the old evidence score but **nothing decides on it** — three revisions cut on it and all three missed the lava, which scores 0.00. Applied at one site in `rtx_instance_manager.cpp`. **Note the trap around the emissive colour: by default the shader re-applies the albedo's texture op to it** — `RtSurface::emissiveSource` (`textureFlags` bits 19–20) is what selects out of that. Also holds `rtx.dusklight.rampMaterials`, the two-colour ramp, which shares the same `D3DMATERIAL9` transport: the fork evaluates the GX combiner `a*(1-c) + b*c` from both endpoints rather than squeezing it into one D3D9 texture op. *Stock* Remix cannot express that lerp; this fork can |
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
