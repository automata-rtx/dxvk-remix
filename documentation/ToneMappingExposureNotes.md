# Tone mapping and auto exposure — defects found, and what changed

Written up separately from the commits because several of these are **upstream
dxvk-remix defects, not fork-specific ones**, and are worth carrying back on their own.

Every measurement here comes from a numerical model of the shader maths, run against the
constants actually in the tree. Where a claim is a mechanism rather than an observation, it says
so. **Most of this document is still model, not observation** — see the closing section for what
has and has not been seen on screen.

---

## 1. Centre-weighted metering scaled the colour, not the weight

**File:** `src/dxvk/shaders/rtx/pass/tonemap/auto_exposure_histogram.comp.slang`
**Status:** upstream defect, fixed here.

The pass computed a per-pixel metering weight and then did this:

```hlsl
const float3 weightedColor = inputColor * centerMeteringWeight(...);
const uint bucketIdx = inputToHistogramBucket(weightedColor);
InterlockedAdd(g_localData[bucketIdx], 1);
```

Multiplying the weight into the *colour* and then binning the result does not down-weight a
pixel. It **darkens** it and bins it as a darker pixel. A bright window at the edge of frame was
recorded as a dark object.

Worse at the extreme: the weight reaches exactly 0 at the edge of the metering falloff, so those
pixels fell below the `1e-5` black epsilon and were binned into bin 0 — the "discard" bin. Turning
on centre metering therefore *manufactured* black pixels, which then interacted with defects 2
and 3 below.

**Fix.** Bin the unmodified colour, and apply the weight to the count instead, accumulated as
fixed point so it can still go through `InterlockedAdd` on a `uint` histogram:

```hlsl
const uint bucketIdx = inputToHistogramBucket(inputColor);
const float weight = centerMeteringWeight(...);
const uint fixedPointWeight = uint(weight * float(EXPOSURE_HISTOGRAM_WEIGHT_SCALE) + 0.5f);
if (fixedPointWeight != 0) { InterlockedAdd(g_localData[bucketIdx], fixedPointWeight); }
```

**Regression signature if this is wrong:** exposure responds to where bright objects are on
screen rather than how bright they are, and enabling centre metering makes scenes brighter
overall rather than more centre-biased.

---

## 2 & 3. The two averaging modes disagreed with themselves about the black bin

**File:** `src/dxvk/shaders/rtx/pass/tonemap/auto_exposure.comp.slang`
**Status:** upstream defects, both removed by the rewrite.

Bin 0 collects sub-epsilon pixels and is meant to be excluded from the average. Neither mode
excluded it correctly.

**Median mode** computed the target as half the *non-black* population:

```hlsl
float median = (weightedSum - discardSampleCount) / 2.f;
```

…and then searched for it in a CDF (`g_localData`) that still **included** bin 0. The search
therefore terminated early, by an amount proportional to how much black was on screen.

**Mean mode** carried a comment claiming it accounted for bin 0 —

> *Weight our sum by the number of pixels (account for the almost black pixels in bucket[0] - which shouldnt contribute to the average)*

— but the code divided by `totalWeight`, which **includes** bin 0, while those pixels contributed
`0 × count` to the numerator. So they dragged the mean toward bin 0 rather than being excluded.

Both errors push metered EV **down**, which makes the image **brighter**. Combined with defect 1,
enabling centre metering amplified both.

**Fix.** The trimmed average sums bins `1..N-1` for both the numerator and the denominator, so
bin 0 cannot enter either side.

**Measured after the fix:** with 0%, 25%, 50% and 75% of the frame pure black over an otherwise
identical scene, metered scene EV is identical to 4 decimal places in all four cases.

---

## 4. The local tone mapper's luminance probe was missing `suppressBlackLevelClamp`

**File:** `src/dxvk/shaders/rtx/pass/local_tonemap/luminance.comp.slang`
**Status:** upstream defect, fixed here. **Latent under default settings.**

`ACES.hlsl` carries an explicit note from NVIDIA:

> *NOTE: ACES with 'suppressBlackLevelClamp=true' is only used in the local tonemapper to calculate
> a multiplier … Without 'suppressBlackLevelClamp=true', the local tonemapper will incorrectly
> crash the almost-black colors because of the local intensity evaluation.*

`final_combine.comp.slang:95` passes it. `luminance.comp.slang` did not — it called the two-argument
overload, and the third parameter defaults to `false`.

This is **inert while `rtx.useLegacyACES` is true** (the default), because the legacy curve ignores
the flag entirely. With the fitted curve it bites:

| stops below mid grey | probe value, clamp on (buggy) | clamp suppressed (fixed) |
|---|---|---|
| −5 | 0.0261 | 0.0370 |
| −6 | **0.0000** | 0.0259 |
| −7 | **0.0000** | 0.0184 |
| −8 | **0.0000** | 0.0133 |
| −10 | **0.0000** | 0.0070 |

Every shadow past about −5 stops reads as the same value, so the exposure fusion cannot
distinguish any of them and its local adaptation goes flat in exactly the region it exists to
serve.

Both ruler sites now route through `localTonemapRuler()` in `local_tonemapping.slangh`, which
passes the flag on the ACES branch, so the two can no longer drift apart. See §8.

---

## 5. Auto exposure never received a reset signal

**File:** `src/dxvk/rtx_render/rtx_context.cpp`
**Status:** upstream defect, fixed here.

`DxvkAutoExposure::dispatch()` takes `bool resetHistory = false`. `dispatchToneMapping` called it
without the argument — while computing exactly the right value on the very next line for the tone
mapper:

```cpp
autoExposure.dispatch(this, sampler, rtOutput, GlobalTime::get().deltaTimeMs());   // no reset
const bool resetToneMapperHistory = m_resetHistory || getSceneManager().getCamera().isCameraCut();
```

So the only reset auto exposure ever saw was first-time resource creation. Eye adaptation eased
across every level load and camera cut.

---

## 6. The histogram covered 7 EV

**Status:** upstream design limit, changed here.

`evMinValue`/`evMaxValue` defaulted to −2 and +5 EV100 and were not only clamps — they were the
histogram's **domain**. 7 EV is narrower than most outdoor scenes, and `saturate()` piled
everything beyond either end into the first and last bins, where it dragged the average.

Now fixed at **−12 to +14 EV100** over the same 256 bins. Cost is nil — same dispatch, same
memory, two constants — at the price of resolution: **0.102 EV per bin** rather than 0.028.

That quantisation interacts with one default: a deadband below ~0.1 EV sits under the noise floor
and will not reliably engage. Shipped at 0.10 rather than the 0.05 originally specified.

---

## 7. Percentile trimming: what it does and does not reject

**Not a defect — a measured property of the new design**, recorded so it is tuned rather than
rediscovered.

`histogramHighPercentile` sets the largest bright region that is rejected **outright**. The rule
is simply `coverage < 1 - highPercentile`. Beyond that, influence grows smoothly.

Exposure swing (EV) caused by a blowout at +11 stops over a normal scene:

| `highPercentile` | 1% of frame | 3% | 5% | 10% | 20% |
|---|---|---|---|---|---|
| 1.00 (no trim) | 0.13 | 0.38 | 0.63 | 1.27 | 2.46 |
| **0.95 (default)** | 0.03 | 0.10 | 0.15 | 0.84 | 2.16 |
| 0.90 | 0.03 | 0.09 | 0.16 | 0.33 | 1.77 |
| 0.80 | 0.03 | 0.08 | 0.15 | 0.29 | 0.66 |

So the default handles a lamp or a distant sun. A muzzle flash filling a fifth of the screen still
moves exposure ~2 EV; that needs `highPercentile` around 0.80. The **trimmed fraction** readout in
the UI is the direct diagnostic.

---

## 8. The local tone mapper's ruler follows the selected operator

**Not a defect. A design decision, recorded with its evidence** — and one that was made twice,
the first time on bad numbers.

The local path is **exposure fusion**: synthesise three exposures from the one image, score each
pixel in each for "well-exposedness" against a target of 0.50, blend per pixel. Building and
scoring those three needs a response curve, and there the operator is a **ruler, not a look**
(three uses in `luminance.comp.slang`, one more as a local intensity probe in
`final_combine.comp.slang` — the five-per-pixel cost is §9's).

> **Correction.** An earlier revision argued for pinning the ruler to ACES, on the grounds that
> AgX was "roughly 4× weaker in the midtones" and worse calibrated to the 0.50 target. **Both
> claims were wrong** — they came from an analysis script carrying the same transposed
> inset/outset matrices later fixed in `agx.slangh`. The tables below are recomputed from the
> shipped constants.

Fusion weight spread (max − min; higher = stronger local adaptation):

| scene brightness | ACES | AgX | GT7 |
|---|---|---|---|
| −8 stops | 0.048 | **0.056** | **0.056** |
| −6 | 0.113 | **0.169** | 0.116 |
| −4 | 0.255 | **0.259** | 0.217 |
| −2 | 0.182 | 0.141 | **0.217** |
| **mid grey** | **0.321** | 0.144 | 0.256 |
| +2 | 0.256 | 0.287 | **0.397** |
| +4 | 0.061 | **0.181** | 0.000 |
| +6 | 0.000 | **0.037** | 0.000 |
| **mean** | 0.162 | **0.166** | 0.158 |

The means are within 5% of each other, so switching the ruler **redistributes where local
adaptation acts rather than weakening it**: ACES is strongest around mid grey and dead above +4
stops, AgX spreads further into shadows and highlights, and GT7 peaks at +2 but goes flat above
+3 — by then all three of its synthetic exposures have reached display white and have nothing
left to disagree about.

Calibration against the hardcoded 0.50 target — the scene value each ruler calls "correctly
exposed":

| ruler | linear | offset from true mid grey |
|---|---|---|
| ACES | 0.151 | −0.25 stops |
| AgX | 0.183 | **+0.03 stops** |
| GT7 | 0.218 | +0.27 stops |

All three sit inside ±0.3 stops, and AgX is the best calibrated of them — the reverse of what the
earlier revision claimed.

**So the ruler follows the selected operator.** ACES keeps behaving exactly as it does in stock
Remix; AgX and GT7 are each judged on their own response curve. `None` keeps ACES, because the
fusion still needs something to measure with when no final look is applied.

**One rule governs what the ruler sees: it measures the operator's CURVE, never the artistic look
layered on top of it.** AgX's `minEv`/`maxEv` and GT7's peak and shoulder stay, because those
shape the response being measured; AgX's ASC-CDL look transform and GT7's saturation boost are
neutralised inside `localTonemapRuler()`. Without that, dragging a saturation slider would change
local contrast as a side effect, which is not what any of those sliders claims to do.

The one genuine cost is that the shipped `rtx.localtonemap.shadows` / `highlights` /
`exposurePreferenceSigma` defaults were tuned against the ACES response, so they may want
revisiting per operator. That is a tuning job, not a correctness problem, and
`exposurePreferenceSigma` remains the direct control over local effect strength.

---

## 9. What the operators cost

**Estimated, not measured.** These are transcendental (SFU) operation counts taken from the
shaders, divided by theoretical SFU throughput. Real cost will be higher - branch divergence,
cache behaviour, no co-issue - so treat them as a floor and roughly double for planning.

The local path evaluates the selected operator **five times per pixel**: three to build the
synthetic exposures in `luminance.comp.slang`, once as the local intensity probe in
`final_combine.comp.slang`, and once for the final look. The global path evaluates it once.
That 5x multiplier is free for ACES and is not free for GT7.

| operator | path | SFU/pixel | 1080p | 1440p | 4K |
|---|---|---|---|---|---|
| ACES (legacy) | Global | 0 | ~0 ms | ~0 ms | ~0 ms |
| ACES (legacy) | Local | 0 | ~0 ms | ~0 ms | ~0 ms |
| AgX | Global | 9 | 0.00 - 0.01 ms | 0.01 - 0.02 ms | 0.01 - 0.05 ms |
| AgX | Local | 45 | 0.01 - 0.06 ms | 0.03 - 0.10 ms | 0.06 - 0.23 ms |
| GT7 | Global | 51 | 0.02 - 0.07 ms | 0.03 - 0.12 ms | 0.07 - 0.26 ms |
| GT7 | Local | 255 | 0.08 - 0.33 ms | 0.15 - 0.58 ms | 0.33 - 1.31 ms |

Per evaluation: ACES-legacy is pure ALU; AgX is ~9 (three `log2`, three `pow`); GT7 is 51,
almost all of it the PQ transfer function - six `gt7InverseEotfSt2084` across the two ICtCp
conversions and three `gt7EotfSt2084` coming back.

**A LUT would flatten this to one texture fetch, and is the wrong trade here.** The reason to run
GT7 is hue accuracy under 1 degree, and trilinear interpolation between LUT nodes interpolates
linearly in encoded RGB — the operation that causes hue error in the first place — at ~3% node
spacing on a 33-cubed grid. It would also need a log shaper (the input is unbounded HDR, a 3D LUT
is indexed on [0,1]^3), and the chroma fade is a smoothstep across a narrow 0.98-1.16 band that
nodes can straddle. Direct evaluation is the faithful choice; revisit only if profiling says
otherwise. *Inference, not established:* the reference ships no LUT because it is a sample
implementation written for clarity, so what the shipping game does is unknown from it.

---

## 10. The GT7 saturation boost is a fork addition, not part of the reference

**Nothing in this section is Polyphony's.** The operator itself is a port whose source of truth
is `shaders/rtx/pass/tonemap/reference/gt7_tone_mapping.cpp` — diff against that file, and fix
the port rather than the reference; `gt7.slangh`'s own header carries why the operator exists,
the ICtCp mechanism, and the two unit/gamma traps. What follows is the fork's addition on top:
GT7's photographic restraint is the point of it, but it can read as flat next to a punchier
operator, so `rtx.tonemap.gt7SaturationBoost` lifts lit surfaces while leaving the sky — the
thing GT7 was chosen for — alone.

**Why intensity and not chroma.** The obvious gate is "boost low-chroma things", but measured in
ICtCp the sky does not separate that way: deep sunset sky reaches chroma 0.219, higher than any
surface sampled, while horizon haze sits at 0.036, lower than most. Intensity separates cleanly,
because the sky is a light source and the surfaces are what it illuminates:

| class | ICtCp intensity (÷ target) | measured chroma gain at boost 2.0 |
|---|---|---|
| lit surfaces | 0.53 – 0.76 | 1.37× – 1.55× |
| **sky** | **0.82 – 0.83** | **1.03× – 1.05×** |
| lava, torch flame | 0.95 – 1.18 | **1.00×** |

The boost fades out across a knee at 0.70 – 0.85, so surfaces get it in full, sky gets a few
percent, and bright emissives get none.

**Why it does not rotate hue.** It scales the ICtCp Ct and Cp components by the same factor, and
`atan2(Cp, Ct)` is invariant under that — so hue is preserved by construction, not by tuning. The
only residual is gamut clipping on the way back to Rec.709. Measured ICtCp hue drift:

| boost | worst-case drift |
|---|---|
| 1.5× | **1.03°** |
| 2.0× | **3.86°** (deeply saturated blue) |

> **Do not measure this in CIELAB.** CIELAB reports up to 11.8° on the same blues where ICtCp
> reports 0.50°. That is CIELAB's own blue hue non-linearity — the defect ICtCp and CAM16 were
> built to fix — and not an error in the boost. An earlier pass at this nearly led to capping the
> slider far tighter than the evidence justified.

**Verified inert at the default.** At 1.0 the term evaluates to exactly 1.0 and the operator is
bit-identical to the reference — checked at 0.00e+00 deviation across sky, surface and emissive
samples.

The boost is neutralised inside `localTonemapRuler()`, alongside AgX's look transform, under the
rule stated in §8: the ruler measures the operator's curve, never the look on top of it.

---

## What was verified, and what was not

**Confirmed in game** (2026-08-07, Dusklight, 3440x1440, DLSS Quality + 2x frame generation):

- GT7 holds the physically scattered sky "considerably better" than the alternatives — the
  headline claim of §8/§9 and the reason the operator was added.
- GT7 retains more colour in strong emissives such as lava.
- `rtx.bloom.dusklightThreshold` at 0.485 under GT7 "noticeably helped", confirming the
  best-fit figure derived in the bloom analysis.
- The GT7 saturation boost (§10) behaves as designed: surfaces lift, sky does not.
- GT7 in **Local** mode was reported as fine for performance. Treat this as reassurance, not
  measurement: it was an unscientific test with DLSS upscaling and frame generation active, both
  of which change how much of the frame the full-resolution tone mapping passes actually cover.
  The 5x per-pixel evaluation cost in §9 is unmeasured and the 4K figures there remain estimates.

**Verified numerically** against a model of the shader maths: metering identity at mid grey;
incomplete-adaptation endpoints at strength 0 and 1; soft limiter monotonicity and asymptotic
bounds; frame-rate independence of the temporal blend at 30/60/144 fps; black-pixel immunity;
blowout rejection across the table in §7; all AgX matrices white-preserving and correctly
transposed; sRGB↔Rec.2020 round trip to 1e-4; grey-stays-grey to 1e-4; AgX monotonic over
1e-4..1e1; no NaN, negative or out-of-range output across 3 looks × 3 contrasts × 2 saturations ×
7 magnitudes × 4 hues. Push constant sizes checked at 80, 96 and 80 of 128 bytes.

**Verified by compiler:** CI only. There is no MSVC or slang toolchain in the environment these
changes were written in.

**Not verified:** the five test cases in the original brief — walking between interior and
exterior, muzzle flash, staring at a lamp, level transitions, AgX vs ACES on saturated
emissives — all remain untested. (This paragraph used to say "no frame of this has been
rendered", which the 2026-08-07 observations four paragraphs above contradict; what is
untested is those five cases, not the whole feature.)
