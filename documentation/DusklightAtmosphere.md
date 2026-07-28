# Dusklight atmosphere: sky, sky light and fog

Design doc for the hybrid atmosphere system in this fork: one participating
medium, derived once per frame from Twilight Princess's own environment state,
driving **the visible sky, the sky's contribution to global illumination, and
the fog** — so the three cannot disagree with each other.

Companion doc on the game side: `dusklight-ao/docs/kankyo-fog.md` (what the
game computes and what it pushes). The wider environment bridge is documented
in `dusklight-ao/docs/kankyo-remix.md`.

Everything below is grounded in code as of 2026-07-27. File references are
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

The current `rtx.conf` guidance in `dusklight-ao/docs/dx9-fixed-function.md`
picks the depth path (`rtx.volumetrics.enable = False`) for exactly these
reasons. That remains correct until Phase A lands.

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
| Lake Hylia, kytag01 at full | `-2000`, `200` | clamped to `zHalfMin` | near-whiteout |

Note the Lake Hylia case: the tag passes `start > end` deliberately
(`dusklight-ao/src/d/actor/d_a_kytag01.cpp:94`), which in the vanilla ramp
means "already ~90% fogged at z=0". The clamp turns that into a very dense
medium, which is the right answer. `zHalfMin` is the one tuning knob and it
exists to stop σ diverging.

**This replaces `rtx.volumetrics.enableFogRemap` entirely for us.** We do not
enable Remix's remap; we write the derived coefficients straight into
`VolumeArgs`. See §6.1 for why that direction matters.

### 5.2 The range split

The mapping above matches density but not shape: at `z = end` vanilla is 100%
opaque and an exponential is ~75%. Distant terrain would stay more visible than
in the original. Fix it by giving each system the range it is good at:

- **`[0, froxelMaxDistance]` — volumetrics.** Shafts, godrays, torch glow,
  sky-light scattering into shadow. Things a depth ramp fundamentally cannot
  do.
- **`(froxelMaxDistance, ∞)` — the vanilla ramp, analytically.** Closes to
  exactly 100% at `end`, matching the original.

Today `src/dxvk/shaders/rtx/pass/composite/composite.comp.slang:608` forbids
this:

```hlsl
if (cb.volumeArgs.enable)
{
  return; // Volumetric fog is applied in volume integration
}
```

The change is to make that early-out conditional on the Dusklight path and, when
active, evaluate the vanilla ramp *rebased* so it contributes only beyond the
froxel grid — i.e. apply `saturate((z - max(start, froxelMaxDistance)) / (end -
max(start, froxelMaxDistance)))` over the residual transmittance the froxel
integration already produced, so the two never double-count.

**This is the answer to "which system owns fog": one owner per unit of
distance, not one owner overall.**

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
it is rasterized in the game's own 8-bit target format
(`rtx_sky.h:153-158`). The dome light replaces it entirely
(`integrator_indirect.slangh:372-379` is an if/else), which is what unlocks
real HDR sky radiance. The dome light is **not** in the sampleable light list —
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
| C11 | **A homogeneous medium cannot be clear near the camera.** The game's ramp is exactly zero before `fogStartZ`; an exponential medium starts extinguishing at the camera. With a large `fogStartZ` (say 50 m of clear air, then fog to 100 m) the derived medium is already ~35% opaque where the original is untouched. | Near-field haze in areas the original left crisp. Worst where `fogStartZ` is large relative to `fogEndZ`. | No single σ fixes it — it is the shape, not the level. `rtx.volumetrics.enableHeterogeneousFog` is the real answer: a density that ramps with distance rather than a constant one. Until then, `densityScale` trades near-field clarity against far-field weight. |
| C10 | **Fog is composited in linear HDR, not the game's display space.** The original blended fog over a finished, display-referred image; here both halves of the range split happen pre-tonemap. | Fog reads with a different contrast curve than vanilla - typically holding its colour longer in the bright end. | `rtx.dusklight.atmosphere.fogRadianceScale`. The structural fix is moving the far ramp post-tonemap, the same correction the bloom needed. |
| C1 | **Dusk saturation.** Physical twilight is more graduated and less saturated than TP's authored dusk. | Sunsets read calmer / less punchy than vanilla. | Lower `physicalWeight`'s `elevationTerm` at low sun; or add a saturation push applied to the *medium's* Rayleigh/Mie tint, not to output pixels. |
| C2 | **Exponential never fully closes.** | Distant terrain slightly more visible than vanilla at `fog_end_z`. | The §5.2 range split is the fix; if still short, lower the split distance so the vanilla ramp owns more. |
| C3 | **Clouds have no physical analogue.** `kumo_top/bottom/shadow` describe painted cloud bands. | Skies read emptier than vanilla if the vrbox is replaced wholesale. | Keep TP's cloud layer as geometry over our sky (Phase D). |
| C4 | **Weather has no physical analogue.** Clear-sky scattering cannot do "rain grey". | Storms look insufficiently oppressive. | `styleTerm` drops `physicalWeight` on weather colpats; overcast can also be faked with high Mie + suppressed sun. |
| C5 | ~~Moya swirl replaced by noise.~~ **Withdrawn - the problem does not exist on this backend.** `mMoyaCount` feeds `mpCloudPacket->mCount` (`d_kankyo_rain.cpp:1616`, inside `cloud_shadow_move`), and `dKankyo_cloud_Packet::draw` already returns early on D3D9 (`d_kankyo_wether.cpp:119-126`). The haze billboards were never drawn here, so there is nothing to double count and no switch was needed. `moyaMode`/`moyaCount` are still pushed, as a signal of how much haze an area wants folded into the medium. | — | — |
| C6 | **Fog-avoid tag ignored.** (§8.2) | No clear bubble around the player in heavy fog. | Deferred feature, not a tuning knob. |
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

**Phase D — clouds, moya, polish.** C3/C5 from the ledger.

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

**New files (zero upstream churn):**
- `src/dxvk/rtx_render/rtx_dusklight_atmosphere.{h,cpp}` — all derivation.
- `src/dxvk/shaders/rtx/pass/dusklight/*.slang` — auto-discovered by
  `compile_shaders.py`'s `os.walk`; no build-file edits (already proven by the
  grade pass).
- `src/dxvk/rtx_render/rtx_dusklight_{env,game}.h` — existing.

**Upstream files touched, and how (each a single guarded hook):**

| File | Change | Guard |
| :-- | :-- | :-- |
| `rtx_scene_manager.cpp:609` | fog state selection | `DusklightAtmosphere::active()` |
| `rtx_global_volumetrics.cpp` `getVolumeArgs` | one early branch to the Dusklight derivation | same |
| `composite.comp.slang` `applyFog` | range split | `cb.dusklightArgs.enable` |
| `raytrace_args.h` / `composite_args.h` | one args struct added | additive only |
| `froxel.slangh` + `VolumeArgs` | `previousFroxelMaxDistance` | additive; also a genuine upstream fix |
| `rtx_light_manager.cpp` | none if B1 supplies a real texture | — |
| `dxvk_imgui.cpp` | Dusklight tab rows | existing tab |

Rule for every hook: **one branch, no reformatting of surrounding code, and the
guarded path calls into our module rather than inlining logic.** A conflict
then resolves by re-applying a single `if`, not by re-deriving intent.

**Game side** (`dusklight-ao`): everything in `src/dusk/remix_*.{cpp,hpp}`.
Churn in `d_kankyo.cpp` is limited to one capture call, matching the existing
`dKy_celestial_orbit_z_ratio` pattern.

---

## 12. Status

| Item | State |
| :-- | :-- |
| Design | this document, 2026-07-27 |
| Phase 0 calibration | run 2026-07-28 — see §13 |
| Phase A (A1–A4) | implemented 2026-07-27, **tested good 2026-07-28** |
| Phase B (B1–B2) | implemented 2026-07-27, **tested good 2026-07-28**; `skyIntensity` raised 1.0 → 6.0 after it read dim |
| Phase C (C1-C3) | implemented 2026-07-28, **untested** |

Still untested at the time of writing, all independent of the atmosphere:
`rtx.dusklight.game.disableFrustumCulling`, `hideSkyBillboards`, and the local
point lights.

### What landed

| Piece | Where |
| :-- | :-- |
| A1 fog push + fog-state override | `rtx_dusklight_env.h` (new keys), `rtx_scene_manager.cpp` (`applyFogOverride`), game `remix_bridge.cpp` |
| A2 medium derivation | `rtx_dusklight_atmosphere.{h,cpp}`, hooked at `rtx_global_volumetrics.cpp` |
| A3 range split | `dusklight_composite_args.h`, `composite.comp.slang` `applyFog`, `rtx_composite.cpp` |
| A4 live grid extent | `volume_args.h` (`previousFroxelMaxDistance`), `froxel.slangh`, `rtx_global_volumetrics.{h,cpp}` |
| B1 generated sky | `dusklight_sky.{h,comp.slang}`, `DxvkDusklightAtmosphere::prepareSceneData` |
| B2 dome suppression | game `d_a_vrbox.cpp`, `d_a_vrbox2.cpp`, `settings.{h,cpp}` |

Bridge protocol went **1 → 2**. A game build older than that will show the "game build is older than this Remix build"
notice in the Dusklight tab rather than silently doing nothing.

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

Nothing else was reported wrong, so `zHalfMin` (100) and `froxelRangeScale`
(0.6) stand unchallenged rather than confirmed - they were simply not the
thing that looked off.

### The measurement pass is still worth doing

The Dusklight tab reports the game's live `fogStartZ` / `fogEndZ` / `fogColor`
and sky colours. Those values live in stage data rather than in source, so this
readout is the only way to see what a given area actually asks for. Recording
them at Hyrule Field, Faron, Lake Hylia, the Goron Mines and the Forest Temple
turns any future tuning from guesswork into arithmetic.
