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
│    scene:  colpat pattern, indoor/outdoor, moya mode/count      │
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
pulses (`d_kankyo_rain.cpp:362`), the `dKy_fog_startendz_set` override that the
Lake Hylia tag drives, and the *second* "gather" colpat blend
(`mColPatBlendGather`). Reading the outputs means every one of those comes
along for free and stays correct when the game changes. Re-deriving from the
palette tables would mean reimplementing all six layers and keeping them in
sync forever.

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
| `styleTerm` | colpat pattern | Pattern 9 = Palace of Twilight → 0. Weather patterns → low. See §8.6. |

At `physicalWeight = 0` the system is a faithful reproduction of the vanilla
gradient and vanilla fog. At `1` it is Hillaire driven by palette-derived
parameters. Every intermediate value blends the **radiance function**, not the
output pixels — so GI, visible sky and aerial perspective stay mutually
consistent at every setting.

---

## 5. Fog: mapping TP's linear ramp onto a medium

### 5.1 The mapping

TP's fog is `f(z) = saturate((z - start) / (end - start))`. We need an
extinction coefficient σ for a Beer-Lambert medium.

**The two functions cannot be made equal, and pretending otherwise was the
defect.** The game's ramp is *flat zero* out to `start` and then linear in
opacity, closing to exactly 1 at `end`. An exponential begins accumulating at
the camera and only asymptotes towards 1. No σ reconciles them; the only
question is where the disagreement is put.

Until 2026-08-06 σ was matched at the **half-density point**
(`σ = ln2 / max((start+end)/2, zHalfMin)`), which put the whole disagreement
where it is most visible. Measured by evaluating both paths from the shipped source - arithmetic
only, no build, no run:

| Ramp | Worst near-field **over**-fog | Opacity at `end` (game: 1.000) |
| :-- | :-- | :-- |
| `start = 0` | +0.04 | 0.81 |
| `start = 0.3 · end` | **+0.27**, at the distance the game shows none | 0.77 |
| `start = 0.5 · end` | **+0.37**, ditto | 0.76 |
| Lake Hylia `-2000 … 200` | −0.79 (far too *thin*) | 0.75 |

Two separate failures are visible in that table. Near geometry was hazed where
the original was clear — the reported symptom. And nothing ever closed to
opaque, so distant terrain never dissolved into the sky the way it does in the
original; §5.2 explains why the range split did not save it.

**The current derivation** solves σ at the one distance where the split between
the two systems is decided — the far edge of the froxel grid — and asks the
medium for only a *fraction* of the game's own opacity there:

```
reach   = froxelMaxDistance
α_ramp  = saturate((reach - start) / (end - start))
target  = min(mediumFraction · α_ramp, 0.95)
σ       = -ln(1 - target) / reach          (then × densityScale, capped at ln2/zHalfMin)
```

Under-running is deliberate and load-bearing: the composite (§5.2) can *add* fog
to a pixel but cannot take it back out, so anything the medium overshoots by is
the one error nothing downstream can correct. `mediumFraction` is the knob for
that trade and it costs no accuracy — the total lands on the game's ramp either
way. What it buys, worst-case over-fog inside the dead zone:

| Ramp | `f=0` | `f=0.25` | `f=0.5` (default) | `f=0.75` | `f=1` |
| :-- | :-- | :-- | :-- | :-- | :-- |
| `start = 0` | 0.000 | 0.000 | 0.000 | 0.000 | 0.068 |
| `start = 0.3 · end` | 0.000 | 0.055 | 0.114 | 0.176 | 0.244 |
| `start = 0.5 · end` | 0.000 | 0.042 | 0.084 | 0.127 | 0.170 |

Still scale-free and still with no per-area handling. Lake Hylia's negative
`start` (`dusklight-ao/src/d/actor/d_a_kytag01.cpp:94`) means "already ~90%
fogged at z=0"; `α_ramp` is then near 1, the medium goes as dense as `zHalfMin`
allows, and the composite supplies the rest — which is the first time that
scripted whiteout has actually reached the screen. (An earlier revision of this
section said `start > end`; that was a recon claim nobody verified, and §14.7
records why it mattered.)

**This replaces `rtx.volumetrics.enableFogRemap` entirely for us.** We do not
enable Remix's remap; we write the derived coefficients straight into
`VolumeArgs`. See §6.1 for why that direction matters.

### 5.2 The correction, and the range split it replaced

**What this used to be.** The composite owned everything past
`froxelMaxDistance` and the medium owned everything inside it — one owner per
unit of distance. It had two faults. It could only ever *add* fog, so the near
field, which is exactly where a homogeneous medium is most wrong, had no
correction available to it at all. And the far half was scaled by
`t *= mean(volumeAttenuation)` to keep it from double-counting the near volume,
which silently broke the invariant the code above it claimed: `t` no longer
reached 1 at `end`, so the fog never closed. That is the second column of the
table in §5.1.

**What it is now.** `applyFog` already receives `volumeAttenuation` — the
transmittance the medium actually produced for that pixel. That is enough to
ask for the residual rather than a continuation:

```hlsl
alphaGame = saturate((viewDistance - rampStart) / (rampEnd - rampStart));
T         = mean(volumeAttenuation);
t         = saturate((alphaGame - (1 - T)) / max(T, 1e-4));
radianceOutput = lerp(radianceOutput, fogRadiance, t);
```

`radianceOutput` at that point is `surface·T + inscatter`, so the blend leaves
the surface weighted by `(1-t)·T`. Setting that equal to `1 - alphaGame` gives
the `t` above, and the fog term then lands at exactly `alphaGame`. **The total
opacity is the game's ramp at every distance, with no handover to seam.**

It also disposes of the in-scatter problem without separating in-scatter out. A
medium whose own in-scatter is a flat `α·fogColour` comes out at exactly
`alphaGame·fogColour`; any *excess* over that — a shaft, a lit patch of air —
survives scaled by the same weight rather than being flattened. That is the
whole reason to run volumetrics under a fog the game authored as a flat colour.

Two second-order notes, both verified by reading rather than assumed:

- Using the *sampled* transmittance rather than a modelled `exp(-σd)` is what
  makes the correction self-adjusting. Outdoors Remix runs the medium inside a
  planet-atmosphere shell and indoors it clamps the path at
  `maxAttenuationDistanceForNoAtmosphere`; both only ever make `T` larger, and
  the residual absorbs the difference.
- `mean(volumeAttenuation)` is exact here rather than an approximation, because
  our `attenuationCoefficient` is deliberately scalar (`Vector3(σ, σ, σ)`) — the
  colour arrives through scattering, never through extinction.

The froxel integrator itself is **not in this repo**: `integrateVolumetricNEE`
and friends live in `submodules/rtxdi/rtxdi-sdk/include/volumetrics/rtx/`, which
is why grepping for them here finds only call sites. Nothing above needs to
change it.

### 5.3 The colour the medium settles at

The medium and the correction have to aim at the same colour or the fix in §5.2
lands the *opacity* correctly and the *colour* wrong.

An unlit stretch of homogeneous medium integrates the constant ambient term to
`albedo · multiScatteringEstimate · (1 - T)`. Injecting the fog colour raw
therefore settles the medium at `albedo ×` it — and with the `0.25` multiplier
this option carried before 2026-08-06, at `0.9 × 0.25 = 0.225` of it. The
composite's half meanwhile blends towards the authored colour at full strength.
**The two halves of one fog were aiming at colours a factor of 4.4 apart**, which
is a colour seam wherever real lighting was not filling the difference in — an
unlit interior most of all, which is where this game puts its most strongly
tinted fog.

So the estimate is divided through by the albedo on the way in:

```
multiScatteringEstimate = fogRadiance · multiScatteringScale / singleScatteringAlbedo
```

At the default `multiScatteringScale = 1.0` the medium's own settled colour is
exactly the authored one, the two halves agree, and light that genuinely reaches
the medium adds on top — which is what makes a shaft a shaft.

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
| Palette blend (fog + sky + ambient) | **game**, once per frame | everything | Reading outputs also inherits addcol, ratio, override and gather layers for free (§3) |
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

`mMoyaMode`/`mMoyaCount` drive *billboard* haze particles, separate from GX
fog: mode 3 is the Lake Hylia fog (`d_a_kytag01.cpp`), 4 is kytag02, 10/11 are
weather (`d_a_kytag06.cpp`), 1 is cutscene. Under a path tracer these become
camera-facing quads. Keeping them *and* a dense medium double-counts the haze.

**Resolution:** suppress the moya billboards (same mechanism as
`hideSkyBillboards`) and fold their contribution into the medium — map
`mMoyaCount` onto `rtx.volumetrics.enableHeterogeneousFog` +
`noiseFieldDensityScale`. Real volumetric swirl instead of quads, which is an
upgrade, and no double-count. Compromise recorded in §9.

### 8.2 The fog-avoid tag (kytag08)

`g_env_light.fog_avoid_tag` tracks a moving position that pushes fog away from
the player. A homogeneous medium cannot express "clear here".

**Resolution:** ignore in Phases A–C. The heterogeneous noise field could carry
it later as a density subtraction around a world position. Recorded in §9.

### 8.3 Per-object fog

Some draws legitimately run `GX_FOG_NONE` (UI, `d_drawlist`, mirrors,
`d_gameover`). Aurora already excludes ortho draws so Remix does not see fog on
UI. But *world* objects can also carry different fog than their room.

**Resolution:** the bridge's global env fog is authoritative, replacing the
first-draw-wins lottery of §2.5. This is strictly better than today but does
flatten genuine per-object variation. Recorded in §9.

### 8.4 Auto-exposure vs dense fog

Dense fog raises mean scene luminance; auto-exposure pulls down; the Lake Hylia
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

### 8.6 Twilight Realm

Less special than it looks: `d_kankyo.cpp:266-270` shows the Palace of Twilight
is simply colpat pattern 9. Same `vrbox_*`, same `fog_col`, same
`fog_start_z/end_z` machinery with different numbers, plus the full-screen
desaturate-and-tint already bridged (`monoAmount` ≈ 0.38).

But it forces one thing to be explicit: **a physical atmosphere has no valid
parameters for the Twilight Realm.** There is no sun; the look is amber over
black with drifting particles. Rayleigh/Mie/ozone cannot produce it at any
setting. `styleTerm` drives `physicalWeight` to 0 there — bypass, not tune.
Because the same weight drives fog colour, the amber fog stays amber
automatically.

---

## 9. Compromise ledger

Recorded so that dissatisfaction with a specific result can be tuned rather
than met by abandoning the physical path. Each row: what is given up, how it
shows, and the first knob to reach for.

| # | Compromise | How it will show | First knob |
| :-- | :-- | :-- | :-- |
| C0 | ~~The calibration pass was never run.~~ **Run 2026-07-28. Phase A/B confirmed good in-game.** One constant was wrong: `skyIntensity` at 1.0 gave a visibly dim sky. The analytic anchor was right but the arithmetic behind it was not — it ignored that the palette colours are decoded out of gamma before they are scaled, which takes a mid blue from 0.5 to about 0.2, so the multiplier needed to be ~6× larger to land the same sky-to-sun ratio. Now 6.0. `zHalfMin` and `froxelRangeScale` were not reported as wrong. | — | — |
| C11 | **A homogeneous medium cannot be clear near the camera.** The game's ramp is exactly zero before `fogStartZ`; an exponential medium starts extinguishing at the camera. **Reduced, not closed, on 2026-08-06.** It used to be the whole error — measured at +0.27 to +0.37 opacity where the original showed none, because σ was matched at the ramp's half-density point and the composite could only add fog past the froxel grid. §5.2's correction now lands the total on the game's ramp everywhere the medium is *thinner* than the ramp, and §5.1 runs the medium deliberately thin so that is almost everywhere. What survives is the dead zone alone: 0.08–0.11 at the default `mediumFraction` of 0.5, and it is the one place the correction cannot reach, because fog already applied to a pixel cannot be taken back out. | Near-field haze in areas the original left crisp, now bounded and confined to distances shorter than `fogStartZ`. Logged: `rtx.dusklight.atmosphere.fogLog` prints `nearHaze=` per derivation - the excess at the ramp's own start distance, which is where it peaks - alongside a `fix=` column per sampled distance. | `rtx.dusklight.atmosphere.mediumFraction`. It scales the residual linearly and costs nothing but shaft strength — at 0 the fog is the game's ramp exactly, with no volumetrics in it. A genuinely heterogeneous density (`σ(d) = 1/(end - d)` reproduces the ramp exactly) would close it outright, but that lives in `submodules/rtxdi`'s `sampleDensityField` and would mean forking a fourth repo. |
| C10 | **Fog is composited in linear HDR, not the game's display space.** The original blended fog over a finished, display-referred image; here both the medium and the correction happen pre-tonemap. | Fog reads with a different contrast curve than vanilla - typically holding its colour longer in the bright end. | `rtx.dusklight.atmosphere.fogRadianceScale`. The structural fix is moving the correction post-tonemap, the same change the bloom needed. |
| C1 | **Dusk saturation.** Physical twilight is more graduated and less saturated than TP's authored dusk. | Sunsets read calmer / less punchy than vanilla. | Lower `physicalWeight`'s `elevationTerm` at low sun; or add a saturation push applied to the *medium's* Rayleigh/Mie tint, not to output pixels. |
| C2 | ~~**Exponential never fully closes.**~~ **Closed 2026-08-06, and it had never actually been fixed before that.** The range split was supposed to close the fog at `fog_end_z` and did not: the far half was scaled by `t *= mean(volumeAttenuation)` to stop it double-counting the near volume, which capped total opacity at 0.75–0.81 instead of 1.0. §5.2's correction reaches 1 by construction. **Arithmetic only — not yet seen in game.** | Was: distant terrain never dissolving into the sky the way vanilla does. | — |
| C3 | **Clouds have no physical analogue.** `kumo_top/bottom/shadow` describe painted cloud bands. | Skies read emptier than vanilla if the vrbox is replaced wholesale. | Keep TP's cloud layer as geometry over our sky (Phase D). |
| C4 | **Weather has no physical analogue.** Clear-sky scattering cannot do "rain grey". | Storms look insufficiently oppressive. | `styleTerm` drops `physicalWeight` on weather colpats; overcast can also be faked with high Mie + suppressed sun. |
| C5 | ~~Moya swirl replaced by noise.~~ **Withdrawn - the problem does not exist on this backend.** `mMoyaCount` feeds `mpCloudPacket->mCount` (`d_kankyo_rain.cpp:1616`, inside `cloud_shadow_move`), and `dKankyo_cloud_Packet::draw` already returns early on D3D9 (`d_kankyo_wether.cpp:119-126`). The haze billboards were never drawn here, so there is nothing to double count and no switch was needed. `moyaMode`/`moyaCount` are still pushed, as a signal of how much haze an area wants folded into the medium. | — | — |
| C6 | **Fog-avoid tag ignored.** (§8.2) | No clear bubble around the player in heavy fog. | Deferred feature, not a tuning knob. |
| C7 | **Per-object fog flattened to one global.** (§8.3) | Objects authored with distinct fog match their room instead. | Could be restored per-instance later; costs a per-instance field. |
| C8 | **Night is fully stylised.** Physics gives near-black without a sun. | No moonlight scattering / no physical night sky. | Deliberate. Moon-driven scattering is possible but is a separate feature. |
| C9 | **`zHalfMin` clamp is a magic number.** (§5.1) | Since 2026-08-06 it caps only how much of a whiteout is *volumetric* - the composite still corrects the total to vanilla - so it no longer shows as thin fog, it shows as a whiteout with fewer shafts in it. | Single tunable; raise the cap. |

Expected fidelity by scenario, as a reference for judging results:

| Situation | TP look retained | What changes |
| :-- | :-- | :-- |
| Midday outdoors | ~90% | Sky nearly identical; gains correct blue sky-fill and correct aerial perspective |
| Morning / afternoon | ~80% | Gentle warming, more graduated |
| Dawn / dusk | ~60–70% | Gains real structure, loses saturation — see C1 |
| Night | ~100% | Stylised, unchanged |
| Rain / storm | ~90% | Stylised-dominant — see C4 |
| Twilight Realm | 100% | Physics bypassed entirely |
| Interiors | n/a | No sky; fog from palette; local lights own the rest |

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
  **plus** godrays and shafts. Lake Hylia whiteout, Goron Mines heat, Faron
  morning and Forest Temple all fall out of one mapping with no per-area code.

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
| `rtx_composite.cpp` | fills `DusklightCompositeArgs` for the fog correction | same |
| `composite.comp.slang` `applyFog` | residual fog correction (was: range split) | `cb.dusklightArgs.enable` |
| `composite_args.h` | one args struct added (`DusklightCompositeArgs`) | additive only |
| `froxel.slangh` + `VolumeArgs` | `previousFroxelMaxDistance` | additive; also a genuine upstream fix |
| `rtx_light_manager.cpp` | none — B1 supplies a real texture | — |

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

*Overlay, bloom and plumbing:*

| File | Change | Guard |
| :-- | :-- | :-- |
| `dxvk_imgui.{cpp,h}` | the F1 Dusklight overlay and its three tabs — full surface in `DusklightOverlay.md` §5 | own functions, called from one place each |
| `rtx_bloom.{h,cpp}` + `bloom.h` | the Dusklight bloom mode and its settings | `rtx.bloom.dusklight` |
| `rtx_context.{h,cpp}` | `dispatchDusklightGrade`, and the bloom stage ordering | `DusklightGrade` enable |
| `dxvk_objects.h`, `dxvk_device.cpp` | the two modules constructed and exposed as `metaDusklight*` | additive members |

Rule for every guarded hook: **one branch, no reformatting of surrounding code,
and the guarded path calls into our module rather than inlining logic.** A
conflict then resolves by re-applying a single `if`, not by re-deriving intent.
The two API rows are the exception — they change upstream behaviour rather than
adding a branch, so a rebase has to re-apply intent there, and they are the two
worth checking first.

**Game side** (`dusklight-ao`): everything in `src/dusk/remix_*.{cpp,hpp}`.
Churn in `d_kankyo.cpp` is limited to one capture call, matching the existing
`dKy_celestial_orbit_z_ratio` pattern. Aurora's half of the material transport
is in `extern/aurora/lib/dx9/` and rebases against aurora, not against Remix.

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
| Fog falloff rework (§5.1–§5.3) | landed 2026-08-06, **UNTESTED in game.** Both paths evaluated arithmetically (§5.1's tables); never run |

**The fog falloff rework, 2026-08-06.** The reported symptom was that the
volumetric fog did not match the game's falloff while the depth-based fog did.
Reading the shipped derivation against the shipped ramp found three things, all
arithmetic rather than opinion:

1. **The near field was over-fogged** by up to +0.27/+0.37 opacity, because σ
   was matched at a single point and the ramp's dead zone was ignored (§5.1).
2. **The fog never closed to opaque** — it topped out at 0.75–0.81 at
   `fog_end_z` — because the far half's `t *= mean(volumeAttenuation)` broke the
   invariant the code above it claimed (§5.2, C2).
3. **The two halves aimed at colours a factor of 4.4 apart**, so the near fog
   read dark wherever real lighting was not filling the gap (§5.3).

The fix: solve σ for a *fraction* of the ramp at the froxel grid's reach, and
turn the composite's far half into a residual correction across the whole
distance. Total opacity is then the game's ramp exactly, at every distance.

**Regression signature, so it can be recognised rather than discovered:** if
`mediumFraction` is too high, near geometry inside `fogStartZ` picks up haze the
original did not have, and `rtx.dusklight.atmosphere.fogLog` tags that line
`OVERSHOOT`. If the medium's transmittance is noisy in very dense fog, the
correction divides by it and can amplify that noise — expect it as grain in a
whiteout, not as a colour shift. And if `multiScatteringScale` is now too high
for a brightly lit scene, fog near light sources will read hot, because real
in-scatter is adding on top of a medium that already settles at the full
authored colour.

Owner's verdict on A + B after testing: *"a massive, frankly monumental
success."* Range, shape and per-area fog scaling all validated; see §13's
"What the pass found".

**The 2026-07-29 session cleared this list.** `hideSkyBillboards`, warp, the
time-of-day slider and Freeze Time all work; local point lights work (they need
`localLightIntensity` 19 and `localLightRadius` 10, both now the defaults — see
`dusklight-ao/docs/remix-open-issues.md` open issue 3); and `hideSkyBillboards`
**fixed the night shadow wandering**, confirming the moon-quad cause rather than
merely masking it.

**Still untested, as of 2026-08-04:** the ambient grade — which should stay
untested until the defect below is fixed, because grading on a wrongly-lit sky
is tuning against a moving target — and everything built since 2026-07-29 and
never run: the `skyFogMode` treatments below and the painted moon (§13.1). The
2026-08-04/05 material work (two-colour ramps, per-draw vertex colour, and the
self-illumination rule) is likewise CI-green and unrun; it is tracked in
`aurora-ao/docs/dx9/remix-material-interface.md` §9–§10, not here.

`disableFrustumCulling` **is** tested: it works and it visibly helps with
light leakage.

### The live defect: the medium dims the generated sky

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
| A3 fog correction (was: range split) | `dusklight_composite_args.h`, `composite.comp.slang` `applyFog`, `rtx_composite.cpp` |
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

A reconnaissance pass claimed Lake Hylia "passes `start > end` deliberately".
It does not — it is `start < end` with a **negative start**. Acting on that
claim would have made Lake Hylia silently report no fog, in exactly the area
whose extreme morning fog is one of the two remaining validation targets.

Verify structural claims against the source before building a special case
around them.

### 14.11 Half the volumetric system is not in this repo

`integrateVolumetricNEE`, `integrateVolume`, `calcVolumetricAttenuation` and
`sampleDensityField` are all called from files here and **defined in none of
them**. Grepping the repo — or GitHub, upstream included — finds only call
sites, which reads exactly like a broken checkout and cost a detour to rule out.

They live in the **RTXDI submodule**, at
`submodules/rtxdi/rtxdi-sdk/include/volumetrics/rtx/`, reached through an extra
shader include path that `meson.build` adds by hand:

```
volumetrics_include_path_string = join_paths(global_src_root_norm,
  'submodules/rtxdi/rtxdi-sdk/include/volumetrics')
```

Four files are involved: `algorithm/volume_integrator.slangh`,
`algorithm/volume_lighting.slangh`, `algorithm/volume_composite_helpers.slangh`
and `pass/volumetrics/volume_restir.slangh`. In a container without submodules
checked out they are simply absent; read them from
`raw.githubusercontent.com/NVIDIA-RTX/RTXDI/<pinned sha>/…` rather than guessing
at their contents.

**Why it matters beyond the detour:** any change to how the medium's density
varies along a ray — the exact fix for C11 — lands in `sampleDensityField`,
i.e. in a fourth repository. The hook is real and clean (it already returns a
per-step density scalar, used for both extinction and out-scatter), so the
constraint is ownership, not capability. §5.2 works around it instead by
correcting in the composite, which is ours.

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
  is a constant. For fog *density* since 2026-08-06 the first knob is
  `mediumFraction`, not `densityScale` - the total opacity is pinned to the
  game's ramp either way, so what a uniform density error usually means now is
  too much or too little of it being volumetric. Then `skyIntensity` for
  brightness, and `zHalfMin` only for scripted whiteouts.
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

**The dense end — half covered as of 2026-07-29.** Lake Hylia in the morning was
visited and its fog is *"suitably intense"*, which is the first real evidence
that the σ mapping holds at the dense end and not just in the thin and mid
regimes.

Two cautions against reading it as more than that:

- **It does not isolate `zHalfMin`.** That clamp only bites where the
  half-density point falls behind the camera. Lake Hylia's kytag01 passes a
  *negative* start with `start < end` (§14.7 — the opposite of what an earlier
  recon claimed), so whether the clamp actually fired in that visit is not
  established. "The dense case looks right" and "the clamp is correct" are
  different claims and only the first has evidence.
- **The Goron Mines are still unvisited**, and they are the other regime — near
  and dense rather than scripted and dense.

`froxelRangeScale` (0.6) remains unchallenged rather than validated.

**And the Lake Hylia visit surfaced something bigger than fog density:** it is
where the sky/fog defect in §12 shows worst, because it is the densest medium
the game asks for. Any future dense-fog reading is partly measuring that defect
until it is fixed.

### The measurement pass is still worth doing

The Dusklight tab reports the game's live `fogStartZ` / `fogEndZ` / `fogColor`
and sky colours. Those values live in stage data rather than in source, so this
readout is the only way to see what a given area actually asks for. Recording
them at Hyrule Field, Faron, Lake Hylia, the Goron Mines and the Forest Temple
turns any future tuning from guesswork into arithmetic.
