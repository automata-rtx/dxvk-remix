# The Dusklight overlay, and the control plane behind it

*Companion to `DusklightAtmosphere.md` (the rendering) and `DusklightRebase.md`
(the upstream files this fork touches). This one covers how any of it gets driven
while the game runs. For material and colour questions neither is the right
document — see `aurora-ao/docs/dx9/remix-material-interface.md`, and
`material-report.md` for reading a log.*

***Verification status is not here.*** *It lives in
`dusklight-ao/docs/remix-open-issues.md`, which is the single ledger.*

---

## Protocol registry

**The game and this DLL are a single protocol.** The game pushes
`rtx.dusklight.env.protocol`; the fork compares it against `kRequiredProtocol`
and names the older side in the status strip drawn above the tab bar.
**Currently 18.**

**Protocol is at 18** (3 = overlay + warp, 4 = the clock, 5 = per-blade grass, 6 = the Controls tab, 7 = effect lights **and** the HD texture pack readouts - two branches took 7 independently and both landed, so a build reporting 7 may carry either or both, 8 = the effect-light exclusion readout, 9 = `effectLightDerivedReach` - **retired at 14**, see below, 10 = `lanternInfiniteOil`, 11 = `effectLightMassExponent`, 13 = `perBladeFlowers`, `colpatPrev`/`colpatBlend`, the three background alphas `bgWaterAlpha`/`bgAuxAlpha`/`bgFakeFogAlpha` **and** the six `roomLights*` readouts, 14 = the effect-light vocabulary rework: `effectLightReachScale`, `effectLightRadiusScale`, `effectLightAuthoredColor`, `effectLightAuthoredRadius`, the four `effectLightLantern*` options and the `effLightsAuthored`/`effLightsClasses` readouts, 15 = the Shadow Insect spark: `effectLightSparks`, `effectLightSparkHold` and the `effLightsSparks` readout, 16 = the Mods tab: `modsRunning`, `modCount` and `modList` outbound, `modsEnabled` inbound, 17 = the local-light mirror **removed** - the four `localLights*` readouts go, and `hideStarBillboards` becomes live after being an inert checkbox since it was added, 18 = `hideVrkumo`, the game's cloud layer - the companion to `hideVrbox` rather than a sub-switch of it). `kRequiredProtocol`
is a single `constexpr` in the anonymous namespace at the head of the Dusklight
block in `dxvk_imgui.cpp` (it lived in `showDusklightRemixTab` until
2026-08-17); bump it in the same commit as the game side.

**12 is missing on purpose and is not free.** It was taken by
`claude/kasumi-naming-correction-w3e204`, and **part of that branch has since
landed here**: the corrected haze blend, as
`rtx.dusklight.atmosphere.kasumiBlendMode` — fork-side only, so it needed no
number of its own — gated, defaulting to the shipped behaviour, and **still never
run in game**. What did *not* land is the `env.*` band alpha that took 12 in the
first place; `rtx.dusklight.env.kasumi*` and `kumo*` are still `Vector3`, which is
why the new option's "from the game" arm is inert. So 12 stays held, and stays a
hole to route around rather than a merge to go and do. **The next branch to need
a number takes 19** — and no script can catch reusing 12, because no script can
see an unmerged branch. Check the live `claude/*` branches before taking one.

**Two rules for taking one.** *(a)* An addition on an already-numbered branch
takes a **new** number when the game reads a new option, and **joins** the
existing one when it only adds readouts — the owner's ruling of 2026-08-13, on
the precedent of `perBladeGrass` (4→5) and `lanternInfiniteOil` (9→10). *(b)*
When two branches hold different numbers, the merge resolution is the **higher**
one and neither branch is rewritten; a **third** branch must take the next free
number rather than the gap. **A union resolves a merge but does not describe a
build:** whichever side merges first, `Fixed-Function-dev` then reports a number
whose meaning grows when the other lands, so two builds can report the same
number and disagree, with the mismatch notice unable to fire. Bumping the second
branch during that merge closes it and costs nothing; it is **not settled**, so
whoever merges decides and writes down which they chose.

---

## 0. Why this exists at all

In fixed-function D3D9 **the game never draws its own UI** — not its settings
screen, not its debug menus, not its warp menu — so everything the game owns
would otherwise be reachable only by editing `config.json` and restarting, and
only in one direction. Every setting here needs live tuning against a path-traced
image to be worth anything, and the only thing drawn in that mode is Remix's own
ImGui.

> **Remix hosts the UI. The game owns the data. Options are the wire.**

Nothing here is a new IPC mechanism; it is the existing `RtxOption` system used
in both directions.

---

## 1. Transport: options as a bidirectional wire

| Direction | Mechanism | Namespace |
| :-- | :-- | :-- |
| Remix → game | game polls the `getRtxOptionValue` export every frame | `rtx.dusklight.game.*`, `rtx.dusklight.warp.*`, `rtx.dusklight.uiActive` |
| game → Remix | game calls `remixapi_SetConfigVariable` | `rtx.dusklight.env.*` |

Both are per-frame, so a slider takes effect on the game's next frame and a
readout appears on the one after.

**A readout that never changes is not evidence the push is dead.**
`SetConfigVariable` takes a global lock and re-parses the value string on every
call, so the game pushes only what actually changed (`remix_bridge.cpp`,
`push()`), and a steady scene costs nothing.

**Both directions of protocol skew are reported**, since 2026-08-11. Until then
the check was `protocol() < kRequiredProtocol`, so a game *newer* than the DLL
fell through to "Connected" — and that is the **common** direction, because a
session branch bumps the protocol several times while `Fixed-Function-dev` stays
put. What skew actually does, read rather than assumed: `getRtxOptionValue`
returns `0` for a name it does not declare (`rtx_option_manager.cpp:522`), the
game's `readOption` treats `0` as failure and returns its own `ConfigVar`
fallback (`remix_bridge.cpp:86`). **Nothing errors and nothing is logged** — the
newer settings silently keep their `config.json` values forever. The status strip
is the only channel that says so, which is why it had to exist before anything
else could be diagnosed.

### 1.1 Traps in this transport

Not obvious from the API, and each cost time:

- **Pushing an empty string is a no-op, not a clear.**
  `Config::parseOptionValue(const std::string&, std::string&)`
  (`src/util/config/config.cpp:1190`) returns `false` when `value.size() == 0`,
  and a failed parse leaves the option at its previous value. So
  `push("...someList", "")` **does not empty the list**. Any new list must push a
  sentinel instead — `-` is the one already in use.
- **Anything that triggers an action must be `RtxOptionFlags::NoSave`.** A commit
  counter persisted to `rtx.conf` fires on the next launch. Act on a counter
  **changing**, and **latch the first value seen without acting**, or connecting
  to a Remix that outlived a game restart triggers the action.
- **`SetConfigVariable` writes to the user layer**
  (`RtxOptionLayer::getUserLayer()`, `rtx_remix_api.cpp:1240`) — the same layer
  the UI writes, so a pushed value and a typed one are indistinguishable
  afterwards.
- **Non-empty strings pass through verbatim** — spaces, pipes and punctuation all
  survive, which is what makes the pipe-delimited list encoding safe.

### 1.2 Persistence — the trap that hid for months

**Until 2026-08-13 every setting in this panel silently reset on every launch,
with nothing logged.** An option edit is routed to a layer by the current **edit
target**, and the default is `Derived` — the layer for code-driven changes, which
is never serialised. Remix's own menus set the `User` target themselves; this
overlay is drawn from `ImGUI::update` (§2, deliberately, so F1 and Alt are
independent) and `update` sets none. Nothing about that looks broken while you
use it: the value takes effect and the widget reads back what you set. The fix is
one line at the top of `ImGUI::showDusklightOverlay`
(`dxvk_imgui.cpp:1035`):

```cpp
RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);
```

**`NoSave` still wins over it**, which is exactly right: `rtx.dusklight.env.*`
are readouts pushed every frame and must never reach a config file.

The window carries its own Save / Discard row with an unsaved marker, because a
setting that has to be re-found in another menu before it survives the session is
one that gets re-tuned every launch instead. `rtx.conf` is written **sorted, with
every `rtx.dusklight.*` in a labelled block at the end**
(`Config::serializeCustomConfig`); before that it was `unordered_map` order,
which moved between runs and made config diffs useless. The banner is inert to
the parser (`#` is not a valid key character), so a config written here still
loads in an unmodified Remix.

---

## 2. The overlay is its own window

`m_dusklightWindowOpen`, toggled by `DusklightGame::menuKeyBinds()` — **F1** by
default, the key the game's own overlay used before this rendering mode stopped
drawing it. It is drawn **outside** the `showUI == Advanced / Basic` branch in
`ImGUI::update`, deliberately: either overlay can be open without the other, F1
and Alt do not close each other, and the cursor is kept while **either** is open
(`anyOverlayOpen`).

### 2.1 Input blocking — why Remix's own switch cannot do this

`rtx.blockInputToGameInUI` works by **sending a window message across the 32-bit
bridge**, which a 64-bit game loading this `d3d9.dll` directly never receives. So
input has always fallen straight through an open menu to the game — walking Link
around while typing in a text field. The fix routes the same intent down a wire
the game *is* listening to:

```
Remix: DusklightGame::uiActive = anyOverlayOpen && blockGameInput
game:  PADBlockInput(readOptionBool("rtx.dusklight.uiActive", false))
```

`PADBlockInput` is aurora's and suppresses the held state on release, so nothing
is left stuck down. `rtx.dusklight.blockGameInput` gates it (default on).

**Rebind capture still works while input is blocked, and that is not luck:**
`PADGetNativeButtonPressed` and SDL's keyboard state read the device directly and
never consult the flag `PADBlockInput` sets (`aurora-ao/lib/dolphin/pad/pad.cpp`
— the block is in the `PADRead` path, not the native polling one). **No carve-out
was required.** Worth knowing before anyone "fixes" that asymmetry.

---

## 3. The window

```
Dusklight  ├─ Go         warp, and the clock
           ├─ Lights     effect lights, sun and moon, room lights
           ├─ Sky        atmosphere, bloom, ambient grade
           ├─ Surfaces   materials, water, HD texture pack
           ├─ Scene      the game's geometry and tuning switches
           ├─ Input      rebind the game's actions
           ├─ Mods       the game's mod inventory
           └─ Readouts   every rtx.dusklight.env.* diagnostic
```

Above the tab bar, visible from every tab: a **status strip** (connection,
protocol agreement, device registration as one coloured token), the **Save /
Discard** row, the eight **master switches**, and an **alert lane** that renders
nothing at all when nothing is wrong.

**The layout contract, which is the thing to preserve** — restructured
2026-08-17, out of four tabs over nineteen sections and roughly seven screens of
scrolling in the *default* state:

- **Each tab is flat for its hot controls**, cold ones behind sibling collapsing
  headers **all one level deep**, no flat section over one screen, and **nothing
  `DefaultOpen`** — ImGui persists only position, size and collapsed flag, so a
  `DefaultOpen` section reopens every launch, which is what the seven screens
  were made of.
- **Prose is a tooltip unless it describes something currently wrong.** Every
  `RemixGui` widget bound to an `RtxOption` already renders a hover tooltip from
  that option's description (`RtxOptionUxWrapper`'s destructor,
  `rtx_gui_widgets.h`), so a sentence the description did not carry gets moved
  **into the description**, never deleted — the header is the authority and it
  feeds `RtxOptions.md`. Group-level prose becomes the header's own tooltip via
  `dusklightHeaderTip` (`dxvk_imgui.cpp:2739`, whose comment states the ordering
  rule it depends on).
- **Every master switch is drawn in exactly one place**, or one option gets two
  widgets with the same ImGui ID — `RtxOptionUxWrapper` keys its ID off the
  option's address. They use `dusklightToggle` (`:2722`) rather than
  `RemixGui::Checkbox`; both that and the alert lane's *report, never silently
  force* rule are argued in their own comments (`:2713`, `:2745`).

**Greying, and the three rules around it.**

1. Grey **and** say why. A greyed group carries a line naming the option and
   where to find it: greyed without an explanation is barely better than broken,
   and **hidden is worse than either**, because there is then nothing on screen
   to explain and nothing to find.
2. **`BeginDisabled` goes *inside* each collapsing header, never around a run of
   them.** A `CollapsingHeader` inside `BeginDisabled` refuses the click that
   opens it, so wrapping a group makes greyed controls unreachable rather than
   merely inert. `showImguiFog` is the densest nesting here and an unbalanced
   stack does not fail loudly — **it greys the rest of the tab.**
3. **Grey a control because the code stops reading it, not because the feature
   description says it should be irrelevant.** `zHalfMin` and `densityScale` were
   greyed in `fogRampMode` 2 on 2026-08-18 and un-greyed the same day: `resolve()`
   still builds σ from both in every mode, so the greying hid the only two
   controls over the one quantity σ still governs there.

**A name collision worth knowing before you go looking.** Remix's own Lighting
tab has a section called **Effect Light** — singular — upstream's
`rtx.effectLight*` / `rtx.lightConverter` feature for attaching a light to a
tagged texture. It has nothing to do with ours,
`rtx.dusklight.game.effectLight*`, on the **Lights** tab. Grepping either the
docs or the source for `effectLight` hits both.

**One UI-only rename:** *Half Density Floor* → **Ramp Match Floor**. Still
`rtx.dusklight.atmosphere.zHalfMin`, so no config and no protocol is affected;
what changed is that the anchor it floors is no longer a half-density distance
(`DusklightAtmosphere.md` §5.1). A note or screenshot from before 2026-08-17 uses
the old label for the same widget.

**Two round-trip contracts, one shape.** The warp destination lists and the input
binds are both **resolved by the game against its own tables and pushed back as
finished strings**; the overlay sends indices and commit counters and decides
nothing. So the list you see is always the real one — it *is* the table the warp
travels on — and rebind conflicts are settled game-side, because the overlay only
knows the binds it is *handed*: anything hardcoded or context-sensitive is
invisible to it, and validating in **both** places would be worse than either.
Round-trip lag of a frame or two is the price and is not a bug. Rebinding
**displaces rather than rejects**, because rejecting leaves someone pressing a
key and watching nothing happen.

Two implementation details, each the second attempt:

- **Warp layer `-1` means "you pick" and `0` does not**, stated in the option's
  own help (`rtx_dusklight_game.h:65-68`). The bound worth adding to it: the max
  is **14**, not 15, because `dComIfGp_setNextStage` folds anything `>= 15` to
  `-1`, so 15 would be a silent alias for it
  (`dusklight-ao/src/dusk/ui/warp.cpp:12-13`).
- **The clock is a value plus a counter, and the counter lives in the UI, not in
  a read-modify-write off the option.** `dxvk_imgui.cpp:3146-3182` carries the
  reasoning — why a button can get away with `commit() + 1` and a slider cannot,
  and why the slider only syncs from the game while it is not held — plus the
  360-degree day and the six preset instants. Clock arithmetic is in **integer
  minutes** deliberately: `dxvk_imgui.cpp` does not include `<cmath>`, and
  relying on a transitive one across three compilers is not worth a CI round.

---

## 4. ImGui version constraints

Remix bundles **ImGui 1.88**.

- **`ImGui::SeparatorText` does not exist** — it arrived in 1.89. Use
  `RemixGui::Separator()` plus a text line.
- **`BeginDisabled` / `EndDisabled` do exist** and are used throughout.
- `ImGui::Combo` wants `const char* const*`; `std::vector<const char*>::data()`
  converts implicitly — a valid qualification conversion, not a warning to
  silence.

`RemixGui::Separator()` and `RemixGui::CollapsingHeader()` are in
`rtx_gui_widgets.h`; `collapsingHeaderClosedFlags` / `collapsingHeaderFlags` are
at file scope in `dxvk_imgui.cpp:502`.

---

## 5. Files

| Piece | Where |
| :-- | :-- |
| The window, the permanent header, all eight tab bodies, and `kRequiredProtocol` | `src/dxvk/imgui/dxvk_imgui.cpp` — `showDusklightOverlay` → `showDusklightWindow` → `showDusklightStatusStrip` / `MasterSwitches` / `Alerts`, then `showDusklight{Go,Lights,Sky,Surfaces,Scene,Controls,Mods,Readouts}Tab` |
| Atmosphere panel, one method per Sky-tab header | `rtx_dusklight_atmosphere.cpp` — `showImguiHot` / `Fog` / `FroxelGrid` / `SkyShape` / `PhysicalSky` / `Readouts` |
| Bloom's Dusklight-mode settings | `rtx_bloom.{h,cpp}` (`showDusklightImguiSettings`) — the one upstream file the overlay touches beyond `dxvk_imgui.cpp`, and it was already fork-only |
| The two option surfaces | `rtx_dusklight_{game,env}.h` |
| Game side of the whole wire; destination table; the game's own warp menu as the reference implementation | `dusklight-ao/src/dusk/remix_bridge.cpp`, `map_loader_definitions.h`, `ui/warp.cpp` |

**Rebase surface.** Everything Dusklight-specific is in its own functions, called
from one place each, so a conflict in `dxvk_imgui.cpp` should resolve by
re-applying a call rather than by re-deriving a tab. `DusklightRebase.md` has the
whole-fork list.
