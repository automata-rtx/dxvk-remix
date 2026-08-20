# Dusklight atmosphere: sky, sky light and fog

One participating medium, derived once per frame from Twilight Princess's own
environment state, driving **the visible sky, the sky's contribution to global
illumination, and the fog** — so the three cannot disagree.

This is the *design*: the shape of the system, and where each thing lives. Every
derivation, tuning rule and option meaning is stated at length in the code, and
the code is better than a paraphrase of it — `rtx_dusklight_atmosphere.{h,cpp}`
(the host, and every option's help text), `volume_lighting.slangh` (the
extinction field), `composite.comp.slang` + `dusklight_composite_args.h` (the far
half), `dusklight_atmosphere_common.slangh` (the physical sky). **Follow the
pointer; do not copy it back here.**

Companion docs: `DusklightOverlay.md` (option wire and overlay),
`DusklightRebase.md` (the upstream files this fork touches),
`dusklight-ao/docs/kankyo-fog.md` (what the game computes),
`dusklight-ao/docs/remix-open-issues.md` — **the single ledger of what is broken
and unverified; status claims live there, not here** — and
`dusklight-ao/docs/japanese-naming-remix.md` for the romanized Japanese quoted
below: `kankyo` (環境) environment, `kumo` (雲) cloud, `moya` (靄) mist, `kytag`
a *kankyo tag* actor, `vrbox` the skybox dome.

---

## 0. The one-sentence design

> Derive the environment once, from the game's own palette, and let the visible
> sky, the light that sky casts, and the colour distant geometry fades towards
> all read that one derivation — so none of them can describe a different day.

---

## 1. Why this is one system and not three

**The game already treats them as one.** `fog_col`, `fog_start_z`, `fog_end_z`
and every `vrbox_*` colour live in the **same** palette entry
(`dusklight-ao/include/d/d_kankyo.h`), selected by the same time-of-day and
colpat indices and blended by the same `dKy_calc_color_set` call at the same
instant. Physics agrees: aerial perspective and sky colour are one scattering
integral over different path lengths.

**But the two media meet at colour, not at density** — the physical medium is
essentially transparent at this game's scale, so it is a source of *radiance*,
not of fog, and the fog stays the game's (figures at
`dusklight_atmosphere_common.slangh:34-37`; it is also why Hillaire's
aerial-perspective LUT is unnecessary here). **Any future "just make it physical"
instinct about the fog has to read that comment first** — this section originally
demanded shared coefficients, which would have deleted the fog.

---

## 3. Architecture

```
LAYER 1  game    dusk::remix::captureEnv(), one snapshot per frame: sky / fog /
                 sun / colpat crossfade / moya / daytime, pushed through the
                 rtx.dusklight.env.* NoSave options
LAYER 2  fork    ONE derivation per frame: physicalWeight, medium, LUTs, VolumeArgs
LAYER 3          dome light (sky GI) | visible sky | volumetrics -> far ramp
```

**Layer 1 reads the game's *outputs*, not its inputs**, and that is load-bearing.
`fog_col` / `mFogNear` / `mFogFar` arrive after the four-way palette blend, the
`addcol_fog` offset, the `now_fogcol_ratio` lightning scale
(`d_kankyo_rain.cpp:362`) and any `dKy_fog_startendz_set` override — reading
outputs inherits all four for free and keeps them right when the game changes.
There are **four** such modifiers, not five or six: the `*Gather` fields are a
staging area copied into the one real blend each frame
(`d_kankyo.cpp:4788-4828`), not a second blend, and `fog_avoid_tag` modifies no
fog at all (§8.2). Both miscounts cost work.

**Layer 2 owns all derivation**; nothing downstream computes atmosphere
parameters. That is what makes "sky and fog cannot disagree" structural rather
than a tuning task.

---

## 4. The single blend weight

`physicalWeight = elevationTerm × outdoorTerm × styleTerm` decides how
stylised-versus-physical the whole system is, and drives sky appearance, sky
light *and* fog colour together — splitting it would give a physical blue sky
over amber stylised haze. Intermediate values blend the **radiance function**,
not output pixels, so GI, visible sky and aerial perspective stay mutually
consistent at every setting. Terms and their sources:
`rtx_dusklight_atmosphere.cpp:1004-1066`.

### 4.1 `styleTerm` follows the game's fade

The game never holds one colour pattern — it holds a crossfade (`wether_pat0`,
`wether_pat1`, `pat_ratio`), and *every* colour this weight is blended against is
that lerp. Pushing `wether_pat1` alone made `styleTerm` **step on the first frame
of a weather transition and stay stepped** while everything beside it moved
continuously. `kytag01`, the Lost Woods mist tag, makes it structural: it takes
the ratio over and ramps it across ~50 frames
(`d_a_kytag01.cpp:129-148`), and **that ramp is the mist's strength**.
`colpatPrev`/`colpatBlend` (protocol 13) made the term a lerp, default-safe by
algebra rather than assertion (`rtx_dusklight_atmosphere.cpp:1043-1056`).

**Regression signature:** the handover becomes gradual where it used to snap. If
it becomes *erratic* during a weather change, `colpatBlend` is read at the wrong
point in the frame — both panels print `prev -> curr @ ratio`, and a healthy
transition sweeps 0 → 1 once and stops.

---

## 5. Fog: mapping TP's linear ramp onto a medium

### 5.1 The mapping

TP's fog is `f(z) = saturate((z − start)/(end − start))`. The medium's extinction
is matched to it at **one anchor distance** — scale-free, no magic endpoints:

```
z_anchor = max((start + end)/2, zHalfMin)      the ramp's midpoint, floored
σ        = -ln(1 - f(z_anchor)) / z_anchor     matched to the game's actual opacity there
```

**The derivation is `rtx_dusklight_atmosphere.cpp:371-412`** — the `ln(2)/anchor`
expression this replaced on 2026-08-17 and why it was wrong, the
`d_a_kytag01.cpp:94` mist-tag citation that exposed it, the `zHalfMin = 100`
clamp, the 0.955 figure, and the `f ≤ 0.999` cap with the case where it binds.
The consequence worth stating here: where the clamp binds — **every scripted fog
bank in the game** — the medium is now **4.46× denser**. Where it is idle,
nothing moved.

`zHalfMin` is a floor on the **anchor distance**, not on a half-density distance,
and neither it nor `densityScale` is inert in `fogRampMode` 2 — the panel said
otherwise until 2026-08-18. This module replaces `rtx.volumetrics.enableFogRemap`
entirely for us (§6.1); the `enable` help lists everything else in
`rtx.volumetrics.*` it takes over, which is worth reading before concluding a
knob does nothing.

### 5.2 The split — by job, not by distance

`fogRampMode`: **0** Handover (the A/B baseline), **1 Top up** (the default),
**2** Exact ramp. **All three are described in full, including why mode 0 had to
go and mode 2's uniqueness argument, in the option's own help at
`rtx_dusklight_atmosphere.h:539-590`.** What belongs here is the principle and
two lessons.

**The principle: mode 1 splits by *job*, not by distance — the ramp owns the
fog's appearance at every distance, the medium owns the light.** The residual
`t = (f − m)/(1 − m)` is an identity, not an approximation
(`composite.comp.slang:763-780`), and the total is `S·(1 − f) + A·f + I_lit·(1 − t)`
— the original's fog formula plus real in-scatter from real lights. That last
term is the shafts, the only thing volumetrics add that a ramp cannot do. **What
it buys is that density stops being a fidelity knob**, so σ is free to be set for
shaft quality alone and held *under* the game's curve. A split by *distance*
cannot do that, because a homogeneous medium cannot be clear where the game is
clear.

> **That identity has one `A` in it. The implementation had two, for four days** —
> the medium's isotropic in-scatter used the dome's sphere mean while the far ramp
> sampled the same dome *directionally*. Both sides said "the dome" and meant
> different integrals of it, and this document called it a feature: *"the two
> agree on average."* **An identity does not hold on average; its terms have to be
> checked for being the same *value*, not the same *name*.** Mechanism and the
> switch back: `dusklight_composite_args.h:72-78`.

Two more lessons from the same work, both about how a change is judged rather
than about fog. Mode 2's regression signature was written down for a day without
the two places the residual is *expected* to vary, one of which is the default
outdoor configuration — **a regression signature that fires on a healthy build is
worse than none.** And its three host floors
(`rtx_dusklight_atmosphere.cpp:146-165`, `:650-661`) exist because **a mode that
switches itself off in response to a legal setting, while every readout goes on
describing the mode, is worse than one that fails loudly.**

### 5.3 The fog's colour

The medium's colour never came from lighting — it comes from
`multiScatteringEstimate`, a constant added at every raymarch step, and it has to
exist because **the RTXDI light list has no dome light in it**, so fog in shadow
receives nothing at all from the sky. That fact, the albedo division that makes
the near and far halves agree, and the 4.4× mismatch it removed are all in the
`multiScatteringScale` help (`rtx_dusklight_atmosphere.h:446-466`) and at
`rtx_dusklight_atmosphere.cpp:544-547`, `:1153-1165`.

The constant follows the sky: `dusklight_sky_stats.comp.slang` reduces the
generated dome to its solid-angle-weighted mean radiance, read a few frames stale
off a non-stalling ring. A **sphere** average is the right quantity rather than a
compromise — the term is an isotropic in-scatter estimate, so what it wants is
the froxel's irradiance from the environment.

**The dome supplies the fog's *hue*, not its *level*.** `skyIntensity` is a
*lighting* calibration; handing that radiance to the fog made it ~6× brighter
than the authored colour and washed out sealed interiors. `skyAmbientMode`
(default Hue only) and `fogColorDirectional` (default false, **and what turning
it off costs**) are stated in full in their option help.

### 5.4 The fog's level — treating a display colour as one

**`fog_col` is not a radiance** — the game blended it over a finished, already
exposed image, so it means "what the screen should show there", and using it raw
in a linear frame is the last of the reasons dark scenes read grey. The fix is to
say the exposure out loud: `fogRadiance = paletteRead · fogRadianceScale /
exposure`. Why that cannot run away, why only the palette-derived part is
corrected, and what each `exposureFogMode` setting is for are in its own help at
`rtx_dusklight_atmosphere.h:482-499`.

**One derivation now feeds both fog paths**, and the consequence outlives the
defect: `applyFogOverride` writes the resolved radiance into `FogState::color`
(**not clamped to 1** — it is a radiance in a pre-tonemap frame) and the
composite skips `rtx.fogColorScale` for it. **So `rtx.fogColorScale` no longer
affects Dusklight's fog in the ordinary case; `fogRadianceScale` is the level
knob for both paths, and any document or `rtx.conf` telling you to calibrate the
former is stale.** The gate is `fogColorOverridden()`, not `active()`, because
the override declines to write when a translucent material already replaced the
fog — **"is the feature on" and "did the feature take this value over" are
different questions, and a consumer downstream of an early return has to ask the
second.**

`fogColorSpace`'s default is Raw, which is deliberately **not** the correct
answer; the honest reason, the 2.3× size of the difference and the recalibration
route are in its own help at `rtx_dusklight_atmosphere.h:414-428`.

### 5.5 Forward scattering

`fogAnisotropy` overrides upstream's isotropic `rtx.volumetrics.anisotropy`
whenever the Dusklight medium is driving, and **defaults to 0**: raised to 0.6 on
2026-08-13 and reverted the same day, because the froxel grid stores in-scatter
as a first-order spherical harmonic that cannot represent a sharp forward lobe.
That cause is **unconfirmed** — one of two candidates for a reported blocky look,
and the one this fork changed. Full account, and the warning not to confuse it
with `mieAnisotropy` (which never reaches the froxel grid), at
`rtx_dusklight_atmosphere.h:522-538`.

---

## 6. Live configuration

**6.1 Push state, derive in Remix — never push renderer options.** The bridge
pushes *game state* (`rtx.dusklight.env.fog*`, all `NoSave`) and this module
writes derived values straight into `VolumeArgs`. It must **not** write
`rtx.volumetrics.*` option values per frame: those are saved options, so a
settings save would bake one moment's weather into `rtx.conf` — already hit once
here; it would fight manual tuning; and it churns dirty tracking every frame.
`rtx.volumetrics.*` keeps its meaning as an artist override *on top of* the
derived values, never as the transport for them.

**6.2 `froxelMaxDistance` must follow the fog — carefully.** Driving it from the
game's fog range puts the grid's 64 slices where the fog actually is, and costs
**nothing**: `m_froxelVolumeExtent` comes from resolution and slice count only
(`rtx_global_volumetrics.cpp:797-802`). **But upstream assumed it never changes** —
it reconstructed the previous frame's froxel mapping with the *current* frame's
value, which smears temporal history. Both mitigations are **implemented**:
`previousFroxelMaxDistance` is carried explicitly (`rtx_global_volumetrics.cpp:617-619`,
`:710`; `volume_args.h`, `froxel.slangh` — a genuine upstream fix rather than a
workaround), and `froxelSmoothingRate` eases the value anyway, which is faithful
as well as safe because the game eases its own fog transitions with `cLib_addCalc`.
`froxelDepthSlices` drives allocation and stays fixed.

---

## 7. Computed once, used many

| Quantity | Computed | Note |
| :-- | :-- | :-- |
| Palette blend | **game**, once per frame | inherits the addcol, ratio and override layers for free (§3) |
| Transmittance and multi-scattering LUTs | on medium change only | functions of the medium alone, **not** of sun direction |
| Sky-view LUT (lat-long) | per frame | there is no separate table: the dome image *is* it, and dome light, visible sky and the far fog's colour all funnel through `sampleDomeLightTexture` |
| Medium extinction/scattering | per frame, once | **not** shared with the sky (§1) |
| Aerial perspective | — | **not computed, and not approximated by default** — the directional dome sample is behind `fogColorDirectional`, off, because it broke §5.2's identity |

---

## 8. Clashes, and how each is resolved

- **8.1 Moya particles.** `mMoyaMode`/`mMoyaCount` would double-count against a
  dense medium — except `dKankyo_cloud_Packet::draw` returns early on D3D9
  (`d_kankyo_wether.cpp:119-126`), so they were never drawn here. Still pushed,
  as a signal of how much haze an area wants folded into the medium.
- **8.2 The fog-avoid tag (kytag08) — no clash, and no work is owed.** Recorded
  only so it is not rediscovered and re-planned: `fog_avoid_tag`'s single read
  aims a projected texture matrix at the `MA11` ground material
  (`d_kankyo.cpp:7594`, `:11608-11637`) and writes **no fog state anywhere**.
- **8.3 Per-object fog.** Remix keeps the first non-`NONE` fog state of a frame,
  so which of the game's per-object states won was a lottery decided by
  submission order. The bridge's global env fog is now authoritative — strictly
  better, but it flattens genuine per-object variation (C7).
- **8.4 Auto-exposure vs dense fog.** A scripted whiteout would be flattened to
  grey. Calibration (clamp adaptation while derived density is high), not
  architecture. Not built.
- **8.5 Sky brightness and the LDR probe.** The auto-detected probe is clamped to
  1.0 because it inherits the game's 8-bit render target format
  (`rtx_sky.h:152-158`); the dome light replaces it. Two design rules follow:
  **never bake the sun disc into the dome texture** — keep it analytic and
  NEE-sampled — and keep the *lighting* sky-view variant smoother than the
  visible one.
- **8.6 Twilight Realm, and the colpat 9 bypass — unverified.** `physicalWeight`
  is driven to 0 there — bypass, not tune — and the amber fog stays amber for
  free because one weight drives both. **The number it cuts on is the problem**:
  9 was read off `dKy_sense_pat_get` (`d_kankyo.cpp:266-270`), the **wolf-sense
  vision pattern**, a different index space. Whether any stage runs *colpat* 9 is
  **UNKNOWN and not knowable from source**, so it was instrumented rather than
  argued about (`rtx_dusklight_atmosphere.cpp:939-988`, which says what each
  outcome decides and why `freezeTime` must be off to read the log).

---

## 9. Live compromises

Verification status is in `dusklight-ao/docs/remix-open-issues.md`.

| # | Compromise | First knob |
| :-- | :-- | :-- |
| C0 | **The calibration pass never exercised `zHalfMin` or `froxelRangeScale`** — it did not visit the scripted-fog regime, and "not reported as wrong" is not a measurement. Both have since moved underneath that sentence, so the measurement is still owed **and is now of different quantities** | — |
| C7 | **Per-object fog flattened to one global** (§8.3) | restorable per-instance, at the cost of a per-instance field |
| C9 | **`zHalfMin` is a magic number** (§5.1), still 100, unmeasured. What changed is the consequence of it binding: a whiteout now closes in *harder* than vanilla, the opposite signature to the one this row used to predict | raise it to thin the densest fog; in `fogRampMode` 2 the view ray does not use it at all |

**Four more are deliberate stylisation rather than debt**, and none is a bug to
chase: TP's authored dusk is more saturated than physical twilight, its clouds
(`kumo_*`) are painted bands with no physical analogue, clear-sky scattering
cannot do "rain grey", and its night is fully stylised because physics gives
near-black without a sun. `styleTerm` and `elevationTerm` already drop
`physicalWeight` in the last three; the first is tuned on the *medium's* tint,
never on output pixels.

Expected fidelity: midday ~90%, morning/afternoon ~80%, dawn/dusk ~60–70%,
night ~100%, storms ~90%, Twilight Realm 100% bypassed. **Interiors** have no sky
and fog from the palette, and **effect lights own the rest** — the only
NEE-sampled source there, since the sun is gated off indoors and Remix has no
dome light type.

---

## 12. Status

**Verification claims live in `dusklight-ao/docs/remix-open-issues.md`.** Two are
recorded here because they are the fog's own:

- **Tested in game 2026-08-18, shipping defaults, improvements reported:** the
  §5.1 anchor correction, the one-colour fix (§5.2/§5.3) and the shared
  derivation for both fog paths (§5.4). The 2026-08-13 rework — top-up ramp,
  density cap, dome-derived ambient, exposure-relative level, panel persistence —
  was **exercised as part of that default fog path**. That is inference, not a
  test of those rows: nothing isolated them.
- **`fogRampMode` 2 was NOT covered and stays UNTESTED IN GAME.** The session ran
  the defaults; mode 2 is not one. This fork cannot be compiled in a Linux
  container, so CI is its first real compile.

The ambient grade is built, CI-green and deliberately unrun.
`disableFrustumCulling` **is** tested: it works and visibly helps light leakage.

---

## 14. Facts that were expensive to learn

**14.1 Remix's lighting model.** **No dome light type exists** —
`shaders/rtx/concept/light/light_types.h:25-33` lists sphere, rect, disk,
cylinder, distant, `lightTypeCount = 5` — so a sky is never NEE-sampled and the
froxel grid is never lit by it (`rtx_dusklight_atmosphere.cpp:545`). Distant-light
irradiance is **independent of angular diameter**, so widening `celestialAngle`
softens shadows and changes nothing about brightness. Remix's `direction` is the
direction light *travels*; its world unit is 1 cm, close enough to TP's to use
directly.

**13.1 The moon is painted into the dome.** `skyMoonEnable`, default on,
untested. `hideSkyBillboards` is the confirmed fix for wandering night shadows
and takes the visible moon with it; a painted disc is correctly placed, moves
with the sky rather than the camera, and cannot cast a shadow because it is not
geometry. **Only the moon** — the sun stays analytic and NEE-sampled, and baking
something that bright into an image reached only by ray miss would double-count
it.

**14.8 The sky is fogged by two paths, and only one of them knows it.** `applyFog`
exempting `primaryMiss` is *not* "the sky is not fogged": `applySkyContribution`
multiplies the dome by `volumeAttenuation` over the whole grid with the
in-scatter already in `radianceOutput`. **A guard in one path is not a guarantee
across the system** — and because our σ is an artistic quantity rather than air,
any Remix code attenuating something by full grid depth behaves very differently
for us than for stock. Re-ask it anywhere `volumeAttenuation` reaches a distant
or infinite source. (Resolved by `skyFogMode` = Exempt, tested good 2026-08-13;
the lesson outlives the defect.)

**14.9 "No texture, therefore untaggable" was wrong — there are three routes.**
`rtx.skyBoxTextures` hashes a texture (`rtx_types.cpp:409`), `rtx.skyBoxGeometries`
hashes **geometry** (`:416`), and `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY` is
declared outright on API-submitted geometry (`rtx_remix_api.cpp:647`). Only the
first needs a texture, so the untextured vrbox **can** be tagged today with a
config line and no code. Check any future "we can't tag that, it has no texture"
against all three.

**14.10 The sky-probe route is not ruled out.** Both stated advantages of the
dome light over it were false — the probe feeds GI exactly as the dome does
(`integrator_indirect.slangh` is a plain if/else) and its 8-bit clamp is
*inherited*, not intrinsic (`rtx.skyForceHDR` overrides it). What honestly
remains for the dome light is that it is tested, non-occluding by construction,
and a texture we already generate. **None of that says the sky-categorised route
would look worse.**

**14.7 `kytag01` is the Lost Woods / Sacred Grove mist tag** (`d_a_kytag01.cpp:1-4`,
`:202`), **not Lake Hylia**, and its fog is `start < end` with a *negative*
start. A recon pass got both wrong; the mechanism error was caught by the code
not working, while the place name survived two more weeks across twelve passages
in two repos, because a wrong place name is invisible until someone is sent there.
