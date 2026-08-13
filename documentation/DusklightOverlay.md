# The Dusklight overlay, and the control plane behind it

*Companion to `DusklightAtmosphere.md`, which covers the rendering. This one
covers how any of it gets driven while the game runs.*

*For material and colour questions, neither of these is the right document —
see `aurora-ao/docs/dx9/remix-material-interface.md` and, for reading a log,
`aurora-ao/docs/dx9/material-report.md`.*

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
compares it against a `kRequiredProtocol` constant and names the older side in
the tab. **Currently 14.**

**Both directions are reported, as of 2026-08-11; until then only one was.** The
check was `protocol() < kRequiredProtocol`, so a game *newer* than the DLL fell
through to the "Connected" branch and the tab said the pairing was fine. That is
the more common direction, not the rarer one: a session branch bumps the
protocol several times while `Fixed-Function-dev` stays put, so a game built
from the branch meeting a `d3d9.dll` built from the trunk is the everyday case —
and this merge is exactly it, a protocol-11 game against a protocol-7 DLL.

What that skew does, read rather than assumed: the game asks for each setting by
name through the `getRtxOptionValue` export, which returns `0` for a name it does
not declare (`rtx_option_manager.cpp:522`); the game's `readOption` treats `0` as
failure and `readOptionBool`/`readOptionFloat` return the caller's fallback,
which is its own `ConfigVar` (`remix_bridge.cpp:86`). So **nothing errors and
nothing is logged.** The newer settings keep their `config.json` values forever,
no control for them is drawn in this tab because this build has never heard of
them, and the readouts they feed are absent. Silent in every channel except this
notice — which is why the notice had to exist before anything else could be
diagnosed.

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
           └─ Controls          rebind the game's actions
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

**Materials.** A third Remix-owned section, above the game's settings and
working whether or not the game is connected, because material translation is
this runtime's half of the wire and does not go through the bridge.

| Control | Option | What it decides |
| :-- | :-- | :-- |
| Reproduce Two-Colour Ramps | `rtx.dusklight.rampMaterials` | whether `lerp(colourA, colourB, texture)` — this game's dominant material shape — is evaluated exactly (default) or approximated by one D3D9 texture op. *Stock* Remix cannot express that lerp; this fork evaluates the GX combiner `a*(1-c) + b*c` from both endpoints |
| Emissive Surfaces Enabled | `rtx.dusklight.emissive.enable` | whether self-lit surfaces emit. The rule needs no tuning: a surface qualifies when its GX colour program never reads the lit channel, it has a colour of its own (authored in GX constants, not from the vertex stream and not a bare texture pass-through), and that colour reads as a glow. Over one measured Goron Mines session that is 6 materials of 77 — every lava and fire surface, nothing else |
| Emitted Colour | `…emissive.colorSource` | what a glowing surface glows. **Reconstructed Albedo (default)** is the two-colour ramp, so the texture drives the colour and neither overpowers the other — on the lava, `lerp(FF0000, FFFE63, texture)`. Albedo Texture pushes the texture through the material's single D3D9 op, which on the lava is an ADD against red: red pinned, bright end washed to white. Presented Colour is one flat colour |
| Emissive Brightness | `…emissive.brightness` | the only dial worth touching, default **10.0**, measured in game rather than guessed. A **target brightness rather than a multiplier**: the emitted radiance is divided by the authored colour's luma, so a dark saturated emitter and a pale one reach the same brightness at one setting. A surface whose colour *sweeps* (the lava's red-to-yellow ramp) is normalised by its dark end, so its bright end overshoots into a white-hot core; a flat pickup glow stays even. Each material's resulting radiance is printed in the log |
| Saturation / Brightness Counts As Glow | `…emissive.glowChroma` / `glowLuma` | **or**'d, not and'd: an authored glow is a strong colour or it is near-white-hot, while a muted mid-tone is a surface colour. This is what stopped the brown false positives. Should not need touching |
| Log Emissive Candidates | `…emissive.log` | one bounded line per candidate, accepted **or** rejected. Colourless rejections are counted rather than enumerated, and moving any control on this page re-reports every candidate |
| Log Material Translation Report | `rtx.dusklight.matrep` | one line per distinct reconstructed material |

**HD Texture Pack.** A fourth Remix-owned section, collapsed by default. The
game's replacement pack does not travel through D3D9 — it is loaded by Remix
from its own files and swapped in at draw time — so the controls for it live
here rather than in the game's settings.

| Control | Option | What it decides |
| :-- | :-- | :-- |
| Use HD Replacements | `rtx.dusklight.texrep.enable` | whether loaded replacements are substituted at all. Off is exactly what a run with no pack installed looks like, because the D3D9 textures are the game's own either way |
| Apply To HUD | `…texrep.applyToRaster` | whether the rasterized (UI) draws get them too. This is the **only** lever on HUD fidelity — Remix rasterizes UI draws instead of path-tracing them, so a USD material replacement can never reach the HUD. Turn it off to isolate a HUD-only regression while the world keeps the pack |
| Hold At Full Resolution | `…texrep.forceFullMips` | rasterized draws generate no sampler feedback, so without this a HUD texture can sit at whatever low-mip tail the streamer happened to load and look *softer* than the game's own |
| Log A Report Next Frame | `…texrep.report` | one bounded `texrep.rmx` summary line. `NoSave`, and it clears itself after reporting |

Two counter rows sit under them, and the split is the point: the first is what
the **game** says it did (selected / handed over / skipped), the second is what
**Remix** did with it (tagged / substituted / still loading / unknown). "The
game never handed it over" and "the fork ignored it" are different bugs that
both read as "the pack does nothing", so each has its own number and its own
named explanation underneath.

`texrepSkipped` is almost always PNG: Remix's asset loader takes `.dds` only,
while the game's own registry accepts both.

Full design, both substitution sites, and the failure table:
`aurora-ao/docs/dx9/texture-replacements.md`.

The rule is deliberately not tunable, and that took three revisions to get to.
No single GX fact identifies an emitter — "GX lighting is off" is true of 45 of
77 materials in one measured scene and *false* for the Goron Mines lava. What
does identify one is a **conjunction**: the colour program never reads the lit
channel (so the surface takes no light in fact, whatever the channel flag says),
it has a colour of its own rather than being a texture pass-through (which is
what every EFB copy and full-screen quad is), and that colour reads as a glow.

Two earlier revisions cut on a weighted score instead. Both missed the lava,
which scores 0.00 on all three of the signals that score is built from. The
score is still logged; nothing decides on it.

`aurora-ao/docs/dx9/remix-material-interface.md` §9 for the rule and the
measurement, §10 for the two-colour ramp.

Then the game's own settings, in collapsible sections: Bridge, Sun / Moon
Light, **Effect Lights**, Local Point Lights (comparison), Geometry, Game,
Bloom, Ambient Grade, Atmosphere - Fog and Sky. The first three are open by
default; the rest start collapsed.

**A name collision worth knowing about before you go looking.** Remix's *own*
Lighting tab has a section also called **Effect Light** — singular — which is
upstream's `rtx.effectLight*` / `rtx.lightConverter` feature for attaching a
light to a tagged texture. It has nothing to do with ours. Ours is
`rtx.dusklight.game.effectLight*` and lives in the Dusklight window's Game tab.
Searching either doc or the source for "effectLight" hits both.

One of those is worth naming here because it is a rendering decision rather than
a preference: **Geometry > Game's Blob Shadows** (`rtx.dusklight.game.blobShadows`,
default **off**, tested in game 2026-08-06 and correct). Blob shadows are the flat discs the game paints under rupees,
hearts and pots — an approximation of a shadow Remix traces for real from the
same geometry, so drawing them puts a painted shadow on top of a correct one.
The game drops them at registration (`dDlst_shadowControl_c::setSimple`), so no
draw call is issued rather than one being hidden downstream. Its *projected*
shadows — Link and the major actors, `dDlst_shadowReal_c` — are a separate
system and are untouched.

**Water** (`rtx.dusklight.water.*`, added 2026-08-11) is its own collapsing
header in this tab rather than a fourth top-level tab — it is a rendering
control like the atmosphere's, not a control plane of its own.

| Group | Controls | Note |
| :-- | :-- | :-- |
| the switch | Translucent Water | everything below is `BeginDisabled` behind it |
| the material | Index of Refraction, Transmittance Color, Transmittance Distance, Thin Walled, Thin Wall Thickness | **Distance is the one to tune first** — it sets how far light travels before reaching the transmittance colour, so it decides how quickly water reads as deep. The default is a starting value in the game's units, not a measurement |
| the surface | Animate Texcoords, UV Tiling, Scroll Speed, Normal Intensity | replaces the transform the draw arrived with. Tiling is what fixes a ripple texture stretched once across Lake Hylia |
| the layers | Shimmer (mera), Waves (nami), Shoreline (mizugiwa), Murk (nigori), Additive (kasan) | **all off by default.** A lake is several stacked draws and only one should be the refracting surface; which one is a look decision. The names are the game's own — `aurora-ao/docs/dx9/remix-material-interface.md` §11 |
| the exceptions | Shoreline Keeps Its Blend, Apply To Replacements, Hide Projected Layer | each exists for a specific reason recorded in its tooltip and in §11 |

**No protocol bump.** Water is read entirely from the D3D9 stream — the game
marks its own draws and the mark rides `D3DMATERIAL9::Power` — so nothing here
crosses the `rtx.dusklight.env.*` wire and `kRequiredProtocol` is unchanged.
The two readouts the water work did add, `env.dash` and `env.camInWater`, are
diagnostic only: they annotate the material report's `dusklight.mark` line and
no control depends on them, so an older game build that does not push them is
not out of date — it simply produces no markers.

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
   `d_a_kytag11` (a *kankyo tag*: an invisible per-area environment override
   actor; the game's names are romanized Japanese,
   `dusklight-ao/docs/japanese-naming.md`) sets for a stage whose sky must not
   move, and what `setDaytime` already tests. Reusing it means the freeze rides a branch the
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

**Built 2026-07-29, protocol 6.** A controller-port selector, the six
rebindable actions with what each is currently bound to, and Rebind / Clear.

**The game owns every decision. This tab decides nothing.**

```
overlay → rtx.dusklight.bind.{port,actionIndex,captureCommit,clearCommit}
game    → rtx.dusklight.env.{bindActions,bindButtons,bindStatus,bindCapturing,bindKeyboard}
```

The overlay sends two indices and two commit counters. The game captures the
press, resolves the conflict, applies it, and pushes back both the resulting
table and a line of prose saying what it did. Everything the tab renders is a
string the game sent.

**Why ownership sits there** — this was the open question and it has a real
answer. The overlay only knows the binds it is *handed*. Anything hardcoded or
context-sensitive is invisible to it, so a check made here would cheerfully
allow a conflict with something outside the rebindable list. The game is the
only side that can be right. Validating in *both* places would be worse than
either: two rules that can disagree today and will certainly drift the first
time one of them is edited.

Round-trip lag is the price, and it is the same price the warp list pays. It is
acceptable here for the same reason: the thing being displayed is by
construction the thing in force.

**Displace, not reject.** The key you press is always taken; whatever else held
it on that port is left unbound and named in the status line. Rejecting was the
alternative and is worse in practice — it leaves someone pressing a key and
watching nothing happen, with nothing on screen explaining why.

**Capture works while input is blocked, and that is not luck.**
`PADGetNativeButtonPressed` and SDL's keyboard state read the device directly
and never consult the flag `PADBlockInput` sets (`aurora-ao/lib/dolphin/pad/pad.cpp`
— the block is checked in the `PADRead` path, not the native polling one). So
the input this overlay is busy blocking from the game is still visible to the
one part of the game that needs it, and **no carve-out in the input blocking
was required**. Worth knowing before anyone "fixes" that asymmetry.

Two details that would otherwise be bugs:

1. **Capture waits for neutral first.** The click or keypress that pressed
   Rebind is still held on the frame after, and without the wait it would be
   captured as the new bind immediately.
2. **The stored value means different things per port** — an SDL scancode where
   the port is keyboard-driven, a native gamepad button otherwise, with both
   spelling "unbound" as -1. That is why the game sends finished display
   strings rather than raw numbers: it is the only side that knows which kind a
   given port holds.

**Escape unbinds** during capture, matching the game's own controller config
screen rather than inventing a second convention.

**Untested in game.**

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
| Self-illumination: options, thresholds, candidate log — and the two-colour ramp, which shares the same `D3DMATERIAL9` transport | `src/dxvk/rtx_render/rtx_dusklight_emissive.h`, applied at one site in `rtx_instance_manager.cpp` |
| Material translation report, Remix half | `src/d3d9/d3d9_rtx_matrep.h` |
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
| Controls tab | landed 2026-07-29, protocol 6 — **not yet run in game** |
| Effect Lights section | landed 2026-08-06, protocol 7 — **CI-green, and run in game 2026-08-07: "it works", merged on that.** The diagnostics below were *not* read, so which effects the classifier accepts is still unknown; `dusklight-ao/docs/remix-open-issues.md` carries the four questions that leaves open. Replaces the local-light mirror as the default. Its readouts are the whole chain, so a light lost at any step is visible without asking anyone to describe a scene; two of them (`effLightsOrphans`, `effLightsVanilla`) exist to settle specific open questions rather than to be watched. `effectLightReportCommit` is the action counter that dumps the classifier's own inputs and verdicts. Design: `dusklight-ao/docs/effect-lights.md` |
| Room Lights section | landed 2026-08-12, joins protocol 13 — **CI-green only, never run in game, and deliberately off by default.** The room's own authored lights (`dungeonlight`), a third registry from either of the two the bridge already forwards and the only one carrying a cone. Its six readouts exist to settle two questions from one log rather than from an argument: `roomLightsFound` vs `roomLightsDrawn` for whether a room has any, and `roomLightsShaped` vs `roomLightsUnshapeable` for whether the cone work carries any weight. The cone's **direction and angle are transcribed** from the game; the **shape of its edge is an approximation** (GX has four falloff curves, Remix has one) and the two ring-shaped curves cannot be expressed at all. Design: `dusklight-ao/docs/effect-lights.md` §8.1 |
| HD Texture Pack section | landed 2026-08-05, protocol 7 — **tested good 2026-08-06, first try.** The counters split game-side from Remix-side exactly as intended. Known characteristic: a long first-launch warm-up, `DusklightAtmosphere.md` §12.1 |
| Materials section (self-illumination + matrep) | landed 2026-08-04, run in game twice since. 2026-08-04: the score and threshold worked, but the accepted materials were brown rock, not lava. 2026-08-05: the lava scores **0.00**, so no threshold could ever reach it. Rev 4 therefore drops the score from the decision entirely and cuts on three measured facts instead — the section now has no threshold in it, and only Emissive Brightness is expected to be touched. **Tested in game 2026-08-06:** the rule accepts the lava, and Emissive Brightness was dialled to 10.0 there, which is now its default. No protocol change: nothing in it is read by the game |

Both of the two designs this document argues for at length are now confirmed in
practice: the **commit counter** (a preset pressed twice works the second time)
and **layer `-1`** (warps land in the right story version). The round-trip list
rebuild behaved as described, lag and all.

**Protocol is at 14** (3 = overlay + warp, 4 = the clock, 5 = per-blade grass, 6 = the Controls tab, 7 = effect lights **and** the HD texture pack readouts - two branches took 7 independently and both landed, so a build reporting 7 may carry either or both, 8 = the effect-light exclusion readout, 9 = `effectLightDerivedReach` - **retired at 14**, see below, 10 = `lanternInfiniteOil`, 11 = `effectLightMassExponent`, 13 = `perBladeFlowers`, `colpatPrev`/`colpatBlend`, the three background alphas `bgWaterAlpha`/`bgAuxAlpha`/`bgFakeFogAlpha` **and** the six `roomLights*` readouts, 14 = the effect-light vocabulary rework: `effectLightReachScale`, `effectLightRadiusScale`, `effectLightAuthoredColor`, `effectLightAuthoredRadius`, the four `effectLightLantern*` options and the `effLightsAuthored`/`effLightsClasses` readouts). `kRequiredProtocol`
lives in `showDusklightRemixTab`; bump it in the same commit as the game side.

> **13 covers everything on its session branch, and was taken once.**
> `perBladeFlowers`, the `colpatPrev` / `colpatBlend` pair, the three
> background alphas (`bgWaterAlpha`, `bgAuxAlpha`, `bgFakeFogAlpha`, added
> 2026-08-12) and the six room-light readouts (`roomLightsRunning`,
> `roomLightsFound`, `roomLightsDrawn`, `roomLightsTracked`, `roomLightsShaped`,
> `roomLightsUnshapeable`, added 2026-08-12) all landed on
> `claude/japanese-naming-worklist-nea1rk` and ship as
> one build, so they share one number rather than taking 13, 14, 15 and 16. A protocol number answers "does the
> build on the other side have this"; two numbers for one build answers it
> twice.
>
> **That last sentence used to read "a further addition on this branch joins 13
> too — it does not take 14", and the effect-light rework on 2026-08-13 took 14
> anyway, at the owner's instruction.** Both readings are defensible and the
> difference is what a number is for: sharing one number keeps it a statement
> about a *build*, and taking a new one keeps it a statement about a *feature*
> the other side can ask for. The precedent the owner cited is the second —
> `perBladeGrass` took 4→5 and `lanternInfiniteOil` took 9→10, and both are
> fork-declared options that only the game reads, exactly like this one. So the
> rule is now: **an addition on a shipped branch takes a new number when the
> game reads a new option, and joins the existing one when it only adds
> readouts.** 13's own contents are left as they are; they shipped together.
>
> **13 does not include 12's vrbox sky-dome alphas**, which are on the separate
> unmerged `claude/kasumi-naming-correction-w3e204`. See the note below for the
> merge-order rule, and one refinement of it this branch's second feature makes
> concrete.

> **12 is missing from that list on purpose, and a build reporting 13 does not
> carry it.** Protocol **12** belongs to `claude/kasumi-naming-correction-w3e204`,
> which adds the vrbox alpha readouts and had **not merged** when 13 was taken on
> 2026-08-11. Taking 12 as well would have been the double-bump this registry
> exists to make visible, so this branch skipped it and took the next free number
> instead — a gap in the ladder is the cheap outcome, two features sharing a
> version is the expensive one.
>
> Whichever of the two merges second, the *merge resolution* is **13**: 13 is
> already the higher number and both features are then present, so keeping 13 is
> correct and neither branch has to be rewritten to merge. The case that needs
> action is a **third** branch: it must take **14**, not 12, even though 12 looks
> free from a checkout that cannot see the kasumi branch. Reusing 12 would ship
> two features claiming one version and `check_dusklight_invariants.py` cannot
> see it, because nothing can see an unmerged branch.
>
> **One refinement, noted 2026-08-11 while adding the second feature to 13.**
> "The union is 13" is a statement about resolving the merge, not a guarantee
> about a build. If **13 merges first**, `Fixed-Function-dev` sits at 13 *without*
> the vrbox alphas until the kasumi branch lands, and after it lands the same
> number means something larger — so a game and a DLL built from either side of
> that merge both report 13 while disagreeing about what exists, and the mismatch
> notice cannot fire. Bumping the kasumi branch to **14** as part of that merge
> would close it. That is the tidier resolution and costs nothing; it is not
> *required*, because the standing rule is to build both sides from the same
> commit point and skew never arises when it is followed. **Recorded as a
> judgement call, not settled** — whoever performs the merge decides, and should
> write down which they chose.

### Open

- **Local point lights: RESOLVED 2026-07-29, then SUPERSEDED 2026-08-06.** The
  entry below is kept because the settings it derived carry straight over to
  effect lights and the diagnostics lesson is the template — but the mirror
  itself now defaults **off**. What replaced it and why is the Effect Lights row
  above: the mirror worked, and working is what exposed the problem, which is
  that a GameCube point light's *position* was never meant to survive a real
  shadow. The loose end at the bottom of this entry — `found 5` but `drawn 4` —
  is therefore no longer on anyone's path.

  Forest Temple first room reads
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

  Two settings came out of the visit — `localLightIntensity` **19** and
  `localLightRadius` **10**. Neither was the default at the time; **both are the
  defaults now.** The 19 is the derived reading of the game's attenuation curve,
  not a taste value. See `dusklight-ao/docs/remix-open-issues.md` open issue 3.

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

  That is the "not in the categorization screen" symptom, and it puts the
  *configuration* fix out of reach: `rtx.uiTextures` is consulted *inside*
  `isRenderingUI()`, which only runs before injection — so a draw cannot be
  tagged UI precisely when it needs to be. Not the same trap as the vrbox sky,
  which turned out to be taggable after all by geometry hash
  (`DusklightAtmosphere.md` §14.9); here the draw never reaches categorization
  at all. And it is a limit of the code as written, not a ceiling: the injection
  boundary is in **this** repo, so moving it is on the table alongside any
  game-side change.

  The game draws the targeting cursor (a real perspective-projected J3D model,
  not UI) at `m_Do_graphic.cpp:2689`, the 2D game particles at `:2714`, and the
  letterbox bars — an ortho, z-write-off draw — at `:2717`. Since the bars come
  *after* both, they cannot be the trigger that rescues them; something else
  correlated with letterbox must inject earlier. Full analysis, ruled-out
  candidates and the three settling experiments are in
  `dusklight-ao/docs/remix-open-issues.md` open issue 6.
- Controls tab landed but untested in game.

The full step-by-step for all of the above, with baseline `rtx.conf` and
failure tables, is in `dusklight-ao/docs/remix-test-playbook.md`. It is kept
there rather than here because it spans all three repos.
