# Dusklight atmosphere: sky, sky light and fog

Design doc for the hybrid atmosphere system in this fork: one participating
medium, derived once per frame from Twilight Princess's own environment state,
driving **the visible sky, the sky's contribution to global illumination, and
the fog** — so the three cannot disagree with each other.

Companion docs:

- `DusklightOverlay.md` — the F1 overlay and the option wire that drives all of
  this while the game runs. Read that one for anything about *controlling* the
  system rather than what it computes.
- `dusklight-ao/docs/kankyo-fog.md` — what the game computes and what it pushes.
- `dusklight-ao/docs/kankyo-remix.md` — the wider environment design; its live
  issue list is `remix-open-issues.md` alongside it.
- `aurora-ao/docs/dx9/remix-material-interface.md` — **how a captured D3D9 draw
  becomes a material in this runtime.** Nothing to do with atmosphere, but it is
  the system most often reasoned about incorrectly here, and material colour
  defects are diagnosed from there rather than from this document. Its **§0**
  also carries the standing statement of what the D3D9 stream is for — Remix's
  renderer is the product, D3D9 is the feed — which is the frame every decision
  below is made in.

- `dusklight-ao/docs/japanese-naming.md` — **how to read the game symbols this
  document quotes.** They are romanized Japanese, kept from the original team:
  `kankyo` (環境) is *environment*, `kumo` (雲) is *cloud*, `kasumi` (霞) is the
  horizon haze band, `moya` (靄) is mist, and `kytag` is a *kankyo tag* actor.
  This document glosses each on first use; that file explains the system,
  including why a grep for one of these can come back empty for a symbol that
  exists.

  **One correction that came out of reading them properly (2026-08-10):**
  `kasumi_outer` is the **near** haze band and `kasumi_inner` the **far** one —
  the reverse of what the English suggests, and the game says so in three
  independent places. `rtx_dusklight_env.h` previously described the pair as
  "on the sun's side" / "away from the sun"; **nothing in the game relates
  either to sun position.** The header is corrected; `RtxOptions.md` is
  generated and still carries the old wording. Derivation:
  `dusklight-ao/docs/japanese-naming.md` §6.

  **Two more of the same kind, found the same way (2026-08-12).**
  `rtx.dusklight.env.skyColor` said "the game's sky colour **at the zenith**" —
  the game says only 空の色, *sora no iro*, the sky's colour
  (`d_kankyo.cpp:6280`, CSV column `:6582`, debug view `d_kankyo_debug.cpp:284`),
  and where on the dome it lands is decided by `vrbox_sora.bmd`, which is in none
  of the three checkouts. The fork treating it as the dome's base colour is a
  modelling choice of ours and now says so. `rtx.dusklight.env.bloomBlurRatio`
  said "the game's bloom **brightness**"; the original team's slider labels the
  field blur **density**, paired with blur **width** beside it
  (`d_kankyo.cpp:7083-7084`, `japanese-naming.md` §8). Neither changes a number —
  both are descriptions someone would have reached for and been misled by. **The
  three `kumo*` cloud descriptions are the same class of error and are
  deliberately left alone here**: they are already corrected on the unmerged
  `claude/kasumi-naming-correction-w3e204`, and fixing them twice would be a
  conflict whose obvious resolution is fine but whose *second* copy of the
  reasoning is not.

Everything below is grounded in code as of 2026-07-28. File references are
repo-relative; `dusklight-ao/` and `aurora-ao/` prefixes point at the other two
repos.

---

## 0. The one-sentence design

> Derive the environment once, from the game's own palette, and let the visible
> sky, the light that sky casts, and the colour distant geometry fades towards
> all read that one derivation — so none of them can describe a different day.

Everything else in this document follows from that sentence.

*(An earlier wording said the atmosphere medium **is** the fog medium. That is
false at this game's scale and §1 records why: air is transparent over a hundred
metres, so the fog's density is the game's and only its colour is shared.)*

---

## 1. Why this is one system and not three

It is tempting to treat "sky rendering" and "fog" as separate features. In this
game they are not, and forcing them apart is what produces the mismatch the
current build shows.

**The game already treats them as one.** From
`dusklight-ao/include/d/d_kankyo.h`, the per-stage palette struct:

```
/* 0x10C0 */ GXColorS10 vrbox_sky_col;      // the sky
...
/* 0x1158 */ GXColorS10 fog_col;            // the fog
```

`fog_col`, `fog_start_z`, `fog_end_z` and every `vrbox_*` colour live in the
**same** palette entry, are selected by the **same** time-of-day and colpat
indices, and are blended by the **same** four-way `dKy_calc_color_set` call at
the same instant. TP's fog colour *is* approximately its sky colour at that
moment — that is why vanilla fog reads as correct: distant geometry dissolves
into the sky because it was authored to.

**Physics agrees.** Aerial perspective — distant things fading toward the sky
colour — is not a separate effect from sky colour. Both are the same scattering
integral over different path lengths. A renderer that computes the sky from a
medium and the fog from an unrelated set of coefficients is doing the same
physics twice with two different answers.

**Corrected 2026-07-28 — the two media meet at colour, not at density.** This
section originally claimed the volumetric medium and the sky medium should be
*the same object*, with the atmosphere's coefficients driving `VolumeArgs`.
Measured against this game's scale that is wrong, and implementing it literally
would have deleted the fog:

| Distance | Real Rayleigh extinction |
| :-- | :-- |
| 100 m — a typical `fog_end_z` | **0.13%** |
| 1 km | 1.3% |
| 10 km | 12.6% |

Air is transparent at the distances this game cares about. The fog TP needs is
orders of magnitude denser than air and comes from its palette. So:

- **Fog density** stays the game's. It is the only thing at this scale that
  produces visible fog at all.
- **Sky radiance** becomes physical (Phase C).
- **Fog colour** follows the sky: the far ramp samples the generated dome *in
  the view direction*. That is what aerial perspective actually is — distant
  things fade towards the sky behind them — and it is also why the original
  authored its fog colour in the same palette entry as its sky.

The consistency requirement is unchanged and still met. One weight drives the
sky's appearance, the light it casts, and the colour distant geometry fades
towards; it is carried by the shared *colour* rather than a shared σ.

Hillaire's fourth (aerial perspective) lookup table is still not needed, for a
blunter reason than the original argument gave: at these distances it would be
computing a fraction of a percent.

---

## 2. Current state, measured

Why the volumetric path currently loses to the depth-fog path. Four independent
causes; all four have to be addressed.

**2.1 The froxel grid is 20 metres.**
`rtx.volumetrics.froxelMaxDistanceMeters` defaults to `20.0`, and
`src/dxvk/shaders/rtx/utility/froxel.slangh:90` clamps with `saturate()` —
everything beyond collapses into the last slice. `rtx.sceneScale` is 1, so a
Remix world unit is 1 cm, and TP's units match it (Link ≈ 170 units ≈ 1.7 m;
`setSunpos`'s 80000 ≈ 800 m). **20 m = 2000 game units.** Field-area
`fog_end_z` values run well into the tens of thousands. The volumetric system
structurally cannot express TP's distance fog.

**2.2 It is not reading the game's fog.**
`rtx.volumetrics.enableFogRemap` defaults to **false**, so volumetrics runs on
`transmittanceColor` (0.999³) at 200 m — generic near-clear grey, unrelated to
kankyo. `enableFogColorRemap` is *also* false, so enabling the first alone
still would not pick up `fog_col`.

**2.3 With remap on, the defaults make it far too thin.**
`getVolumeArgs` (`src/dxvk/rtx_render/rtx_global_volumetrics.cpp:494-520`) maps
`fogState.end` through `fogRemapMaxDistanceMin/MaxMeters` = 1→40 m. TP's `end`
exceeds 40 m constantly, and `normalizedRange` at line 517 is **never clamped**
— so the derived measurement distance overshoots its intended ceiling and the
medium comes out much thinner than the game's fog.

**2.4 The curve shape is wrong in principle.**
TP uses `GX_FOG_PERSP_LIN` and nothing else — `aurora-ao/lib/dx9/dx9_draw.cpp`
`apply_fog_state()` solves start/end from the BP curve and emits
`D3DFOG_LINEAR`. Linear fog is a **ramp**: zero before `start`, *fully opaque*
at `end`. Beer-Lambert asymptotes and never closes. Remix's own comment at
`rtx_global_volumetrics.cpp:433` concedes the point: *"the density can only be
approximated."*

**2.5 A fifth, separate problem: the fog-state lottery.**
`src/dxvk/rtx_render/rtx_scene_manager.cpp:609-611`:

```cpp
} else if (m_fog.mode == D3DFOG_NONE) {
  m_fog = input.getFogState();
}
```

**Remix keeps the first non-`NONE` fog state it sees in a frame and discards
every later one.** TP sets fog *per object* — `dKy_tevstr_c::mFogStartZ/
mFogEndZ/FogCol` are written per-tevstr in `setLight_bg`/`setLight_actor` and
applied by a `GXSetFog` per draw (`dusklight-ao/src/d/d_kankyo.cpp:9458`). So
the fog Remix uses for the whole frame is decided by whichever draw happens to
be submitted first. That is unstable across frames and blind to the per-room
variation the game relies on.

**Also relevant:** `rtx.volumetrics.enableAtmosphere` is **false** by default,
and its own description says it *"should generally be enabled if volumetrics
are used in outdoor settings as without a finite atmosphere infinite light
sources such as the skybox and distant lights will not function properly."*

The `rtx.conf` guidance in `dusklight-ao/docs/dx9-fixed-function.md` picked the
depth path (`rtx.volumetrics.enable = False`) for exactly these reasons.

**Superseded 2026-07-28 — all five causes are addressed and Phase A tested
good (§12).** With the atmosphere on, volumetrics is the path to run. §2 is
kept as the measurement it was, not as current advice; in particular 2.1's
"structurally cannot" is a statement about the **default** 20 m grid, and A4
drives that extent from the game's fog range every frame.

---

## 3. Architecture

Three layers. The boundary between them is the thing that makes rebasing cheap
and the consistency guarantee structural.

```
┌─ LAYER 1 ── game (dusklight-ao) ────────────────────────────────┐
│  dusk::remix::captureEnv()  — one snapshot per frame            │
│  Reads the *outputs* of kankyo's blend, never re-derives them.  │
│    sky:    vrbox_sky_col, kasumi_inner/outer, kumo_*,           │
│            hide_vrbox                                           │
│    fog:    fog_col, mFogNear, mFogFar                           │
│    sun:    azimuth, elevation, fade, isDay                      │
│    scene:  colpat crossfade (prev, curr, ratio), indoor/outdoor,│
│            moya mode/count                                      │
│    time:   daytime                                              │
│  Pushed through the existing rtx.dusklight.env.* NoSave options │
└──────────────────────────────┬──────────────────────────────────┘
                               │
┌─ LAYER 2 ── DusklightAtmosphere (this fork) ────────────────────┐
│  ONE derivation per frame, in rtx_dusklight_atmosphere.cpp:     │
│    physicalWeight  = f(sun elevation) × g(outdoor) × h(colpat)  │
│    Medium          = { rayleigh, mie, ozone, ground albedo }    │
│    LUTs            = transmittance, multiscattering, sky-view   │
│    VolumeArgs      = extinction/scattering from the SAME medium │
└───────┬──────────────┬──────────────┬───────────────────────────┘
        │              │              │
┌───────▼───┐  ┌───────▼──────┐  ┌────▼─────────────┐
│ dome light│  │ visible sky  │  │ volumetrics      │
│ (sky GI)  │  │ (primary ray)│  │ (+ aerial persp) │
└───────────┘  └──────────────┘  └──────────────────┘
                                       │
                                 ┌─────▼────────────┐
                                 │ far-field ramp   │
                                 │ (composite)      │
                                 └──────────────────┘
```

**Layer 1 reads outputs, not inputs.** This is a deliberate and load-bearing
choice. `g_env_light.fog_col` / `mFogNear` / `mFogFar` are the *final* values
after every modifier the game applies: the four-way palette blend, the
`addcol_fog` additive offset, the `now_fogcol_ratio` scale that lightning
pulses (`d_kankyo_rain.cpp:362`), and the `dKy_fog_startendz_set` override that
the Lost Woods mist tag drives. Reading the outputs means every one of those
comes along for free and stays correct when the game changes. Re-deriving from
the palette tables would mean reimplementing all four modifiers and keeping
them in sync forever.

**Four, and both of the other counts were wrong.** `kankyo-fog.md` listed a
*sixth* modifier, the `fog_avoid_tag`, until 2026-08-11; it modifies no fog
(§8.2). And what this paragraph used to call a fifth — "the *second* 'gather'
colpat blend (`mColPatBlendGather`)" — **is not a second blend at all.** The
`*Gather` fields are the staging area every tag and event writes into;
`exeKankyo` copies them onto `wether_pat0` / `wether_pat1` / `pat_ratio` once a
frame and clears them back to sentinels
(`dusklight-ao/src/d/d_kankyo.cpp:4788-4828`), and `dKy_change_colpat`
(`:9528-9533`) writes only there. There is exactly one `pat_ratio` and one
blend on it (`:2406-2409`). Corrected on both sides 2026-08-11;
`dusklight-ao/docs/kankyo-fog.md` §2 has the game-side account, and §4 below has
what the miscount cost this renderer.

**Layer 2 owns all derivation.** Nothing downstream computes atmosphere
parameters; they consume. That is what makes "sky and fog cannot disagree" a
structural property rather than a tuning task.

---

## 4. The single blend weight

One scalar governs how stylised-versus-physical the whole system is, and it
drives sky appearance, sky light *and* fog colour together. Splitting it would
produce a physical blue sky over amber stylised haze.

```
physicalWeight = elevationTerm × outdoorTerm × styleTerm
```

| Term | Source | Rationale |
| :-- | :-- | :-- |
| `elevationTerm` | smoothstep on sun elevation | Real and stylised skies converge at high sun; they diverge most at dawn/dusk, and a physical model has nothing to say at night. |
| `outdoorTerm` | `g_env_light.hide_vrbox` + colpat | The game already reports "this area has no sky" — `d_a_vrbox.cpp:69` sets `hide_vrbox` when the sky colours sum to zero. Free and authoritative. |
| `styleTerm` | colpat **crossfade** — both patterns and the ratio | Weather patterns → low, and the term follows the game's own fade between them rather than cutting on the incoming index (below). It also drops to 0 on colpat 9, on the belief that colpat 9 is the Palace of Twilight — **that belief is unverified and its stated source is a different index space entirely.** §8.6 has the correction, the instrumentation added to settle it, and what happens next in each case. |

At `physicalWeight = 0` the system is a faithful reproduction of the vanilla
gradient and vanilla fog. At `1` it is Hillaire driven by palette-derived
parameters. Every intermediate value blends the **radiance function**, not the
output pixels — so GI, visible sky and aerial perspective stay mutually
consistent at every setting.

### 4.1 `styleTerm` follows the game's fade — protocol 13, UNTESTED IN GAME

**What was wrong.** The game never holds one colour pattern. It holds a
crossfade: `wether_pat0` (outgoing), `wether_pat1` (incoming) and `pat_ratio`
between them, and *every* colour this weight is blended against — sky, fog,
ambient, bloom — is that lerp. The bridge pushed `wether_pat1` alone, so
`styleTerm` cut hard on the incoming index: it **stepped on the first frame of
a weather transition and stayed stepped for its duration**, while everything
beside it moved continuously.

Worst exactly at the change frame. `dKy_change_colpat` sets the ratio to `0.0f`
and does not touch `wether_pat0` (`d_kankyo.cpp:9528-9533`), so the frame the
index changes on the wire is the frame the palette is **100% the old pattern**.
The renderer's answer was as far from the game's as it ever gets, at the one
moment the two are compared.

And the kankyo tags make it structural rather than transient. `kytag01`, the
Lost Woods mist tag, writes both endpoints *and* the ratio every frame with
`mColPatModeGather = 1` (`d_a_kytag01.cpp:96-99`), which stops the game
advancing the ratio itself (`d_kankyo.cpp:2192`) and hands the tag the whole
blend. Its ratio is a `cLib_addCalc` ramp taking roughly 50 frames to travel
0 → 1 (`:129-148`) — **that ramp is the mist's strength**, and reading the index
alone saw full mist from the first frame the tag was in range.

**What landed.** Two more readouts, `rtx.dusklight.env.colpatPrev` and
`colpatBlend` (quantized to 0.01 game-side), and `styleTerm` became a lerp:

```
weatherTerm = w(colpatPrev) * (1 - blend) + w(colpat) * blend
w(p)        = p == 0 ? 1 : clamp(physicalWeatherWeight, 0, 1)
```

**Default-safe by algebra, not by assertion.** `colpatBlend` defaults to `1.0f`
and the game pins it at `1.0f` whenever no transition is running, so the
ordinary reading — and the reading from a game build too old to push it — is
exactly `1.0f`. At `blend == 1.0f` the first product is multiplied by exactly
`1.0f - 1.0f`, and since `w(p)` is either `1.0f` or a value already clamped to
`[0,1]` it is always finite, so `w * 0.0f` is exactly `+0.0f` (not NaN) and
`0.0f + w(colpat)` is exactly `w(colpat)` in IEEE-754. The result is the
previous expression bit for bit, not approximately.

**The colpat 9 guard is untouched**, deliberately: it returns before this line,
so nothing here bypasses it, and it is still under instrumentation (§8.6).
One stated consequence rather than a discovered one — a transition *out of*
pattern 9 now lerps from that pattern's ordinary weather weight rather than
from the guard's zero, which is a fade beginning at the moment the guard stops
firing, not a new step.

**Regression signature.** The stylised/physical handover becomes **gradual
where it used to snap**. If instead it becomes erratic or oscillates during a
weather change, `colpatBlend` is being read at the wrong point in the frame.
The atmosphere panel and the Dusklight tab both now print the pattern as
`prev -> curr @ ratio` beside the resolved physical weight, which is where that
distinction is visible: a healthy transition sweeps the ratio 0 → 1 once and
stops, and outside a transition the two patterns are equal with the ratio at
`1.00`.

**Untested in game.** CI-only as of 2026-08-11.

---

## 5. Fog: mapping TP's linear ramp onto a medium

### 5.1 The mapping

TP's fog is `f(z) = saturate((z - start) / (end - start))`. We need an
extinction coefficient σ for a Beer-Lambert medium. Replace Remix's hand-tuned
linear remap with an analytic match at the **half-density point**:

```
z_half = max((start + end) / 2, zHalfMin)
σ      = ln(2) / z_half
```

Scale-free, no magic endpoints, and correct across every regime the game uses:

| Situation | `start`, `end` (units) | z_half | Result |
| :-- | :-- | :-- | :-- |
| Hyrule Field, clear day | large, very large | far | thin medium, long-range haze |
| Faron Woods, morning | mid | mid | visible mood fog, thins as the palette advances |
| Forest Temple | near-mid | mid-near | interior depth |
| Goron Mines | near | near | dense, hot |
| Lost Woods mist tag, kytag01 at full | `-2000`, `200` | clamped to `zHalfMin` | near-whiteout |

Note the mist-tag case: the tag — a **kytag**, i.e. a *kankyo tag*:
`d_a_kytag00`…`d_a_kytag17` are invisible actors that override environment state
for the area they sit in — passes a **negative start** with
`start < end` (`dusklight-ao/src/d/actor/d_a_kytag01.cpp:94`), which in the
vanilla ramp means "already ~90% fogged at z=0". The clamp turns that into a
very dense medium, which is the right answer. (An earlier revision of this
section said `start > end`; that was a recon claim nobody verified, and §14.7
records why it mattered.) `zHalfMin` is the one tuning knob and it
exists to stop σ diverging.

**Two corrections to this row, both landed 2026-08-11 and both from the same
misreading.** They matter because this is the regime the ledger's C0 still calls
uncalibrated, and the test plan was aiming at the wrong place.

- **The area is the Lost Woods / Sacred Grove, not Lake Hylia.** The actor says
  so twice — `d_a_kytag01.cpp:1-4` is commented *"Sacred Grove Mist Tag"*, and
  `:202` is an authored `OS_REPORT` reading 「迷いの森　霧タグの…」, *Lost
  Woods fog tag*. Both name one stage, `F_SP117`. Whether Lake Hylia *also*
  carries a kytag01 is **not knowable from source** — actor placement is in
  `.dzs` stage data. Full evidence and the probable origin of the mix-up:
  `dusklight-ao/docs/kankyo-fog.md` §3.3.
- **"At full" is away from the tag, not at it.** The third argument to
  `dKy_fog_startendz_set` is a lerp weight toward the override
  (`d_kankyo.cpp:747-748`: `value += ratio * (override − value)`), and it is `0`
  inside `mNamiInnerRange`, `1` beyond `mNamiOuterRange`
  (`d_a_kytag01.cpp:53-69`). The view term runs the same way — `0.2` facing the
  tag, `1.0` looking away (`:81-92`). **The tag marks a clear centre.** And the
  whole layer is gated by the switch-driven fade at `:71`/`:124-144`: with the
  switches off this row does not occur at all.

**This replaces `rtx.volumetrics.enableFogRemap` entirely for us.** We do not
enable Remix's remap; we write the derived coefficients straight into
`VolumeArgs`. See §6.1 for why that direction matters.

### 5.2 The split — by job, not by distance

**Reworked 2026-08-13. `rtx.dusklight.atmosphere.fogRampMode` selects between the
two, defaulting to the new one; the old one is kept as the A/B baseline. What
follows describes both, because the reason the first one had to go is the most
useful thing in this section.**

#### The original: split by distance

The §5.1 mapping matches density but not shape: at `z = end` vanilla is 100%
opaque and an exponential is ~75%. So each system got the range it was good at:

- **`[0, froxelMaxDistance]` — volumetrics.** Shafts, godrays, torch glow.
- **`(froxelMaxDistance, ∞)` — the vanilla ramp, analytically.** Closes to
  exactly 100% at `end`.

**The flaw is at the near end, not the far one, and it is C11.** A homogeneous
medium cannot be clear where the game is clear. TP's ramp is *exactly zero*
before `fogStartZ`; an exponential starts extinguishing at the camera. With
`start` halfway to `end`, `z_half = 0.75·end`, so at `z = start` the derived
medium is already `1 − 2^(−0.667) ≈ 37%` opaque where the original is untouched.
Under a split-by-distance scheme that error is unreachable — the near field is
precisely the region the medium was made responsible for.

#### The rework: split by job

The ramp owns the fog's **appearance** at every distance; the medium owns the
**light**. The ramp is applied as the residual against whatever the medium
already did:

```
f = saturate((z − start) / (end − start))     the game's ramp, unchanged
m = 1 − T                                      what the medium already achieved
t = saturate((f − m) / (1 − m))                what the ramp still owes
out = lerp(radiance, fogColour, t)
```

**This is an identity, not an approximation.** Writing `T = 1 − m`:

```
T·(1 − t) = (1 − m)·(1 − (f − m)/(1 − m)) = (1 − m) − (f − m) = 1 − f
```

so the surface arrives attenuated by **exactly** the game's ramp, at every
distance, *for any density the medium happens to have.* And with the medium's
ambient in-scatter set to the same radiance the ramp blends towards — which is
what `multiScatteringScale = 1` plus the albedo division in `applyVolumeArgs`
means — the colour terms close the same way:

```
A·m·(1 − t) + A·t = A·[m(1 − f) + f − m]/(1 − m) = A·f
```

Total: `S·(1 − f) + A·f + I_lit·(1 − t)`. **The original's fog formula, plus the
real in-scatter from real lights.** That last term is the shafts, and it is the
only thing the volumetrics add to the picture — which is the correct division of
labour, because it is the only thing they can do that a ramp cannot.

**What this buys, and it is the whole point: density stops being a fidelity
knob.** It no longer decides how thick the fog looks, so it is free to be set
purely for shaft quality — and, critically, free to be held *under* the game's
own curve so C11 stops happening.

#### Holding the medium under the ramp

`rtx.dusklight.atmosphere.limitDensityToRamp` (default on) lowers σ until

```
max over z of [ (1 − e^(−σz)) − f(z) ]  ≤  clearZoneTolerance
```

The peak is analytic — the difference of an exponential and a clamped line can
only peak where the line starts, where the two slopes match
(`z* = ln(σ·span)/σ`, real only once `σ·span > 1`), or at an endpoint — and it is
monotone in σ, so a bisection solves it exactly. Both σ values, the peak and where
it occurs are in the panel and in the log line.

`clearZoneTolerance` (default 0.08) is **the one real trade in the fog system**.
At 0 the near field is as crisp as the original and there is almost no medium
left to scatter anything; higher buys shaft presence with a thin near-field haze.

**Since 2026-08-15 that budget is spent in luminance, not in coverage**, and the
tolerance above is its ceiling. Coverage alone is half the picture: what a viewer
sees is coverage × brightness, so the same 0.08 is invisible in a dim cave and a
haze hanging in front of the player in a lava-lit one.

That is measured rather than supposed. One Goron Mines log (2026-08-14) carries
both cases at the same setting:

| Area | fog ambient | luminance | peak excess | at |
| :-- | :-- | --: | --: | --: |
| Death Mountain, outdoors | 0.11, 0.08, 0.06 | 0.085 | 0.04 | 14137 units |
| Goron Mines, interior | 0.44, 0.36, 0.09 | **0.358** | 0.08 | **500 units** |

Four times the light, the same budget, and indoors the peak sits at the ramp
start — the closest fogged point to the player. That run was reported as fog far
too dense near Link.

So the effective tolerance is
`min(clearZoneTolerance, clearZoneVeilTarget / luminance(fogAmbient))`.
`clearZoneVeilTarget` (default 0.01) takes Goron Mines to **0.028** and leaves
both outdoor states untouched at the 0.08 ceiling, because there the quotient
lands above it. Set it to 0 to restore the pure coverage budget. The panel and
the log both report the budget actually used beside the ceiling, so it is visible
when the weighting binds and by how much.

Note this is still a bound on the excess, not a removal of it — see the clamp
below. **Untested in game.**

The residual clamps at zero rather than going negative, because subtracting
already-integrated light means dividing it back out, which amplifies the froxel
grid's noise. The cap is what keeps the clamp from being reached.

#### Two consequences worth stating up front

- **Alpha-blended surfaces need the same treatment.** They are fogged by the
  medium alone (`composite.comp.slang`, the stochastic alpha blend block). That
  was survivable while the medium carried most of the fog; with a thin medium a
  particle in front of distant terrain would sit almost unfogged in front of it.
  The same residual is applied there, on `surface.hitT`.
- **The near half now needs a colour that means something.** See §5.3.

Upstream, `src/dxvk/shaders/rtx/pass/composite/composite.comp.slang` forbids the
ramp running at all under volumetrics (landed as A3 — the guarded version is
§12's "What landed"):

```hlsl
if (cb.volumeArgs.enable)
{
  return; // Volumetric fog is applied in volume integration
}
```

That early-out is conditional on the Dusklight path, and inside it the mode
above decides what the ramp contributes.

**The answer to "which system owns fog" changed on 2026-08-13.** It used to be
*one owner per unit of distance*. It is now **one owner per job**: the ramp owns
appearance, the medium owns light. The first answer forced a homogeneous medium
to imitate a linear ramp in the region where it is least able to.

### 5.3 The fog's colour, and the dome light that is not in the light list

**Landed 2026-08-13. Untested in game.**

The medium's colour never came from lighting. It came from
`multiScatteringEstimate`, a constant added at every raymarch step, and there is
a hard reason it has to exist:

> **Remix's froxel grid is lit by next event estimation over the RTXDI light
> list, and that list holds five types — sphere, rect, disk, cylinder, distant
> (`shaders/rtx/concept/light/light_types.h`). There is no dome light in it.**
> `sampleDomeLightTexture` appears in the geometry resolver, the indirect
> integrator and the composite, and **nowhere in the volumetric code**.

So the volumetric fog is lit by the sun and by analytic lights and by nothing
else. Outdoors, **fog standing in a shadow receives nothing at all from the
sky**; indoors it receives nothing but the effect lights. Everything it showed in
shadow was that flat constant, which is why shadowed fog read as a grey veil
rather than as air.

Three things were wrong with the constant, and all three are now addressed:

1. **It was 4.4× darker than the far ramp, for the same fog colour.** The
   raymarch adds `estimate × scatteringCoefficient` integrated along the ray, and
   `scatteringCoefficient = σ·albedo`, so a constant `M` over a path of opacity
   `1 − T` contributes `M·albedo·(1 − T)` — with `albedo` 0.9 and the old
   `multiScatteringScale` default of 0.25, that is `0.225·C` where the far ramp
   converges to `1.0·C`. **No setting could correct it**: `multiScatteringScale`
   moves only the near half and `fogRadianceScale` moves both. Fixed by dividing
   by the albedo in `applyVolumeArgs` and defaulting the scale to 1.0, which
   makes the two halves algebraically equal (§5.2).
2. **It was a display colour used as a radiance.** `fog_col` was authored to be
   blended over a finished, exposed image; here it is a quantity of light in a
   linear frame. Its *level* therefore means nothing. Ledger C10, still the root
   cause of dark scenes reading grey, and only partly addressed — see below.
3. **It could not follow the sky, because the fog cannot see the sky.** Fixed by
   measuring the dome instead.

**The measurement.** A new one-workgroup pass,
`shaders/rtx/pass/dusklight/dusklight_sky_stats.comp.slang`, reduces the 256×128
generated dome to its solid-angle-weighted mean radiance and writes one
`DusklightSkyStats`. The host copies it into a `kMaxFramesInFlight` ring and
reads the oldest slot — the same non-stalling pattern the auto-exposure debug
stats use. The sky changes over minutes; a few frames of lag is invisible.

A sphere average is the *right* quantity rather than a compromise: the term it
feeds is an isotropic in-scatter estimate, so what it wants is the froxel's
irradiance from the environment. The far ramp keeps sampling the dome in the
**view direction**, which is aerial perspective proper; the two agree on average,
which is the kind of agreement that matters.

**Corrected 2026-08-13, from the first test session: the dome supplies the fog's
*hue*, not its *level*.** `skyIntensity` is a **lighting** calibration — it sets
how strongly the dome lights the world against the sun, and it is 6 because the
palette colours it scales are small once decoded out of gamma. Handing that
radiance to the fog as its colour made the fog about **six times** brighter than
the colour the game authored: the session log measured a palette fog colour of
`0.12` against a dome-derived ambient of `0.71`. Worse, `multiScatteringEstimate`
is **not shadowed by anything**, so an interior received full open-sky in-scatter
inside a sealed room — which is what "washed out and bright indoors" was.

`rtx.dusklight.atmosphere.skyAmbientMode` now selects:

| Mode | The dome supplies | |
| :-- | :-- | :-- |
| 0 Off | nothing | palette only; fog in shadow loses the sky's hue |
| 1 **Hue only** *(default)* | colour, and its variation across the frame | level comes from the palette, after the exposure correction |
| 2 Full radiance | colour and brightness | physically the better answer for an open sky; try it if terrain reads darker than the sky behind it |

In Hue only mode the dome's radiance is normalised by luminance to the palette's
own level, and **the same scale is handed to the composite**, so the far ramp's
view-direction sample lands where the near half's sphere average did.

`skyAmbientWeight` (default 1.0) drives **both halves**. It used to be
`physicalWeight` on the far half only, on the reasoning that the palette stops
describing the sky once the sky is simulated — true, but too narrow: the game's
fog colour is its sky colour under either model. Splitting the two weights is how
the fog ends up one colour close by and another in the distance.

**How much of C10 this fixes: outdoors, most of it; indoors, none.** A dome
radiance is measured in the renderer's own units, so outdoor fog is now bright
when the scene is bright and dark when it is dark without anyone tuning a level.
Indoors there is no dome and the flat palette colour still stands, so a dark room
can still have its blacks lifted by fog. **The exposure-relative treatment is
deliberately not in this change** — it is the remaining half of C10.

### 5.4 The fog's level — treating a display colour as one

**Landed 2026-08-13. Untested in game.** This is the remaining half of ledger
C10, and it closes it.

`fog_col` is not a radiance. The game blended it over a **finished, already
exposed image**, so its meaning is *"what the screen should show there"*.
Decoding it out of gamma (§5.1) fixes its shape; nothing fixes its **level**,
because a display colour has no level until you say what exposure it was meant
to be seen at.

Used raw in a linear frame it is a fixed quantity of light — far too much for an
unlit cave and not enough for a sunlit field — and **no single value can serve
both**. That is the whole of "dark scenes read grey".

The fix is to say the exposure out loud:

```
fogRadiance = sRGBToLinear(fog_col) · fogRadianceScale / exposure
```

`exposure` is what the tonemapper is about to multiply the frame by
(`tonemapping.slangh`, `getExposure`; applied as `color *= getExposure(...)` in
`tonemapping_apply_tonemapping.comp.slang`). Its auto-exposure half comes from
`DxvkAutoExposure::getExposureMultiplier()`, a few frames stale off the same
non-stalling host ring the Readout panel uses.

**It cannot run away.** After tonemapping the fog contributes
`(C / exposure) · exposure = C` — its *display* value is invariant to exposure
by construction, so it adds exactly zero gain to the eye-adaptation loop. The
fog still attenuates the scene, and that term is exposure-independent.

**Only the palette's share is corrected.** Whatever the fog takes from the sky
dome (§5.3) is already a real radiance measured in the renderer's own units, and
rescaling it would be wrong. So outdoors with `skyAmbientWeight` at 1.0 this does
essentially nothing — which is why the default mode is *Indoors only*.

`rtx.dusklight.atmosphere.exposureFogMode`:

| Mode | Where it applies | Why you would pick it |
| :-- | :-- | :-- |
| 0 Off | nowhere | The pre-2026-08-13 behaviour, kept as the A/B baseline |
| 1 **Indoors only** *(default)* | areas the game reports as having no sky | Exactly where there is no dome to take a real radiance from, so exactly where the problem still bites |
| 2 Always | everywhere, to the palette's share | If outdoor fog still reads mis-levelled with `skyAmbientWeight` below 1 |

**Deliberately reads the auto-exposure multiplier only, not
`rtx.tonemap.exposureBias`.** A manual bias is a considered look adjustment to
the whole image and the fog should ride along with it; eye adaptation is an
automatic normalisation the original never had, and it is that normalisation
which breaks the display-colour assumption.

**Two consequences to expect.** A dark room's fog stops lifting the blacks —
that is the point. And fog brightness now moves *with* eye adaptation rather
than against it, so walking from sun into a cave should no longer show the fog
brightening as the scene darkens. The panel prints the live correction
(`exposure correction: x…`); below 1 means a bright scene, above 1 a dark one.

### 5.5 Forward scattering

`rtx.volumetrics.anisotropy` is **0** upstream — perfectly isotropic — which
spreads every light's in-scatter evenly in all directions and leaves shafts and
torch glow flat and shapeless. Real atmospheric haze is strongly forward
scattering. `rtx.dusklight.atmosphere.fogAnisotropy` (default 0.6) overrides
`volumeArgs.volumetricFogAnisotropy` whenever the Dusklight medium is driving,
the same way `froxelMaxDistance` already did.

**Do not confuse it with `mieAnisotropy` (0.76)**, which shapes the sun's glow
*inside the generated dome image* and never reaches the froxel grid. Both exist,
both are about forward scattering, and they act on different things.

---

## 6. Live configuration

The game's fog changes with area, room, time of day and story state. The
renderer configuration has to follow it. Two rules make that safe.

### 6.1 Push state, derive in Remix — never push renderer options

The bridge pushes *game state* (`rtx.dusklight.env.fog*`, all `NoSave`), and
`DusklightAtmosphere` writes the derived values directly into `VolumeArgs`
each frame. It must **not** write `rtx.volumetrics.*` option values per frame:

- those are saved options, so a settings save would bake one moment's weather
  into `rtx.conf` — the exact failure mode already hit once on this project;
- it would fight any manual tuning the user does in the ImGui panel;
- it churns the option system's dirty tracking every frame.

`rtx.volumetrics.*` therefore keeps its meaning as **artist override / scale on
top of** the derived values, not as the transport for them.

### 6.2 `froxelMaxDistance` must follow the fog — carefully

Driving `froxelMaxDistanceMeters` from the game's fog range is the single
biggest quality win available, because it puts the grid's 64 slices where the
fog actually is: a tight grid in the Goron Mines (near, dense, fine detail),
a wide one in Hyrule Field (far, thin). It costs **nothing** at runtime —
`m_froxelVolumeExtent` is computed from render resolution and
`froxelDepthSlices()` only (`rtx_global_volumetrics.cpp:797-802`), never from
max distance, so no reallocation is involved.

**But upstream explicitly assumes it never changes.** From
`src/dxvk/shaders/rtx/utility/froxel.slangh:237-239`:

> *"Current froxel depth slices, depth slice distribution exponent and max
> distance are used even though these may have changed between now and the
> previous frame, resulting in incorrect reprojection. This is not a huge issue
> though as these settings should not be changed dynamically in production…"*

The previous frame's froxel mapping is reconstructed using the *current*
frame's `froxelMaxDistance`. Change it between frames and the temporal history
reprojects from the wrong place; with `maxAccumulationFrames` at 128 that
smears badly.

Two mitigations, both wanted:

1. **Add `previousFroxelMaxDistance` to `VolumeArgs`** and use it for the
   previous-frame mapping. Three lines, entirely correct, and it removes the
   upstream limitation rather than working around it.
2. **Smooth the value anyway.** The game's own fog transitions are smoothed
   (`cLib_addCalc`), so following them with a time constant is faithful as well
   as safe. Snap only on hard cuts (room change, area load), where the
   denoiser history is being reset regardless.

`froxelDepthSlices` is *not* safe to change live — it is documented "must be
constant after initialization" and does drive allocation. It stays fixed.

---

## 7. Computed once, used many

The efficiency structure. Each row is a place where a naive implementation
would compute the same thing twice.

| Quantity | Computed | Consumed by | Note |
| :-- | :-- | :-- | :-- |
| Palette blend (fog + sky + ambient) | **game**, once per frame | everything | Reading outputs also inherits the addcol, ratio and override layers for free, and the tag/event writes that stage through the `*Gather` fields into that same blend (§3) |
| Transmittance LUT (256×64) | on medium change only | sky-view LUT, aerial persp | Function of the medium alone — **not** of sun direction. Dirty-flag it on colpat/time-slot change; it does not belong in the per-frame path |
| Multi-scattering LUT (32×32) | on medium change only | sky-view LUT | Same |
| Sky-view LUT (lat-long) | per frame | dome light, visible sky **and** the far fog's colour | There is no separate sky-view table: the dome image *is* it. All three consumers already funnel through `sampleDomeLightTexture`, so one evaluation serves appearance, GI and aerial perspective with no new plumbing |
| Medium extinction/scattering | per frame, once | volumetrics only | **Not** shared with the sky — see the correction in §1. Air is transparent at this game's distances, so the fog's medium and the sky's are different things that agree on colour |
| Aerial perspective | — | — | **Not computed.** The far ramp samples the dome in the view direction, which is the same thing for a fraction of the cost. No fourth LUT |

The two big ones are the last two rows: unifying the medium removes a whole
derivation *and* removes the possibility of sky and fog disagreeing, and the
aerial-perspective LUT from the Hillaire paper is simply not needed in this
architecture.

---

## 8. Clashes, and how each is resolved

### 8.1 Moya particles vs volumetric density

`mMoyaMode`/`mMoyaCount` — **moya** is 靄, mist — drive *billboard* haze
particles, separate from GX fog: mode 3 is the Lost Woods / Sacred Grove mist
tag (`d_a_kytag01.cpp`), 4 is kytag02, 10/11 are weather (`d_a_kytag06.cpp`), 1
is cutscene. Under a path tracer these become
camera-facing quads. Keeping them *and* a dense medium double-counts the haze.

**Resolution:** suppress the moya billboards (same mechanism as
`hideSkyBillboards`) and fold their contribution into the medium — map
`mMoyaCount` onto `rtx.volumetrics.enableHeterogeneousFog` +
`noiseFieldDensityScale`. Real volumetric swirl instead of quads, which is an
upgrade, and no double-count. Compromise recorded in §9.

### 8.2 The fog-avoid tag (kytag08) — no clash, and no work is owed

> **Withdrawn 2026-08-11. This section described a clash that does not exist.**
> It said `g_env_light.fog_avoid_tag` "tracks a moving position that pushes fog
> away from the player", that "a homogeneous medium cannot express *clear
> here*", and it booked a heterogeneous-fog feature against carrying it later.
> **The tag writes no fog state anywhere.** Kept rather than deleted so the
> claim is not rediscovered and re-planned.

What was read in the game tree, all of it citable:

- The field has exactly four references in the game tree: its declaration
  (`include/d/d_kankyo.h:332`), one write (`d_a_kytag08.cpp:264`, on actor
  create), and one read, tested then dereferenced (`d_kankyo.cpp:11608`,
  `:11610`). There is no other consumer.
- That read sits inside `dKy_bg_MAxx_proc`, in the branch for background
  materials named `MA11` (`d_kankyo.cpp:11539`), on the non-twilight side of
  `dKy_darkworld_check()`. All it does is build a `C_MTXLightPerspective`
  projection aimed at the tag's `mAvoidPos` and hand it to that material's
  **texture matrix 0** via `setEffectMtx` (`:11608-11637`). That is a projected
  texture on ordinary drawn geometry — no `J3DFogInfo`, no `GXSetFog`, no
  palette field.
- **The game names the geometry.** The same branch sets the material's TEV
  colours 1 and 2 (`:11584`, `:11590`), and the HIO panel that overrides exactly
  those two in a debug build (`mist_twilight_c1_col` / `c2_col`, `:11594-11604`)
  is headed 「霧沼　トワイライト時　色設定」 — *kirinuma*, **fog swamp**,
  "colour settings while in twilight" (`d_kankyo.cpp:7594`, sliders
  `:7596-7603`). So the "fog" the tag clears is a **painted ground surface**,
  and the tag's job is to punch a projected hole in it.
- The rest of `d_a_kytag08.cpp` is two JPA particle emitters (`0x84A0`, plus
  `0x84A1` or `0x84A2` by world, `:252-257`), audio
  (`mDoAud_setFogWipeWidth` `:80`, `mDoAud_startFogWipeTrigger` `:97`) and one
  player flag (`onFogFade()` `:157`, which sets `FLG2_FOG_FADE`). Grepping the
  file for `fog_col`, `mFogNear`, `mFogFar`, `dKy_fog_startendz_set`,
  `GXSetFog`, `J3DFogInfo`, `addcol_fog` and `now_fogcol_ratio` returns nothing.

**Resolution: no work is owed, in any phase.** The clear bubble is a mesh with a
projected texture on it plus particles, so it arrives through the ordinary draw
stream exactly like any other geometry. It was never in our medium, so it cannot
be lost from it — and a density subtraction around that position would carve a
hole in the atmosphere that the original never carved, on top of a bubble the
draw stream already delivers. C11 (a homogeneous medium cannot be clear near the
camera) is still a real reason to want heterogeneous fog; **this tag is not, and
nothing is queued behind it.**

**What this does not claim.** Whether that projected texture still *looks* right
once path traced is a separate, untested question about materials. Aurora does
forward the matrix — a `GX_TG_MTX3x4` camera-space texgen becomes
`D3DTTFF_COUNT3 | D3DTTFF_PROJECTED`
(`aurora-ao/lib/dx9/dx9_tev.cpp:858`, `:919-921`) — but nobody has looked at the
result in game. It is not tracked here because it is not a fog question.

### 8.3 Per-object fog

Some draws legitimately run `GX_FOG_NONE` (UI, `d_drawlist`, mirrors,
`d_gameover`). Aurora already excludes ortho draws so Remix does not see fog on
UI. But *world* objects can also carry different fog than their room.

**Resolution:** the bridge's global env fog is authoritative, replacing the
first-draw-wins lottery of §2.5. This is strictly better than today but does
flatten genuine per-object variation. Recorded in §9.

### 8.4 Auto-exposure vs dense fog

Dense fog raises mean scene luminance; auto-exposure pulls down; the mist-tag
whiteout would auto-correct itself into flat grey and lose the drama it exists
to create.

**Resolution:** clamp exposure adaptation while derived density is high, or
pin exposure during scripted fog events. Needs calibration, not architecture.

### 8.5 Sky brightness and the LDR probe

Covered in the sky work: the auto-detected sky probe is clamped to 1.0 because
it inherits the format of the game's own render target, which here is 8-bit
(`rtx_sky.h:152-158`). The dome light replaces it entirely
(`integrator_indirect.slangh:372-379` is an if/else).

**Corrected 2026-07-29 — this section originally added "which is what unlocks
real HDR sky radiance", and that was wrong twice over.** The clamp is inherited
rather than intrinsic (`rtx.skyForceHDR` overrides the format outright), and the
probe feeds GI by ray miss exactly as the dome does. §14.10 has the reading;
what actually argues for the dome light is listed there too.

The dome light is **not** in the sampleable light list —
`light_types.h` has Sphere/Rect/Distant only — so it contributes by ray-miss
alone. Two consequences carried into this design: **never bake the sun disc
into the dome texture** (keep it analytic and NEE-sampled), and keep the
*lighting* variant of the sky-view LUT smoother than the visible one.

### 8.6 Twilight Realm — and the colpat 9 bypass, which is unverified

The design point still stands: **a physical atmosphere has no valid parameters
for the Twilight Realm.** There is no sun; the look is amber over black with
drifting particles. Rayleigh/Mie/ozone cannot produce it at any setting, so
`physicalWeight` is driven to 0 there — bypass, not tune. Because the same
weight drives fog colour, the amber fog stays amber automatically. It is
otherwise unremarkable machinery: the same `vrbox_*`, `fog_col` and
`fog_start_z/end_z` blend with different numbers, plus the full-screen
desaturate-and-tint already bridged (`monoAmount` ≈ 0.38).

**What was wrong was the number the bypass cuts on, and where it came from.**

> **Corrected 2026-08-11.** This section used to say "`d_kankyo.cpp:266-270`
> shows the Palace of Twilight is simply colpat pattern 9", and
> `resolvePhysicalWeight` still carries `kPalaceOfTwilightColpat = 9` on that
> reading. **That citation is a different index space.**
> `d_kankyo.cpp:266-270` is inside `dKy_sense_pat_get` (`:135-316`), which
> returns the **wolf-sense vision pattern** — the index that picks what
> senses-mode looks like, in `dKy_WolfPowerup_BgAmbCol` (`:318`) and
> `dKy_WolfPowerup_FogNearFar` (`:416`), both reached only from the
> wolf-powerup branches of the `setLight` family (`:2452-2453`, `:2519`,
> `:2928`, `:2982`, `:3040`, `:3157`). The game's
> own debug panel confirms the space: the combo box bound to
> `twilight_sense_pat`, under the heading
> 「■ トワイライト　センスパターン」 (`d_kankyo.cpp:7456`, combo at `:7459-7474`),
> labels entry 9 as 「９：Ｌｖ８専用　D_MN08」 — *"9: for Lv8 only, D_MN08"*
> (`:7469`). So "9 = Palace of Twilight" is a true statement **about the sense
> patterns**, and nothing anywhere relates it to colpat.
>
> `rtx.dusklight.env.colpat` is `g_env_light.wether_pat1`
> (`dusklight-ao/src/dusk/remix_bridge.cpp:1878-1879`). That field indexes
> `stage_envr_info_class::pselect_id[65]` (`d_stage.h:171`) through the pattern
> switch at `d_kankyo.cpp:1896-1990`, which handles 0–7 explicitly and 8–63 in
> its `default` arm. It is a per-stage palette-slot selector authored in stage
> data, unrelated to the sense patterns and with no shared meaning for any
> value.
>
> **That one panel has now produced three wrong attributions, and this is the
> first one that is traced rather than suspected.** The other two both come from
> its entry 2, 「２：ハイリア湖専用」 — *"2: Lake Hylia only"* (`d_kankyo.cpp:7462`)
> — read as colpat, which is the *probable* origin of the Lost Woods fog tag
> being placed at Lake Hylia across twelve passages in two repos
> (`dusklight-ao/docs/kankyo-fog.md` §3.3, `docs/japanese-naming-audit.md` §4.1;
> stated there as a likely cause, not a proven one). Reading a label out of its
> panel is the recurring failure here, not a one-off.

**Does any stage actually run colpat 9? UNKNOWN, and not knowable from source.**
What is established:

- No literal 9 is written to colpat anywhere in the game tree.
  `dKy_change_colpat` is called with 0–6 and 10–12 only, and the direct writes
  to `wether_pat1` use 1, 2, 3, 4 and 6.
- But colpat **can** be 9, from stage data, through three routes.
  `d_a_kytag06.cpp:1105-1107` writes `wether_pat0`/`wether_pat1` straight from
  the actor's own parameter (`field_0x591 = fopAcM_GetParam(a_this) & 0xFF`,
  `:1070`); `d_a_kytag01.cpp:174,182-183` does the same with
  `fopAcM_GetParam(i_this)`; and `d_a_kytag06.cpp:876` passes a **path point's**
  `mArg0` to `dKy_change_colpat`, which reaches `wether_pat1` via
  `mColpatCurrGather` at `d_kankyo.cpp:4798`/`:4819`. Actor parameters and path
  points live in `.dzs`/`.dzr` stage data, which this checkout does not contain.
- So the honest answer is UNKNOWN. It is **not** "no". A related detail worth
  noting rather than concluding from: pselect slots 8 and 9 are also what the
  game's own *underwater* override reaches for (`d_kankyo.cpp:1996-2010`), so a
  stage authoring colpat 9 would land on the same slot. Whether that means
  authors avoided 9 or reused it is **not established**.

**Where the bypass sits, and why that matters.** In `resolvePhysicalWeight` the
order is: feature switches → `skyHidden()` → **colpat 9** → `sunIsDay()`. The
`skyHidden()` test above it already returns 0 for every area with no sky, so the
bypass can only fire outdoors. And the Palace of Twilight — the thing it was
written for — is permanently in the dark world (`l_darkworld_tbl`,
`d_kankyo_data.cpp:135`, `D_MN08` at `KY_DARKLV_UNCLEARABLE`, a level nothing
ever clears), where `setDaytime` pins `daytime = 0` (`d_kankyo.cpp:1630-1636`),
which makes the bridge's `sunIsDay` false (`remix_bridge.cpp:632`, day is
`daytime` in 67.5–292.5). **The `sunIsDay()` test four lines below would return
0 there anyway.** So in the one place the bypass names, it changes nothing; the
only place it can change anything is an outdoor, daylit stage that happens to
select pattern 9 — which is the failure mode, not the feature.

**It has not been removed, because "probably inert" is exactly the reasoning
that produced three no-op fixes on this project.** Instead
`resolvePhysicalWeight` now calls `logColpatOnce`, which emits **one line per
distinct colpat per run**, only when a sky is visible, reporting the pattern,
the sun elevation, whether it is daylight, and whether the bypass fired. It is
capped by a 64-bit mask over the game's own 0–63 pattern range, so a whole
session cannot spam it.

**What each possible result decides.** Written down now so the follow-up is
mechanical rather than another judgement call:

| What the log shows over a session that visits outdoor areas **and** the Palace of Twilight | Reading | What will be done |
| :-- | :-- | :-- |
| No line ever says `FIRED` | colpat 9 never reaches the bypass; the literal has no target | **Delete the bypass and `kPalaceOfTwilightColpat`.** Dead code. Say so in this section and in §4. |
| `FIRED` appears, always with `daylight no` | the bypass fires only where `sunIsDay()` would return 0 four lines later | **Delete the bypass.** It is redundant, not load-bearing; `physicalWeight` stays 0 with it gone. |
| `FIRED` appears with `daylight yes` | an outdoor, daylit area is losing its entire physical sky on this literal | **Remove the bypass**, and note the colpat value that did it. The Twilight Realm does not need it — `sunIsDay` already covers it — so what remains is a stage being penalised for a palette-slot number. |

> **One trap in reading that table.** The fork's own clock freeze
> (`rtx.dusklight.game.freezeTime`) deliberately skips the `daytime = 0` that
> the dark-world branch applies (`d_kankyo.cpp:1560-1567`), so a frozen clock
> carried into the Twilight Realm **can** produce `FIRED` with `daylight yes`
> *inside* the Palace of Twilight. Read the third row only from a session with
> the clock freeze **off**.

**Deferred, not open.** No log exists yet and none can be produced from a
checkout, so the decision waits on exactly one play session. Until then the
bypass stays as it is. Nothing else in this document is blocked on it.

---

## 9. Compromise ledger

Recorded so that dissatisfaction with a specific result can be tuned rather
than met by abandoning the physical path. Each row: what is given up, how it
shows, and the first knob to reach for.

| # | Compromise | How it will show | First knob |
| :-- | :-- | :-- | :-- |
| C0 | ~~The calibration pass was never run.~~ **Run 2026-07-28. Phase A/B confirmed good in-game.** One constant was wrong: `skyIntensity` at 1.0 gave a visibly dim sky. The analytic anchor was right but the arithmetic behind it was not — it ignored that the palette colours are decoded out of gamma before they are scaled, which takes a mid blue from 0.5 to about 0.2, so the multiplier needed to be ~6× larger to land the same sky-to-sun ratio. Now 6.0. `zHalfMin` and `froxelRangeScale` were not reported as wrong. | — | — |
| C11 | ~~**A homogeneous medium cannot be clear near the camera.**~~ **Largely resolved 2026-08-13, untested in game.** The observation stands and the arithmetic was worse than this row said: with `start` halfway to `end` the matched medium is **~37%** opaque where the original is untouched. What changed is that the medium no longer has to reproduce the fog at all (§5.2), so σ is capped until its peak excess over the game's own ramp is within `clearZoneTolerance` (default 0.08) and the ramp supplies the rest. | Residual near-field haze bounded by `clearZoneTolerance`, and reported exactly: "peak excess over the game's ramp" in the panel and the log. | `rtx.dusklight.atmosphere.clearZoneTolerance` — lower for a crisper near field, higher for more shaft presence. This is now the *only* trade in the fog system. |
| C10 | ~~**Fog is composited in linear HDR, not the game's display space.**~~ **Resolved 2026-08-13, untested in game, in two halves.** Outdoors the fog's colour is measured off the generated dome (§5.3), a real radiance in the renderer's units. Everywhere the palette is still supplying it — indoors above all — the colour is divided by the exposure the tonemapper is about to apply (§5.4), which is what a display colour means. `rtx.dusklight.atmosphere.exposureFogMode` selects Off / Indoors only / Always. | If it is wrong it will be *level*, not hue: fog too bright or too dim for the scene it sits in. The panel and log both print the live correction factor. | `exposureFogMode` to change where it runs, `fogRadianceScale` for the overall level, `skyAmbientScale` for the dome's share. |
| C1 | **Dusk saturation.** Physical twilight is more graduated and less saturated than TP's authored dusk. | Sunsets read calmer / less punchy than vanilla. | Lower `physicalWeight`'s `elevationTerm` at low sun; or add a saturation push applied to the *medium's* Rayleigh/Mie tint, not to output pixels. |
| C2 | **Exponential never fully closes.** | Distant terrain slightly more visible than vanilla at `fog_end_z`. | The §5.2 range split is the fix; if still short, lower the split distance so the vanilla ramp owns more. |
| C3 | **Clouds have no physical analogue.** `kumo_top/bottom/shadow` (**kumo** = 雲, cloud) describe painted cloud bands. | Skies read emptier than vanilla if the vrbox — the game's skybox dome — is replaced wholesale. | Keep TP's cloud layer as geometry over our sky (Phase D). |
| C4 | **Weather has no physical analogue.** Clear-sky scattering cannot do "rain grey". | Storms look insufficiently oppressive. | `styleTerm` drops `physicalWeight` on weather colpats; overcast can also be faked with high Mie + suppressed sun. |
| C5 | ~~Moya swirl replaced by noise.~~ **Withdrawn - the problem does not exist on this backend.** `mMoyaCount` feeds `mpCloudPacket->mCount` (`d_kankyo_rain.cpp:1616`, inside `cloud_shadow_move`), and `dKankyo_cloud_Packet::draw` already returns early on D3D9 (`d_kankyo_wether.cpp:119-126`). The haze billboards were never drawn here, so there is nothing to double count and no switch was needed. `moyaMode`/`moyaCount` are still pushed, as a signal of how much haze an area wants folded into the medium. | — | — |
| C6 | ~~**Fog-avoid tag ignored.**~~ **Withdrawn 2026-08-11 — nothing was being given up.** `fog_avoid_tag` (kytag08) writes no fog state at all: its only reader aims a projected texture matrix at the `MA11` ground material the game itself calls 霧沼 *kirinuma*, "fog swamp", and the rest of the actor is particles, audio and a player flag. The clear bubble is drawn geometry and reaches Remix through the ordinary draw stream. §8.2 has every citation. **No heterogeneous-fog work is owed to this** — C11 is still a reason to want it, this is not. | — | — |
| C7 | **Per-object fog flattened to one global.** (§8.3) | Objects authored with distinct fog match their room instead. | Could be restored per-instance later; costs a per-instance field. |
| C8 | **Night is fully stylised.** Physics gives near-black without a sun. | No moonlight scattering / no physical night sky. | Deliberate. Moon-driven scattering is possible but is a separate feature. |
| C9 | **`zHalfMin` clamp is a magic number.** (§5.1) | Extremely dense scripted fog may cap below vanilla. | Single tunable; raise the cap. |

Expected fidelity by scenario, as a reference for judging results:

| Situation | TP look retained | What changes |
| :-- | :-- | :-- |
| Midday outdoors | ~90% | Sky nearly identical; gains correct blue sky-fill and correct aerial perspective |
| Morning / afternoon | ~80% | Gentle warming, more graduated |
| Dawn / dusk | ~60–70% | Gains real structure, loses saturation — see C1 |
| Night | ~100% | Stylised, unchanged |
| Rain / storm | ~90% | Stylised-dominant — see C4 |
| Twilight Realm | 100% | Physics bypassed entirely |
| Interiors | n/a | No sky; fog from palette; **effect lights own the rest** - and they are the only NEE-sampled source there, since the sun/moon is gated off indoors and Remix has no dome light type |

---

## 10. Phases

Each phase is independently shippable and states its predicted look up front.

**Phase A — fog fidelity (no atmosphere yet).** Fixes the current complaint.
- A1 Push global env fog through the bridge; replace the §2.5 lottery.
- A2 New `rtx_dusklight_atmosphere` module: §5.1 mapping → `VolumeArgs`.
- A3 Range split in `applyFog` (§5.2).
- A4 Live `froxelMaxDistance` + `previousFroxelMaxDistance` (§6.2); force
  `enableAtmosphere` on outdoors.
- *Predicted:* volumetric fog matching vanilla density at every distance,
  **plus** godrays and shafts. Lost Woods mist-tag whiteout, Goron Mines heat,
  Faron morning and Forest Temple all fall out of one mapping with no per-area
  code.

**Phase B — sky as dome light.**
- B1 Generate a lat-long sky texture from the kankyo colours; register it as a
  normal Remix dome light. *Supplying a real texture means zero changes to the
  dome sampling path* — a deliberate modularity choice over the untextured
  route.
- B2 Hide the vrbox; disable auto sky detection.
- *Predicted:* visually near-identical sky, but HDR, controllable, and the
  sky-tagging problem is permanently solved. Sky and skylight guaranteed equal.

**Phase C — physical atmosphere.**
- C1 Hillaire transmittance and multiscattering LUTs, rebuilt only when the
  medium changes. There is no separate sky-view LUT: the dome image already is
  one, so the physical evaluation goes straight into it.
- C2 `physicalWeight` blend (§4).
- C3 **Couple:** the far fog's colour follows the sky, sampled in the view
  direction. Aerial perspective for the cost of one texture fetch.
  for free (§7).
- *Predicted:* §9's fidelity table.

**Phase D — clouds, moya, polish.** C3 from the ledger. (C5 was withdrawn: the
haze billboards are never drawn on this backend, so there is nothing to fold in.)

**Phase 0, before A: the free calibration pass.** Raising
`froxelMaxDistanceMeters` costs nothing at runtime, so a config-only experiment
answers whether range or curve shape dominates, with no rebuild:

```
rtx.volumetrics.enable = True
rtx.volumetrics.enableAtmosphere = True
rtx.volumetrics.froxelMaxDistanceMeters = 200
rtx.volumetrics.enableFogRemap = True
rtx.volumetrics.enableFogColorRemap = True
rtx.volumetrics.fogRemapMaxDistanceMaxMeters = 300
```

If distant terrain still refuses to close, that is §2.4 (shape) and only the
range split fixes it. Either way Phase A starts from a measurement.

---

## 11. Rebase surface

Maintaining this against upstream dxvk-remix is a stated requirement. The
design keeps the upstream diff to a checklist.

> **Scope, corrected 2026-08-04.** `CLAUDE.md` routes every rebase here, so this
> section covers **the whole fork**, not just the atmosphere. It used to list the
> fog/sky hooks only, which meant the materials work, the emissive/ramp transport,
> the API capture change, the grade pass and the Dusklight bloom were all absent
> from the one list a rebase reads.
>
> **Extended 2026-08-07** with the tone mapping and auto exposure work, which is the first block
> here that is *not* Dusklight-named. That block was assembled from the change itself rather than
> by name matching, because name matching would have found none of it.
>
> **How this list was produced, and what that is worth:** every file below either
> names `Dusklight`/`dusklight` in the working tree or was identified from the
> change that owns it. It was *not* produced by diffing against an upstream tag,
> so treat it as "everywhere our changes are visible by name", not as a proof
> that nothing else moved. A rebase should still read the conflict list. Line
> numbers are deliberately omitted — the previous revision cited
> `rtx_scene_manager.cpp:609` for a hook that now lives near line 2070.

**New files (zero upstream churn):**
- `src/dxvk/rtx_render/rtx_dusklight_atmosphere.{h,cpp}` — all fog/sky derivation.
- `src/dxvk/rtx_render/rtx_dusklight_grade.{h,cpp}` — the ambient grade stage.
- `src/dxvk/rtx_render/rtx_dusklight_emissive.h` — self-illumination cut and the
  two-colour ramp, which shares the same `D3DMATERIAL9` transport.
- `src/dxvk/rtx_render/rtx_dusklight_texrep.{h,cpp}` — HD texture packs: handle
  decode, residency tracking, both substitution sites' logic, counters.
- `src/dxvk/rtx_render/rtx_dusklight_water.h` — water: the `Power` decode, the
  translucent material construction, the layer switches and the log.
- `src/dxvk/rtx_render/rtx_dusklight_{env,game}.h` — the two option surfaces.
- `src/d3d9/d3d9_rtx_matrep.h` — the material translation report.
- `src/dxvk/shaders/rtx/pass/dusklight/*` and
  `src/dxvk/shaders/rtx/pass/bloom/bloom_dusklight_*.comp.slang` — shaders are
  auto-discovered by `compile_shaders.py`'s `os.walk`; no build-file edits.
- `.cpp`/`.h` files do need a line in `src/dxvk/meson.build` (`src/d3d9/meson.build`
  for the d3d9 half). `rtx_dusklight_emissive.h` and `d3d9_rtx_matrep.h` are not
  listed there today — header-only, so the build does not care, but it is a
  divergence from the convention in `AGENTS.md` and a rebase will not flag it.

**Upstream files touched, and how.** Each is a single guarded hook unless the
row says otherwise.

*Atmosphere — fog, sky, sky-light:*

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp` | fog state selection + `prepareSceneData` | `DusklightAtmosphere::active()` |
| `rtx_global_volumetrics.cpp` `getVolumeArgs` | one early branch to the Dusklight derivation | same |
| `rtx_composite.cpp` | fills `DusklightCompositeArgs` for the far half of the fog | same |
| `composite.comp.slang` `applyFog` | range split (mode 0) / top-up residual (mode 1), plus three helpers next to `sampleDomeLightTexture` | `cb.dusklightArgs.enable` |
| `composite.comp.slang` stochastic alpha blend | the same top-up applied to alpha-blended surfaces, so particles are fogged like the geometry behind them | `cb.dusklightArgs.enable` and `fogRampMode != 0` |
| `rtx_global_volumetrics.cpp` | `volumetricFogAnisotropy` taken from the Dusklight medium instead of `rtx.volumetrics.anisotropy` | `dusklight` |
| `composite_args.h` | one args struct added (`DusklightCompositeArgs`) | additive only |
| `froxel.slangh` + `VolumeArgs` | `previousFroxelMaxDistance` | additive; also a genuine upstream fix |
| `rtx_light_manager.cpp` | **two** related corrections, both unguarded, both about the RTXDI buffer index on API lights — a rebase that re-applies one and not the other gets the worse half of each. See the row below and *Effect lights* | none — straight corrections, and they apply to every API light |

*Effect lights (2026-08-06) — design in `dusklight-ao/docs/effect-lights.md`.
Almost all of this system is game-side; the fork's share is small and listed
here in full:*

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_light_manager.cpp` `addExternalLight` | preserves the light's buffer index across an overwrite, matching the game-light path a few lines above. Three lines, no reformatting | none |
| `rtx_light_manager.cpp` `prepareSceneData` | range-checks `previousBufferIdx` before using it. An index is only meaningful if it was assigned **last** frame, and a light that leaves `m_linearizedLights` entirely — which an API light does on any frame `DrawLightInstance` is not called for it — is never reset by the loop's else branch. Trusting it wrote past the end of `m_lightMappingData` or mapped one light's temporal history onto another | none |
| `rtx_dusklight_game.h` | the 18 `effectLight*` options | additive |
| `rtx_dusklight_env.h` | the 10 `effLights*` readouts | additive |
| `dxvk_imgui.cpp` | the **Effect Lights** section of the Game tab, and the warning shown when both light systems are on | own block |

*Materials — the 2026-08-04 work (see `aurora-ao/docs/dx9/remix-material-interface.md` §9–§10):*

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_instance_manager.cpp` | the emissive patch, one site; plus `CheckRtInstanceSize` needs its constant updated whenever `RtSurface` grows | `rtx.dusklight.emissive.enable` |
| `rtx_materials.h` | ramp endpoints on `RtSurface` (`data15.w`, `textureFlags` bits 15–16 — both were spare) | additive; struct size is guarded |
| `rtx_materials.cpp` | the ramp/emissive members added to `hashStructByMemory` | must sum to `sizeof(T)` exactly |
| `surface.h` (shader) | the same two fields on the GPU `Surface` | additive; no growth |
| `opaque_surface_material_interaction.slangh` | `albedo = mix(rampLo, rampHi, albedo)` | `rtx.dusklight.rampMaterials` |
| `d3d9_rtx_utils.cpp` | `isVertexColorBakedLighting` taken per draw from `D3DMATERIAL9::Specular.r` instead of the global option | falls back to the option |
| `d3d9_rtx.cpp` | one guarded call to the matrep at the tail of `processTextures` | `rtx.dusklight.matrep` |

*API assets, made capturable and replaceable (2026-08-04):*

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_remix_api.cpp` | API mesh hashes derived from the submitted vertex/index data; upstream's `hack_getNextGeomHash` removed | unconditional — a behaviour change against upstream, not a guarded hook |
| `rtx_scene_manager.cpp` `submitExternalDraw` | consults `getReplacementMaterial` before using the supplied material | unconditional, same |

*HD texture packs — 2026-08-05 (`aurora-ao/docs/dx9/texture-replacements.md`):*

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp` `determineMaterialData` | one call at the tail, after `as<OpaqueMaterialData>()`, overwriting only the albedo texture. **Order matters:** the conversion is what sets the sampler override and the ignore-alpha flag, and it must not become a merge | `rtx.dusklight.texrep.enable` |
| `rtx_scene_manager.cpp` `usePreservePath` | one extra `&&` term, same shape as the existing `terrainCascadesJustChanged` | same |
| `rtx_scene_manager.cpp` `onFrameEnd` | two counter calls | same |
| `d3d9_device.cpp` `BindTexture` | the rasterized/HUD site: the emitted CS lambda gains a captured handle and may swap the bound view. **This is the only place the fork touches `d3d9_device.cpp`** — a rebase that drops it loses the HUD half silently, with the world half still working | `rtx.dusklight.texrep.applyToRaster` |

Note that `d3d9_device.cpp` was not an upstream file this fork touched before
this change; a rebase reading an older copy of this list will not expect a
conflict there. **Verified rather than assumed** —
`git diff origin/main...HEAD -- src/d3d9/d3d9_device.cpp` against the upstream
tracker returns exactly two hunks, the include and `BindTexture`. That command
is also how to re-check it after any future change.

*Water — 2026-08-11 (`aurora-ao/docs/dx9/remix-material-interface.md` §11):*

> **Two of these rows are shared with HD texture packs and one is shared with the
> ramp.** That is the whole reason this feature had to be rebased rather than
> merged: it was developed against a side band that had since been claimed, and
> the conflict's obvious resolution deletes texture packs. If a future rebase
> conflicts here, read `aurora-ao/docs/dx9/in-flight-allocation.md` before
> resolving.

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp` `determineMaterialData` | the water branch, **after** the replacement lookup and **before** `as<OpaqueMaterialData>()`. Order is load-bearing in both directions: a hand-authored material must still win, and the legacy conversion is what made every water layer an opaque white sheet | `rtx.dusklight.water.enable` |
| `rtx_scene_manager.cpp` | the replacement-coercion branch and the two bounded logs | `rtx.dusklight.water.applyToReplacements` |
| `rtx_instance_manager.cpp` | hides the projected overlay; drives water texcoords from the options instead of the draw's transform | `hideProjectedLayer`, `animateTexcoords` |
| `rtx_materials.h` | `RtSurface` gains `texcoordElementCount` and `isTexcoordProjected`, **placed in existing padding so the struct does not grow** — `CheckRtInstanceSize<784>` is therefore undisturbed. Also the projective-transform encoding in `writeGPUData` | additive |
| `rtx_materials.cpp` | `Ambient.g`/`.b`/`.a` and `Power` added to `computeIdentityHash`. **Not optional:** a side channel outside that hash lets two draws differing only in it collide, and the preserve path then serves a stale material. `hashStructByMemory` must still sum to `sizeof(T)` — 152 bytes, `padding[3]` | must sum exactly |
| `rtx_draw_call_tracker.cpp` | `_pad0` becomes `texcoordProjection`; same size, so the hash struct is unchanged in shape | additive |
| `surface.h`, `surface_interaction.slangh` (shaders) | the projective texture divisor, carried in the eye-origin words when there is no eye | `textureFlags` bits 21–22 |
| `opaque_surface_material_interaction.slangh` | one line, shared with the ramp | `rtx.dusklight.rampMaterials` |
| `rtx_opacity_micromap_manager.cpp`, `rtx_types.h` | small plumbing for the above | additive |
| `d3d9_rtx.cpp`, `d3d9_rtx_utils.cpp`, `d3d9_rtx_matrep.h` | texcoord element count and projection flag carried through; matrep reports them | additive |

*Overlay, bloom and plumbing:*

| File | Change | Guard |
| :-- | :-- | :-- |
| `dxvk_imgui.{cpp,h}` | the F1 Dusklight overlay and its three tabs, including the Water panel — full surface in `DusklightOverlay.md` §5 | own functions, called from one place each |
| `rtx_bloom.{h,cpp}` + `bloom.h` | the Dusklight bloom mode and its settings | `rtx.bloom.dusklight` |
| `rtx_context.{h,cpp}` | `dispatchDusklightGrade`, and the bloom stage ordering | `DusklightGrade` enable |
| `dxvk_objects.h`, `dxvk_device.cpp` | the two modules constructed and exposed as `metaDusklight*` | additive members |

*Tone mapping and auto exposure (2026-08-06/07) — see `ToneMappingExposureNotes.md`:*

> **This block is a different shape from the rows above and a rebase should expect that.** The
> Dusklight work is concentrated in `dusklight_*` files with single guarded hooks into upstream.
> This work is not: it **rewrites** two upstream passes outright and edits eight upstream shaders.
> There is no `Dusklight` in most of these filenames, so the "names dusklight in the tree" heuristic
> that produced the rest of this list would have missed all of it.

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_auto_exposure.{h,cpp}` | **rewritten.** Trimmed log-average metering, tanh soft limiter, asymmetric tau, deadband, cut snap, debug readback. Removed options stay registered as deprecated no-ops | none — replaces the upstream design wholesale |
| `auto_exposure.comp.slang` | **rewritten** reduction; the mean/median modes are gone | same |
| `auto_exposure_histogram.comp.slang` | metering weight applied to the count rather than the colour; fixed-point accumulation | same |
| `rtx_tone_mapping.{h,cpp}` | `tonemapOperator` enum replaces `finalizeWithACES` (migrated on load); GT7 bypasses the dynamic tone curve; push-constant `static_assert` | additive plus one branch |
| `rtx_local_tone_mapping.{h,cpp}` | same operator enum and migration; luminance pass gains the operator args | additive |
| `rtx_context.cpp` | auto exposure now receives `resetHistory` on camera cut — upstream never passed it | one argument added |
| `tonemap/tonemapping.h` | `AgxArgs`, `Gt7Args`, operator constants, histogram domain constants; `ToneMappingApplyToneMappingArgs` is now **exactly 128 bytes** | additive, but the budget is full |
| `tonemapping_apply_tonemapping.comp.slang` | operator dispatch; GT7 replaces the dynamic curve | one branch |
| `local_tonemap/local_tonemapping.h` | `LuminanceArgs` and `FinalCombineArgs` gain the operator args | additive |
| `local_tonemap/local_tonemapping.slangh` | `localTonemapRuler()` — the fusion's ruler follows the selected operator | new function in an upstream header |
| `local_tonemap/luminance.comp.slang` | three ruler calls; also fixes the missing `suppressBlackLevelClamp` | rewritten lines |
| `local_tonemap/final_combine.comp.slang` | operator dispatch plus the ruler probe | two sites |
| `ThirdPartyLicenses.txt` | AgX (Wrensch, MIT), three.js (MIT), GT7 (Polyphony Digital, MIT) | additive |
| `RtxOptions.md` | generated; regenerate after any option change. It will conflict on a rebase and the resolution is to regenerate, never to merge by hand | generated |

**New files (zero upstream churn) for this work:**
- `src/dxvk/rtx_render/rtx_agx.{h,cpp}` — AgX look presets, the `TonemapOperator` enum, and the
  `finalizeWithACES` migration helper shared by both tone mapping paths.
- `src/dxvk/rtx_render/rtx_gt7.{h,cpp}` — GT7 setup, a transcription of the reference's
  `initializeAsSDR()`/`initializeCurve()`.
- `src/dxvk/shaders/rtx/pass/tonemap/agx.slangh`, `gt7.slangh` — the two operators.
- `src/dxvk/shaders/rtx/pass/tonemap/reference/gt7_tone_mapping.cpp` — Polyphony's GT7 sample
  implementation kept verbatim as the source of truth, plus `reference/README.md` recording its
  provenance and how it was verified. **Not built**; meson lists sources explicitly and
  `compile_shaders.py` only walks `.slang`. Never edit it to fix a port bug - fix the port.
- `documentation/ToneMappingExposureNotes.md` — six defects (five of them upstream), the design
  decisions and their measurements.

**Three things a rebase should check first here:**
1. `ToneMappingApplyToneMappingArgs` is exactly at the 128-byte push-constant limit. Three
   `static_assert`s guard it. If upstream adds a field to that struct, the fix is to move the
   operator argument blocks into a uniform buffer, not to shrink them.
2. The operator enum is mirrored in two places — `dxvk::TonemapOperator` in `rtx_agx.h` and the
   `tonemapOperator*` constants in `tonemapping.h` — with `static_assert`s tying them together.
3. `rtx.autoExposure` deprecated options must stay registered. Deleting them makes existing
   `rtx.conf` files log unknown-option noise.

Rule for every guarded hook: **one branch, no reformatting of surrounding code,
and the guarded path calls into our module rather than inlining logic.** A
conflict then resolves by re-applying a single `if`, not by re-deriving intent.
The two API rows are the exception — they change upstream behaviour rather than
adding a branch, so a rebase has to re-apply intent there, and they are the two
worth checking first.

**Game side** (`dusklight-ao`): everything in `src/dusk/remix_*.{cpp,hpp}` and
`src/dusk/effect_lights.{cpp,hpp}`. Churn in `d_kankyo.cpp` is limited to one
capture call, matching the existing `dKy_celestial_orbit_z_ratio` pattern;
`d_particle.cpp` carries one guarded call at the tail of
`dPa_simpleEcallBack::set`, which is the only point at which each instance of a
shared "simple" effect is still distinguishable from the others. Aurora's half
of the material transport is in `extern/aurora/lib/dx9/` and rebases against
aurora, not against Remix.

---

## 12. Status

| Item | State |
| :-- | :-- |
| Design | this document, 2026-07-27 |
| Phase 0 calibration | run 2026-07-28 — see §13 |
| Phase A (A1–A4) | implemented 2026-07-27, **tested good 2026-07-28** |
| Phase B (B1–B2) | implemented 2026-07-27, **tested good 2026-07-28**; `skyIntensity` raised 1.0 → 6.0 after it read dim |
| Phase C (C1-C3) | implemented 2026-07-28, **run 2026-07-29 — scattering confirmed, verdict blocked by the sky/fog defect below** |
| Overlay, warp, input blocking | landed 2026-07-28, **tested good 2026-07-29** |
| Time-of-day scrub + freeze | landed 2026-07-28, **tested good 2026-07-29** — `DusklightOverlay.md` §3.2.1 |
| Fog-avoid tag (kytag08) | **closed 2026-08-11 without code.** It touches no fog; §8.2 and C6 record why, and nothing is owed |
| colpat 9 bypass | **instrumented 2026-08-11, decision deferred.** `logColpatOnce` added; the literal's source is the wrong index space and whether any stage runs colpat 9 is UNKNOWN. **Untested in game — one play session with the clock freeze off settles it.** §8.6 has the three possible results and what each one triggers |
| `skyFogMode` = Exempt | **TESTED GOOD 2026-08-13.** The sky no longer picks up the medium; the horizon seam and the dingy sky are gone. Weighted stays as a taste control for foggy weather rather than as a rival candidate, and Off stays as the A/B baseline. This closes the "live defect" below |
| Fog rework: top-up ramp, density cap, dome-derived ambient, forward scattering | **landed 2026-08-13, UNTESTED IN GAME.** No protocol bump — this is fork-only and reads game state that was already on the wire. §5.2–§5.5 carry the design and the derivations; the causes it addresses are C11, C10, the 4.4× near/far colour mismatch and the isotropic phase function |
| First test-session corrections | **landed 2026-08-13, from the owner's first run of the rework.** Four defects, all found from the session log: the fog's dome-derived ambient was ~6x the palette's colour and unshadowed, so interiors washed out (`skyAmbientMode`, new, defaulting to Hue only); the sky-stats readback served a previous area's sky for `kMaxFramesInFlight` frames on resume; the fog log burned its whole 32-line budget in 450 ms of one transition; and `fogAnisotropy` went back to upstream's 0 as the prime suspect for a reported grid-like look. §5.3 and §5.5 |
| Exposure-relative fog level | **landed 2026-08-13, UNTESTED IN GAME.** §5.4. Closes the second half of C10. Also made the auto-exposure stats readback unconditional — it used to run only while the Readout panel was open, which would have made the fog's brightness depend on whether anyone was looking at it |
| Dusklight panel settings persisting | **landed 2026-08-13, UNTESTED IN GAME.** Every control in the F1 overlay was writing to the Derived layer, which is never serialised, so the whole panel silently reset on each launch. `DusklightOverlay.md` §1.2 has the mechanism and the trap |
| colpat crossfade (`styleTerm`) | **landed 2026-08-11, protocol 13, UNTESTED IN GAME.** The bridge pushed one third of the game's palette blend; `styleTerm` now follows all three and lerps instead of cutting on the incoming index. Default-safe by algebra at `colpatBlend == 1.0`, which is both the option default and the game's steady state. §4.1 has the derivation, the regression signature and why the colpat 9 guard was left alone |

Owner's verdict on A + B after testing: *"a massive, frankly monumental
success."* Range, shape and per-area fog scaling all validated; see §13's
"What the pass found".

**The 2026-07-29 session cleared this list.** `hideSkyBillboards`, warp, the
time-of-day slider and Freeze Time all work; local point lights work (they need
`localLightIntensity` 19 and `localLightRadius` 10, both now the defaults — see
`dusklight-ao/docs/remix-open-issues.md` open issue 3); and `hideSkyBillboards`
**fixed the night shadow wandering**, confirming the moon-quad cause rather than
merely masking it.

**Local point lights were superseded on 2026-08-06** and now default off — not
because they stopped working but because working exposed what was wrong with
them: they place the light where the *game* put it, and a GameCube point light
casts no shadow, so those positions were never meant to survive a path tracer.
**Effect lights** replace them, anchored at the origin of the effect that draws
the fire, and inherit both numbers above as `effectLightDerivedIntensity` and
`effectLightDerivedRadius`. Run in game 2026-08-07. Design:
`dusklight-ao/docs/effect-lights.md`; overlay surface and status:
`DusklightOverlay.md` §6.

**HD texture packs: tested good 2026-08-06**, first try. The pack reaches Remix
without its bytes entering D3D9, so texture tagging is unchanged. One known
characteristic: a long first-launch warm-up (§12.1 below). Design and failure
modes live in `aurora-ao/docs/dx9/texture-replacements.md`.

**Still untested, as of 2026-08-04:** the ambient grade — which should stay
untested until the defect below is fixed, because grading on a wrongly-lit sky
is tuning against a moving target — and everything built since 2026-07-29 and
never run: the `skyFogMode` treatments below and the painted moon (§13.1). The
2026-08-04/05 material work (two-colour ramps, per-draw vertex colour, and the
self-illumination rule) is likewise CI-green and unrun; it is tracked in
`aurora-ao/docs/dx9/remix-material-interface.md` §9–§10, not here.

`disableFrustumCulling` **is** tested: it works and it visibly helps with
light leakage.

### 12.1 The HD texture pack's first-launch warm-up

**Observed 2026-08-06:** with a pack installed, the first launch spends a long
period at poor performance before the replacements appear; every later launch
has them essentially immediately. Not a defect — but it is a real cost, and it
is worth knowing it is expected rather than rediscovering it.

**What is verified in source:** Remix keeps **no on-disk cache of loaded
textures.** `AssetDataManager::findAsset` opens the `.dds` with `std::fopen`
for the header and `CreateFileMapping`/`MapViewOfFile` for the data, every
launch (`rtx_asset_data_manager.cpp:200,294-305`). The only in-memory dedupe is
`m_assetHashToTextures` (`rtx_texture_manager.cpp:1435-1448`), which does not
survive the process. So nothing in the runtime is warm on launch 2 that was
cold on launch 1.

**But that does not make the OS file cache the cause, and an earlier revision of
this section said it did.** Correction, same day: reasoning from "no *texture*
cache" to "no durable cache" skipped one. **DXVK writes a pipeline state cache
to disk** (`<exe>.dxvk-cache`, `dxvk_state_cache.cpp:1120-1128`, with
`dxvk.enableStateCache` defaulting true at `dxvk_options.cpp:29`), and this
runtime's own options describe the effect: a significant performance impact
"whenever shaders are uncached (e.g. on first load)" (`rtx_options.h:397`).
Every first launch is slow for that reason, pack or no pack.

Two candidates, then, and only one of them survives a reboot:

| Contributor | Cached where | Survives a reboot? |
| :-- | :-- | :-- |
| Pipeline/shader compilation | `<exe>.dxvk-cache` | **Yes** |
| The pack's `.dds` reads | nowhere durable; OS page cache only | **No** |

**Which dominates is unmeasured**, and the texture side is partly a symptom
rather than a cause: creation is budgeted per frame, so slow frames from *any*
source stretch how long the pack takes to finish arriving. Reboot and relaunch
to separate them — that clears the page cache and keeps `.dxvk-cache`.

**Our own contribution, and it is real:** the game creates materials at
`kTexRepCreationsPerFrame = 16` per frame (`dusklight-ao/src/dusk/remix_bridge.cpp`),
and each `remixapi_CreateMaterial` reads the DDS header **synchronously on the
CS thread** — the `// async load` comment above it notwithstanding, both
`preloadTextureAsset` branches pass `async=false`. Sixteen cold-cache file opens
per frame is a per-frame stall for as many frames as the pack has entries / 16.

**How to confirm it rather than argue about it:** the game logs
`texrep: N replacement(s) selected by the registry` when it starts and
`texrep: N material(s) created, M skipped` when it finishes. The wall-clock gap
between those two lines, compared between a cold first launch and a warm second
one, measures exactly this. No one has to describe how it felt.

**If it needs fixing** — and only if the reboot test says texture I/O is the
dominant term — the cheap change is a time budget instead of a count (bounds the
per-frame stall rather than the per-frame count), and the better one is
prefetching the pack on a worker thread so the CS thread never waits on cold
reads. Neither was done as part of the tested 2026-08-06 change.
`aurora-ao/docs/dx9/texture-replacements.md` §9.

### The live defect: the medium dims the generated sky — CLOSED 2026-08-13

**Exempt was run in game and confirmed good.** The section below is kept as the
record of the defect and of how it was found, because §14.8's general lesson —
a guard in one path is not a guarantee across the system — outlives it. Both
other modes remain: Off as the A/B baseline, Weighted as a taste control for
genuinely foggy weather. Neither is a candidate any more.

Found 2026-07-29. Frozen noon with `physicalSky` on, and worse in Lake Hylia
morning fog with it off: the visible sky reads dim and dingy, and there is a
seam where distant terrain is convincingly blue while the sky just above the
silhouette is duller. Lowering `densityScale` improves it markedly.

**Verified cause — the two halves of our fog disagree about the sky:**

| Half | What it does to a sky pixel |
| :-- | :-- |
| Far ramp, `applyFog` (`composite.comp.slang:625`) | **Exempts it.** `if (primaryMiss) return;` — the comment there says running the ramp on the sky "would drive it to full fog and replace the sky with a flat colour" |
| Volumetric half, `applySkyContribution` (`:585`) | **Fogs it.** `domeLightArgs.radiance * sampleDomeLightTexture(...) * volumeAttenuation` over the *whole* froxel grid, with the in-scatter already in `radianceOutput` |

A sky pixel therefore comes out as *(in-scatter over the full grid)* + *(dome ×
transmittance over the full grid)*. The far ramp was carefully taught not to do
this; the volumetric path does it anyway by another route.

**Why it hurts us more than stock Remix:** §14.2. Our medium is deliberately far
denser than air, so `exp(-σ · gridDepth)` is large — and all of it lands on the
sky. Remix's near-clear default medium would barely show it.

**Why the seam looks the way it does:** distant terrain fades toward the far
ramp's colour, which per C3 samples the dome in the view direction — the right
colour. The sky beside it is attenuated dome plus `fog_col`-tinted in-scatter. Two
descriptions of one day, which is exactly what §0 exists to prevent, arriving
through the one path §0 did not cover.

**Not fixable by tagging *this*** — but read §14.9 before repeating the wider
claim, which was wrong. Category flags apply to *instances*; the generated sky
is a dome light sampled on ray miss, so nothing here can be categorised and the
exemption still has to happen in the composite. What was wrong was the reason
recorded for ruling tagging out **everywhere else**.

**Fix BUILT 2026-07-29, UNTESTED — and deliberately built twice,
`rtx.dusklight.atmosphere.skyFogMode`.** Both candidate treatments are built and
mutually exclusive, so the choice can be made by looking rather than by
argument. One of them is meant to be **deleted** once it has been.

*(This heading said "FIXED" until 2026-08-03. It was never run in game. Built,
CI-green and fixed are three different claims and this project has been bitten
by conflating them — see `CLAUDE.md` §"How this project works" rule 5.)*

| Mode | What it does | What it costs |
| :-- | :-- | :-- |
| 0 Off | The untreated behaviour | Nothing — it is the defect, kept as the A/B baseline |
| 1 **Exempt** (default) | Sky ignores the medium entirely, which is what the original did — it drew its dome with fog switched off at any density | Light shafts that would have been visible **against the sky**, since those are the same in-scatter |
| 2 Weighted | Sky picks up `skyFogAmount` (0.15) of the medium | Nothing structural; needs one number tuned |

**Where the fix had to go, and why not where it looked like it should.** The
obvious site is `applySkyContribution`, since that is where the dome is
multiplied by `volumeAttenuation`. That would be half a fix: the sky is dimmed
by the transmittance *and* tinted by the in-scatter that was added to
`radianceOutput` before it. Treating only the first leaves the sky the right
brightness and still the wrong colour. Both are settled immediately after
`integrateVolumetricNEE`, before either is used, so they cannot disagree.

Touching `volumeAttenuation` on a miss is safe: its only other consumer is
`remodulatedTotalPrimaryRadiance`, which is what the primary ray hit, and on a
miss it hit nothing.

Settled by testing: `celestialNoonElevation` at **80** — the owner's choice,
deliberately short of 90 because the azimuth flips instantaneously at exactly
90.

**Test Freeze Time before testing Phase C.** The blend is driven by sun
elevation, so a Phase C A/B needs the sun held in one place — otherwise part of
the difference between the two shots is the clock. Freeze at **180 (noon)**,
where the sun is well above the 28° full-strength threshold, and A/B
`physicalMaxWeight` 0 ↔ 1. The step-by-step version, including the failure
table, is in `dusklight-ao/docs/remix-test-playbook.md`.

### What landed

| Piece | Where |
| :-- | :-- |
| A1 fog push + fog-state override | `rtx_dusklight_env.h` (new keys), `rtx_scene_manager.cpp` (`applyFogOverride`), game `remix_bridge.cpp` |
| A2 medium derivation | `rtx_dusklight_atmosphere.{h,cpp}`, hooked at `rtx_global_volumetrics.cpp` |
| A3 range split | `dusklight_composite_args.h`, `composite.comp.slang` `applyFog`, `rtx_composite.cpp` |
| A4 live grid extent | `volume_args.h` (`previousFroxelMaxDistance`), `froxel.slangh`, `rtx_global_volumetrics.{h,cpp}` |
| B1 generated sky | `dusklight_sky.{h,comp.slang}`, `DxvkDusklightAtmosphere::prepareSceneData` |
| B2 dome suppression | game `d_a_vrbox.cpp`, `d_a_vrbox2.cpp`, `settings.{h,cpp}` |

Bridge protocol went **1 → 2** for this work. A game build older than the
fork's `kRequiredProtocol` shows the "game build is older than this Remix
build" notice in the Dusklight tab rather than silently doing nothing.
**Protocol has since advanced to 6** (3 = overlay + warp, 4 = the clock,
5 = per-blade grass, 6 = the Controls tab), so that number is the historical one
for phases A/B, not the current requirement.

### To turn it on

```
rtx.dusklight.atmosphere.enable = True
rtx.volumetrics.enable = True          # the atmosphere drives it; leaving this False keeps the old depth-only path

# Sky (all three together, or you will be looking at more than one sky)
rtx.dusklight.atmosphere.skyEnable = True
rtx.dusklight.game.hideVrbox = True
rtx.skyAutoDetect = None
```

Everything defaults off, so a build with none of these set behaves as upstream.

The full recommended configuration — including the game-side options and the
values settled by testing — is in `dusklight-ao/docs/dx9-fixed-function.md`.

---

## 14. Facts that were expensive to learn

Everything here was either wrong in an earlier draft of this document or cost a
CI round, an evening, or a wrong conclusion. None of it is discoverable by
reading the API.

### 14.1 Remix's lighting model

- **There is no dome light type.** `light_types.h` has Sphere = 0, Rect = 1,
  Distant = 4 and no dome. A sky is therefore **never NEE-sampled** — it
  contributes only through ray miss. This is the single most consequential fact
  about the whole sky design and it is invisible from the API surface.
- **Distant light irradiance is independent of angular diameter.**
  `distant_light.slangh` sets `lightSample.radiance = radiance / sin²(halfAngle)`,
  so irradiance ≈ π × radiance regardless of how wide you make the sun. Widening
  `celestialAngle` softens shadows and changes *nothing* about brightness.
- **Remix's `direction` is the direction light travels**, not the direction to
  the body: `distant_light.slangh:78` samples at `position - direction·100000`.
  Our `-toBody` is correct.
- **Scene scale.** Remix's world unit is 1 cm;
  `getMeterToWorldUnitScale() = 100 × sceneScale`. TP's units match closely
  enough to use directly — Link is about 170 units tall.

### 14.2 The atmosphere/fog relationship — the error at the centre of the first draft

The original §1 claimed the atmosphere medium and the fog medium should be **one
object with shared coefficients**. That was wrong, and implementing it literally
would have deleted the fog.

**Measured: real Rayleigh extinction over 100 m is 0.13%.** Air is transparent
at the scale TP works at. The game's fog is an artistic device that reaches full
opacity in tens of metres; nothing physical does that.

The redesign (C3): **density stays the game's, colour is shared.** The far fog
samples the generated dome for its colour, so the two agree on hue without the
atmosphere dictating how thick anything is.

Corollary worth holding onto: *any* future "just make it physical" instinct
about the fog needs this number checked first.

### 14.3 The sky brightness bug — and the general shape of it

`skyIntensity` at 1.0 read dim in game. The anchor (a real midday sky sits near
a fifth of its sun) was right; the arithmetic forgot that **the palette colours
are sRGB-decoded before being scaled**, which takes a mid blue from 0.5 to about
0.2. Six times larger was needed. Now 6.0.

The general form: **the palette is authored in sRGB and every consumer of it
decodes first.** Any anchor computed against the raw 0–255 or 0–1 values will be
off by roughly the gamma curve at that value.

### 14.4 Coordinate conventions

`cartesianDirectionToLatLongSphere` uses `theta = acos(direction.z)` — a **+Z
pole**. The world is **Y-up**. The swap lives in the dome light's `worldToLight`
matrix and nowhere else:

```cpp
const Matrix4 kWorldToDomeLight {
  1.0f, 0.0f, 0.0f, 0.0f,
  0.0f, 0.0f, 1.0f, 0.0f,
  0.0f, 1.0f, 0.0f, 0.0f,
  0.0f, 0.0f, 0.0f, 1.0f,
};
```

If the sky ever appears rotated 90° about the horizon, this is the first and
almost certainly only place to look.

### 14.5 Shader/host type conversions

The shader-side `vec3` **converts from `Vector3` but not back**. Assigning a
`vec3` expression into a `Vector3` fails to compile, and it failed twice in the
same CI round before being fixed with an explicitly typed local. Watch for it
anywhere host code reads back something a shader header declared.

Slang compute shaders are auto-discovered by `compile_shaders.py` via
`os.walk`, so a new `.comp.slang` needs no build-system edit — but it **also
means a shader with a missing `#include` only fails at CI**, not locally. Our
case: `math.slangh` omitted from `dusklight_sky.comp.slang` gave
`undefined identifier 'pi' / 'twoPi' / 'square'`.

### 14.6 Frame-latching a per-frame derivation

The medium is resolved once per frame and cached against the frame id, so
consumers can call in any order without re-deriving or racing:

```cpp
const uint32_t frameId = m_device->getCurrentFrameId();
if (m_resolvedFrame == frameId) { return; }
m_resolvedFrame = frameId;
m_derived = resolve();
```

Any new consumer should call the same accessor rather than deriving its own.

### 14.8 The sky is fogged by two paths, and only one of them knows it

Found 2026-07-29, written up in §12. The short form, because it is the kind of
thing that will be rediscovered otherwise:

**`applyFog` exempting `primaryMiss` is not the same as "the sky is not fogged".**
The composite reaches the sky twice. `applyFog` skips it deliberately and says so
in a comment. `applySkyContribution` (`composite.comp.slang:585`) multiplies the
dome by `volumeAttenuation` — the froxel transmittance over the full grid — and
by then the froxel in-scatter is already sitting in `radianceOutput`. Reading the
first and concluding the sky is exempt is wrong, and the comment at the exemption
site actively encourages that misreading.

Two general lessons in it:

1. **A guard in one path is not a guarantee across the system.** The consistency
   §0 promises is only structural where every consumer goes through one
   derivation. The fog does not: it has a near half and a far half, and they were
   given different rules about infinity.
2. **Density and visibility of the sky are coupled here in a way they are not
   upstream.** Because our σ is an artistic quantity rather than air (§14.2), any
   Remix code that attenuates something by the full grid depth behaves very
   differently for us than it does for stock. Worth checking the same question
   anywhere else `volumeAttenuation` is applied to a distant or infinite source.

### 14.11 The volumetric fog cannot see the sky, and never could

Found 2026-08-13, and it explains a complaint nobody had connected to a
mechanism: outdoor fog reading as a flat grey veil in shadow rather than as air.

**Remix's froxel grid is lit by next event estimation over the RTXDI light list.
That list holds five types — sphere, rect, disk, cylinder, distant
(`shaders/rtx/concept/light/light_types.h`, `lightTypeCount = 5`). A dome light
is not one of them.** It lives in `domeLightArgs` and is sampled by
`sampleDomeLightTexture`, which appears in the geometry resolver, the indirect
integrator and the composite — and in none of the volumetric code.

So the fog is lit by the sun and by analytic lights and by nothing else.
Everything it showed in shadow came from `multiScatteringEstimate`, a flat
constant. The fidelity table in §9 already said "Remix has no dome light type"
for interiors; what nobody drew out is that it is equally true **outdoors**,
where there very much is a sky and the fog still cannot see it.

Two lessons:

1. **"The sky lights the scene" and "the sky lights the fog" are separate
   claims here.** The first is true through ray miss on indirect bounces; the
   second is false, and no amount of dome-light tuning changes it.
2. **The general shape: a system fed by one list is blind to anything not in
   that list, however visible it is elsewhere in the frame.** Worth checking
   the same question for any other consumer of `RAB_GetLightRange`.

The fix is §5.3 — measure the dome and hand the fog its average — rather than
adding a light type, which would mean changing RTXDI.

### 14.9 "No texture, therefore untaggable" was wrong — there are three routes, not one

Corrected 2026-07-29, after an NVIDIA engineer working on upstream Remix pointed
at `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY`. Verified in this fork.

This document, `kankyo-remix.md`, and `dx9-fixed-function.md` all carried the
same reasoning: *the vrbox is painted with vertex colours, so there is no
texture, so Remix can never hash it, so it can never be categorised as Sky.*
The first three clauses are true. **The conclusion does not follow**, because
texture hashing is only one of three ways a category is assigned:

| Route | Mechanism | Needs a texture? |
| :-- | :-- | :-- |
| `rtx.skyBoxTextures` | texture hash on a captured draw (`rtx_types.cpp:409`) | **yes** — the only one that does |
| `rtx.skyBoxGeometries` | **geometry/asset hash** on a captured draw, via `geometryAssetHashRule` (`rtx_types.cpp:416`, option at `rtx_options.h:191`) | no |
| `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY` | declared outright on geometry submitted through the API (`remix_c.h:457` → `rtx_remix_api.cpp:647`, on `remixapi_InstanceInfo.categoryFlags` for `DrawInstance`) | no |

So the game's own untextured sky dome **can** be tagged — by geometry hash,
today, with a config line and no code — and any geometry we submit ourselves can
simply declare the category. An instance tagged Sky also gets `CameraType::Sky`
(`rtx_remix_api.cpp:637`) and is excluded from visibility rays, which is the
same property that makes the painted moon safe.

**What this does and does not change.**

- It does **not** invalidate the dome light (B1) — but the reason given for B1
  in the first revision of this section was **also wrong**, and it is corrected
  in §14.10 below. The generated dome is not an instance, so no category flag
  reaches it, and the §12 composite fix is still the fix for the fog defect.
- It does mean **the reason we stopped considering tagging was wrong**, and any
  future "we can't tag that, it has no texture" should be checked against this
  table first.
- §8.5's other objection — that the sky probe is rasterized in the game's own
  8-bit format and clamped — is weaker than recorded too: `rtx.skyForceHDR`
  (`rtx_sky.h:156`) forces `B10G11R11_UFLOAT` for exactly that reason.

**Also surfaced while checking:** `rtx.fogIgnoreSky` (default false) makes fog
capture skip sky-categorised draws. That is about *which draw's fog state wins
the frame* — the old §2.5 lottery — not about exempting sky pixels from fog, so
it is not a second fix for §12. Worth knowing now that sky draws can actually be
categorised.

The general lesson, and it is the same one as §14.7: a true premise chained to
a plausible inference is still not a verified conclusion. "There is no texture"
was checked. "Therefore it cannot be categorised" never was.

### 13.1 The moon, painted into the dome

Built 2026-07-29, `rtx.dusklight.atmosphere.skyMoonEnable`, default **on**.
Untested.

`hideSkyBillboards` is confirmed as the fix for the wandering night shadows, and
it takes the visible moon with it. This gives the moon back without giving the
billboard back: a disc painted into the generated sky is correctly placed, moves
with the sky rather than with the camera, and cannot cast a shadow because it is
not geometry.

| Choice | Value | Why |
| :-- | :-- | :-- |
| Size | **5.7°** (`skyMoonAngularDiameterDegrees`) | The game's own — an 8000-unit quad at an 80000 orbit radius. Eleven times the real moon, and what a player of this game is used to |
| Brightness | 4.0 (`skyMoonIntensity`), applied **after** the sky's intensity | Absolute rather than a multiple of the palette, so it does not swing with the weather. Appearance only — the moonlight is the distant light |
| Edge | 0.15 of the radius (`skyMoonEdgeSoftness`) | A hard circle aliases badly in a lat-long map, whose angular sampling rate varies with latitude |

Three implementation points worth keeping:

1. **Only the moon.** The sun stays out of this image deliberately — it is
   analytic and NEE-sampled, and baking something that bright into an image only
   ever reached by ray miss would double count it and sample it terribly (§8.5).
2. **It reuses the pushed celestial direction.** The game sends one direction —
   whichever body is driving the light — so `sunAzimuth`/`sunElevation` *is* the
   moon's direction while `sunIsDay` is false, and no second pair was needed.
3. **Faded by `sunFade`, not by an elevation threshold.** That value already
   falls to zero across the dawn and dusk handovers, which is exactly where the
   pushed direction stops meaning the moon. Keying off elevation instead would
   have snapped it out while it was still on screen.

The sky image is dispatched unconditionally every frame (only the two LUTs are
staleness-gated), which is what lets the moon move and fade at all — worth
knowing before anyone adds a cache there.

### 14.10 The stated advantages of the dome light over the sky probe were both false

Corrected 2026-07-29, immediately after §14.9, and by the same owner question.
§14.9 said the dome light was still right "for HDR sky radiance feeding GI,
which a rasterized probe does not give". Both halves of that are wrong.

**The sky probe feeds GI exactly as the dome light does.**
`integrator_indirect.slangh:371-379` — the *indirect* integrator — is a plain
if/else, and both branches add to `emissiveRadiance` on ray miss:

```hlsl
if (cb.domeLightArgs.active)  skyRadiance = domeLightArgs.radiance * sampleDomeLightTexture(...);
else                          skyRadiance = cb.skyBrightness * SkyProbe.SampleLevel(...);
emissiveRadiance += skyRadiance * radianceAttenuation;
```

There is no GI difference. There never was one; the claim was inferred from
"there is no dome light *type* in `light_types.h`, so a sky is never
NEE-sampled" (§14.1, which is true) and then wrongly extended into "so the probe
does not light the scene". Ray miss *is* how both of them light the scene.

**The 8-bit clamp is inherited, not intrinsic.** `rtx_sky.h:152` takes the sky
render target's format from *the game's own bound render target*, which is why
a game with an LDR backbuffer gets an LDR sky. Two things follow: `skyForceHDR`
overrides it outright (`:156`, forcing `B10G11R11_UFLOAT`), and a sky whose
content we supply is not bound by whatever format the game happened to be
rendering into.

**And sky geometry may not be rasterized at all.** `rtx_sky.h:165-192` routes
sky-categorised draws two ways: rasterized into the cubemap, or pushed to
`m_delayedRayTracedSky` and reprojected from sky camera space into the main
camera's. Which one depends on `rtx.skyReprojectToMainCameraSpace` (default
**false**, so today it rasterizes) and on whether the draw is a skybox quad.

**What actually remains in the dome light's favour**, stated honestly because
the previous version of this list was invented rather than measured:

- It is **tested and working**, which the alternative is not.
- It is infinitely far and non-occluding *by construction* rather than by
  category — there is no draw to accidentally intersect anything.
- It is one texture we already generate, with no second geometry path to keep
  alive.

Those are real but they are a different argument, and none of them says the
sky-categorised route would look worse. That is now an open question to settle
by looking, which is what the toggles being added exist for.

**`rtx.fogIgnoreSky` is inert for us.** It sets a sky draw's `fogState.mode` to
`D3DFOG_NONE` so sky draws are skipped when Remix picks the frame's fog values
(`d3d9_rtx.cpp:733`). We do not use captured fog state at all while the
atmosphere is on — `applyFogOverride` (`rtx_scene_manager.cpp:2073`) replaces it
wholesale. So it is dead code on this path, in the same way
`rtx.volumetrics.enableFogRemap` is, and setting it will look like it does
nothing because it does.

### 14.7 Don't trust a recon report you did not verify

A reconnaissance pass claimed the kytag01 fog "passes `start > end`
deliberately". It does not — it is `start < end` with a **negative start**.
Acting on that claim would have made the tag silently report no fog, in exactly
the regime that is one of the two remaining validation targets.

**The same recon pass also put the tag in the wrong place**, and that half went
uncaught for two more weeks. It said Lake Hylia; the actor is the Lost Woods /
Sacred Grove mist tag, which `d_a_kytag01.cpp` states twice — once in English at
`:1-4` and once in the original team's own Japanese at `:202`. Corrected
2026-08-11 across **twelve** passages in two repos — §3, §5.1, §8.1, §8.4, §10,
§13 (twice) and this section here; `kankyo-fog.md` §3.3, §5, §6 and §7 there.
The audit that caught it listed five; the other seven turned up only by grepping
`Hylia` across both repos rather than working from that list, which is the
re-derive-from-the-diff rule applied to a correction instead of a merge. The area
name had been carried forward from document to document without anyone opening
the file — which is this section's lesson a second time, at lower stakes and
longer duration.

Verify structural claims against the source before building a special case
around them. **And verify the incidental nouns too** — a wrong mechanism gets
caught by the code not working, whereas a wrong place name is invisible until
someone is sent there.

---

## 13. Phase 0 — the A/B calibration, and how to re-run it

Run 2026-07-28 and reported as clearly better than the pre-atmosphere build.
Recorded here in unambiguous form, because the instructions given during
development changed as the code landed and the older version is now wrong: the
`rtx.volumetrics.enableFogRemap` family it used is **bypassed entirely** when
the atmosphere is on, so setting those tests nothing.

The comparison is one switch. Everything else stays put.

### Set once, for both sides of the test

```
# The measurement itself
rtx.volumetrics.enable            = True
rtx.dusklight.game.bridgeEnable   = True

# Sky (all three together, or you see more than one sky)
rtx.dusklight.atmosphere.skyEnable = True
rtx.dusklight.game.hideVrbox       = True
rtx.skyAutoDetect                  = None

# Hold exposure still, or every brightness difference is partly undone
# before you can see it
rtx.autoExposure.enabled = False
```

### Must be OFF for a clean read

```
# Double-counts against real sky fill now that the dome lights the scene.
# This is the one that will mislead you if left on.
rtx.dusklight.grade.enable = False

# Remix's own fog remap. Not merely redundant - it is dead code while the
# atmosphere is on, so a value here is a false lead.
rtx.volumetrics.enableFogRemap      = False
rtx.volumetrics.enableFogColorRemap = False

# The legacy depth-fog path. Superseded; it is skipped whenever volumetrics
# are enabled, so these only matter if you turn volumetrics off.
# rtx.maxFogDistance / rtx.fogColorScale - leave as they are.
```

### The A/B

Flip **one** option and compare:

```
rtx.dusklight.atmosphere.enable = False   # A: Remix's own handling
rtx.dusklight.atmosphere.enable = True    # B: the derived medium
```

Both are reachable without a rebuild, which is what makes this a better
experiment than the original pre-implementation Phase 0.

### Reading the result

- A **uniform** error — thick or thin everywhere, bright or dim everywhere —
  is a constant. Reach for `densityScale` first (one number, whole scene),
  then `skyIntensity`, then `zHalfMin`.
- A **per-area** error — right in the field, wrong in the mines — is the
  mapping, and that is a real bug worth reporting rather than tuning around.

### What the pass found

`skyIntensity` at 1.0 read dim. The anchor was right — a real midday sky sits
near a fifth of its sun — but the arithmetic behind it forgot that the palette
colours are decoded out of gamma before being scaled, which takes a mid blue
from 0.5 to about 0.2. The multiplier needed to be about six times larger to
land the same ratio. Now 6.0.

Everything else read correct on a second pass, and two observations are worth
recording because of what they each rule out:

**"Distance scaling is perfect; very far geometry almost entirely blends into
the fog."** Range is solved — §2.1's 20 metre grid is gone. The "almost" is the
right answer rather than a shortfall: the original only reaches full opacity
exactly at `fog_end_z`, so geometry short of that should be partially fogged.
The failure signatures to watch for instead are distant terrain looking pasted
on, or fog visibly *stopping* partway towards it.

**"Fog is noticeably scaling with different areas."** The most informative
sentence of the pass, because it confirms three things at once:

- the fog-state lottery fix (§2.5) is working. Before it, a frame's fog was
  whichever draw arrived first, so per-area variation could not have been
  stable;
- the σ mapping generalises across regimes, which was the central design bet —
  one scale-free expression, no per-area code anywhere;
- the froxel grid really is resizing per area (A4).

**The dense end — half covered as of 2026-07-29, and less than that after
2026-08-11.** Lake Hylia in the morning was visited and its fog is *"suitably
intense"*, which is the first real evidence that the σ mapping holds at the
dense end and not just in the thin and mid regimes.

Three cautions against reading it as more than that:

- **It was very likely not the scripted regime at all.** This bullet used to say
  "Lake Hylia's kytag01"; the kytag01 mist tag is the **Lost Woods / Sacred
  Grove** (§14.7), and nothing establishes that Lake Hylia carries one — actor
  placement is `.dzs` stage data, so it cannot be settled from source either
  way. What that visit measured was most likely Lake Hylia's **palette** fog,
  which is a genuinely dense regime and genuinely good news for the mapping, but
  it is not the `-2000`/`200` override.
- **So it does not isolate `zHalfMin`.** That clamp only bites where the
  half-density point `(start + end)/2` falls behind the camera. For the mist tag
  that is `(-2000 + 200)/2 = -900`, and the clamp fires hard. **This is done by
  the near `end`, not by the negative `start`** — negative starts are ordinary
  (`start` was zero or negative in *every* area measured on 2026-08-06), so the
  sign of `start` discriminates nothing.

  Which is settled empirically rather than by argument: Lake Hylia's own ramp
  was **measured** at `[-3000, 70000]` on 2026-08-06, a half-density point of
  ~33500 — three orders of magnitude clear of `zHalfMin`, and nothing like
  `-2000`/`200`. So the visit demonstrably did not exercise the clamp, and that
  measurement is also the nearest thing to positive evidence that Lake Hylia
  does not show the kytag01 whiteout — which `.dzs` placement cannot settle
  either way. (That measurement table lands in §5.1 with the volumetric-shell
  work, which was still unmerged when this was written; if §5.1 has no ramp
  table, it has not arrived yet.) "The dense case looks right" and "the clamp is correct" were
  already different claims with evidence for only the first; the gap between
  them is now wider, not narrower.
- **The Goron Mines are still unvisited**, and they are the other regime — near
  and dense rather than scripted and dense.

`froxelRangeScale` (0.6) remains unchallenged rather than validated. Ledger row
C0 is unchanged by this and stays open; what changed is *where to go* to close
it — `kankyo-fog.md` §5 now carries the walk-out procedure, which is the
opposite of the intuitive one because the tag sits in a clear centre.

**And the Lake Hylia visit surfaced something bigger than fog density:** it is
where the sky/fog defect in §12 shows worst, because it is the densest medium
**yet observed** — the mist tag at full weight is denser still, and has not been
seen. Any future dense-fog reading is partly measuring that defect until it is
fixed.

### The measurement pass is still worth doing

The Dusklight tab reports the game's live `fogStartZ` / `fogEndZ` / `fogColor`
and sky colours. Those values live in stage data rather than in source, so this
readout is the only way to see what a given area actually asks for. Recording
them at Hyrule Field, Faron, the **Lost Woods / Sacred Grove** (`F_SP117`, for
the kytag01 whiteout — the one regime that challenges `zHalfMin`), Lake Hylia,
the Goron Mines and the Forest Temple turns any future tuning from guesswork
into arithmetic. `dusklight-ao/docs/kankyo-fog.md` §5 has the per-area
procedure; the mist tag needs a **walk-out from the clear centre**, not a walk
into the fog.
