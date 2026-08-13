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
#include "rtx_dusklight_atmosphere.h"
#include "rtx_dusklight_game.h"
#include "rtx_scene_manager.h"
#include "rtx_light_manager.h"
#include "rtx_options.h"
#include "rtx_types.h"
#include "dxvk_device.h"
#include "dxvk_scoped_annotation.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx/pass/dusklight/dusklight_sky.h"
#include "rtx/pass/dusklight/dusklight_atmosphere.h"
#include "rtx/pass/dusklight/dusklight_composite_args.h"

#include <rtx_shaders/dusklight_sky.h>
#include <rtx_shaders/dusklight_transmittance.h>
#include <rtx_shaders/dusklight_multiscatter.h>
#include "rtx_imgui.h"
#include "../../util/util_color.h"

#include <algorithm>
#include <cmath>

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

  void DxvkDusklightAtmosphere::releaseTargetResource() {
    m_skyTexture.reset();
    m_skyTextureIndex = UINT32_MAX;
    m_transmittanceLut.reset();
    m_multiScatterLut.reset();
    // Forget what the tables held, or they would be considered current after being destroyed.
    m_lutSkyColor = Vector3(-1.0f, -1.0f, -1.0f);
    m_lutPaletteInfluence = -1.0f;
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

  DxvkDusklightAtmosphere::Derived DxvkDusklightAtmosphere::resolve() const {
    Derived out = {};
    out.outdoor = DusklightEnv::enable() && !DusklightEnv::skyHidden();
    // Resolved before the fog checks below, deliberately: an area can have a sky and no fog, and
    // the sky must not go dark just because nobody put haze in the room.
    out.physicalWeight = resolvePhysicalWeight();

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

    out.sigma = std::max(std::log(2.0f) / zHalf, 0.0f) * std::max(densityScale(), 0.0f);
    out.rampStart = start;
    out.rampEnd = end;

    // The game authors its fog colour to be blended over a finished, display referred image. Here
    // it is a quantity of light in a linear frame that has not been tone mapped yet, so it is
    // decoded rather than used raw. The two halves of the range split share this, which is what
    // keeps the near medium and the far ramp the same colour.
    out.fogRadiance = sRGBGammaToLinear(sanitizeColor(DusklightEnv::fogColor())) *
                      std::max(fogRadianceScale(), 0.0f);

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

    return out;
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
    // towards an authored colour, not a simulation, so an unlit room still had coloured fog.
    multiScatteringEstimate = d.fogRadiance * std::max(multiScatteringScale(), 0.0f);
  }

  void DxvkDusklightAtmosphere::fillCompositeArgs(DusklightCompositeArgs& args) const {
    args = {};

    if (!active()) {
      return;
    }

    const Derived& d = m_derived;

    args.enable = 1;
    args.fogColor = d.fogRadiance;
    args.rampStart = d.rampStart;
    args.rampEnd = d.rampEnd;
    args.handoverDistance = d.froxelMaxDistance;
    // The same weight the sky itself was blended with. Once the sky stops coming from the palette,
    // the palette's fog colour stops describing it, and letting the fog keep the old colour would
    // leave the horizon one weather and the air in front of it another.
    args.skyColorWeight = skyActive() ? std::clamp(d.physicalWeight, 0.0f, 1.0f) : 0.0f;
    args.skyFogMode = static_cast<uint32_t>(std::clamp(skyFogMode(), 0, 2));
    args.skyFogAmount = std::clamp(skyFogAmount(), 0.0f, 1.0f);
  }

  void DxvkDusklightAtmosphere::prepareSceneData(Rc<RtxContext> ctx, SceneManager& sceneManager) {
    if (!skyActive()) {
      return;
    }

    if (RtxOptions::skyAutoDetect() != SkyAutoDetectMode::None) {
      ONCE(Logger::warn("[Dusklight] rtx.dusklight.atmosphere.skyEnable is on while rtx.skyAutoDetect is not None. "
                        "The auto detected sky keeps rasterizing into Remix's own probe, so the game's dome is still being "
                        "captured behind the generated one. Set rtx.skyAutoDetect = None."));
    }

    Rc<DxvkContext> baseCtx = ctx;

    // Created once and kept for the life of the device: the dome light holds a bindless index into
    // this image, and losing it for even a single frame drops the sky back to Remix's own probe.
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
    RemixGui::DragFloat("Half Density Floor##dusklightAtmo", &zHalfMinObject(), 1.0f, 1.f, 10000.f, "%.0f units");
    RemixGui::DragFloat("Density Scale##dusklightAtmo", &densityScaleObject(), 0.01f, 0.f, 8.f, "%.2f");
    RemixGui::DragFloat("Fog Radiance Scale##dusklightAtmo", &fogRadianceScaleObject(), 0.01f, 0.f, 16.f, "%.2f");
    RemixGui::DragFloat("Multi Scattering##dusklightAtmo", &multiScatteringScaleObject(), 0.01f, 0.f, 4.f, "%.2f");

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

    // Two candidate fixes for one defect, side by side so they can be compared rather than argued
    // about. One of them is meant to be deleted once the comparison has been made.
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
                           "light shaft that would have been visible against the sky - that is the same in-scatter.");
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
      ImGui::Text("fog radiance: %.3f, %.3f, %.3f", d.fogRadiance.x, d.fogRadiance.y, d.fogRadiance.z);
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

    ImGui::TextWrapped("The constants above were derived analytically and have never been measured against a "
                       "running build. Read the game's own fog range off the Dusklight tab in the places that "
                       "look wrong and tune from there.");

    ImGui::Unindent();
    ImGui::Unindent();
  }
}
