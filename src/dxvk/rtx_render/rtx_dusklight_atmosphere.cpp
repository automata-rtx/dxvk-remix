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

    // Match the game's ramp where it is half opaque. Scale free, so one expression covers a scripted
    // whiteout closing in over two metres and an open field hazing out over two hundred, with no per
    // area handling anywhere. DusklightAtmosphere.md §5.1.
    //
    // Scripted fog banks deliberately put the ramp's start behind the camera, which drags the half
    // opaque point behind it too; the clamp is what stops the density running away there.
    const float zHalf = std::max((start + end) * 0.5f, std::max(zHalfMin(), 1e-3f));

    out.sigmaMatched = std::max(std::log(2.0f) / zHalf, 0.0f) * std::max(densityScale(), 0.0f);
    out.sigma = out.sigmaMatched;
    out.rampStart = start;
    out.rampEnd = end;

    // sigma stays at the half density match for now. Holding it under the game's own ramp happens
    // further down, once fogAmbient exists, because how much haze the clear zone can afford depends
    // on how bright that haze is going to be. See the block below fogAmbient.

    // The game authors its fog colour to be blended over a finished, display referred image. Here
    // it is a quantity of light in a linear frame that has not been tone mapped yet, so it is
    // decoded rather than used raw. The two halves of the range split share this, which is what
    // keeps the near medium and the far ramp the same colour.
    //
    // The exposure correction finishes that thought. Decoding gamma fixes the colour's *shape*; it
    // does nothing about its *level*, because a display colour has no level until you say what
    // exposure it was meant to be seen at. Dividing by the exposure the tonemapper is about to
    // apply says exactly that, and is what stops one number having to serve both a sunlit field and
    // an unlit cave. exposureFogMode decides where it runs.
    out.exposureCorrection = resolveExposureCorrection(out.outdoor);
    out.fogRadiance = sRGBGammaToLinear(sanitizeColor(DusklightEnv::fogColor())) *
                      (std::max(fogRadianceScale(), 0.0f) * out.exposureCorrection);

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

    if (fogRampMode() == 1 && limitDensityToRamp()) {
      out.sigma = solveSigmaWithinTolerance(out.sigmaMatched, start, end, out.clearZoneToleranceUsed);
    }

    out.excessPeak = rampExcessPeak(out.sigma, start, end, out.excessPeakDistance);

    // Size the grid from the game's own fog range, so its fixed slice count lands where the fog
    // actually is. This costs nothing: the slice count does not change, only how far it reaches.
    const float meterToWorld = RtxOptions::getMeterToWorldUnitScale();
    const float minDistance = std::max(froxelMaxDistanceMinMeters(), 0.0f) * meterToWorld;
    const float maxDistance = std::max(froxelMaxDistanceMaxMeters() * meterToWorld, minDistance);
    const float target = std::clamp(end * std::max(froxelRangeScale(), 0.0f), minDistance, maxDistance);

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

    Logger::info(str::format(
      "[Dusklight] fog: ramp ", std::llround(d.rampStart), "..", std::llround(d.rampEnd),
      " units, extinction ", d.sigma, "/unit",
      capped ? str::format(" (capped from ", d.sigmaMatched, " to keep the medium under the game's ramp)")
             : std::string(" (uncapped - the half density match already fits under the game's ramp)"),
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
      " units, ambient ", round2(d.fogAmbient.x), ",", round2(d.fogAmbient.y), ",", round2(d.fogAmbient.z),
      " (", round2(d.skyAmbientWeight * 100.0f), "% from the sky dome at level scale x", round2(d.skyLevelScale),
      ", the rest from the palette after correction ",
      round2(d.fogRadiance.x), ",", round2(d.fogRadiance.y), ",", round2(d.fogRadiance.z),
      "), area ", d.outdoor ? "has a sky (the game's own hide_vrbox test says visible)"
                            : "has no sky (the game hides its own dome here)",
      ", dome ", d.skyAmbientWeight > 0.0f ? "on" : "off",
      ", ramp mode ", fogRampMode() == 1 ? "top-up" : "handover",
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

    fog.mode = D3DFOG_LINEAR;
    fog.color = sanitizeColor(DusklightEnv::fogColor());
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

  void DxvkDusklightAtmosphere::fillCompositeArgs(DusklightCompositeArgs& args) const {
    args = {};

    if (!active()) {
      return;
    }

    const Derived& d = m_derived;

    args.enable = 1;
    // The palette colour, not the resolved ambient: the shader does its own lerp towards the dome,
    // sampled in the view direction rather than averaged, and passing the already-blended value
    // would apply the sky twice.
    args.fogColor = d.fogRadiance;
    args.rampStart = d.rampStart;
    args.rampEnd = d.rampEnd;
    args.handoverDistance = d.froxelMaxDistance;
    // The same weight the medium's ambient was blended with, so the fog is one colour at every
    // distance. It used to be the physical sky's blend weight, which meant the far fog followed the
    // sky only while the scattering model was running - and left the near half following a palette
    // colour regardless. Both halves now follow the dome whenever there is one.
    args.skyColorWeight = std::clamp(d.skyAmbientWeight, 0.0f, 1.0f);
    // Carries the Hue only normalisation as well as the trim, so the far ramp's view-direction dome
    // sample lands at the same level the near half's sphere average did. Splitting these is how the
    // fog ends up one brightness close by and another in the distance.
    args.skyAmbientScale = std::max(skyAmbientScale(), 0.0f) * std::max(d.skyLevelScale, 0.0f);
    args.skyFogMode = static_cast<uint32_t>(std::clamp(skyFogMode(), 0, 2));
    args.skyFogAmount = std::clamp(skyFogAmount(), 0.0f, 1.0f);
    args.fogRampMode = static_cast<uint32_t>(std::clamp(fogRampMode(), 0, 1));
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

  void DxvkDusklightAtmosphere::showImguiSettings() {
    ImGui::Indent();
    RemixGui::Checkbox("Atmosphere Enabled", &enableObject());

    ImGui::Indent();

    if (!DusklightEnv::enable()) {
      ImGui::TextWrapped("Waiting for the game's environment feed (rtx.dusklight.env.enable). "
                         "Nothing here does anything until the game's bridge is running.");
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Fog");

    {
      static const char* kFogRampModes[] = { "Handover (legacy)", "Top up" };
      static int rampMode;
      rampMode = std::clamp(fogRampMode(), 0, 1);
      if (RemixGui::Combo("Ramp Owns##dusklightAtmo", &rampMode, kFogRampModes, IM_ARRAYSIZE(kFogRampModes))) {
        fogRampMode.setDeferred(rampMode);
      }
      if (rampMode == 0) {
        ImGui::TextWrapped("The medium reproduces the fog out to the froxel grid's edge and the ramp takes over past it. A "
                           "homogeneous medium cannot be clear where the original is clear, so the near field hazes over. "
                           "Here to be compared against.");
      } else {
        ImGui::TextWrapped("The ramp reaches the game's exact opacity at every distance and the medium only has to carry "
                           "light. Density then costs nothing but shaft quality, so it can be held under the original's "
                           "own fog curve.");
      }
    }

    RemixGui::DragFloat("Half Density Floor##dusklightAtmo", &zHalfMinObject(), 1.0f, 1.f, 10000.f, "%.0f units");
    RemixGui::DragFloat("Density Scale##dusklightAtmo", &densityScaleObject(), 0.01f, 0.f, 8.f, "%.2f");
    RemixGui::Checkbox("Hold Density Under The Ramp##dusklightAtmo", &limitDensityToRampObject());
    ImGui::BeginDisabled(!limitDensityToRamp() || fogRampMode() != 1);
    RemixGui::DragFloat("Clear Zone Tolerance##dusklightAtmo", &clearZoneToleranceObject(), 0.005f, 0.f, 0.5f, "%.3f");
    RemixGui::DragFloat("Clear Zone Veil Target##dusklightAtmo", &clearZoneVeilTargetObject(), 0.002f, 0.f, 0.5f, "%.3f");
    ImGui::TextWrapped(
      "The tolerance above is coverage; this is the brightness that coverage is allowed to have. "
      "The same 8% of a dim cave is invisible and 8% of lava-lit orange hangs in front of the "
      "player, so the budget is spent in luminance and the tolerance above becomes a ceiling. "
      "Bright interiors tighten; dim and outdoor scenes do not move. 0 disables the weighting and "
      "restores the pure coverage budget. The readout above reports the budget actually used.");
    ImGui::EndDisabled();
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
      switch (ambientMode) {
      case 0:
        ImGui::TextWrapped("The palette's colour only. Fog standing in shadow loses the sky's hue, since Remix's froxel "
                           "grid samples no dome light and so sees nothing of the sky by itself.");
        break;
      case 1:
        ImGui::TextWrapped("The dome decides what colour the sky is; the palette decides how bright the fog reads. Sky "
                           "Intensity is a lighting calibration, not an appearance one, so letting it set the fog's level "
                           "made indoor fog about six times too bright - and the medium's ambient term is not shadowed, so "
                           "a sealed room got open-sky in-scatter.");
        break;
      default:
        ImGui::TextWrapped("The dome supplies colour and brightness. Physically the better answer for an open sky, since "
                           "distant fog really should approach the sky's own radiance. Try this if terrain now reads darker "
                           "than the sky behind it.");
        break;
      }
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
      switch (exposureMode) {
      case 0:
        ImGui::TextWrapped("The palette's fog colour is used as a radiance. It is a display colour, so its level means "
                           "nothing here: too much light for a dark cave, not enough for a sunlit field.");
        break;
      case 1:
        ImGui::TextWrapped("Applied where the game reports no sky - which is exactly where there is no dome to take a real "
                           "radiance from. Outdoors the dome already supplies one, so this changes nothing there.");
        break;
      default:
        ImGui::TextWrapped("Also applied outdoors, to whatever share of the fog colour still comes from the palette rather "
                           "than the dome. With Sky Ambient at 1.0 that share is nothing, so the two upper modes converge.");
        break;
      }
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Froxel grid");
    RemixGui::DragFloat("Range Scale##dusklightAtmo", &froxelRangeScaleObject(), 0.01f, 0.1f, 4.f, "%.2f");
    RemixGui::DragFloat("Range Min##dusklightAtmo", &froxelMaxDistanceMinMetersObject(), 0.5f, 0.5f, 100.f, "%.1f m");
    RemixGui::DragFloat("Range Max##dusklightAtmo", &froxelMaxDistanceMaxMetersObject(), 1.0f, 5.f, 1000.f, "%.0f m");
    RemixGui::DragFloat("Smoothing Rate##dusklightAtmo", &froxelSmoothingRateObject(), 0.005f, 0.005f, 1.f, "%.3f");

    RemixGui::Separator();
    ImGui::TextUnformatted("Sky");
    RemixGui::Checkbox("Generate Sky##dusklightAtmo", &skyEnableObject());
    RemixGui::DragFloat("Sky Intensity##dusklightAtmo", &skyIntensityObject(), 0.05f, 0.f, 32.f, "%.2f");
    RemixGui::DragFloat("Horizon Sharpness##dusklightAtmo", &skyHorizonSharpnessObject(), 0.05f, 0.25f, 16.f, "%.2f");
    RemixGui::DragFloat("Ground Fraction##dusklightAtmo", &skyGroundFractionObject(), 0.01f, 0.f, 1.f, "%.2f");

    RemixGui::Checkbox("Paint Moon##dusklightAtmo", &skyMoonEnableObject());
    if (skyMoonEnable()) {
      RemixGui::DragFloat("Moon Size##dusklightAtmo", &skyMoonAngularDiameterDegreesObject(), 0.1f, 0.1f, 30.f, "%.1f deg");
      RemixGui::DragFloat("Moon Brightness##dusklightAtmo", &skyMoonIntensityObject(), 0.1f, 0.f, 64.f, "%.2f");
      RemixGui::DragFloat("Moon Edge##dusklightAtmo", &skyMoonEdgeSoftnessObject(), 0.01f, 0.f, 1.f, "%.2f");
      ImGui::TextWrapped("Gives back the moon that Hide Sky Billboards removes, without the camera-anchored quad that "
                         "made shadows wander at night. Appearance only - the moonlight comes from the distant light, so "
                         "this does not change how bright the night is to stand in.");
    }

    // Two candidate fixes for one defect, built side by side so the choice could be made by looking.
    // It was: Exempt was run in game on 2026-08-13 and confirmed good, and is the default. Weighted
    // is kept as a taste control for foggy weather rather than as a rival candidate.
    {
      static const char* kSkyFogModes[] = { "Off (untreated)", "Exempt", "Weighted" };
      static int mode;
      mode = std::clamp(skyFogMode(), 0, 2);
      if (RemixGui::Combo("Fog On Sky##dusklightAtmo", &mode, kSkyFogModes, IM_ARRAYSIZE(kSkyFogModes))) {
        skyFogMode.setDeferred(mode);
      }
      if (mode == 2) {
        RemixGui::DragFloat("Sky Fog Amount##dusklightAtmo", &skyFogAmountObject(), 0.01f, 0.f, 1.f, "%.2f");
      }
      switch (mode) {
      case 0:
        ImGui::TextWrapped("The defect, on purpose: the sky is dimmed by the whole depth of the froxel grid and tinted "
                           "towards the fog colour, so it reads dingy against terrain that fades towards the sky's own "
                           "colour. Here to be compared against, not to be used.");
        break;
      case 1:
        ImGui::TextWrapped("Faithful to the original, which drew its sky with fog switched off at any density. Costs any "
                           "light shaft that would have been visible against the sky - that is the same in-scatter. "
                           "Tested in game 2026-08-13 and confirmed good; this is the default.");
        break;
      default:
        ImGui::TextWrapped("A foggy day still veils the sky, and shafts against it survive in proportion, but the sky is "
                           "not erased by a medium calibrated to close in tens of metres. Raise until weather reads, and "
                           "stop before the horizon seam comes back.");
        break;
      }
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Physical sky");
    RemixGui::Checkbox("Simulate Scattering##dusklightAtmo", &physicalSkyObject());
    RemixGui::DragFloat("Max Weight##dusklightAtmo", &physicalMaxWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Blend From##dusklightAtmo", &physicalElevationLowDegreesObject(), 0.5f, -10.f, 45.f, "%.1f deg");
    RemixGui::DragFloat("Blend To##dusklightAtmo", &physicalElevationHighDegreesObject(), 0.5f, 0.f, 90.f, "%.1f deg");
    RemixGui::DragFloat("Weather Weight##dusklightAtmo", &physicalWeatherWeightObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Palette Influence##dusklightAtmo", &paletteInfluenceObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Haze Forward Scatter##dusklightAtmo", &mieAnisotropyObject(), 0.01f, 0.f, 0.95f, "%.2f");
    RemixGui::DragFloat("Multi Scatter##dusklightAtmo", &multiScatterScaleObject(), 0.01f, 0.f, 4.f, "%.2f");
    ImGui::TextWrapped(
      "The blend follows the sun's height because that is where the two skies actually disagree. At midday both are a "
      "plain blue gradient and the change is nearly invisible, while everything it brings - sky fill in shadow, haze "
      "with distance - is not. At dusk the game's version is deliberately more saturated than physics produces, so it "
      "keeps the bottom of the arc. Night and the Twilight Realm are the game's outright.");

    if (skyEnable() && RtxOptions::skyAutoDetect() != SkyAutoDetectMode::None) {
      ImGui::TextWrapped("rtx.skyAutoDetect is not None: the game's own dome is still being captured behind "
                         "the generated sky. Set it to None.");
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Resolved");
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
      ImGui::Text("fog radiance (palette): %.3f, %.3f, %.3f", d.fogRadiance.x, d.fogRadiance.y, d.fogRadiance.z);
      ImGui::Text("fog ambient (used): %.3f, %.3f, %.3f   %.0f%% from the dome%s",
                  d.fogAmbient.x, d.fogAmbient.y, d.fogAmbient.z, d.skyAmbientWeight * 100.0f,
                  d.skyAmbientWeight > 0.0f ? "" : "  (no sky, or the reduction has not run yet)");
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
    // The colour pattern printed as the crossfade it actually is. Reading these three beside the
    // physical weight below is what tells a gradual handover from a broken one: during a weather
    // change the blend should sweep 0 -> 1 once and stop, and the weight should move with it. A
    // blend that jitters, or that sits away from 1.0 with the two patterns equal, means the value
    // is being read at the wrong point in the frame rather than that the fade looks wrong.
    ImGui::Text("area: %s   colpat %d -> %d @ %.2f   sun %.1f deg %s",
                d.outdoor ? "outdoor" : "no sky",
                DusklightEnv::colpatPrev(), DusklightEnv::colpat(), DusklightEnv::colpatBlend(),
                DusklightEnv::sunElevation(), DusklightEnv::sunIsDay() ? "(day)" : "(night)");
    ImGui::Text("physical weight: %.3f%s", d.physicalWeight,
                d.physicalWeight <= 0.0f ? "  (the game's own sky)" : "");

    ImGui::TextWrapped("Everything above is also written to the log once per distinct fog state, so an area that reads "
                       "wrong can be diagnosed from a session log rather than from a description. Read 'peak excess' "
                       "first: above zero means the medium is thicker than the original's fog somewhere, which the ramp "
                       "cannot undo, and lowering Clear Zone Tolerance is the answer.");

    ImGui::Unindent();
    ImGui::Unindent();
  }
}
