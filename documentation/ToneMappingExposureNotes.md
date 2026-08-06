# Tone mapping and auto exposure — defects found, and what changed

Written up separately from the commits because several of these are **upstream
dxvk-remix defects, not fork-specific ones**, and are worth carrying back on their own.

Every measurement here comes from a numerical model of the shader maths, run against the
constants actually in the tree. **Nothing in this document has been observed in game.** Where a
claim is a mechanism rather than an observation, it says so.

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

## 8. Why AgX was not put inside the local tone mapper

**Not a defect. A design decision, recorded with its evidence** because the code now looks
deliberately inconsistent and would otherwise invite a "fix".

The local path is **exposure fusion**: synthesise three exposures from the one image, score each
pixel in each for "well-exposedness" against a target of 0.50, blend per pixel. It calls ACES in
three places to *build and score* those synthetic exposures, and once at the end as the look.

In the first three, the operator is a **ruler, not a look**. Fusion weight spread (max − min;
higher = stronger local adaptation):

| scene brightness | ACES (current) | AgX |
|---|---|---|
| −8 stops | 0.048 | **0.056** |
| −6 | 0.113 | **0.169** |
| −4 | 0.255 | **0.259** |
| −2 | **0.182** | 0.141 |
| **mid grey** | **0.321** | 0.144 |
| +2 | 0.256 | **0.287** |
| +4 | 0.061 | **0.181** |
| +6 | 0.000 | **0.037** |

> **Correction.** An earlier revision of this document claimed AgX was "roughly 4× weaker in the
> midtones" and that the fusion's hardcoded 0.50 well-exposedness target was better calibrated to
> ACES. **Both were wrong**, produced by a throwaway analysis script that carried the same
> transposed inset/outset matrices later caught and fixed in `agx.slangh`. The table above is
> recomputed from the shipped constants.

What the corrected numbers actually say is narrower and more mixed. AgX is weaker only around mid
grey (0.144 against 0.321, about 2.2×) and at −2 stops; it is **equal or stronger everywhere
else**, markedly so in deep shadows and in the highlights. And on calibration the result reverses:
the 0.50 target corresponds to a scene value **+0.03 stops** off true mid grey under AgX against
**−0.25 stops** under ACES, so AgX is the better-calibrated ruler, not the worse one.

So only one of the three original arguments survives: the shipped
`shadows`/`highlights`/`exposurePreferenceSigma` defaults are tuned against the ACES response, and
swapping the operator silently invalidates that tuning. That is a real cost but a re-tunable one.

**The case for leaving the internals on ACES is therefore much weaker than first stated.** Trying
AgX as the ruler is a reasonable experiment — expect flatter midtone adaptation, better shadow and
highlight separation, and a re-tune of those three sliders. It was not done here only because the
decision to keep it was taken on the strength of the bad numbers.

**If a stronger local effect is wanted without changing the operator,
`exposurePreferenceSigma` is the direct control.**

---

## What was verified, and what was not

**Verified numerically** against a model of the shader maths: metering identity at mid grey;
incomplete-adaptation endpoints at strength 0 and 1; soft limiter monotonicity and asymptotic
bounds; frame-rate independence of the temporal blend at 30/60/144 fps; black-pixel immunity;
blowout rejection across the table in §7; all AgX matrices white-preserving and correctly
transposed; sRGB↔Rec.2020 round trip to 1e-4; grey-stays-grey to 1e-4; AgX monotonic over
1e-4..1e1; no NaN, negative or out-of-range output across 3 looks × 3 contrasts × 2 saturations ×
7 magnitudes × 4 hues. Push constant sizes checked at 80, 96 and 80 of 128 bytes.

**Verified by compiler:** CI only. There is no MSVC or slang toolchain in the environment these
changes were written in.

**Not verified at all:** anything about how this looks. No frame of this has been rendered.
The five test cases in the original brief — walking between interior and exterior, muzzle flash,
staring at a lamp, level transitions, AgX vs ACES on saturated emissives — all remain untested.
