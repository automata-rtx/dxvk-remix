# The Dusklight overlay, and the control plane behind it

*Companion to `DusklightAtmosphere.md`, which covers the rendering. This one
covers how any of it gets driven while the game runs.*

Last updated 2026-07-28.

---

## 0. Why this exists at all

The game renders through a fixed-function D3D9 path. In that mode **the game
never draws its own UI** — not its settings screen, not its debug menus, not
its warp menu. Everything the game owns is therefore reachable only by editing
`config.json` and restarting, and only in one direction: a value set there
cannot be turned back off while running.

Every setting in this project needs live tuning against a path-traced image to
be worth anything. So the settings had to move somewhere that *is* drawn — and
the only thing drawn in that mode is Remix's own ImGui.

That gives the shape of the whole thing:

> **Remix hosts the UI. The game owns the data. Options are the wire.**

Nothing here is a new IPC mechanism. It is the existing `RtxOption` system used
in both directions.

---

## 1. Transport: options as a bidirectional wire

| Direction | Mechanism | Namespace |
| :-- | :-- | :-- |
| Remix → game | game polls `getRtxOptionValue` export every frame | `rtx.dusklight.game.*`, `rtx.dusklight.warp.*`, `rtx.dusklight.uiActive` |
| game → Remix | game calls `remixapi_SetConfigVariable` | `rtx.dusklight.env.*` |

Both are per-frame. Moving a slider in the overlay takes effect on the game's
next frame; a readout the game pushes is visible in the overlay on the next
one.

**Diff cache.** `SetConfigVariable` takes a global lock and re-parses the value
string on every call, so the game only pushes values that actually changed
(`remix_bridge.cpp`, `push()`). A steady scene costs nothing. This is why a
readout that never changes is *not* evidence the push is dead.

**Protocol version.** The game pushes `rtx.dusklight.env.protocol`. Remix
compares it against a `kRequiredProtocol` constant and says so in the tab when
the game is older. **Currently 5.**

> **Standing rule, already paid for twice:** the game and the Remix DLL are one
> protocol. Build both from the same point. Both directions of skew have cost
> an evening. When you bump the protocol, bump `kRequiredProtocol` in
> `showDusklightRemixTab` in the same commit.

### 1.1 Traps in this transport

These are not obvious from the API and each one cost time:

- **Pushing an empty string is a no-op, not a clear.**
  `Config::parseOptionValue(const std::string& value, std::string& result)`
  (`src/util/config/config.cpp:1190`) returns `false` when `value.size() == 0`,
  and a failed parse leaves the option at its previous value. So
  `push("...someList", "")` **does not empty the list** — the stale one stays.
  Anywhere that needs a genuine clear must push a sentinel instead. Currently
  only reachable through dead paths (every region has maps, every map has
  rooms, every room has at least one spawn point), but any new list must
  respect it.
- **`SetConfigVariable` writes to the user layer** (`RtxOptionLayer::getUserLayer()`,
  `rtx_remix_api.cpp:1240`). That is the same layer the UI writes, so a value the
  game pushes and a value a user typed are indistinguishable afterwards.
- **Non-empty strings pass through verbatim** — spaces, pipes, punctuation all
  survive. That is what makes the pipe-delimited list encoding safe.
- **Anything that triggers an action must be `NoSave`.** A commit counter
  persisted to `rtx.conf` would fire on the next launch. Everything under
  `rtx.dusklight.warp.*` and `rtx.dusklight.env.*` is `RtxOptionFlags::NoSave`
  for this reason.

---

## 2. The overlay is its own window

`m_dusklightWindowOpen`, toggled by `DusklightGame::menuKeyBinds()` — **F1** by
default, matching the key the game's own overlay used before this rendering
mode stopped drawing it.

It is drawn **outside** the `showUI == Advanced / Basic` branch in
`ImGUI::update`, deliberately. Consequences:

- Either overlay can be open without the other. Both can be open at once.
- Alt (Remix's menu) does not close it, and F1 does not close Remix's.
- The cursor is kept while **either** is open (`anyOverlayOpen`), or opening
  this one alone would leave the mouse unusable.

### 2.1 Input blocking — why Remix's own switch cannot do this

`rtx.blockInputToGameInUI` works by **sending a window message across the 32-bit
bridge**. A 64-bit game that loads this `d3d9.dll` directly never receives it.
So on this setup input has always fallen straight through an open menu to the
game — you would be walking Link around while typing in a text field.

The fix routes the same intent down a wire the game *is* listening to:

```
Remix: DusklightGame::uiActive = anyOverlayOpen && blockGameInput
game:  PADBlockInput(readOptionBool("rtx.dusklight.uiActive", false))
```

`PADBlockInput` is Aurora's, and it suppresses the held state on release, so
nothing is left stuck down when the overlay closes.

`rtx.dusklight.blockGameInput` gates it (default on) so the behaviour can be
turned off without turning off the overlay.

---

## 3. Tabs

```
Dusklight  ├─ Dusklight Remix   everything that changes the image
           ├─ Warp              travel to any level
           └─ Controls          placeholder
```

### 3.1 Dusklight Remix

Two sections at the top exist because of a failure this project has already
paid for more than once — a switch that does nothing because an unrelated Remix
option is off.

**Requirements.** Every Remix rendering option these features depend on but do
not own, named with its live state and a button that sets it:

| Requirement | Option | Why |
| :-- | :-- | :-- |
| Volumetrics enabled | `rtx.volumetrics.enable` | the atmosphere's fog rides on it |
| Bloom enabled | `rtx.bloom.enable` | the Dusklight bloom is a *mode* of that pass |
| Sky auto-detect off | `rtx.skyAutoDetect = None` | or the detected sky rasterizes behind the generated one |

These stay Remix's own settings on purpose — they are the renderer's, not the
game's — so they are *reported and offered*, never silently forced.

**What this overrides in Remix.** The inverse list: options that will appear to
do nothing while the atmosphere is on, because it takes them over. Written down
because "I changed it and nothing happened" is the most expensive kind of bug
here.

- `rtx.volumetrics.froxelMaxDistanceMeters` — sized from the game's fog range
- `rtx.volumetrics.transmittanceColor` / `transmittanceMeasurementDistanceMeters`
- `rtx.volumetrics.singleScatteringAlbedo`
- `rtx.volumetrics.enableFogRemap` / `enableFogColorRemap` — **bypassed entirely**, so a value here is a false lead
- `rtx.volumetrics.enableAtmosphere` — forced on outdoors
- `rtx.skyBrightness` — scales the probe the generated dome replaces
- `rtx.fogColorScale` / `rtx.maxFogDistance` — legacy depth fog, skipped whenever volumetrics run

Then the game's own settings, in collapsible sections: Bridge, Sun / Moon
Light, Local Point Lights, Geometry, Game, Bloom, Ambient Grade, Atmosphere.

**Greying.** Sections whose Remix dependency is off are wrapped in
`ImGui::BeginDisabled` *and* carry a line naming the option and where to find
it. Greyed without an explanation is barely better than broken.

### 3.2 Warp

Region and Level dropdowns, a Warp button, and Room / Point / Layer under a
collapsed header — they are rarely wanted, since the defaults land at the
level's first entrance.

**The names come from the game, not from Remix.** The destination table
(`src/dusk/map_loader_definitions.h`, 18 regions / 103 levels) belongs to the
game, and a copy here would guarantee the two drift apart. So:

```
overlay → rtx.dusklight.warp.{regionIndex,mapIndex,roomIndex,pointIndex,layer,commit}
game    → rtx.dusklight.env.{warpRegions,warpMaps,warpRooms,warpPoints,warpStage}
```

pipe-delimited, resolved by the game against its own table. The list you see is
always the real one because it *is* the table the warp travels on.

**The commit counter.** The Warp button increments
`rtx.dusklight.warp.commit`; the game acts on the value **changing**, not on it
being set. The game also **latches the first value it sees without acting on
it** (`s_commitPrimed`), so a game restarting under a Remix that kept running
does not teleport on connect.

**Round-trip lag is visible and is not a bug.** Change region, and the Level
list is rebuilt *by the game* and pushed back — one or two frames. Selecting a
new region resets level, room, point and layer rather than leaving them
pointing at whatever sits at the same offset in a different region's list.

**Layer: `-1` means "you pick", and `0` does not.** This bit us, and it is
counter-intuitive enough to have been questioned twice, so the evidence is
recorded rather than the conclusion alone.

`dComIfGp_setNextStage` folds anything `>= 15` to `-1`, but **nothing folds 0
to -1** — 0 is a real layer. Defaulting to 0 lands you in the wrong version of
anywhere whose default layer is not 0, which shows up as a stage in the wrong
story state.

The game's own warp menu (`dusklight-ao/src/dusk/ui/warp.cpp`) is the reference,
and **all four places it touches the layer agree on -1**:

| Site | Value |
| :-- | :-- |
| `WarpSelectionState` struct initializer, `warp.cpp:20` | `int layer = -1` |
| `reset_selection`, `warp.cpp:109` | `state.layer = kMinLayer` |
| the layer picker list, `warp.cpp:292` | iterates from `kMinLayer` |
| every `clamp_indices` path | `std::clamp(layer, kMinLayer, kMaxLayer)` |

with `kMinLayer = -1`, `kMaxLayer = 14` (`warp.cpp:12-13`). Our bounds match on
both sides of the wire; 15 would be a silent alias for -1, which is why the max
is 14 rather than 15.

#### 3.2.1 Time of day

Shares the Warp tab because both answer "put me somewhere specific". A slider,
four presets on the quarter points, and **Freeze Time**.

```
overlay → rtx.dusklight.game.{timeOfDay,timeCommit,freezeTime}
game    → rtx.dusklight.env.daytime          (quantized to 0.25 deg = 1 minute)
```

The day is **360 degrees**, so 15 is an hour and a degree is four minutes.
0 midnight, 90 sunrise, 180 noon, 270 sunset. The moon/sun handover sits around
67–75 (`dKyr_moon_arrival_check` draws the moon when `daytime > 285 ||
daytime < 67.5`).

**Freeze is the point of the feature.** An A/B pair shot minutes apart has the
sun in two places, so part of every difference measured before this landed was
the clock rather than the setting under test.

Three things here were each the second attempt, and the first would have been
subtly wrong in a way that is hard to see:

1. **Freeze sets the game's own `using_time_control_tag`** — what
   `d_a_kytag11` sets for a stage whose sky must not move, and what
   `setDaytime` already tests. Reusing it means the freeze rides a branch the
   game exercises every frame. *Consequence:* it also holds the Twilight Realm
   clock and skips the reset to midnight that entering twilight normally does.
2. **A value plus a counter, not a bare value.** Acting on the value alone pins
   the clock; acting on the value *changing* makes pressing the same preset
   twice do nothing the second time. Same shape as the warp commit, including
   latching the first count seen.
3. **The counter lives in the UI, not read-modify-write off the option.** The
   warp button can get away with `commit() + 1` because it fires at most once a
   frame. A slider fires on consecutive frames, where that pattern only stays
   monotonic if every deferred set lands before the next read.

**The slider only syncs from the game while it is not held**
(`ImGui::IsItemActive()`). Feed a slider the value returning over the bridge a
frame or two late and it fights the hand holding it — the same class of bug as
any round-trip-latency control.

Clock arithmetic is in **integer minutes** deliberately: `dxvk_imgui.cpp` does
not include `<cmath>`, and relying on a transitive one across three compilers
is not worth a CI round.

### 3.3 Controls

**Placeholder.** Nothing built. What it needs, when it is picked up:

- live key capture in ImGui, then bindings crossing the bridge in both
  directions (the game owns the current binds; the overlay has to read them
  before it can show them)
- a decision on **conflict-resolution ownership** — if the overlay lets you
  bind a key the game already uses for something else, who refuses? Doing it in
  both places means two different answers.

---

## 4. ImGui version constraints

Remix bundles **ImGui 1.88**. Two things that cost a CI round:

- **`ImGui::SeparatorText` does not exist** — it arrived in 1.89. Use
  `RemixGui::Separator()` plus a text line.
- **`ImGui::BeginDisabled` / `EndDisabled` do exist** and are used throughout.

`RemixGui::Separator()` and `RemixGui::CollapsingHeader()` are declared in
`src/dxvk/rtx_render/rtx_gui_widgets.h`. `collapsingHeaderClosedFlags` /
`collapsingHeaderFlags` are at file scope in `dxvk_imgui.cpp:502`.

`ImGui::Combo` wants `const char* const*`; a `std::vector<const char*>::data()`
(`const char**`) converts implicitly — that is a valid qualification
conversion, not a warning to be silenced.

---

## 5. Files

| Piece | Where |
| :-- | :-- |
| Overlay window, tabs, all three tab bodies | `src/dxvk/imgui/dxvk_imgui.cpp` (`showDusklightOverlay` → `showDusklightWindow` → `showDusklight{Remix,Warp,Controls}Tab`) |
| Time of day (called from the Warp tab, and from its early-return path too, so the clock survives the destination list lagging) | `showDusklightTimeOfDay` in the same file |
| Game-owned settings, hosted in Remix | `src/dxvk/rtx_render/rtx_dusklight_game.h` |
| Game-pushed readouts | `src/dxvk/rtx_render/rtx_dusklight_env.h` |
| Bloom's Dusklight-mode settings, split out for reuse | `src/dxvk/rtx_render/rtx_bloom.{h,cpp}` (`showDusklightImguiSettings`) |
| Game side of the whole wire | `dusklight-ao/src/dusk/remix_bridge.cpp` |
| Destination table | `dusklight-ao/src/dusk/map_loader_definitions.h` |
| The game's own warp menu, as the reference implementation | `dusklight-ao/src/dusk/ui/warp.cpp` |

**Rebase surface.** Everything Dusklight-specific in the overlay is in its own
functions, called from one place each. A conflict in `dxvk_imgui.cpp` should
resolve by re-applying a call, not by re-deriving a tab.

---

## 6. Status

| Piece | State |
| :-- | :-- |
| Separate F1 overlay, coexisting with Remix's | landed 2026-07-28, CI green |
| Three tabs | landed 2026-07-28, CI green |
| Requirements / overrides sections | landed 2026-07-28, CI green |
| Input blocking via `PADBlockInput` | landed 2026-07-28, CI green |
| Recording mode toggle | landed 2026-07-28, CI green |
| Warp | landed 2026-07-28, **tested 2026-07-29: "exactly as intended, no issues"** |
| Time of day: slider, presets, Freeze Time | landed 2026-07-28, **tested 2026-07-29: "flawlessly and as expected"** |
| Controls tab | placeholder only |

Both of the two designs this document argues for at length are now confirmed in
practice: the **commit counter** (a preset pressed twice works the second time)
and **layer `-1`** (warps land in the right story version). The round-trip list
rebuild behaved as described, lag and all.

**Protocol is at 5** (3 = overlay + warp, 4 = the clock, 5 = per-blade grass). `kRequiredProtocol`
lives in `showDusklightRemixTab`; bump it in the same commit as the game side.

### Open

- **Local point lights: RESOLVED 2026-07-29.** Forest Temple first room reads
  `Registered by the game: 5   drawn this frame: 4   tracked: 4`.

  The diagnostics did their job — the visit that used them took minutes and
  named the state immediately, where the bare-zero report before them could not
  distinguish three different failures. Worth keeping as the template: **when a
  readout cannot distinguish its failure modes, the fix is another readout, not
  another guess.**

  What did not happen is a proven root cause. The lights simply work in the
  build that carries the diagnostics, most plausibly because that same change
  added the `efplight[0..4]` array the first implementation never read, or
  because of the NaN guards landed alongside. Recorded as unresolved rather than
  dressed up: if they regress, re-check both arrays first.

  Two settings came out of the visit and **neither is the default** —
  `localLightIntensity` **19** and `localLightRadius` **10**. The 19 is the
  derived reading of the game's attenuation curve, not a taste value. See
  `dusklight-ao/docs/kankyo-remix.md` open issue 3.

  Loose end: `found 5` but `drawn 4`. One light is being rejected on the way
  through, and "harmless" is currently an assumption.
- **The wolf-senses overlay covers the screen.** Black heavy surround, pure
  white centre where the see-through region belongs. Not investigated. It blocks
  the wolf-senses route to testing the mono overlay and base weight — but not
  the **twilight** route, which reaches the same code through bloom tables 1/2
  and is how that test should now be done.
- **World-space UI billboards appear only intermittently — the RTX injection
  boundary.** Investigated 2026-07-29. The targeting arrow and torch fire
  billboards appear together, inconsistently, and only while the letterbox bars
  are up (necessary, not sufficient).

  The mechanism is in this repo, not in aurora: `isRenderingUI()`
  (`src/d3d9/d3d9_rtx.cpp:559`) classifies the first orthographic,
  z-write-disabled draw on the primary RT as UI and **triggers RTX injection**
  (`makeDrawCallType`, `:519`). After that, `internalPrepareDraw` early-returns
  for every remaining draw in the frame (`:576-591`), so those draws never enter
  the raytraced scene and **never reach texture categorization**.

  That is the "not in the categorization screen" symptom, and it makes the
  natural fix unavailable: `rtx.uiTextures` is consulted *inside*
  `isRenderingUI()`, which only runs before injection — so a draw cannot be
  tagged UI precisely when it needs to be. Same shape of trap as the vrbox sky.

  The game draws the targeting cursor (a real perspective-projected J3D model,
  not UI) at `m_Do_graphic.cpp:2689`, the 2D game particles at `:2714`, and the
  letterbox bars — an ortho, z-write-off draw — at `:2717`. Since the bars come
  *after* both, they cannot be the trigger that rescues them; something else
  correlated with letterbox must inject earlier. Full analysis, ruled-out
  candidates and the three settling experiments are in `kankyo-remix.md`
  open issue 6.
- Controls tab not started.

The full step-by-step for all of the above, with baseline `rtx.conf` and
failure tables, is in `dusklight-ao/docs/kankyo-remix.md` §"Test session
playbook". It is kept there rather than here because it spans all three repos.
