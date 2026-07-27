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
#include "rtx/pass/dusklight/dusklight_composite_args.h"

#include <rtx_shaders/dusklight_sky.h>
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

      PUSH_CONSTANTS(DusklightSkyArgs)

      BEGIN_PARAMETER()
        RW_TEXTURE2D(DUSKLIGHT_SKY_OUTPUT)
      END_PARAMETER()
    };

    PREWARM_SHADER_PIPELINE(DusklightSkyShader);

    // Small on purpose. The dome is a smooth gradient with no detail to lose, and every ray that
    // misses geometry samples it, so a compact image stays resident in cache. It is also the
    // shape a physically based sky-view lookup wants later, which keeps that phase a change of
    // contents rather than a change of plumbing.
    constexpr uint32_t kSkyWidth = 256;
    constexpr uint32_t kSkyHeight = 128;

    // Any nonzero value works; it only has to be stable across frames and distinct from every
    // handle the game's own bridge creates for its analytical lights.
    constexpr uint64_t kDomeLightHandle = 0xD05C11'A7'000501ull;

    // World is Y up; Remix's lat-long sampling puts the pole on Z. This carries the swap, so the
    // generator can work entirely in light space and treat +Z as up. Columns are the images of the
    // world basis vectors, so world +Y lands on light +Z and the U axis' phi = atan2(x, y) ends up
    // measuring the same angle about the up axis that the game reports its sun azimuth in.
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

    // Match the game's ramp where it is half opaque. That point is the whole derivation: it is
    // scale free, so the same expression covers a scripted whiteout closing in over two metres and
    // an open field hazing out over two hundred, with no per area handling anywhere.
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

    if (m_smoothedFroxelMaxDistance <= 0.0f || !isFinite(m_smoothedFroxelMaxDistance)) {
      m_smoothedFroxelMaxDistance = target;
    } else {
      const float rate = std::clamp(froxelSmoothingRate(), 0.0f, 1.0f);
      m_smoothedFroxelMaxDistance += (target - m_smoothedFroxelMaxDistance) * rate;
    }

    out.froxelMaxDistance = m_smoothedFroxelMaxDistance;
    out.fogValid = true;

    return out;
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

    // Scalar extinction, deliberately. The game's fog dims every channel by the same fraction
    // because it was a lerp driven by one scalar ramp; a per channel extinction pulled out of the
    // fog colour would make the fog's strength channel dependent, which the original never does.
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

    // Created once and kept for the life of the device, not rebuilt per frame: the dome light
    // holds a bindless index into this image, and losing it for even a single frame drops the sky
    // back to Remix's own probe, which is a different and much dimmer picture.
    if (m_skyTexture.image == nullptr) {
      m_skyTexture = Resources::createImageResource(
        baseCtx, "dusklight sky", VkExtent3D { kSkyWidth, kSkyHeight, 1 },
        VK_FORMAT_R16G16B16A16_SFLOAT);
    }

    if (m_skyTexture.view == nullptr) {
      ONCE(Logger::err("[Dusklight] failed to create the generated sky image; falling back to Remix's sky probe."));
      return;
    }

    {
      ScopedGpuProfileZone(ctx, "Dusklight Sky");
      ctx->setFramePassStage(RtxFramePassStage::FrameBegin);
      ctx->setPushConstantBank(DxvkPushConstantBank::RTX);

      constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

      DusklightSkyArgs pushArgs = {};
      pushArgs.imageSize = { kSkyWidth, kSkyHeight };
      pushArgs.sunAzimuthRadians = DusklightEnv::sunAzimuth() * kDegreesToRadians;
      pushArgs.horizonSharpness = std::max(skyHorizonSharpness(), 1e-3f);
      pushArgs.skyColor = sRGBGammaToLinear(sanitizeColor(DusklightEnv::skyColor()));
      pushArgs.groundFraction = std::clamp(skyGroundFraction(), 0.0f, 1.0f);
      pushArgs.kasumiInner = sRGBGammaToLinear(sanitizeColor(DusklightEnv::kasumiInner()));
      pushArgs.intensity = std::max(skyIntensity(), 0.0f);
      pushArgs.kasumiOuter = sRGBGammaToLinear(sanitizeColor(DusklightEnv::kasumiOuter()));

      ctx->pushConstants(0, sizeof(pushArgs), &pushArgs);

      const VkExtent3D workgroups = util::computeBlockCount(
        VkExtent3D { kSkyWidth, kSkyHeight, 1 }, VkExtent3D { 16, 16, 1 });

      ctx->bindResourceView(DUSKLIGHT_SKY_OUTPUT, m_skyTexture.view, nullptr);
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

    if (skyEnable() && RtxOptions::skyAutoDetect() != SkyAutoDetectMode::None) {
      ImGui::TextWrapped("rtx.skyAutoDetect is not None: the game's own dome is still being captured behind "
                         "the generated sky. Set it to None.");
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Resolved");
    const Derived& d = derived();
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
    ImGui::Text("area: %s", d.outdoor ? "outdoor" : "no sky");

    ImGui::TextWrapped("The constants above were derived analytically and have never been measured against a "
                       "running build. Read the game's own fog range off the Dusklight tab in the places that "
                       "look wrong and tune from there.");

    ImGui::Unindent();
    ImGui::Unindent();
  }
}
