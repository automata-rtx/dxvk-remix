/*
* Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#include "rtx_context.h"
#include "rtx_auto_exposure.h"
#include "rtx_dusklight_atmosphere.h"
#include "rtx_dusklight_game.h"
#include "rtx_scene_manager.h"
#include "rtx_light_manager.h"
#include "rtx_options.h"
#include "rtx_types.h"
#include "dxvk_device.h"
#include "dxvk_objects.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/dusklight/dusklight_sky.h"
#include "rtx/pass/dusklight/dusklight_sky_stats.h"
#include "rtx/pass/dusklight/dusklight_atmosphere.h"
#include "rtx/pass/dusklight/dusklight_composite_args.h"
// Note: Included for the definition only, the same way dusklight_composite_args.h is - the header
// forward declares VolumeArgs so this file does not drag the volumetrics' shader-shared struct
// through dxvk_objects.h and into every translation unit.
#include "rtx/pass/volume_args.h"

#include <rtx_shaders/dusklight_sky.h>
#include <rtx_shaders/dusklight_sky_stats.h>
#include <rtx_shaders/dusklight_transmittance.h>
#include <rtx_shaders/dusklight_multiscatter.h>
#include "rtx_imgui.h"
#include "rtx_utils.h"
#include "../../util/util_color.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dxvk {
  // Defined within an unnamed namespace to ensure unique definition across binary
  namespace {
    class DusklightSkyShader : public ManagedShader
    {
      SHADER_SOURCE(DusklightSkyShader, VK_SHADER_STAGE_COMPUTE_BIT, dusklight_sky)

      PUSH_CONSTANTS(DusklightAtmosphereArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(DUSKLIGHT_SKY_OUTPUT)
        SAMPLER2D(DUSKLIGHT_SKY_TRANSMITTANCE)
        SAMPLER2D(DUSKLIGHT_SKY_MULTISCATTER)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DusklightSkyShader);

    class DusklightTransmittanceShader : public ManagedShader
    {
      SHADER_SOURCE(DusklightTransmittanceShader, VK_SHADER_STAGE_COMPUTE_BIT, dusklight_transmittance)

      PUSH_CONSTANTS(DusklightAtmosphereArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(DUSKLIGHT_TRANSMITTANCE_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DusklightTransmittanceShader);

    class DusklightMultiScatterShader : public ManagedShader
    {
      SHADER_SOURCE(DusklightMultiScatterShader, VK_SHADER_STAGE_COMPUTE_BIT, dusklight_multiscatter)

      PUSH_CONSTANTS(DusklightAtmosphereArgs)

      BEGIN_PARAMETER()
        SAMPLER2D(DUSKLIGHT_MULTISCATTER_TRANSMITTANCE)
        RW_TEXTURE2D(DUSKLIGHT_MULTISCATTER_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DusklightMultiScatterShader);

    class DusklightSkyStatsShader : public ManagedShader
    {
      SHADER_SOURCE(DusklightSkyStatsShader, VK_SHADER_STAGE_COMPUTE_BIT, dusklight_sky_stats)

      PUSH_CONSTANTS(DusklightAtmosphereArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D_READONLY(DUSKLIGHT_SKY_STATS_INPUT)
        RW_STRUCTURED_BUFFER(DUSKLIGHT_SKY_STATS_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DusklightSkyStatsShader);

    // Small on purpose. The dome is a smooth gradient with no detail to lose, and every ray that
    // misses geometry samples it, so a compact image stays resident in cache. It doubles as the
    // physical sky's sky-view lookup - that is why there is no fourth Hillaire table here.
    constexpr uint32_t kSkyWidth = 256;
    constexpr uint32_t kSkyHeight = 128;

    // Any nonzero value works; it only has to be stable across frames and distinct from every
    // handle the game's own bridge creates for its analytical lights.
    constexpr uint64_t kDomeLightHandle = 0xD05C11'A7'000501ull;

    // World is Y up; Remix's lat-long sampling puts the pole on Z. This carries the swap, so the
    // generator can work entirely in light space and treat +Z as up. Columns are the images of the
    // world basis vectors, so world +Y lands on light +Z and the U axis' phi = atan2(x, y) ends up
    // measuring the same angle about the up axis that the game reports its sun azimuth in.
    // If the sky ever appears rotated 90 degrees about the horizon, this is the first place to look.
    const Matrix4 kWorldToDomeLight {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
    };

    // A session that visits more distinct fog states than this gets one notice and then silence.
    // Areas reuse a handful of ramps each, so in practice this is never reached; it exists so that
    // an area which retunes its fog every frame cannot fill a disk.
    constexpr size_t kMaxLoggedFogStates = 32;

    // Floor on rtx.dusklight.atmosphere.fogRampFloor, applied where Derived::rampMinRemaining is
    // produced rather than where it is consumed. ADDED 2026-08-18: the option's slider and its own
    // text advertise 0 as a legal value, and it is - the shader divides by max(rampMinRemaining,
    // 1e-4) precisely so that it stays finite - but fillVolumeRampArgs used to refuse a
    // rampMinRemaining of exactly 0 and leave the mode at 0, so setting a documented value turned
    // Exact ramp off while the composite still ran mode 2's residual against an uncapped homogeneous
    // medium and every readout still said "exact ramp". A floor here means the two sides agree that
    // 0 is legal, and it costs nothing: at this fraction the medium's transmittance bottoms out at
    // 0.0001, four decades below anything visible.
    constexpr float kMinRampFloorFraction = 1e-4f;

    // Floor on the scalar extinction, applied in fogRampMode 2 ONLY. ADDED 2026-08-18.
    //
    // In that mode the view ray's optical depth comes from the ramp field and the scalar divides
    // straight back out of it (rtx/algorithm/volume_lighting.slangh), so sigma's remaining jobs are
    // light visibility through the fog and transmittanceMeasurementDistance. But the cancellation is
    // exact only while sigma is above the shader's own 1e-8 reference clamp: at sigma = 0 exactly -
    // which rtx.dusklight.atmosphere.densityScale = 0 produces, and its slider allows - the
    // extinction and the scattering coefficient are both zero, so the medium vanishes entirely with
    // the panel still reporting an exact ramp. This floor is two decades above that clamp and is
    // still a half density distance of ~693,000 units, i.e. no measurable light attenuation, so it
    // preserves what densityScale = 0 is asking for and only stops the mode becoming a silent no-op.
    // Deliberately NOT applied in modes 0 and 1, where sigma = 0 means "no medium at all" and is a
    // legitimate setting - mode 1 with no medium is the composite's ramp on its own.
    constexpr float kExactRampMinSigma = 1e-6f;

    bool isFinite(float v) {
      return !std::isnan(v) && !std::isinf(v);
    }

    Vector3 sanitizeColor(const Vector3& c) {
      const auto clean = [](float v) {
        return isFinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
      };
      return Vector3(clean(c.x), clean(c.y), clean(c.z));
    }
  }

  DxvkDusklightAtmosphere::DxvkDusklightAtmosphere(DxvkDevice* device)
    : RtxPass(device), m_device(device) {
  }

  DxvkDusklightAtmosphere::~DxvkDusklightAtmosphere() {
  }

  bool DxvkDusklightAtmosphere::isEnabled() const {
    return enable() && DusklightEnv::enable();
  }

  // Released on a genuine enable/disable transition only. This was releaseTargetResource() until
  // 2026-08-16, and RtxPass calls that hook from onTargetResize() as well
  // (`if (m_isActive) { releaseTargetResource(); createTargetResource(...); }`), while this class
  // overrides no createTargetResource - so any change of target extent threw away the sky image,
  // both lookup tables and the stats ring, none of which depend on the target extent. prepareSceneData
  // rebuilds them, but it also forces a lookup table regeneration (m_lutSkyColor is reset below) and
  // restarts the readback ring, so the fog fell back to the game's palette colour after every
  // resolution or upscaler change - for the first kMaxFramesInFlight frames on which the stats pass
  // then ran. That count is the gate `m_skyStatsFramesActive > kMaxFramesInFlight` read against a
  // counter this function resets to 0 and the pass pre-increments on each frame it runs, so the
  // frames on which it holds are counter values 1..kMaxFramesInFlight. Stated as the mechanism
  // rather than as a bare number because a reader checking it has to know which end the increment is
  // on. Read from the code and from rtx_resources.cpp; not measured, and not tested in game.
  //
  // Regression signature if this ends up on the wrong hook: not anything visible in the image, but a
  // live image view surviving into shutdown and a validation-layer complaint at device destroy.
  void DxvkDusklightAtmosphere::onDeactivation() {
    m_skyTexture.reset();
    m_skyTextureIndex = UINT32_MAX;
    m_transmittanceLut.reset();
    m_multiScatterLut.reset();
    // Forget what the tables held, or they would be considered current after being destroyed.
    m_lutSkyColor = Vector3(-1.0f, -1.0f, -1.0f);
    m_lutPaletteInfluence = -1.0f;
    // The dome is gone, so its average describes nothing. The flag rather than the value, so the fog
    // falls back to the palette colour instead of holding a stale radiance.
    m_skyStatsGpu = nullptr;
    m_skyStatsHost = nullptr;
    m_skyAmbientValid = false;
    m_skyStatsFramesActive = 0;
  }

  bool DxvkDusklightAtmosphere::active() const {
    return enable() && DusklightEnv::enable() && DusklightEnv::fogActive() && derived().fogValid;
  }

  bool DxvkDusklightAtmosphere::outdoor() const {
    return enable() && DusklightEnv::enable() && !DusklightEnv::skyHidden();
  }

  bool DxvkDusklightAtmosphere::skyActive() const {
    return skyEnable() && outdoor();
  }

  const DxvkDusklightAtmosphere::Derived& DxvkDusklightAtmosphere::derived() const {
    resolveIfStale();
    return m_derived;
  }

  void DxvkDusklightAtmosphere::resolveIfStale() const {
    // Latched per frame rather than resolved from a frame-begin hook, so that no consumer has to
    // know where in the frame it sits relative to the others. The smoothing below depends on being
    // advanced exactly once per frame, which is what the latch buys.
    const uint32_t frameId = m_device->getCurrentFrameId();

    if (m_resolvedFrame == frameId) {
      return;
    }

    m_resolvedFrame = frameId;
    m_derived = resolve();
  }

  // Peak of (medium opacity - the game's ramp) over the ramp's own range.
  //
  // Both curves are known analytically, so this is four candidate points rather than a search. The
  // medium is 1 - exp(-sigma * z); the game is a clamped line. Their difference can only peak where
  // the line starts (the medium has been extinguishing since the camera and the line has not begun),
  // where their slopes are equal, or at an end of the interval.
  float DxvkDusklightAtmosphere::rampExcessPeak(float sigma, float start, float end, float& peakDistance) {
    peakDistance = 0.0f;

    const float span = end - start;

    if (!(span > 0.0f) || !(sigma > 0.0f) || !(end > 0.0f)) {
      return 0.0f;
    }

    const auto excessAt = [&](float z) {
      const float mediumOpacity = 1.0f - std::exp(-sigma * z);
      const float gameOpacity = std::clamp((z - start) / span, 0.0f, 1.0f);
      return mediumOpacity - gameOpacity;
    };

    // The clear zone's far edge, which is where a positive excess almost always lives. Clamped to
    // zero because a scripted fog bank sets a negative start and then has no clear zone at all.
    float bestDistance = std::max(start, 0.0f);
    float best = excessAt(bestDistance);

    const auto consider = [&](float z) {
      if (z <= 0.0f || z > end) {
        return;
      }

      const float value = excessAt(z);

      if (value > best) {
        best = value;
        bestDistance = z;
      }
    };

    // Where the exponential's slope falls to the line's. Only exists once sigma * span > 1; below
    // that the line is always steeper and the difference is decreasing from the clear zone onward.
    if (sigma * span > 1.0f) {
      consider(std::log(sigma * span) / sigma);
    }

    consider(end);

    peakDistance = bestDistance;

    return std::max(best, 0.0f);
  }

  // The same curves as rampExcessPeak, measured the other direction: how far the game's ramp is
  // ahead of the medium, which is exactly the residual the composite's top up has to supply. Kept
  // separate rather than folded into rampExcessPeak behind a sign flag, because that function's
  // clamp-to-zero and its "start is where a positive excess almost always lives" seed are both
  // statements about the excess direction and neither survives the flip.
  float DxvkDusklightAtmosphere::rampShortfallPeak(float sigma, float start, float end, float& peakDistance) {
    peakDistance = 0.0f;

    const float span = end - start;

    if (!(span > 0.0f) || !(end > 0.0f)) {
      return 0.0f;
    }

    const auto shortfallAt = [&](float z) {
      const float mediumOpacity = sigma > 0.0f ? 1.0f - std::exp(-sigma * z) : 0.0f;
      const float gameOpacity = std::clamp((z - start) / span, 0.0f, 1.0f);
      return gameOpacity - mediumOpacity;
    };

    // Seeded the way rampExcessPeak seeds, and CORRECTED 2026-08-18 because it was not.
    //
    // This used to seed at the ramp's far end and offer max(start, 0) to `consider` below, whose
    // z <= 0 guard then silently threw that candidate away for every ramp starting at or behind the
    // camera - which is every scripted fog bank. That is precisely the case with the largest
    // shortfall in it: at the camera the medium has extinguished nothing while the game is already
    // -start / span fogged, so the peak is that head start and it lives at d = 0. The Lost Woods
    // mist tag (-2000..200, sigma 0.030910) reported exp(-sigma * end) = 0.00207 at 200 units where
    // the answer is 0.90909 at 0 - a 439x under-report, in the fog log line and in the Readouts
    // panel alike. Diagnostic only, since nothing shades from residualPeak, but a lying log is worse
    // than no log.
    float bestDistance = std::max(start, 0.0f);
    float best = shortfallAt(bestDistance);

    const auto consider = [&](float z) {
      if (z <= 0.0f || z > end) {
        return;
      }

      const float value = shortfallAt(z);

      if (value > best) {
        best = value;
        bestDistance = z;
      }
    };

    // The ramp's far end, which is where the shortfall lives whenever the medium is thin: the game
    // closes to fully opaque there and an exponential never does.
    consider(std::max(end, 0.0f));

    // The stationary point rampExcessPeak uses is deliberately NOT a candidate here, and the comment
    // that stood in its place had the derivation inverted - it claimed the point "is a maximum of the
    // shortfall exactly when it is a minimum of the excess", which is backwards. The medium's opacity
    // is concave (d2/dz2 of 1 - exp(-sigma z) is -sigma^2 exp(-sigma z) < 0 everywhere) and the
    // ramp is linear, so the excess m - f is concave - which is why rampExcessPeak wants its
    // stationary point - and the shortfall f - m is convex. A stationary point of a convex function
    // is its minimum, so it can never win the comparison above; it was evaluated and rejected on
    // every call. Removed rather than kept for symmetry, since keeping it is what would make the
    // inverted sentence look load-bearing on the next read. Corrected 2026-08-18.

    peakDistance = bestDistance;

    return std::max(best, 0.0f);
  }

  // Extinction of a homogeneous medium that reproduces the game's own ramp at one anchor distance.
  //
  // CORRECTED 2026-08-17, and the correction is the whole reason this is a function rather than a
  // line. It used to be ln(2) / anchor, which asserts the game is *half* opaque at the anchor. That
  // is true only while the anchor is the ramp's midpoint - which is where it starts, and which the
  // clamp in resolve() then moves. Scripted fog banks are precisely the case that moves it: the Lost
  // Woods mist tag calls dKy_fog_startendz_set(-2000, 200, ratio)
  // (dusklight-ao/src/d/actor/d_a_kytag01.cpp:94), whose midpoint is -900, so the anchor clamps to
  // zHalfMin = 100 - where the game's ramp is (100 + 2000) / 2200 = 0.955 opaque, not 0.500. The old
  // expression gave that whiteout a medium that was half clear at the point the original had almost
  // closed, and every scripted fog bank in the game had the same defect.
  //
  // Reduces to the old expression exactly when the clamp does not bind: at the midpoint the game's
  // opacity is (mid - start) / (end - start) = 0.5, and -ln(1 - 0.5) is ln(2).
  float DxvkDusklightAtmosphere::sigmaMatchingRampAt(float anchorDistance, float start, float end) {
    const float span = end - start;

    if (!(anchorDistance > 0.0f) || !(span > 0.0f)) {
      return 0.0f;
    }

    // Clamped below 1 because the game's ramp really does reach fully opaque, and no finite
    // extinction reproduces that: -ln(0) is infinite. 0.999 caps the medium at ln(1000) / anchor,
    // which is a whiteout rather than a divide by zero.
    //
    // WHEN IT BINDS, corrected 2026-08-18. This comment used to say the clamp "cannot be reached
    // from the anchor's own definition - the anchor is at least the midpoint, where the ramp is
    // 0.5", and that is false. resolve() sets the anchor to max(midpoint, max(zHalfMin, 1e-3)) and
    // zHalfMin defaults to 100, so any ramp ending at or below about 100 units puts the anchor at or
    // past the ramp's end, where the ramp is already fully opaque and this clamp is what picks
    // sigma - an arbitrary constant rather than anything about the game's fog. That is inside the
    // stated design envelope, not outside it: the zHalfMin option advertises "a scripted whiteout
    // closing in over two metres", and two metres is 200 units. Left as a clamp rather than moved
    // onto the anchor because the anchor's definition is resolve()'s and not this function's to
    // depend on; what changes here is only that the case is now written down.
    const float gameOpacity = std::clamp((anchorDistance - start) / span, 0.0f, 0.999f);

    if (!(gameOpacity > 0.0f)) {
      return 0.0f;
    }

    return -std::log(1.0f - gameOpacity) / anchorDistance;
  }

  float DxvkDusklightAtmosphere::solveSigmaWithinTolerance(float sigmaMatched, float start, float end,
                                                           float tolerance) {
    float unusedDistance = 0.0f;

    if (!(sigmaMatched > 0.0f) || rampExcessPeak(sigmaMatched, start, end, unusedDistance) <= tolerance) {
      return sigmaMatched;
    }

    // The peak rises monotonically with sigma - every point of the medium's curve does, and the ramp
    // it is measured against does not move - so a plain bisection converges without a starting guess
    // and cannot land on a local answer. 40 steps takes the bracket below single precision.
    float low = 0.0f;
    float high = sigmaMatched;

    for (int i = 0; i < 40; ++i) {
      const float mid = (low + high) * 0.5f;

      if (rampExcessPeak(mid, start, end, unusedDistance) <= tolerance) {
        low = mid;
      } else {
        high = mid;
      }
    }

    return low;
  }

  float DxvkDusklightAtmosphere::resolveExposureCorrection(bool outdoor) const {
    const int mode = std::clamp(exposureFogMode(), 0, 2);

    if (mode == 0) {
      return 1.0f;
    }

    // Indoors only. "Indoors" is the game's own statement rather than ours - skyHidden, which
    // resolveIfStale has already folded into Derived::outdoor. It is also the case where there is
    // no dome to take a real radiance from, so the palette colour is doing all the work and this is
    // the whole of the correction rather than a share of it.
    if (mode == 1 && outdoor) {
      return 1.0f;
    }

    const float exposure = m_device->getCommon()->metaAutoExposure().getExposureMultiplier();

    // Clamped rather than trusted. The multiplier comes off a GPU readback of a histogram
    // reduction, and one bad frame scaling the fog by 1e30 would be a white screen rather than a
    // subtle artifact.
    if (!isFinite(exposure) || !(exposure > 0.0f)) {
      return 1.0f;
    }

    return 1.0f / std::clamp(exposure, 1e-4f, 1e4f);
  }

  DxvkDusklightAtmosphere::Derived DxvkDusklightAtmosphere::resolve() const {
    Derived out = {};
    out.outdoor = DusklightEnv::enable() && !DusklightEnv::skyHidden();
    // Resolved before the fog checks below, deliberately: an area can have a sky and no fog, and
    // the sky must not go dark just because nobody put haze in the room.
    out.physicalWeight = resolvePhysicalWeight();
    out.fogAnisotropy = std::clamp(fogAnisotropy(), -0.95f, 0.95f);

    if (!enable() || !DusklightEnv::enable() || !DusklightEnv::fogActive()) {
      return out;
    }

    const float start = DusklightEnv::fogStartZ();
    const float end = DusklightEnv::fogEndZ();

    // The game leaves these completely unclamped, and its debug menu can drive them to millions in
    // either direction, so nothing about the feed is trusted here.
    if (!isFinite(start) || !isFinite(end) || !(end > start) || !(end > 0.0f)) {
      return out;
    }

    // Match the game's ramp at one anchor distance - its own midpoint, or the floor below. Scale
    // free, so one expression covers a scripted whiteout closing in over two metres and an open
    // field hazing out over two hundred, with no per area handling anywhere.
    // DusklightAtmosphere.md §5.1.
    //
    // Scripted fog banks deliberately put the ramp's start behind the camera, which drags the
    // midpoint behind it too; the clamp is what stops the density running away there. What the
    // medium is matched to *at* that clamped anchor is the game's actual opacity there and not 0.5 -
    // see sigmaMatchingRampAt for why assuming 0.5 made every scripted fog bank far too thin.
    const float anchorDistance = std::max((start + end) * 0.5f, std::max(zHalfMin(), 1e-3f));

    out.sigmaMatched = std::max(sigmaMatchingRampAt(anchorDistance, start, end), 0.0f) *
                       std::max(densityScale(), 0.0f);
    out.sigma = out.sigmaMatched;
    out.rampStart = start;
    out.rampEnd = end;

    // sigma stays at the half density match for now. Holding it under the game's own ramp happens
    // further down, once fogAmbient exists, because how much haze the clear zone can afford depends
    // on how bright that haze is going to be. See the block below fogAmbient.

    // The game authors its fog colour to be blended over a finished, display referred image. Here
    // it is a quantity of light in a linear frame that has not been tone mapped yet, so something
    // has to be decided about how to read it. Both conversions are computed, one is selected by
    // fogColorSpace, and the pair is logged - because until 2026-08-17 the two fog paths silently
    // made *different* decisions and there was no way to see that from anything but a screenshot.
    //
    // WHICH IS CORRECT: Decoded. fog_col is a display value with no tone mapping anywhere behind it,
    // and this frame is pre-tonemap linear radiance, so using the encoded triple as a radiance is a
    // category error - and not only in level, since decoding also changes the ratios between the
    // channels and therefore the fog's saturation. Raw is the default anyway, because it is the
    // reading the legacy depth path has always used and therefore the one that has actually been
    // judged; Decoded is about 2.3x darker at a mid grey and swapping the default without
    // recalibrating fogRadianceScale would be a look change wearing a correctness fix's clothes.
    //
    // The exposure correction is a separate thought and runs after either. Decoding gamma fixes the
    // colour's *shape*; it does nothing about its *level*, because a display colour has no level
    // until you say what exposure it was meant to be seen at. Dividing by the exposure the
    // tonemapper is about to apply says exactly that, and is what stops one number having to serve
    // both a sunlit field and an unlit cave. exposureFogMode decides where it runs. Note this is why
    // "the two paths agree" is a statement about the authored colour rather than about the final
    // number: the depth path now shares this correction too, because it shares the whole derivation.
    out.paletteRaw = sanitizeColor(DusklightEnv::fogColor());
    out.paletteDecoded = sRGBGammaToLinear(out.paletteRaw);

    const Vector3 paletteRead = fogColorSpace() == 0 ? out.paletteDecoded : out.paletteRaw;

    out.exposureCorrection = resolveExposureCorrection(out.outdoor);
    out.fogRadiance = paletteRead * (std::max(fogRadianceScale(), 0.0f) * out.exposureCorrection);

    // Steer that colour towards the sky the scene is actually standing under.
    //
    // Two things are wrong with the palette colour on its own, and this fixes the second and softens
    // the first. It is a display colour rather than a radiance, so its level means nothing in a
    // linear frame - and the froxel grid is lit by next event estimation over the RTXDI light list,
    // which has no dome light in it, so fog in shadow receives nothing from the sky and falls back
    // to that colour entirely. The dome's mean radiance is a real measurement of the same thing the
    // palette entry was describing, taken in the units the renderer works in.
    //
    // Gated on an actual reduction having run. Until then, and anywhere there is no sky at all, the
    // palette colour stands - which is also exactly the behaviour this had before the change.
    const bool skyAmbientAvailable = m_skyAmbientValid && skyActive() && skyAmbientMode() != 0;

    out.skyAmbientWeight = skyAmbientAvailable ? std::clamp(skyAmbientWeight(), 0.0f, 1.0f) : 0.0f;
    out.fogAmbient = out.fogRadiance;
    out.skyLevelScale = 1.0f;

    if (out.skyAmbientWeight > 0.0f) {
      // Hue only, and this is the correction for a category error that shipped and was measured in
      // game the same day. skyIntensity is a *lighting* calibration - it decides how strongly the
      // dome lights the world against the sun, and it is 6 because the palette colours it scales are
      // small once decoded out of gamma. Handing that radiance to the fog as its colour made the fog
      // about six times brighter than the colour the game authored: with the palette's fog colour at
      // 0.12 the dome-derived ambient measured 0.71. And the medium's ambient term is not shadowed by
      // anything, so an interior received full open-sky in-scatter inside a sealed room, which is
      // what "washed out and bright indoors" was.
      //
      // Normalising by luminance keeps everything the dome was worth - what colour the sky is, and,
      // through the composite's own view-direction sample, how that colour varies across the frame -
      // and takes the level from the palette instead, where the exposure correction has already given
      // it a meaning. The same scale goes to the composite so the near and far halves stay one fog.
      if (skyAmbientMode() == 1) {
        const float domeLuminance = sRGBLuminance(m_skyAmbient);
        const float paletteLuminance = sRGBLuminance(out.fogRadiance);

        out.skyLevelScale = domeLuminance > 1e-6f ? paletteLuminance / domeLuminance : 0.0f;
      }

      const Vector3 domeAmbient = m_skyAmbient * (std::max(skyAmbientScale(), 0.0f) * out.skyLevelScale);

      out.fogAmbient = out.fogRadiance * (1.0f - out.skyAmbientWeight) + domeAmbient * out.skyAmbientWeight;
    }

    // Hold the medium under the game's own ramp - see the note above where sigmaMatched is set.
    //
    // The budget is spent in luminance rather than in coverage, which is the correction made on
    // 2026-08-15. clearZoneTolerance alone asks "how much of the near field may the medium cover",
    // and that is only half of what anyone sees: the other half is how bright the thing doing the
    // covering is. The 2026-08-14 Goron Mines log has both cases in it at the same setting - an
    // outdoor ambient at luminance 0.085 with its excess peaking 14137 units away, and a lava-lit
    // interior at 0.357 peaking at 500 units, the second reported as fog far too dense near the
    // player. Same 0.08, four times the light, and the near field is where it lands.
    //
    // Dividing the target by the medium's own luminance makes the *veil* the constant instead, and
    // clearZoneTolerance stays as the ceiling so nothing dim or outdoor moves: there the quotient
    // lands above the ceiling and the ceiling wins, exactly as before.
    const float toleranceCeiling = std::clamp(clearZoneTolerance(), 0.0f, 0.5f);
    const float veilTarget = std::max(clearZoneVeilTarget(), 0.0f);
    const float ambientLuminance = sRGBLuminance(out.fogAmbient);

    out.clearZoneToleranceUsed = toleranceCeiling;

    if (veilTarget > 0.0f && ambientLuminance > 1e-6f) {
      out.clearZoneToleranceUsed = std::min(toleranceCeiling, veilTarget / ambientLuminance);
    }

    const int rampMode = std::clamp(fogRampMode(), 0, 2);

    if (rampMode == 1 && limitDensityToRamp()) {
      out.sigma = solveSigmaWithinTolerance(out.sigmaMatched, start, end, out.clearZoneToleranceUsed);
    }

    if (rampMode == 2) {
      // Both numbers are known in closed form here rather than searched, and neither is what
      // rampExcessPeak/rampShortfallPeak would return: those describe a *homogeneous* medium of
      // extinction sigma, and no such medium is in front of the camera in this mode. sigma is still
      // carried and still used - by the light visibility path, which has no camera to measure a
      // radial distance from - so it is not meaningless, it is just not what the view ray sees.
      //
      // EXCESS IS EXACTLY ZERO, and that is a property rather than an assertion: the field starts
      // extinguishing at max(rampStart, 0) and follows 1 / (end - d), so its opacity is
      // (d - a) / (end - a) with a = max(rampStart, 0), which is <= (d - rampStart) / (end - rampStart)
      // for every d whenever rampStart <= a. So the medium can never out-fog the game's ramp, the
      // residual can never be clamped, and limitDensityToRamp has nothing to do.
      out.excessPeak = 0.0f;
      out.excessPeakDistance = 0.0f;

      // THE RESIDUAL IS NOT ALWAYS ZERO, and the earlier revision of this comment said it was.
      // Writing a = max(rampStart, 0), the medium's opacity is m(d) = (d - a) / (end - a) and the
      // game's is f(d) = (d - start) / (end - start), so the top-up residual
      // (f - m) / (1 - m) collapses to the constant -start / (end - start) - which is f(0), the fog
      // the original had *already applied at the camera*. A medium that begins at the camera cannot
      // produce that, whatever its density; the composite's ramp adds it as a flat veil instead, and
      // the identity in DusklightAtmosphere.md §5.2 still lands on S * (1 - f) + A * f exactly.
      //
      // So: zero whenever the game's ramp starts at or beyond the camera, and a constant equal to
      // the ramp's head start when it starts behind it - which scripted fog banks routinely do.
      // Checked numerically against the field itself on 2026-08-17 over four real ramps, including
      // the Lost Woods mist tag's -2000..200 (predicted 0.909091, measured 0.909090..0.909092).
      out.residualPeak = std::max(-start, 0.0f) / (end - start);
      // Constant with distance, so there is no peak location to report. Zero rather than a made-up
      // number, and the log says "constant" beside it.
      out.residualPeakDistance = 0.0f;

      // The divergence clamp, as a distance. Measured against the ramp's *visible* range - from the
      // camera, not from a rampStart that may be far behind it - so the medium's floor transmittance
      // is exactly fogRampFloor in every case. Against the full span it is not: the Lost Woods ramp
      // spans 2200 units of which only 200 are in front of the camera, so a 1% floor of the span
      // would have frozen the medium over the last 11% of what anyone can see.
      //
      // Floored at kMinRampFloorFraction rather than passed through, so that a fogRampFloor of 0 -
      // which the slider allows and the option's own text advertises - stays in this mode instead of
      // silently dropping out of it. The multiplier is always positive here: end > start and
      // end > 0 are both already established, so (end - max(start, 0)) > 0 either way.
      out.rampMinRemaining = std::max(std::clamp(fogRampFloor(), 0.0f, 0.25f), kMinRampFloorFraction) *
                             (end - std::max(start, 0.0f));

      // See kExactRampMinSigma. sigma is not what the view ray sees in this mode, but it is what
      // light visibility through the fog is attenuated by, and at exactly zero the medium disappears
      // altogether. Applied after sigmaMatched is recorded, so the readouts still show the anchor
      // match the settings actually produced and the floor is visible as the difference.
      out.sigma = std::max(out.sigma, kExactRampMinSigma);
    } else {
      out.excessPeak = rampExcessPeak(out.sigma, start, end, out.excessPeakDistance);
      out.residualPeak = rampShortfallPeak(out.sigma, start, end, out.residualPeakDistance);
    }

    // Size the grid from the game's own fog range, so its fixed slice count lands where the fog
    // actually is. This costs nothing: the slice count does not change, only how far it reaches.
    const float meterToWorld = RtxOptions::getMeterToWorldUnitScale();
    const float minDistance = std::max(froxelMaxDistanceMinMeters(), 0.0f) * meterToWorld;
    const float maxDistance = std::max(froxelMaxDistanceMaxMeters() * meterToWorld, minDistance);
    // froxelRangeScale is deliberately below 1 so the composite's ramp closes the fog that an
    // exponential medium can only asymptote towards. In mode 2 the medium closes the ramp itself, so
    // that reason is gone - and leaving the far stretch outside the grid there would be actively
    // wrong, because the surface would arrive correctly attenuated with none of the fog's colour
    // deposited on it (the in-scatter is only integrated inside the grid, while the top-up residual
    // that would otherwise supply it is zero in this mode). The metre ceiling still binds, and when
    // it does the residual comes back and covers the difference the way it does in mode 1; the log
    // prints the grid's reach against the ramp's end so that case is visible.
    const float rangeScale = rampMode == 2 ? std::max(froxelRangeScale(), 1.0f)
                                           : std::max(froxelRangeScale(), 0.0f);
    const float target = std::clamp(end * rangeScale, minDistance, maxDistance);

    // Snap rather than ease when the target jumps by more than this much in either direction. Easing
    // is right for the palette drifting through a sunrise; it is wrong for a room change or an area
    // load, where easing would drag the grid through a range that matches neither the room left nor
    // the one entered, for as long as the ease takes.
    constexpr float kSnapRatio = 4.0f;

    const bool noHistory = m_smoothedFroxelMaxDistance <= 0.0f || !isFinite(m_smoothedFroxelMaxDistance);
    const bool jumped = !noHistory &&
                        (target > m_smoothedFroxelMaxDistance * kSnapRatio ||
                         target * kSnapRatio < m_smoothedFroxelMaxDistance);

    if (noHistory || jumped) {
      m_smoothedFroxelMaxDistance = target;
    } else {
      const float rate = std::clamp(froxelSmoothingRate(), 0.0f, 1.0f);
      m_smoothedFroxelMaxDistance += (target - m_smoothedFroxelMaxDistance) * rate;
    }

    out.froxelMaxDistance = m_smoothedFroxelMaxDistance;
    out.fogValid = true;

    logFogStateOnce(out);

    return out;
  }

  // Everything the fog's level and shape depend on, in one line, once per distinct state.
  //
  // It exists so that "the fog looks wrong in this cave" can be answered from a log rather than by
  // asking someone to describe a colour. The three numbers that actually decide the look are the two
  // extinctions (equal unless the clear zone cap is binding, and their ratio is how much density the
  // cap is spending), the peak excess (how far the medium out-fogs the original, which is what the
  // top up ramp cannot undo), and the ambient with its source (palette or dome, which is what decides
  // whether a dark room's fog can lift the blacks).
  void DxvkDusklightAtmosphere::logFogStateOnce(const Derived& d) const {
    // Quantised so that a fog range easing through a transition reports its endpoints rather than
    // every frame in between. Coarse on purpose: two states that differ by less than these steps
    // cannot look different.
    const auto bucket = [](float value, float step) {
      return static_cast<int64_t>(std::llround(value / step));
    };

    uint64_t key = 1469598103934665603ull;

    const auto mix = [&key](int64_t value) {
      key ^= static_cast<uint64_t>(value);
      key *= 1099511628211ull;
    };

    // Keyed on the game's own state and on nothing that drifts continuously. The resolved ambient
    // would have been the obvious choice and is the wrong one: it moves with the sun through the
    // dome average and with eye adaptation through the exposure correction, so keying on it would
    // burn through the 32-line cap during one sunrise and then go silent for the rest of the
    // session - a bounded instrument that is bounded in the wrong place. The values printed below
    // are still live; only what counts as a *distinct state* is restricted to the game's.
    const Vector3 paletteFog = sanitizeColor(DusklightEnv::fogColor());

    // Coarse, and the first revision of this was not coarse enough. A session log spent the whole
    // 32-line budget in 450 milliseconds of one area transition, because rampStart and rampEnd sweep
    // continuously through a fade and a 16-unit bucket resolves every frame of it as a new state.
    // The rate limit below is the other half of the fix; neither alone is enough, because a coarse
    // key still steps several times across a long fade and a rate limit alone would suppress a
    // genuinely new area arriving straight after one.
    // Coarsened again on 2026-08-15, and the reason is worth keeping. A session that went to Goron
    // Mines and Arbiter's Grounds reported no fog line from either: the 32 state budget was spent in
    // the first six and a half minutes, entirely on Hyrule Field, because outdoors the ramp and the
    // palette *drift continuously with the time of day*. The endpoints crawled -11831 -> -20874 and
    // the palette with them, and at a 256 unit bucket every step of that sunrise is a new state.
    // The overflow notice fired correctly and the log was honest; the budget was simply spent
    // somewhere useless before the areas anyone wanted to see were reached.
    //
    // The buckets below are sized against that drift rather than against what is visible: a ramp
    // start moving nine thousand units over seven minutes should read as a handful of states, not
    // ten. The separate indoor budget under kMaxLoggedFogStates is the other half, because no
    // bucketing makes a sunrise finite and an interior must not be starved by one.
    mix(bucket(d.rampStart, 4096.0f));
    mix(bucket(d.rampEnd, 16384.0f));
    mix(bucket(paletteFog.x + paletteFog.y + paletteFog.z, 0.25f));
    mix(bucket(d.skyAmbientWeight > 0.0f ? 1.0f : 0.0f, 1.0f));
    mix(bucket(d.outdoor ? 1.0f : 0.0f, 1.0f));
    // The three switches that decide what this line is *about*, added 2026-08-17. They are not game
    // state, which is the rule for everything above, and they are here as a deliberate exception:
    // each of them exists to be A/B'd, and an A/B whose log does not reprint after the switch moves
    // cannot be read. All three are discrete and only a person moves them, so the cost to the budget
    // is bounded by how many times someone toggles a control rather than by anything the game does.
    mix(bucket(static_cast<float>(std::clamp(fogRampMode(), 0, 2)), 1.0f));
    mix(bucket(static_cast<float>(std::clamp(fogColorSpace(), 0, 1)), 1.0f));
    mix(bucket(fogColorDirectional() ? 1.0f : 0.0f, 1.0f));

    // Indoors and outdoors get separate budgets. Outdoor states drift with the sun and interiors do
    // not, so one shared budget is always spent by the sky - which is exactly what happened on
    // 2026-08-15, and it made a truncated log look like an area with no fog at all.
    auto& seenStates = d.outdoor ? m_loggedFogStates : m_loggedFogStatesIndoor;
    bool& overflowed = d.outdoor ? m_loggedFogStateOverflow : m_loggedFogStateOverflowIndoor;

    for (const uint64_t seen : seenStates) {
      if (seen == key) {
        return;
      }
    }

    // At most one line every kFogLogFrameGap frames, whatever the key says. A transition is a
    // continuum, not a sequence of states, and no bucketing turns it into one.
    constexpr uint32_t kFogLogFrameGap = 120;

    const uint32_t frameId = m_device->getCurrentFrameId();

    if (m_lastFogLogFrame != UINT32_MAX && frameId - m_lastFogLogFrame < kFogLogFrameGap) {
      return;
    }

    m_lastFogLogFrame = frameId;

    if (seenStates.size() >= kMaxLoggedFogStates) {
      if (!overflowed) {
        overflowed = true;
        Logger::info(str::format(
          "[Dusklight] fog: ", kMaxLoggedFogStates, " distinct ",
          d.outdoor ? "OUTDOOR" : "INDOOR",
          " fog states reported; further ", d.outdoor ? "outdoor" : "indoor",
          " states are suppressed for the rest of this run. The other budget is unaffected - "
          "indoors and outdoors are counted separately precisely so a drifting sky cannot silence "
          "an interior."));
      }
      return;
    }

    seenStates.push_back(key);

    const auto round2 = [](float value) {
      return std::round(value * 100.0f) / 100.0f;
    };

    const bool capped = d.sigma < d.sigmaMatched * 0.999f;
    const int rampMode = std::clamp(fogRampMode(), 0, 2);

    // The two conversions, side by side, on one line - because "which colour does each path aim at"
    // was unanswerable from a log until 2026-08-17 and the answer turned out to be "different ones".
    // Both paths now consume the resolved ambient printed further down; what is printed here is the
    // choice that produced it, so a fog that reads wrong can be attributed to the convention, to the
    // level, or to the dome without anyone being asked to describe a colour.
    const std::string colourSpaceLine = str::format(
      ", palette raw ", round2(d.paletteRaw.x), ",", round2(d.paletteRaw.y), ",", round2(d.paletteRaw.z),
      " decodes to ", round2(d.paletteDecoded.x), ",", round2(d.paletteDecoded.y), ",", round2(d.paletteDecoded.z),
      " and this fog is using the ", fogColorSpace() == 0 ? "DECODED" : "RAW",
      " reading (rtx.dusklight.atmosphere.fogColorSpace; decoded is the correct one, raw is the "
      "default because it is what the legacy depth fog has always shown)",
      "; the legacy depth fog and the volumetric fog now share this whole derivation and "
      "rtx.fogColorScale no longer applies to either, so both aim at the ambient above - except in "
      "the one case this atmosphere declines to write the fog state at all, when a translucent "
      "material has already replaced the fog, where the depth path keeps the game's own colour and "
      "rtx.fogColorScale with it (the Readouts panel reports which of the two is live)",
      fogColorDirectional()
        ? " - except that the far ramp is sampling the dome in the view direction (fogColorDirectional), "
          "so the near and far halves are deliberately NOT the same colour"
        : "");

    // Derived from what actually reached the medium rather than by re-reading the option, so a
    // fogRampFloor that resolve() floored (kMinRampFloorFraction) cannot print one number here while
    // the shader integrates against another.
    const float visibleRampRange = d.rampEnd - std::max(d.rampStart, 0.0f);
    const float rampFloorPercent = visibleRampRange > 0.0f ? d.rampMinRemaining / visibleRampRange * 100.0f
                                                           : 0.0f;

    // What the composite's ramp actually has left to do. In mode 2 it is a constant over the stretch
    // the medium owns - so a reading that varies there is the regression signature for the exact
    // ramp, and it means the medium did not receive the field it was supposed to. The two places it
    // is *expected* to vary are spelled out below rather than left for someone to report.
    const std::string residualLine = rampMode == 2
      ? str::format(
          ", residual owed to the top-up ", d.residualPeak, " CONSTANT with distance out to ",
          std::llround(d.rampEnd - d.rampMinRemaining), " units",
          d.residualPeak > 1e-6f
            ? str::format(" - the ramp starts ", std::llround(-d.rampStart),
                          " units behind the camera, so the original was already that fogged at the "
                          "camera and no medium beginning there can reproduce it; the ramp adds it as a "
                          "flat veil")
            : std::string(" - the ramp starts at or beyond the camera, so the medium reproduces it "
                          "exactly and the composite adds nothing"),
          ". IT IS NOT CONSTANT OVER THE LAST ", round2(rampFloorPercent), "% of the visible ramp (",
          std::llround(d.rampMinRemaining),
          " units before the end): the divergence clamp freezes the medium there while the game's ramp "
          "keeps closing, so the residual rises off that constant to exactly 1 at the ramp's end, which "
          "is how the last sliver gets closed at all. A residual that varies *before* that point, and "
          "within the grid's reach, is the regression signature for this mode",
          d.froxelMaxDistance < d.rampEnd * 0.999f
            ? str::format(". NOTE the grid reaches only ", std::llround(d.froxelMaxDistance),
                          " of the ramp's ", std::llround(d.rampEnd),
                          " units because the metre ceiling (froxelMaxDistanceMaxMeters) is binding, so "
                          "beyond that point the residual is carrying the fog's colour the way it does in "
                          "mode 1 and varies with distance there - EXPECTED, not the defect above")
            : std::string())
      : str::format(", peak residual the top-up must supply ", round2(d.residualPeak),
                    " at ", std::llround(d.residualPeakDistance), " units");

    Logger::info(str::format(
      "[Dusklight] fog: ramp ", std::llround(d.rampStart), "..", std::llround(d.rampEnd),
      " units, extinction ", d.sigma, "/unit",
      rampMode == 2
        ? std::string(" (scalar; the view ray's medium is the ramp itself, so this is only what light "
                      "visibility through the fog is attenuated by)")
        : (capped ? str::format(" (capped from ", d.sigmaMatched, " to keep the medium under the game's ramp)")
                  : std::string(" (uncapped - the anchor match already fits under the game's ramp)")),
      ", half density at ", d.sigma > 1e-8f ? std::llround(std::log(2.0f) / d.sigma) : 0ll,
      " units, grid reach ", std::llround(d.froxelMaxDistance),
      " units, peak excess over the game's ramp ", round2(d.excessPeak),
      " at ", std::llround(d.excessPeakDistance),
      " units (budget ", d.clearZoneToleranceUsed,
      d.clearZoneToleranceUsed < std::clamp(clearZoneTolerance(), 0.0f, 0.5f) * 0.999f
        ? str::format(", tightened from the ", std::clamp(clearZoneTolerance(), 0.0f, 0.5f),
                      " ceiling because the medium's ambient luminance is ",
                      round2(sRGBLuminance(d.fogAmbient)), " - a bright haze is allowed less of the clear zone")
        : std::string(" - the ceiling, the brightness weighting is not binding here"),
      ")",
      residualLine,
      ", ambient ", round2(d.fogAmbient.x), ",", round2(d.fogAmbient.y), ",", round2(d.fogAmbient.z),
      " (", round2(d.skyAmbientWeight * 100.0f), "% from the sky dome at level scale x", round2(d.skyLevelScale),
      ", the rest from the palette after correction ",
      round2(d.fogRadiance.x), ",", round2(d.fogRadiance.y), ",", round2(d.fogRadiance.z),
      ")",
      colourSpaceLine,
      ", area ", d.outdoor ? "has a sky (the game's own hide_vrbox test says visible)"
                           : "has no sky (the game hides its own dome here)",
      ", dome ", d.skyAmbientWeight > 0.0f ? "on" : "off",
      ", ramp mode ", rampMode == 2 ? "exact ramp" : (rampMode == 1 ? "top-up" : "handover"),
      ", exposure correction x", round2(d.exposureCorrection),
      " (mode ", exposureFogMode() == 0 ? "off" : (exposureFogMode() == 1 ? "indoors only" : "always"),
      ", sampled once at first report - the panel shows it live)",
      ", sky ambient mode ", skyAmbientMode() == 0 ? "off" : (skyAmbientMode() == 1 ? "hue only" : "full radiance"),
      ", anisotropy ", round2(d.fogAnisotropy),
      ". One line per distinct fog state, capped at ", kMaxLoggedFogStates,
      ". See documentation/DusklightAtmosphere.md section 5.2."));
  }

  // Reports each distinct colour pattern that reaches the Palace of Twilight bypass, once per run.
  //
  // Bounded by construction rather than by a rate limit: one bit per pattern, and the game's own
  // pattern switch accepts 0-63, so a whole session cannot produce more than 64 lines plus one
  // notice for anything outside that range. In practice an area uses two or three patterns.
  //
  // Written to be read by someone without the source: it spells out what colpat is, that the
  // bypass is the colpat == 9 one, and whether it fired. The sun readings are here because they
  // decide whether firing changed anything - the next test after this one drops the weight to zero
  // at night anyway, so a bypass that only ever fires in the dark is costing nothing.
  void DxvkDusklightAtmosphere::logColpatOnce(int colpat, bool guardFired) const {
    if (colpat < 0 || colpat > 63) {
      if (m_loggedColpatOutOfRange) {
        return;
      }

      m_loggedColpatOutOfRange = true;
      Logger::warn(str::format(
        "[Dusklight] atmosphere: the game reported colour pattern (colpat) ", colpat,
        ", which is outside the 0-63 range the game's own pattern table accepts - so either the "
        "bridge or the game's field is wrong. Reported once per run. See "
        "documentation/DusklightAtmosphere.md section 8.6."));
      return;
    }

    const uint64_t bit = uint64_t(1) << colpat;

    if ((m_loggedColpatMask & bit) != 0) {
      return;
    }

    m_loggedColpatMask |= bit;

    const float elevation = std::round(DusklightEnv::sunElevation() * 10.0f) / 10.0f;

    Logger::info(str::format(
      "[Dusklight] atmosphere: colour pattern (colpat) ", colpat,
      " reached the physical sky with a visible sky present; sun elevation ", elevation,
      " deg, daylight ", DusklightEnv::sunIsDay() ? "yes" : "no",
      ". Palace of Twilight bypass (fires only on colpat 9, and switches the physical sky off "
      "entirely when it does): ", guardFired ? "FIRED" : "not fired",
      ". One line per distinct colpat per run. See documentation/DusklightAtmosphere.md section "
      "8.6 for what this result decides."));
  }

  float DxvkDusklightAtmosphere::resolvePhysicalWeight() const {
    if (!physicalSky() || !enable() || !DusklightEnv::enable()) {
      return 0.0f;
    }

    // Indoors there is no sky to simulate, and the game says so itself.
    if (DusklightEnv::skyHidden()) {
      return 0.0f;
    }

    // The Palace of Twilight is a colour pattern like any other as far as the game is concerned,
    // but there is no physical description of it to reach for: it has no sun, and its sky is an
    // authored amber rather than anything air does. The model is bypassed there rather than tuned.
    //
    // THE LITERAL'S SOURCE IS THE WRONG INDEX SPACE, verified 2026-08-11. "9 = Palace of Twilight"
    // is true of the game's wolf *sense* vision pattern - dKy_sense_pat_get returns 9 for stage
    // D_MN08 (d_kankyo.cpp:266-270) and the debug combo box that overrides the same value labels
    // entry 9 as "Lv8 only, D_MN08" (d_kankyo.cpp:7469). That is not colpat. colpat is
    // g_env_light.wether_pat1, which indexes stage_envr_info_class::pselect_id[65] and is a
    // separate numbering the game never relates to the sense patterns. Whether any stage authors
    // colpat 9 lives in .dzs stage data, which the game checkout does not contain, so it is
    // UNKNOWN rather than false - and the risk runs both ways: an outdoor stage that happens to
    // select pattern 9 loses its physical sky for no reason.
    //
    // So the guard stays until observed, and the line below is what observes it. What each result
    // means, and what will be done about it, is written down in DusklightAtmosphere.md 8.6.
    constexpr int kPalaceOfTwilightColpat = 9;

    const int colpat = DusklightEnv::colpat();
    const bool guardFires = colpat == kPalaceOfTwilightColpat;

    // Placed here, after the skyHidden early-out, on purpose: reaching this point already means the
    // sky path is live and the area reports a visible sky, which is exactly the case where firing
    // would cost something.
    logColpatOnce(colpat, guardFires);

    if (guardFires) {
      return 0.0f;
    }

    // At night the celestial light is the moon, and a clear-sky scattering model with no sun in it
    // returns very close to black - correct, and useless. The game's night is a deliberately
    // readable blue with stars in it, so it keeps the night outright.
    if (!DusklightEnv::sunIsDay()) {
      return 0.0f;
    }

    const float low = physicalElevationLowDegrees();
    const float high = physicalElevationHighDegrees();
    const float elevation = DusklightEnv::sunElevation();

    float elevationTerm = 1.0f;

    if (high > low) {
      const float t = std::clamp((elevation - low) / (high - low), 0.0f, 1.0f);
      // Smoothstep rather than linear so the handover has no visible edge as the sun climbs.
      elevationTerm = t * t * (3.0f - 2.0f * t);
    } else {
      elevationTerm = elevation >= high ? 1.0f : 0.0f;
    }

    // The game does not hold one colour pattern - it holds a crossfade between two, and every
    // palette colour this weight is blended against is a lerp on the same ratio
    // (dusklight-ao/src/d/d_kankyo.cpp:2406-2409, and the blend itself at :2435 onward). Cutting
    // on the incoming index alone made this term step on the first frame of a transition and stay
    // stepped for its duration, while every colour beside it moved continuously. Worse at the
    // change frame specifically: dKy_change_colpat resets the ratio to 0 without touching the
    // outgoing pattern (:9528-9533), so the frame the index flips on the wire is the frame the
    // palette is still 100% the old pattern.
    //
    // Following the same ratio is the whole fix. It is not a smoothing filter - there is nothing
    // to tune and no state kept here; it is the game's own number, applied to the same lerp the
    // game applies it to.
    //
    // No capture: physicalWeatherWeight is an inline static RtxOption member of this class, not
    // per-instance state, so there is nothing to capture and nothing to keep alive.
    const auto patternWeight = [](int pattern) {
      return pattern == 0 ? 1.0f : std::clamp(physicalWeatherWeight(), 0.0f, 1.0f);
    };

    // DEFAULT-SAFE, and this is the point rather than a nicety. rtx.dusklight.env.colpatBlend
    // defaults to 1.0f and the game pins it at 1.0f whenever no transition is running, so the
    // ordinary reading - and the reading from a game build too old to push it at all - is exactly
    // 1.0f. At blend == 1.0f:
    //
    //     weatherTerm = patternWeight(colpatPrev) * (1.0f - 1.0f) + patternWeight(colpat) * 1.0f
    //                 = patternWeight(colpatPrev) * 0.0f + patternWeight(colpat)
    //                 = patternWeight(colpat)
    //
    // which is the expression that shipped before this change, bit for bit: patternWeight returns
    // either 1.0f or a value already clamped to [0,1], so it is always finite and x * 0.0f is
    // exactly +0.0f rather than NaN, and 0.0f + y is exactly y in IEEE-754 for every finite y.
    // Nothing here can perturb the old result by a single ulp.
    const float blend = std::clamp(DusklightEnv::colpatBlend(), 0.0f, 1.0f);
    const float weatherTerm = patternWeight(DusklightEnv::colpatPrev()) * (1.0f - blend) +
                              patternWeight(colpat) * blend;

    // Deliberately NOT extended to the colpat 9 bypass above. That guard is an unresolved question
    // (section 8.6) currently under instrumentation, it returns before this line so nothing here
    // can bypass it, and giving it a second index to fire on would change what the pending log is
    // measuring. Consequence, stated rather than discovered: a transition *out of* pattern 9 lerps
    // from that pattern's ordinary weather weight, not from the guard's zero. That is a fade at
    // the same moment the guard stops firing, not a new step.
    return std::clamp(physicalMaxWeight(), 0.0f, 1.0f) * elevationTerm * weatherTerm;
  }

  bool DxvkDusklightAtmosphere::mediumChangedSince(const Vector3& skyColor, float influence) const {
    constexpr float kEpsilon = 1e-3f;

    return std::fabs(skyColor.x - m_lutSkyColor.x) > kEpsilon ||
           std::fabs(skyColor.y - m_lutSkyColor.y) > kEpsilon ||
           std::fabs(skyColor.z - m_lutSkyColor.z) > kEpsilon ||
           std::fabs(influence - m_lutPaletteInfluence) > kEpsilon;
  }

  void DxvkDusklightAtmosphere::applyFogOverride(FogState& fog, bool fogReplacedByMaterial) const {
    // Cleared first and set only once the write below has actually happened. rtx_composite.cpp reads
    // it to decide whether rtx.fogColorScale applies, and that decision has to follow this function
    // rather than active(): every early return below leaves the game's own raw D3D9 fog colour in
    // the state, which is exactly the value fogColorScale was there to scale.
    m_fogColorOverridden = false;

    if (!active()) {
      return;
    }

    // A mod that replaces the game's fog with a translucent material has already consumed it into
    // a medium the camera starts inside. Overriding on top of that would leave both running and
    // count the same fog twice.
    if (fogReplacedByMaterial) {
      return;
    }

    const Derived& d = m_derived;
    const float span = d.rampEnd - d.rampStart;

    if (!(span > 0.0f)) {
      return;
    }

    m_fogColorOverridden = true;
    fog.mode = D3DFOG_LINEAR;
    // The resolved radiance, not the palette entry, and not clamped to [0,1].
    //
    // This is half of the 2026-08-17 colour agreement; the other half is in rtx_composite.cpp, which
    // stops applying rtx.fogColorScale to this value while the atmosphere is active. Before that,
    // this path aimed at paletteRaw * rtx.fogColorScale (a gamma-encoded triple used as a radiance,
    // scaled by an option calibrated against a different game's fog) while the volumetric path aimed
    // at the decoded, exposure-referenced, dome-steered fogAmbient - two colours for one fog, with
    // nothing anywhere reconciling them. One derivation now feeds both.
    //
    // Not clamped to 1 on purpose: fogAmbient is a radiance in a pre-tonemap linear frame and the
    // exposure correction routinely takes it above 1 in a dark room. Clamping it here would silently
    // reintroduce the disagreement in exactly the areas exposureFogMode exists for. Still guarded
    // against NaN and negatives, which are the failure modes a bad feed can actually produce.
    fog.color = Vector3(std::max(isFinite(d.fogAmbient.x) ? d.fogAmbient.x : 0.0f, 0.0f),
                        std::max(isFinite(d.fogAmbient.y) ? d.fogAmbient.y : 0.0f, 0.0f),
                        std::max(isFinite(d.fogAmbient.z) ? d.fogAmbient.z : 0.0f, 0.0f));
    fog.end = d.rampEnd;
    fog.scale = 1.0f / span;
    fog.density = 0.0f;
  }

  void DxvkDusklightAtmosphere::applyVolumeArgs(Vector3& attenuationCoefficient,
                                                Vector3& scatteringCoefficient,
                                                float& transmittanceMeasurementDistance,
                                                Vector3& multiScatteringEstimate) const {
    if (!active()) {
      return;
    }

    const Derived& d = m_derived;

    // Scalar extinction, deliberately - see Derived::sigma in the header for why the fog colour
    // must not become a per channel extinction.
    attenuationCoefficient = Vector3(d.sigma, d.sigma, d.sigma);

    const Vector3 albedo = sanitizeColor(singleScatteringAlbedo());
    scatteringCoefficient = Vector3(attenuationCoefficient.x * albedo.x,
                                    attenuationCoefficient.y * albedo.y,
                                    attenuationCoefficient.z * albedo.z);

    // Reported back so the volumetrics' own distance derived terms stay consistent with the
    // medium we just handed it rather than with the option it would have used.
    if (d.sigma > 1e-8f) {
      transmittanceMeasurementDistance = std::log(2.0f) / d.sigma;
    }

    // The colour arrives here rather than through extinction. Without it the fog is only ever as
    // bright as the lights that reach it, and the game's fog is never black - it was a blend
    // towards an authored colour, not a simulation, so an unlit room still had coloured fog. It is
    // also the only thing the fog receives from the sky, since the froxel grid's next event
    // estimation runs over the RTXDI light list and no dome light appears in it.
    //
    // Divided by the albedo on the way in, and that division is what makes the two halves of the fog
    // agree. The raymarch adds this term as (estimate * scatteringCoefficient) integrated along the
    // ray, and scatteringCoefficient is sigma * albedo, so a constant estimate M over a path of
    // opacity (1 - T) contributes M * albedo * (1 - T). Undoing the albedo here lands that on
    // exactly fogAmbient * (1 - T) - the same radiance, and the same curve, that the composite's
    // ramp blends towards. Without it the near half of the fog reached 0.9 of the far half's colour
    // at best, and 0.225 of it with the multiScatteringScale default that shipped before 2026-08-13.
    const Vector3 albedoFloor(std::max(albedo.x, 1e-3f), std::max(albedo.y, 1e-3f), std::max(albedo.z, 1e-3f));
    const Vector3 ambient = d.fogAmbient * std::max(multiScatteringScale(), 0.0f);

    multiScatteringEstimate = Vector3(ambient.x / albedoFloor.x,
                                      ambient.y / albedoFloor.y,
                                      ambient.z / albedoFloor.z);
  }

  // Hands the medium the game's ramp as an extinction field, for fogRampMode 2.
  //
  // Written unconditionally rather than under the active() guard the other appliers use, because
  // "off" here has to be *written* as off: VolumeArgs is zero-initialised by its caller today, but a
  // mode left at whatever the struct happened to hold would put the shader's ramp branch in charge
  // of a medium nobody derived. Zero is upstream's homogeneous path, which is the correct behaviour
  // whenever the bridge is not running.
  void DxvkDusklightAtmosphere::fillVolumeRampArgs(VolumeArgs& args) const {
    args.dusklightFogRampMode = 0;
    args.dusklightRampStart = 0.0f;
    args.dusklightRampEnd = 0.0f;
    args.dusklightRampMinRemaining = 0.0f;

    if (!active() || std::clamp(fogRampMode(), 0, 2) != 2) {
      return;
    }

    const Derived& d = m_derived;

    // The span is already known good - resolve() refuses to set fogValid without end > start - so
    // this is belt and braces against a Derived that never went through resolve().
    //
    // rampMinRemaining is deliberately NOT tested here any more (2026-08-18). It used to be, and a
    // fogRampFloor of 0 therefore turned this mode off with nothing anywhere reporting it: the
    // composite still received fogRampMode 2, so its top-up residual ran against a homogeneous
    // medium that mode 2's branch in resolve() had never capped, which is the near-field haze that
    // limitDensityToRamp exists to prevent - while the log line and the panel both went on saying
    // "exact ramp". resolve() floors the value at kMinRampFloorFraction instead, so 0 is legal on
    // both sides and this guard has nothing left to refuse.
    if (!(d.rampEnd > d.rampStart)) {
      return;
    }

    args.dusklightFogRampMode = 2;
    args.dusklightRampStart = d.rampStart;
    args.dusklightRampEnd = d.rampEnd;
    args.dusklightRampMinRemaining = d.rampMinRemaining;
  }

  void DxvkDusklightAtmosphere::fillCompositeArgs(DusklightCompositeArgs& args) const {
    args = {};

    if (!active()) {
      return;
    }

    const Derived& d = m_derived;

    args.enable = 1;
    args.rampStart = d.rampStart;
    args.rampEnd = d.rampEnd;
    args.handoverDistance = d.froxelMaxDistance;
    // Carries the Hue only normalisation as well as the trim, so that when fogColorDirectional puts
    // the far ramp back on its own view-direction dome sample, that sample lands at the same level
    // the near half's sphere average did. Splitting these is how the fog ends up one brightness
    // close by and another in the distance. Inert on the default path below, where skyColorWeight is
    // zero and the shader never reaches its dome lerp at all.
    args.skyAmbientScale = std::max(skyAmbientScale(), 0.0f) * std::max(d.skyLevelScale, 0.0f);
    args.skyFogMode = static_cast<uint32_t>(std::clamp(skyFogMode(), 0, 2));
    args.skyFogAmount = std::clamp(skyFogAmount(), 0.0f, 1.0f);
    args.fogRampMode = static_cast<uint32_t>(std::clamp(fogRampMode(), 0, 2));

    // The two halves of the fog must blend towards ONE colour, and by default that is the sphere
    // average - fogAmbient - handed over already blended, with the shader's own dome lerp switched
    // off by a zero weight.
    //
    // This closes the last gap in the top-up identity, and it was a real one. The medium's ambient
    // in-scatter is isotropic and gets one colour per frame (the dome's mean radiance); the ramp was
    // sampling the same dome in the *view direction* instead. So the near half of the fog blended
    // towards the average sky and the far half towards the sky in front of the camera, and
    // "S * (1 - f) + A * f" quietly had two different A's in it - by construction, wherever the dome
    // is not uniform, which outdoors is always. The identity in DusklightAtmosphere.md §5.2 does not
    // hold under that and never did.
    //
    // THE CHOICE, AND ITS COST: the near half cannot be made directional - there is one
    // multiScatteringEstimate for the whole frame and nowhere to put a direction - so agreement can
    // only be reached by making the far half isotropic too. What that costs is aerial perspective's
    // directional tint: the distance no longer warms towards the sun. fogColorDirectional puts it
    // back for anyone who would rather have the tint than the identity.
    if (fogColorDirectional()) {
      // The palette colour, not the resolved ambient: the shader does its own lerp towards the dome,
      // sampled in the view direction rather than averaged, and passing the already-blended value
      // would apply the sky twice.
      args.fogColor = d.fogRadiance;
      // The same weight the medium's ambient was blended with. It used to be the physical sky's
      // blend weight, which meant the far fog followed the sky only while the scattering model was
      // running - and left the near half following a palette colour regardless.
      args.skyColorWeight = std::clamp(d.skyAmbientWeight, 0.0f, 1.0f);
    } else {
      args.fogColor = d.fogAmbient;
      args.skyColorWeight = 0.0f;
    }
  }

  void DxvkDusklightAtmosphere::prepareSceneData(Rc<RtxContext> ctx, SceneManager& sceneManager) {
    if (!skyActive()) {
      // No dome this frame, so the last reduction describes a sky that is no longer there. Dropping
      // the flag rather than the value sends the fog back to the game's palette colour, which is the
      // right answer indoors and the only answer available there.
      m_skyAmbientValid = false;
      m_skyStatsFramesActive = 0;
      return;
    }

    if (RtxOptions::skyAutoDetect() != SkyAutoDetectMode::None) {
      ONCE(Logger::warn("[Dusklight] rtx.dusklight.atmosphere.skyEnable is on while rtx.skyAutoDetect is not None. "
                        "The auto detected sky keeps rasterizing into Remix's own probe, so the game's dome is still being "
                        "captured behind the generated one. Set rtx.skyAutoDetect = None."));
    }

    Rc<DxvkContext> baseCtx = ctx;

    // Created on demand and then kept until the pass deactivates - in particular it survives a
    // target resize, which is what onDeactivation above is for. The dome light holds a bindless
    // index into this image, and losing it for even a single frame drops the sky back to Remix's
    // own probe.
    if (m_skyTexture.image == nullptr) {
      m_skyTexture = Resources::createImageResource(
        baseCtx, "dusklight sky", VkExtent3D { kSkyWidth, kSkyHeight, 1 },
        VK_FORMAT_R16G16B16A16_SFLOAT);
    }

    if (m_transmittanceLut.image == nullptr) {
      m_transmittanceLut = Resources::createImageResource(
        baseCtx, "dusklight atmosphere transmittance",
        VkExtent3D { DUSKLIGHT_TRANSMITTANCE_WIDTH, DUSKLIGHT_TRANSMITTANCE_HEIGHT, 1 },
        VK_FORMAT_R16G16B16A16_SFLOAT);
    }

    if (m_multiScatterLut.image == nullptr) {
      m_multiScatterLut = Resources::createImageResource(
        baseCtx, "dusklight atmosphere multiscatter",
        VkExtent3D { DUSKLIGHT_MULTISCATTER_SIZE, DUSKLIGHT_MULTISCATTER_SIZE, 1 },
        VK_FORMAT_R16G16B16A16_SFLOAT);
    }

    if (m_skyTexture.view == nullptr || m_transmittanceLut.view == nullptr || m_multiScatterLut.view == nullptr) {
      ONCE(Logger::err("[Dusklight] failed to create the generated sky images; falling back to Remix's sky probe."));
      return;
    }

    // One device local element the reduction writes, plus a host visible ring of kMaxFramesInFlight
    // that is copied into and read from at an offset old enough to have landed. The same shape as
    // the auto exposure debug stats, and for the same reason: reading a GPU write in the frame that
    // produced it means a stall, and nothing here is worth a stall.
    if (m_skyStatsGpu == nullptr) {
      DxvkBufferCreateInfo statsInfo;
      statsInfo.size = sizeof(DusklightSkyStats);
      statsInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      statsInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      statsInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      m_skyStatsGpu = m_device->createBuffer(statsInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                             DxvkMemoryStats::Category::RTXBuffer, "dusklight sky stats");
      ctx->clearBuffer(m_skyStatsGpu, 0, statsInfo.size, 0);

      statsInfo.size = sizeof(DusklightSkyStats) * kMaxFramesInFlight;
      statsInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      statsInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
      statsInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT;
      m_skyStatsHost = m_device->createBuffer(statsInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                              DxvkMemoryStats::Category::RTXBuffer, "dusklight sky stats HOST");

      if (m_skyStatsHost != nullptr && m_skyStatsHost->mapPtr(0) != nullptr) {
        std::memset(m_skyStatsHost->mapPtr(0), 0, statsInfo.size);
      }
    }

    const Derived& d = derived();

    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

    // Kept as a Vector3 as well as being written into the args: the shader side vec3 converts from
    // Vector3 but not back to it, and the lookup table staleness check needs the engine type.
    const Vector3 skyColorLinear = sRGBGammaToLinear(sanitizeColor(DusklightEnv::skyColor()));
    const float clampedPaletteInfluence = std::clamp(paletteInfluence(), 0.0f, 1.0f);

    DusklightAtmosphereArgs pushArgs = {};
    pushArgs.sunAzimuthRadians = DusklightEnv::sunAzimuth() * kDegreesToRadians;
    pushArgs.sunElevationRadians = DusklightEnv::sunElevation() * kDegreesToRadians;
    pushArgs.skyColor = skyColorLinear;
    pushArgs.intensity = std::max(skyIntensity(), 0.0f);
    pushArgs.kasumiInner = sRGBGammaToLinear(sanitizeColor(DusklightEnv::kasumiInner()));
    pushArgs.physicalWeight = std::clamp(d.physicalWeight, 0.0f, 1.0f);
    pushArgs.kasumiOuter = sRGBGammaToLinear(sanitizeColor(DusklightEnv::kasumiOuter()));
    pushArgs.paletteInfluence = clampedPaletteInfluence;
    pushArgs.horizonSharpness = std::max(skyHorizonSharpness(), 1e-3f);
    pushArgs.groundFraction = std::clamp(skyGroundFraction(), 0.0f, 1.0f);
    pushArgs.mieAnisotropy = std::clamp(mieAnisotropy(), 0.0f, 0.95f);
    pushArgs.multiScatterScale = std::max(multiScatterScale(), 0.0f);

    // The moon. The game pushes one celestial direction - whichever body drives the light - so
    // sunAzimuth/sunElevation *is* the moon's direction while sunIsDay is false; no second pair.
    // Faded by sunFade rather than by an elevation threshold, because sunFade already falls to zero
    // exactly where the pushed direction jumps from one body to the other, so the moon fades as its
    // direction stops meaning anything instead of snapping out while still on screen.
    // DusklightAtmosphere.md §13.1.
    const bool moonVisible = skyMoonEnable() && !DusklightEnv::sunIsDay() && DusklightEnv::sunActive();
    const float moonFade = moonVisible ? std::clamp(DusklightEnv::sunFade(), 0.0f, 1.0f) : 0.0f;

    pushArgs.moonColor = Vector3(1.0f, 1.0f, 1.0f);
    pushArgs.moonRadiance = std::max(skyMoonIntensity(), 0.0f) * moonFade;
    pushArgs.moonAngularRadiusRadians =
      std::max(skyMoonAngularDiameterDegrees(), 0.0f) * 0.5f * kDegreesToRadians;
    pushArgs.moonEdgeSoftness = std::clamp(skyMoonEdgeSoftness(), 0.0f, 1.0f);

    ctx->setFramePassStage(RtxFramePassStage::FrameBegin);
    ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

    Rc<DxvkSampler> linearSampler = ctx->getResourceManager().getSampler(
      VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // Only the two tables are staleness-gated - they are functions of the medium alone, so rebuilding
    // them per frame would be most of the cost of the feature for no change in the result. The sky
    // image below is dispatched every frame regardless, which is what lets the sun move and the moon
    // fade at all; do not add a cache there without solving that.
    const bool needsLutRebuild =
      pushArgs.physicalWeight > 0.0f && mediumChangedSince(skyColorLinear, clampedPaletteInfluence);

    if (needsLutRebuild) {
      {
        ScopedGpuProfileZone(ctx, "Dusklight Atmosphere Transmittance");

        DusklightAtmosphereArgs lutArgs = pushArgs;
        lutArgs.imageSize = { DUSKLIGHT_TRANSMITTANCE_WIDTH, DUSKLIGHT_TRANSMITTANCE_HEIGHT };
        ctx->pushConstants(0, sizeof(lutArgs), &lutArgs);

        const VkExtent3D groups = util::computeBlockCount(
          VkExtent3D { DUSKLIGHT_TRANSMITTANCE_WIDTH, DUSKLIGHT_TRANSMITTANCE_HEIGHT, 1 }, VkExtent3D { 8, 8, 1 });

        ctx->bindResourceView(DUSKLIGHT_TRANSMITTANCE_OUTPUT, m_transmittanceLut.view, nullptr);
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, DusklightTransmittanceShader::getShader());
        ctx->dispatch(groups.width, groups.height, groups.depth);
      }

      {
        ScopedGpuProfileZone(ctx, "Dusklight Atmosphere Multi Scatter");

        DusklightAtmosphereArgs lutArgs = pushArgs;
        lutArgs.imageSize = { DUSKLIGHT_MULTISCATTER_SIZE, DUSKLIGHT_MULTISCATTER_SIZE };
        ctx->pushConstants(0, sizeof(lutArgs), &lutArgs);

        const VkExtent3D groups = util::computeBlockCount(
          VkExtent3D { DUSKLIGHT_MULTISCATTER_SIZE, DUSKLIGHT_MULTISCATTER_SIZE, 1 }, VkExtent3D { 8, 8, 1 });

        ctx->bindResourceView(DUSKLIGHT_MULTISCATTER_TRANSMITTANCE, m_transmittanceLut.view, nullptr);
        ctx->bindResourceSampler(DUSKLIGHT_MULTISCATTER_TRANSMITTANCE, linearSampler);
        ctx->bindResourceView(DUSKLIGHT_MULTISCATTER_OUTPUT, m_multiScatterLut.view, nullptr);
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, DusklightMultiScatterShader::getShader());
        ctx->dispatch(groups.width, groups.height, groups.depth);
      }

      m_lutSkyColor = skyColorLinear;
      m_lutPaletteInfluence = clampedPaletteInfluence;
    }

    {
      ScopedGpuProfileZone(ctx, "Dusklight Sky");

      pushArgs.imageSize = { kSkyWidth, kSkyHeight };
      ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

      const VkExtent3D workgroups = util::computeBlockCount(
        VkExtent3D { kSkyWidth, kSkyHeight, 1 }, VkExtent3D { 16, 16, 1 });

      ctx->bindResourceView(DUSKLIGHT_SKY_OUTPUT, m_skyTexture.view, nullptr);
      ctx->bindResourceView(DUSKLIGHT_SKY_TRANSMITTANCE, m_transmittanceLut.view, nullptr);
      ctx->bindResourceSampler(DUSKLIGHT_SKY_TRANSMITTANCE, linearSampler);
      ctx->bindResourceView(DUSKLIGHT_SKY_MULTISCATTER, m_multiScatterLut.view, nullptr);
      ctx->bindResourceSampler(DUSKLIGHT_SKY_MULTISCATTER, linearSampler);
      ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, DusklightSkyShader::getShader());
      ctx->dispatch(workgroups.width, workgroups.height, workgroups.depth);
    }

    // Reduce the dome to the one radiance the fog needs, and collect the answer from a few frames
    // ago. Runs every frame, unconditionally, rather than only while a tuning panel is open: this
    // one feeds the image rather than a readout, and a value that appears only when someone is
    // looking is the definition of a heisenbug.
    if (m_skyStatsGpu != nullptr && m_skyStatsHost != nullptr) {
      {
        ScopedGpuProfileZone(ctx, "Dusklight Sky Ambient");

        // imageSize is already the sky's extent from the dispatch above; the reduction reads the
        // same two numbers, so nothing is re-derived here.
        ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

        ctx->bindResourceView(DUSKLIGHT_SKY_STATS_INPUT, m_skyTexture.view, nullptr);
        ctx->bindResourceBuffer(DUSKLIGHT_SKY_STATS_OUTPUT, DxvkBufferSlice(m_skyStatsGpu));
        ctx->bindShader(VK_SHADER_STAGE_COMPUTE_BIT, DusklightSkyStatsShader::getShader());
        ctx->dispatch(1, 1, 1);
      }

      ++m_skyStatsFramesActive;

      const uint32_t frameId = m_device->getCurrentFrameId();
      const uint32_t writeIndex = frameId % kMaxFramesInFlight;

      ctx->copyBuffer(m_skyStatsHost, sizeof(DusklightSkyStats) * writeIndex,
                      m_skyStatsGpu, 0, sizeof(DusklightSkyStats));

      const uint32_t readIndex = (frameId + 1) % kMaxFramesInFlight;

      if (const void* mapped = m_skyStatsHost->mapPtr(sizeof(DusklightSkyStats) * readIndex)) {
        DusklightSkyStats stats = {};
        std::memcpy(&stats, mapped, sizeof(stats));

        const Vector3 ambient(stats.ambient.x, stats.ambient.y, stats.ambient.z);

        // Trusted only once the ring can no longer be serving a slot from before the dome came
        // back. valid alone is not enough: it stays set in a slot written by a *previous* run of
        // this pass, so on resume the first kMaxFramesInFlight frames would read another area's sky.
        if (m_skyStatsFramesActive > kMaxFramesInFlight && stats.valid != 0 &&
            isFinite(ambient.x) && isFinite(ambient.y) && isFinite(ambient.z)) {
          m_skyAmbient = Vector3(std::max(ambient.x, 0.0f), std::max(ambient.y, 0.0f), std::max(ambient.z, 0.0f));
          m_skyAmbientValid = true;
        }
      }
    }

    TextureRef skyRef(m_skyTexture.view);
    sceneManager.trackTexture(skyRef, m_skyTextureIndex, true, false);

    DomeLight domeLight;
    // The generator already scaled by the intensity option, so the radiance here is a plain
    // multiplier and stays at one; keeping the scaling in one place stops the two disagreeing.
    domeLight.radiance = Vector3(1.0f, 1.0f, 1.0f);
    domeLight.texture = skyRef;
    domeLight.worldToLight = kWorldToDomeLight;

    LightManager& lightManager = sceneManager.getLightManager();
    lightManager.addExternalDomeLight(reinterpret_cast<remixapi_LightHandle>(kDomeLightHandle), domeLight);
    lightManager.addExternalLightInstance(reinterpret_cast<remixapi_LightHandle>(kDomeLightHandle));
  }

  namespace {
    // The mode combos below drive their option through a clamped static int rather than binding
    // the widget to the option, so they get none of RtxOptionUxWrapper's per-row UX - including
    // its automatic tooltip. This puts the option's own description back, which for every one of
    // these combos enumerates ALL of the modes rather than only the one selected. That is why the
    // per-mode paragraphs that used to sit under each combo are gone: reading what a mode does no
    // longer requires selecting it first.
    template <typename T>
    void dusklightOptionTip(dxvk::RtxOption<T>& option) {
      if (ImGui::IsItemHovered()) {
        RemixGui::SetTooltipUnformatted(RemixGui::BuildRtxOptionTooltip(&option).c_str());
      }
    }
  }

  // This panel is drawn by the overlay's Sky tab as one flat row of hot controls plus four
  // collapsing groups. The headers, the indentation and the volumetrics BeginDisabled pairs all
  // belong to the overlay - see the declarations in the header for why - so nothing here indents
  // and nothing here draws a header.
  //
  // Atmosphere Enabled and Generate Sky are deliberately absent: they are on the window's master
  // switch row, and drawing either in both places would give one RtxOption two widgets with the
  // same ImGui ID, because RtxOptionUxWrapper keys its ID off the option's address.
  void DxvkDusklightAtmosphere::showImguiHot() {
    if (!DusklightEnv::enable()) {
      ImGui::TextWrapped("Waiting for the game's environment feed (rtx.dusklight.env.enable). "
                         "Nothing here does anything until the game's bridge is running.");
    }

    RemixGui::DragFloat("Sky Intensity##dusklightAtmo", &skyIntensityObject(), 0.05f, 0.f, 32.f, "%.2f");

    // Two candidate fixes for one defect, built side by side so the choice could be made by
    // looking. It was: Exempt was run in game on 2026-08-13 and confirmed good, and is the
    // default. Weighted is kept as a taste control for foggy weather rather than as a rival.
    {
      static const char* kSkyFogModes[] = { "Off (untreated)", "Exempt", "Weighted" };
      static int mode;
      mode = std::clamp(skyFogMode(), 0, 2);
      if (RemixGui::Combo("Fog On Sky##dusklightAtmo", &mode, kSkyFogModes, IM_ARRAYSIZE(kSkyFogModes))) {
        skyFogMode.setDeferred(mode);
      }
      dusklightOptionTip(skyFogModeObject());
      if (mode == 2) {
        RemixGui::DragFloat("Sky Fog Amount##dusklightAtmo", &skyFogAmountObject(), 0.01f, 0.f, 1.f, "%.2f");
      } else if (mode == 0) {
        // A state that is currently wrong rather than a description of a control, so it stays on
        // screen instead of moving into the tooltip with the rest.
        ImGui::TextWrapped("Fog On Sky is on its untreated setting - the defect kept for comparison. "
                           "The sky is dimmed by the whole depth of the froxel grid and tinted "
                           "towards the fog colour.");
      }
    }

    RemixGui::Checkbox("Simulate Scattering##dusklightAtmo", &physicalSkyObject());
  }

  void DxvkDusklightAtmosphere::showImguiFog() {
    {
      static const char* kFogRampModes[] = { "Handover (legacy)", "Top up", "Exact ramp" };
      static int rampMode;
      rampMode = std::clamp(fogRampMode(), 0, 2);
      if (RemixGui::Combo("Ramp Owns##dusklightAtmo", &rampMode, kFogRampModes, IM_ARRAYSIZE(kFogRampModes))) {
        fogRampMode.setDeferred(rampMode);
      }
      dusklightOptionTip(fogRampModeObject());
      if (rampMode == 0) {
        ImGui::TextWrapped("Ramp Owns is on its legacy setting, kept to be compared against: a "
                           "homogeneous medium cannot be clear where the original is clear, so the "
                           "near field hazes over.");
      } else if (rampMode == 2) {
        // A state that has never been run rather than a description of a control, so it stays on
        // screen rather than moving into the tooltip with the rest.
        //
        // The caveat in the second half is not decoration. froxelMaxDistanceMaxMeters defaults to
        // 120 m = 12000 units and the open world's fogEndZ has been measured around 33500, so in the
        // commonest scene in the game the grid covers about a third of the ramp and the residual
        // genuinely varies past that - the signature as originally written would have fired on a
        // correct build, outdoors, immediately. Qualified 2026-08-18.
        ImGui::TextWrapped("Exact ramp is UNTESTED IN GAME. The medium is given the game's own fog "
                           "curve as its extinction, so the top-up residual on the Readouts tab "
                           "should read flat WITHIN THE FROXEL GRID'S REACH - 0 where the ramp starts "
                           "in front of the camera, and the ramp's head start where it starts behind "
                           "it. The defect to report is a residual that varies with distance inside "
                           "that reach. Two places it varies and should: past the grid's reach, which "
                           "the Readouts tab prints against the ramp's end and which the metre "
                           "ceiling below makes routine outdoors, and over the last Exact Ramp Floor "
                           "fraction of the ramp, where the divergence clamp freezes the medium.");
      }
    }

    // NOT greyed out in mode 2, corrected 2026-08-18. They were, on the reading that the mode has no
    // scalar to derive - but resolve() still builds sigma out of both there, and sigma is what light
    // visibility through the fog and the transmittance measurement distance are computed from. So
    // this was the one mode that hid the only two controls over the one quantity sigma still governs.
    RemixGui::DragFloat("Ramp Match Floor##dusklightAtmo", &zHalfMinObject(), 1.0f, 1.f, 10000.f, "%.0f units");
    RemixGui::DragFloat("Density Scale##dusklightAtmo", &densityScaleObject(), 0.01f, 0.f, 8.f, "%.2f");
    // Clamped the same way every functional consumer clamps it, so a hand-edited
    // rtx.conf value above the range cannot run mode 2 while greying out its own control.
    ImGui::BeginDisabled(std::clamp(fogRampMode(), 0, 2) != 2);
    RemixGui::DragFloat("Exact Ramp Floor##dusklightAtmo", &fogRampFloorObject(), 0.002f, 0.f, 0.25f, "%.3f");
    ImGui::EndDisabled();
    RemixGui::Checkbox("Hold Density Under The Ramp##dusklightAtmo", &limitDensityToRampObject());
    // Both halves of this pair sit inside one header on purpose: splitting a BeginDisabled from
    // its EndDisabled across a header boundary unbalances the stack.
    ImGui::BeginDisabled(!limitDensityToRamp() || fogRampMode() != 1);
    RemixGui::DragFloat("Clear Zone Tolerance##dusklightAtmo", &clearZoneToleranceObject(), 0.005f, 0.f, 0.5f, "%.3f");
    RemixGui::DragFloat("Clear Zone Veil Target##dusklightAtmo", &clearZoneVeilTargetObject(), 0.002f, 0.f, 0.5f, "%.3f");
    ImGui::EndDisabled();

    {
      static const char* kFogColorSpaces[] = { "Decoded (correct)", "Raw (matches the depth fog)" };
      static int colorSpace;
      colorSpace = std::clamp(fogColorSpace(), 0, 1);
      if (RemixGui::Combo("Palette Reads As##dusklightAtmo", &colorSpace, kFogColorSpaces,
                          IM_ARRAYSIZE(kFogColorSpaces))) {
        fogColorSpace.setDeferred(colorSpace);
      }
      dusklightOptionTip(fogColorSpaceObject());
    }

    RemixGui::Checkbox("Directional Far Fog##dusklightAtmo", &fogColorDirectionalObject());
    RemixGui::DragFloat("Fog Radiance Scale##dusklightAtmo", &fogRadianceScaleObject(), 0.01f, 0.f, 16.f, "%.2f");
    RemixGui::DragFloat("Ambient In-Scatter##dusklightAtmo", &multiScatteringScaleObject(), 0.01f, 0.f, 4.f, "%.2f");
    RemixGui::DragFloat("Forward Scatter##dusklightAtmo", &fogAnisotropyObject(), 0.01f, -0.95f, 0.95f, "%.2f");

    {
      static const char* kSkyAmbientModes[] = { "Off", "Hue only", "Full radiance" };
      static int ambientMode;
      ambientMode = std::clamp(skyAmbientMode(), 0, 2);
      if (RemixGui::Combo("Sky Ambient Mode##dusklightAtmo", &ambientMode, kSkyAmbientModes,
                          IM_ARRAYSIZE(kSkyAmbientModes))) {
        skyAmbientMode.setDeferred(ambientMode);
      }
      dusklightOptionTip(skyAmbientModeObject());
    }

    ImGui::BeginDisabled(skyAmbientMode() == 0);
    RemixGui::DragFloat("Sky Ambient##dusklightAtmo", &skyAmbientWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Sky Ambient Scale##dusklightAtmo", &skyAmbientScaleObject(), 0.01f, 0.f, 8.f, "%.2f");
    ImGui::EndDisabled();

    {
      static const char* kExposureFogModes[] = { "Off", "Indoors only", "Always" };
      static int exposureMode;
      exposureMode = std::clamp(exposureFogMode(), 0, 2);
      if (RemixGui::Combo("Exposure-Relative##dusklightAtmo", &exposureMode, kExposureFogModes,
                          IM_ARRAYSIZE(kExposureFogModes))) {
        exposureFogMode.setDeferred(exposureMode);
      }
      dusklightOptionTip(exposureFogModeObject());
    }
  }

  void DxvkDusklightAtmosphere::showImguiFroxelGrid() {
    RemixGui::DragFloat("Range Scale##dusklightAtmo", &froxelRangeScaleObject(), 0.01f, 0.1f, 4.f, "%.2f");
    RemixGui::DragFloat("Range Min##dusklightAtmo", &froxelMaxDistanceMinMetersObject(), 0.5f, 0.5f, 100.f, "%.1f m");
    RemixGui::DragFloat("Range Max##dusklightAtmo", &froxelMaxDistanceMaxMetersObject(), 1.0f, 5.f, 1000.f, "%.0f m");
    RemixGui::DragFloat("Smoothing Rate##dusklightAtmo", &froxelSmoothingRateObject(), 0.005f, 0.005f, 1.f, "%.3f");
  }

  void DxvkDusklightAtmosphere::showImguiSkyShape() {
    RemixGui::DragFloat("Horizon Sharpness##dusklightAtmo", &skyHorizonSharpnessObject(), 0.05f, 0.25f, 16.f, "%.2f");
    RemixGui::DragFloat("Ground Fraction##dusklightAtmo", &skyGroundFractionObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::Checkbox("Paint Moon##dusklightAtmo", &skyMoonEnableObject());
    ImGui::BeginDisabled(!skyMoonEnable());
    RemixGui::DragFloat("Moon Size##dusklightAtmo", &skyMoonAngularDiameterDegreesObject(), 0.1f, 0.1f, 30.f, "%.1f deg");
    RemixGui::DragFloat("Moon Brightness##dusklightAtmo", &skyMoonIntensityObject(), 0.1f, 0.f, 64.f, "%.2f");
    RemixGui::DragFloat("Moon Edge##dusklightAtmo", &skyMoonEdgeSoftnessObject(), 0.01f, 0.f, 1.f, "%.2f");
    ImGui::EndDisabled();
  }

  void DxvkDusklightAtmosphere::showImguiPhysicalSky() {
    RemixGui::DragFloat("Max Weight##dusklightAtmo", &physicalMaxWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Blend From##dusklightAtmo", &physicalElevationLowDegreesObject(), 0.5f, -10.f, 45.f, "%.1f deg");
    RemixGui::DragFloat("Blend To##dusklightAtmo", &physicalElevationHighDegreesObject(), 0.5f, 0.f, 90.f, "%.1f deg");
    RemixGui::DragFloat("Weather Weight##dusklightAtmo", &physicalWeatherWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Palette Influence##dusklightAtmo", &paletteInfluenceObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Haze Forward Scatter##dusklightAtmo", &mieAnisotropyObject(), 0.01f, 0.f, 0.95f, "%.2f");
    RemixGui::DragFloat("Multi Scatter##dusklightAtmo", &multiScatterScaleObject(), 0.01f, 0.f, 4.f, "%.2f");
  }

  // Resolved state. No controls, so the overlay draws this on its Readouts tab rather than beside
  // the settings it is derived from.
  void DxvkDusklightAtmosphere::showImguiReadouts() {
    // peek, not derived: this runs on the presenting thread and must not drive the per-frame latch.
    const Derived& d = peek();
    if (!d.fogValid) {
      ImGui::TextUnformatted("fog: inactive");
    } else {
      ImGui::Text("ramp: %.0f .. %.0f units", d.rampStart, d.rampEnd);
      ImGui::Text("sigma: %.6f /unit   half density at %.0f units",
                  d.sigma, d.sigma > 1e-8f ? std::log(2.0f) / d.sigma : 0.0f);
      if (d.sigma < d.sigmaMatched * 0.999f) {
        ImGui::Text("  capped from %.6f (x%.2f) to hold the medium under the game's ramp",
                    d.sigmaMatched, d.sigmaMatched > 0.0f ? d.sigma / d.sigmaMatched : 1.0f);
      }
      // The number that decides whether the top up ramp is exact or clamped. Zero means the medium is
      // nowhere thicker than the original's fog and the composite reproduces it exactly; anything
      // above the tolerance means density is being spent where the original was clear.
      ImGui::Text("peak excess over the game's ramp: %.3f at %.0f units (budget %.3f)",
                  d.excessPeak, d.excessPeakDistance, d.clearZoneToleranceUsed);
      if (d.clearZoneToleranceUsed < clearZoneTolerance() * 0.999f) {
        ImGui::Text("  tightened from the %.3f ceiling: ambient luminance %.3f",
                    clearZoneTolerance(), sRGBLuminance(d.fogAmbient));
      }
      // The other direction, and in Exact ramp mode it is the mode's whole claim: zero means the
      // medium reproduced the game's curve and the composite had nothing left to add.
      if (std::clamp(fogRampMode(), 0, 2) == 2) {
        ImGui::Text("top-up residual owed: %.4f, constant out to %.0f units%s", d.residualPeak,
                    d.rampEnd - d.rampMinRemaining,
                    d.residualPeak > 1e-6f
                      ? "  (the ramp's head start behind the camera, which no medium can produce)"
                      : "  (the ramp starts at or beyond the camera - the medium reproduces it exactly)");
        // Both of these are places the residual is SUPPOSED to vary, and both were unqualified until
        // 2026-08-18 while the settings tab called a varying residual the defect to report.
        ImGui::Text("  divergence clamp freezes the medium over the last %.0f units of the ramp - the "
                    "residual climbs off the constant to 1 there, by construction", d.rampMinRemaining);
        if (d.froxelMaxDistance < d.rampEnd * 0.999f) {
          ImGui::Text("  grid reaches %.0f of the ramp's %.0f units - the metre ceiling is binding, so "
                      "the residual carries the far colour past that and varies with distance there "
                      "(expected, not a defect)", d.froxelMaxDistance, d.rampEnd);
        }
      } else {
        ImGui::Text("peak residual the top-up must supply: %.3f at %.0f units",
                    d.residualPeak, d.residualPeakDistance);
      }
      ImGui::Text("palette: %.3f, %.3f, %.3f raw / %.3f, %.3f, %.3f decoded  (using %s)",
                  d.paletteRaw.x, d.paletteRaw.y, d.paletteRaw.z,
                  d.paletteDecoded.x, d.paletteDecoded.y, d.paletteDecoded.z,
                  fogColorSpace() == 0 ? "decoded" : "raw");
      ImGui::Text("fog radiance (palette): %.3f, %.3f, %.3f", d.fogRadiance.x, d.fogRadiance.y, d.fogRadiance.z);
      ImGui::Text("fog ambient (used): %.3f, %.3f, %.3f   %.0f%% from the dome%s",
                  d.fogAmbient.x, d.fogAmbient.y, d.fogAmbient.z, d.skyAmbientWeight * 100.0f,
                  d.skyAmbientWeight > 0.0f ? "" : "  (no sky, or the reduction has not run yet)");
      // Both fog paths aim at the line above. Which is worth saying out loud, because until
      // 2026-08-17 they did not, and the volumetric one was the only one this panel described.
      //
      // Reports which, rather than asserting it: applyFogOverride declines to write the fog state
      // when a translucent material has already replaced the fog, and there the depth path is still
      // showing the game's own colour with rtx.fogColorScale still applied to it. Rare, but it is
      // the one configuration where the two paths legitimately disagree and it should be readable.
      ImGui::Text("  the legacy depth fog %s%s",
                  fogColorOverridden()
                    ? "aims at the same value (rtx.fogColorScale is bypassed)"
                    : "is NOT being overridden this frame - a translucent material has replaced the "
                      "fog, so it shows the game's own colour times rtx.fogColorScale",
                  fogColorDirectional()
                    ? "; the far ramp samples the dome in the view direction instead"
                    : "; the far ramp aims at the same value too");
      // Far from 1 means the dome and the palette disagreed badly about how bright this sky is. In
      // Hue only mode that gap is closed here; in Full radiance mode it is shown and left alone.
      if (d.skyAmbientWeight > 0.0f) {
        ImGui::Text("dome level scale: x%.3f%s", d.skyLevelScale,
                    skyAmbientMode() == 1 ? "  (normalised to the palette)" : "  (full radiance, not normalised)");
      }
      // Below 1 means a bright scene and the fog scaled up to sit in it; above 1 means a dark scene
      // and the fog scaled down so it cannot lift the blacks. Exactly 1 means the mode is off or
      // this area is not one it covers.
      ImGui::Text("exposure correction: x%.3f%s", d.exposureCorrection,
                  d.exposureCorrection == 1.0f ? "  (off here)" : "");
      ImGui::Text("froxel grid reaches %.0f units (%.1f m)",
                  d.froxelMaxDistance, d.froxelMaxDistance / RtxOptions::getMeterToWorldUnitScale());
    }
    // The colpat crossfade this line used to carry as well is printed once, by the Environment
    // response group a few lines above it on the same tab. It was printed twice in two different
    // sections until 2026-08-17, and the two disagreed about which fields were worth showing.
    ImGui::Text("area: %s   sun %.1f deg %s",
                d.outdoor ? "outdoor" : "no sky",
                DusklightEnv::sunElevation(), DusklightEnv::sunIsDay() ? "(day)" : "(night)");
    ImGui::Text("physical weight: %.3f%s", d.physicalWeight,
                d.physicalWeight <= 0.0f ? "  (the game's own sky)" : "");

    ImGui::TextWrapped("Everything above is also written to the log once per distinct fog state, so an area that reads "
                       "wrong can be diagnosed from a session log rather than from a description. Read 'peak excess' "
                       "first: above zero means the medium is thicker than the original's fog somewhere, which the ramp "
                       "cannot undo, and lowering Clear Zone Tolerance is the answer.");
  }
}
