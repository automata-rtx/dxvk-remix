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
the game is older. **Currently 3.**

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
| Warp | landed 2026-07-28, CI green — **not yet run in game** |
| Controls tab | placeholder only |

### Open

- **Local point lights do not work.** Toggling `rtx.dusklight.game.localLights`
  changes nothing; the tab reported `drawn: 0, tracked: 0` in both states.
  Confirmed *not* the cause: device registration (reports yes) and the sun/moon
  distant light (works). Static analysis says a torch (`d_a_ep`,
  `mColor = (175,93,0)`, `mPow = 500 × strength`) should pass the brightness
  and reach test.

  Diagnostics were added so the next build **names which of three states it is
  in** rather than reporting a bare zero:

  | Readout | Meaning |
  | :-- | :-- |
  | `localLightsRunning = false` | never reached the submit loop — the switch is not reaching the game, or the device did not register |
  | `localLightsFound = 0` | the game has no lights registered here at all |
  | `localLightsDrawn = 0` with `found > 0` | lights exist and are being **rejected on the way through** |

  `found` is counted **ahead of every gate**, over both arrays
  (`env->pointlight[100]` and `env->efplight[5]`), so it stays truthful
  whichever gate turns the loop back. Note that rejection happens *before* the
  vector push, so `tracked: 0` is equally consistent with "loop never ran" and
  "every light rejected" — which is exactly why `localLightsRunning` had to be
  added separately.

  **Next step: read the three values from a build with these diagnostics while
  stood at a lit torch.**
- Warp untested in game.
- Controls tab not started.
