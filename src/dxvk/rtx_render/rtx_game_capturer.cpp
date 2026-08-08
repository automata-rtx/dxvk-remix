/*
* Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx_game_capturer.h"

#include "rtx_context.h"
#include "rtx_types.h"
#include "rtx_camera.h"
#include "rtx_options.h"
#include "rtx_utils.h"
#include "rtx_instance_manager.h"
#include "rtx_scene_manager.h"
#include "rtx_materials.h"
#include "rtx_dusklight_texrep.h"

#include "../dxvk_device.h"

#include "../../util/log/log.h"
#include "../../util/util_once.h"
#include "../../util/config/config.h"
#include "../../util/util_filesys.h"
#include "../../util/util_vector.h"
#include "../../util/util_window.h"

#include "../../lssusd/game_exporter.h"
#include "../../lssusd/game_exporter_paths.h"
#include "../../lssusd/game_exporter_types.h"
#include "../../lssusd/usd_common.h"
#include "../../lssusd/usd_include_begin.h"
#include <pxr/base/gf/rotation.h>
#include "../../lssusd/usd_include_end.h"

#include "rtx_matrix_helpers.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include "rtx_lights.h"

#include "../util/util_global_time.h"

#include <filesystem>

#define BASE_DIR (util::RtxFileSys::path(util::RtxFileSys::Captures).string())

namespace dxvk {
  namespace {
    static inline pxr::GfMatrix4d matrix4ToGfMatrix4d(const Matrix4& mat4) {
      const auto& float4x4 = reinterpret_cast<const float(&)[4][4]>(mat4);
      return pxr::GfMatrix4d{pxr::GfMatrix4f(float4x4)};
    }
    static inline pxr::VtMatrix4dArray matrix4VecToGfMatrix4dVec(const std::vector<Matrix4>& mat4s) {
      pxr::VtMatrix4dArray result(mat4s.size());
      for (int i = 0; i < mat4s.size(); ++i) {
        const auto& float4x4 = reinterpret_cast<const float(&)[4][4]>(mat4s[i]);
        result[i] = pxr::GfMatrix4d { pxr::GfMatrix4f(float4x4) };
      }
      return result;
    }
    static inline std::string buildStagePath(const std::string& stageName) {
      std::stringstream stagePathSS;
      stagePathSS << BASE_DIR << "/" << stageName;
      const auto stagePath =
        std::filesystem::absolute(std::filesystem::path(stagePathSS.str()));
      return stagePath.string();
    }
    static lss::RenderingMetaData createDrawCallMetadata(const RtInstance& rtInstance) {
      lss::RenderingMetaData meta;
      meta.alphaTestEnabled = rtInstance.surface.alphaState.alphaTestType != AlphaTestType::kAlways;
      meta.alphaTestReferenceValue = rtInstance.surface.alphaState.alphaTestReferenceValue;
      meta.alphaTestCompareOp = (uint32_t) rtInstance.surface.alphaState.alphaTestType;
      meta.alphaBlendEnabled = !rtInstance.surface.alphaState.isBlendingDisabled;
      meta.srcColorBlendFactor = (uint32_t) rtInstance.surface.blendModeState.colorSrcFactor;
      meta.dstColorBlendFactor = (uint32_t) rtInstance.surface.blendModeState.colorDstFactor;
      meta.colorBlendOp = (uint32_t) rtInstance.surface.blendModeState.colorBlendOp;
      meta.srcAlphaBlendFactor = (uint32_t) rtInstance.surface.blendModeState.alphaSrcFactor;
      meta.dstAlphaBlendFactor = (uint32_t) rtInstance.surface.blendModeState.alphaDstFactor;
      meta.alphaBlendOp = (uint32_t) rtInstance.surface.blendModeState.alphaBlendOp;
      meta.writeMask = (uint32_t) rtInstance.surface.blendModeState.writeMask;
      meta.textureColorArg1Source = (uint32_t) rtInstance.surface.textureColorArg1Source;
      meta.textureColorArg2Source = (uint32_t) rtInstance.surface.textureColorArg2Source;
      meta.textureColorOperation = (uint32_t) rtInstance.surface.textureColorOperation;
      meta.textureAlphaArg1Source = (uint32_t) rtInstance.surface.textureAlphaArg1Source;
      meta.textureAlphaArg2Source = (uint32_t) rtInstance.surface.textureAlphaArg2Source;
      meta.textureAlphaOperation = (uint32_t) rtInstance.surface.textureAlphaOperation;
      meta.tFactor = rtInstance.surface.tFactor;
      meta.isTextureFactorBlend = rtInstance.surface.isTextureFactorBlend;
      meta.isVertexColorBakedLighting = rtInstance.surface.isVertexColorBakedLighting;
      return meta;
    }

    static std::string getBakedSkyProbeName(const std::string& captureName) {
      const std::string bakedSkyProbeSuffix("_T_SkyProbe" + lss::ext::dds);
      return captureName + bakedSkyProbeSuffix;
    }
  }

  // For capture tests, we cannot include the config data because it may contain paths/settings which are respective to the users PC.
  static const bool s_captureRemixConfigs = env::getEnvVar("RTX_CAPTURE_REMIX_CONFIGS", true);

  size_t GameCapturer::Capture::nextId = 0;

  GameCapturer::GameCapturer(DxvkDevice* const pDevice,
                             SceneManager& sceneManager,
                             AssetExporter& exporter)
    : m_pDevice(pDevice)
    , m_sceneManager(sceneManager)
    , m_exporter(exporter)
    , m_options{ getOptions() } {

    if(!env::getEnvVar("DXVK_RTX_CAPTURE_ENABLE_ON_FRAME").empty()) {
      Logger::info(str::format("[GameCapturer] DXVK_RTX_CAPTURE_ENABLE_ON_FRAME: ", env::getEnvVar("DXVK_RTX_CAPTURE_ENABLE_ON_FRAME")));
    }
    env::createDirectory(BASE_DIR);
    env::createDirectory(BASE_DIR + lss::commonDirName::texDir);
    env::createDirectory(BASE_DIR + lss::commonDirName::matDir);
    lss::GameExporter::setMultiThreadSafety(true);
  }

  GameCapturer::~GameCapturer() {
  }

  void GameCapturer::step(const Rc<DxvkContext> ctx, const HWND hwnd) {
    trigger(ctx);
    if(m_state.has<State::Initializing>()) {
      initCapture(ctx, hwnd);
    }
    if (m_state.has<State::Capturing>()) {
      capture(ctx, GlobalTime::get().deltaTimeMs());
    }
    if (m_state.has<State::BeginExport>()) {
      exportUsd(ctx);
    }
  }

  void GameCapturer::setInstanceUpdateFlag(const RtInstance& rtInstance, const InstFlag flag) {
    if (!m_state.has<State::Capturing>()) {
      return;
    }
    m_pCap->instanceFlags[rtInstance.getId()] |= (1 << uint8_t(flag));
  }

  void GameCapturer::trigger(const Rc<DxvkContext> ctx) {
    if (m_bTriggerCapture) {
      m_bTriggerCapture = false;
      if(isIdle()) {
        if (RtxOptions::getEnableAnyReplacements() && m_sceneManager.areAllReplacementsLoaded()) {
          Logger::warn("[GameCapturer] Cannot begin capture when replacement assets are enabled/loaded.");
        } else if (m_state.has<State::Capturing>()) {
          Logger::warn("[GameCapturer] Cannot begin new capture, one currently in progress.");
        } else {
          m_state.set<State::Initializing, true>();
        }
      }
    }
  }

  void GameCapturer::initCapture(const Rc<DxvkContext> ctx, const HWND hwnd) {
    assert(m_state.has<State::Initializing>());
    assert(!m_state.has<State::Capturing>());
    
    m_options = getOptions();
    
    m_pCap = std::make_unique<Capture>();
    m_pCap->idStr = hashToString(Capture::nextId++).substr(4, 4);
    m_pCap->bCaptureInstances = m_options.bCaptureInstances;
    m_pCap->bSkyProbeBaked = false;
    if (m_pCap->bCaptureInstances) {
      prepareInstanceStage(ctx);
    }
    Logger::info("[GameCapturer][" + m_pCap->idStr + "] New capture");
    m_pCap->instanceFlags.clear();

    m_pCap->hwnd = hwnd;

    m_state.set<State::Capturing, true>();
    m_state.set<State::Initializing, false>();
    m_state.set<State::Complete, false>();
    m_state.set<State::Failed, false>();
  }
  
  void GameCapturer::prepareInstanceStage(const Rc<DxvkContext> ctx) {
    const auto ogStagePathStr = buildStagePath(m_options.instanceStageName);
    std::string stagePathStr;
    if (RtxOptions::captureOverwriteExistingCapture()) {
      if (std::filesystem::exists(ogStagePathStr)) {
        std::filesystem::remove(ogStagePathStr);
      }
      stagePathStr = ogStagePathStr;
    } else {
      stagePathStr = env::dedupeFilename(ogStagePathStr);
    }
    const auto resolvedFileName = std::filesystem::path(stagePathStr).stem().string();
    m_pCap->instance.stageName = resolvedFileName;
    m_pCap->instance.stagePath = stagePathStr;
    m_exporter.generateSceneThumbnail(ctx, BASE_DIR + lss::commonDirName::thumbDir, resolvedFileName);
  }

  void GameCapturer::capture(const Rc<DxvkContext> ctx, const float frameTimeMilliseconds) {
    assert(m_state.has<State::Capturing>());

    m_pCap->currentFrameNum += (frameTimeMilliseconds * 0.001f) * static_cast<float>(m_options.fps);
    captureFrame(ctx);

    if (m_pCap->numFramesCaptured >= m_options.numFrames) {
      m_state.set<State::BeginExport, true>();
      m_state.set<State::Capturing, false>();
    }
  }

  void GameCapturer::captureFrame(const Rc<DxvkContext> ctx) {
    Logger::debug("[GameCapturer][" + m_pCap->idStr + "] Begin frame capture");
    if (m_pCap->bCaptureInstances) {
      captureCamera();
      captureLights();
    }
    captureInstances(ctx);
    // After captureInstances, not before: that is where a sky probe gets baked from sky-camera
    // geometry, and the probe is the better source when the game draws one. Running the dome-light
    // fallback second lets geometry win within a frame. The choice is still made on the first frame
    // either source is available, so a game that has both but shows its sky geometry late would
    // latch the dome light - not a case that exists here, where there is no sky geometry at all.
    if (m_pCap->bCaptureInstances) {
      captureSkyDomeLight(ctx);
    }
    ++m_pCap->numFramesCaptured;
    Logger::debug("[GameCapturer][" + m_pCap->idStr + "] End frame capture");
  }

  void GameCapturer::captureCamera() {
    auto& sceneCamera = m_sceneManager.getCamera();
    auto& capCamera = m_pCap->camera; // Convenience for captured camera
    if (isnan(capCamera.firstTime)) {
      Logger::debug("[GameCapturer][" + m_pCap->idStr + "][Camera] New");

      // Step 1: Extract easy-to-determine properties from helper function
      float shearX, shearY;
      const auto& viewToProj = sceneCamera.getViewToProjection();
      bool bIsProjLhs = false;
      decomposeProjection(viewToProj,
                          capCamera.aspectRatio,
                          capCamera.fov,
                          capCamera.nearPlane,
                          capCamera.farPlane,
                          shearX,
                          shearY,
                          bIsProjLhs,
                          capCamera.isReverseZ, true);
      // Infinite projection is legit, but USD doesnt take kindly to it
      constexpr float kMaxFarPlane = 100000000;
      capCamera.farPlane = clampHigh(capCamera.farPlane, kMaxFarPlane);

      // Step 2: Handedness, Orientation, Inversion correction
      capCamera.proj.coord = bIsProjLhs ? lss::LHS : lss::RHS;
      capCamera.proj.bInv = (viewToProj[0][0] * viewToProj[1][1] < 0.0f);
      // Use viewToWorld here instead of view matrix b/c it's easier to extract the camera vectors and matches isMirrorTransform
      const auto viewToWorld = sceneCamera.getViewToWorld();
      if (capCamera.proj.bInv) {
        // Check if the up vector in view matrix is upside down
        const auto& up = viewToWorld[1].xyz();
        if ((!RtxOptions::zUp() && dot(up, Vector3d(0.0, 1.0, 0.0)) < 0.0) ||
            (RtxOptions::zUp() && dot(up, Vector3d(0.0, 0.0, 1.0)) < 0.0)) {
          capCamera.view.bInv = true;
        }
      }
      capCamera.view.coord = isMirrorTransform(viewToWorld) ? lss::LHS : lss::RHS;
      Logger::debug(str::format("Is Camera Left Handed: ", capCamera.isLHS()));
      
      // Set first frame time (should be 0.f)
      capCamera.firstTime = m_pCap->currentFrameNum;
    }
    assert(!isnan(capCamera.fov));
    assert(!isnan(capCamera.aspectRatio));
    assert(!isnan(capCamera.nearPlane));
    assert(!isnan(capCamera.farPlane));
    capCamera.finalTime = m_pCap->currentFrameNum;

    // Step 3: Construct capture Camera
    pxr::GfVec3d position = pxr::GfVec3d(sceneCamera.getPosition().x, sceneCamera.getPosition().y, sceneCamera.getPosition().z);
    pxr::GfVec3d direction = pxr::GfVec3d(sceneCamera.getDirection().x, sceneCamera.getDirection().y, sceneCamera.getDirection().z);
    pxr::GfVec3d up = pxr::GfVec3d(sceneCamera.getUp().x, sceneCamera.getUp().y, sceneCamera.getUp().z);

    // Flip the up vector of camera if the game flips this vector in the view matrix
    up = capCamera.view.bInv ? -up : up;

    // Transform camera to match the world space corrections (flipping or mirror) in GameExporter::exportInstances
    if (capCamera.proj.bInv && !capCamera.view.bInv) {
      position[0] *= -1.0;
      position[1] *= -1.0;
      direction[0] *= -1.0;
      direction[1] *= -1.0;
      up[0] *= -1.0;
    } else {
      if (capCamera.isLHS() && !capCamera.view.bInv) {
        position[0] *= -1.0;
        direction[0] *= -1.0;
      }
    }

    pxr::GfMatrix4d worldToView;
    worldToView.SetLookAt(position, position + direction, up);
    worldToView.Orthonormalize();
    capCamera.xforms.push_back({ m_pCap->currentFrameNum, worldToView.GetInverse() });
  }

  void GameCapturer::captureLights() {
    auto captureLight = [&](const RtLight& rtLight, const std::optional<XXH64_hash_t> keyOverride) {
      assert(rtLight.getInitialHash() != 0);
      switch (rtLight.getType()) {
      default:
      case RtLightType::Sphere:
        captureSphereLight(rtLight.getSphereLight(), keyOverride);
        break;
      case RtLightType::Rect:
        // Todo: Handle Rect lights
        // Note: bounded rather than per-frame - API lights reach this every frame, and an
        // unimplemented type used to be able to fill a log on its own.
        ONCE(Logger::err("[GameCapturer][" + m_pCap->idStr + "] RectLight not implemented"));
        break;
      case RtLightType::Disk:
        // Todo: Handle Disk lights
        ONCE(Logger::err("[GameCapturer][" + m_pCap->idStr + "] DiskLight not implemented"));
        break;
      case RtLightType::Cylinder:
        // Todo: Handle Cylinder lights
        ONCE(Logger::err("[GameCapturer][" + m_pCap->idStr + "] CylinderLight not implemented"));
        break;
      case RtLightType::Distant:
        captureDistantLight(rtLight.getDistantLight(), keyOverride);
        break;
      }
    };

    for (auto&& pair : m_sceneManager.getLightManager().getLightTable()) {
      captureLight(pair.second, std::nullopt);
    }
    for (auto&& pair : m_sceneManager.getLightManager().getExternallyTrackedLightTable()) {
      captureLight(pair.second, std::nullopt);
    }
    // Lights the application submitted through remixapi_CreateLight/remixapi_DrawLightInstance.
    // Keyed by the application's handle rather than the light's parameter hash: a game that
    // re-submits a moving light every frame changes the latter and would otherwise write one
    // light per frame into the capture instead of one light.
    for (const auto& [handle, rtLight] : m_sceneManager.getLightManager().getActiveExternalLights()) {
      captureLight(rtLight, static_cast<XXH64_hash_t>(handle));
    }
  }

  void GameCapturer::captureSphereLight(const dxvk::RtSphereLight& rtLight,
                                        const std::optional<XXH64_hash_t> keyOverride) {
    const XXH64_hash_t hash = keyOverride.value_or(rtLight.getHash());
    pxr::GfRotation  rotation;
    rotation.SetIdentity();
    if (m_pCap->sphereLights.count(hash) == 0) {
      const std::string name = dxvk::hashToString(hash);
      lss::SphereLight& sphereLight = m_pCap->sphereLights[hash];
      sphereLight.lightName = name;
      const auto colorAndIntensity = rtLight.getColorAndIntensity();
      sphereLight.color[0] = colorAndIntensity.r;
      sphereLight.color[1] = colorAndIntensity.g;
      sphereLight.color[2] = colorAndIntensity.b;
      sphereLight.intensity = colorAndIntensity.w;
      sphereLight.radius = rtLight.getRadius();
      sphereLight.xforms.reserve(m_options.numFrames - m_pCap->numFramesCaptured);
      sphereLight.firstTime = m_pCap->currentFrameNum;
      const dxvk::RtLightShaping& shaping = rtLight.getShaping();
      if (shaping.getEnabled()) {
        sphereLight.shapingEnabled = true;
        sphereLight.coneAngleDegrees = std::acos(shaping.getCosConeAngle()) * kRadiansToDegrees;
        sphereLight.coneSoftness = shaping.getConeSoftness();
        sphereLight.focusExponent = shaping.getFocusExponent();
        rotation = pxr::GfRotation(-pxr::GfVec3d::ZAxis(), pxr::GfVec3f(&shaping.getDirection()[0]));
      }
      Logger::debug("[GameCapturer][" + m_pCap->idStr + "][SphereLight:" + name + "] New");
    }

    lss::SphereLight& sphereLight = m_pCap->sphereLights[hash];
    const auto position = rtLight.getPosition();
    pxr::GfMatrix4d usdXform(rotation, pxr::GfVec3f(&position[0]));
    sphereLight.xforms.push_back({ m_pCap->currentFrameNum, usdXform });
    sphereLight.finalTime = m_pCap->currentFrameNum;
  }

  void GameCapturer::captureDistantLight(const RtDistantLight& rtLight,
                                         const std::optional<XXH64_hash_t> keyOverride) {
    const XXH64_hash_t hash = keyOverride.value_or(rtLight.getHash());
    // Note: this consulted `sphereLights` before 2026-08-08, so the "is this light new" test could
    // never be false for a distant light and the block below re-ran every frame. The damage was to
    // `firstTime`, which ended up holding the *last* captured frame; exportDistantLights then wrote
    // an intensity of zero from t=0 up to that frame, so a multi-frame capture opened in the
    // toolkit with no sun at all. Single-frame captures were unaffected, which is why it survived.
    if (m_pCap->distantLights.count(hash) == 0) {
      const std::string name = dxvk::hashToString(hash);
      lss::DistantLight& distantLight = m_pCap->distantLights[hash];
      distantLight.lightName = name;
      const auto colorAndIntensity = rtLight.getColorAndIntensity();
      distantLight.color[0] = colorAndIntensity.r;
      distantLight.color[1] = colorAndIntensity.g;
      distantLight.color[2] = colorAndIntensity.b;
      distantLight.intensity = colorAndIntensity.w;
      distantLight.angleDegrees = rtLight.getHalfAngle() * 2.0 * kRadiansToDegrees;
      distantLight.direction = pxr::GfVec3f(rtLight.getDirection().data);
      distantLight.firstTime = m_pCap->currentFrameNum;
      Logger::debug("[GameCapturer][" + m_pCap->idStr + "][DistantLight:" + name + "] New");
    }
    lss::DistantLight& distantLight = m_pCap->distantLights[hash];
    distantLight.finalTime = m_pCap->currentFrameNum;
  }

  void GameCapturer::captureSkyDomeLight(const Rc<DxvkContext> ctx) {
    // A sky probe baked from geometry drawn with a sky camera is the better source when it exists -
    // it is the sky the game itself drew, at whatever resolution it drew it. This is the fallback
    // for runtimes whose sky is an API dome light and never reaches the rasterizer at all, which
    // until now produced captures with no sky and no sky lighting.
    if (m_pCap->bSkyProbeBaked || !captureApiDomeLightAsSky()) {
      return;
    }

    DomeLight domeLight;
    if (!m_sceneManager.getLightManager().getActiveExternalDomeLight(domeLight)) {
      return;
    }
    // A dome light whose texture has not finished uploading resolves to a null view; dumping that
    // would write a black sky and then latch bSkyProbeBaked so the real one never lands.
    if (!domeLight.texture.isValid() || domeLight.texture.isImageEmpty()) {
      return;
    }

    const std::string skyProbeFilename = getBakedSkyProbeName(m_pCap->instance.stageName);
    m_exporter.dumpImageToFile(ctx, BASE_DIR + lss::commonDirName::texDir,
                               skyProbeFilename,
                               domeLight.texture.getImageView()->image());
    m_pCap->bSkyProbeBaked = true;
    m_pCap->bSkyFromDomeLight = true;
    // The texture is authored in the light's own space; the exporter needs the other direction to
    // place it back in the capture's world.
    m_pCap->skyDomeLightToWorld = inverse(domeLight.worldToLight);
    m_pCap->skyDomeRadiance = domeLight.radiance;

    const VkExtent3D extent = domeLight.texture.getImageView()->image()->info().extent;
    Logger::info(str::format(
      "[GameCapturer][", m_pCap->idStr, "][SkyDomeLight] Captured API dome light as the sky: ",
      skyProbeFilename, " ", extent.width, "x", extent.height,
      " radiance=(", domeLight.radiance.x, ", ", domeLight.radiance.y, ", ", domeLight.radiance.z, ")"));
  }

  void GameCapturer::captureInstances(const Rc<DxvkContext> ctx) {
    for (const RtInstance* pRtInstance : m_sceneManager.getInstanceTable()) {
      assert(pRtInstance->getBlas() != nullptr);

      const XXH64_hash_t instanceId = pRtInstance->getId();
      if (instanceId == UINT64_MAX) {
        // Ignore "virtual" instances, as they are used primarily for special render
        // passes, rather than representing real entities that we want captured
        continue;
      }
      assert(instanceId != UINT64_MAX);

      if (pRtInstance->getBlas()->input.cameraType == CameraType::Sky) {
        if (!m_pCap->bSkyProbeBaked) {
          const std::string skyProbeFilename = getBakedSkyProbeName(m_pCap->instance.stageName);
          m_exporter.bakeSkyProbe(ctx, BASE_DIR + lss::commonDirName::texDir, skyProbeFilename);
          m_pCap->bSkyProbeBaked = true;
          Logger::debug("[GameCapturer][" + m_pCap->idStr + "][SkyProbe] Bake scheduled to " + skyProbeFilename);
        }
      }

      const uint8_t instanceFlags = m_pCap->instanceFlags[instanceId];
      const bool bIsNew = m_pCap->instances.count(instanceId) == 0;
      const bool bPointsUpdate = checkInstanceUpdateFlag(instanceFlags, InstFlag::PositionsUpdate);
      const bool bNormalsUpdate = checkInstanceUpdateFlag(instanceFlags, InstFlag::NormalsUpdate);
      const bool bIndexUpdate = checkInstanceUpdateFlag(instanceFlags, InstFlag::IndexUpdate);
      const bool bXformUpdate = checkInstanceUpdateFlag(instanceFlags, InstFlag::XformUpdate);
      Instance& instance = m_pCap->instances[instanceId];
      if (bIsNew) {
        newInstance(ctx, *pRtInstance);
      }
      if (m_pCap->bCaptureInstances && !bIsNew && (bPointsUpdate || bNormalsUpdate || bIndexUpdate)) {
        const BlasEntry* pBlas = pRtInstance->getBlas();
        assert(pBlas != nullptr);

        captureMesh(ctx, instance.meshHash, *pBlas, pRtInstance->getCategoryFlags(), false, bPointsUpdate, bNormalsUpdate, bIndexUpdate, pRtInstance->isFrontFaceFlipped);
      }
      if (m_pCap->bCaptureInstances && (bIsNew || bXformUpdate)) {
        pxr::GfMatrix4d xform { 1.0 };
        if (m_pCap->camera.proj.bInv && !m_pCap->camera.view.bInv) {
          // Add flip transformation for games that render world upside down
          xform[0][0] = xform[1][1] = -1.0;
        } else {
          // Add mirror transformation for games that need additional basis changing because view and projection transformation have different handedness.
          // Note1: Don't do this transformation if the view and projection have same handedness, they will automatically cancel out.
          // Note2: Don't do this transformation if the game also flip the up vector in view matrix.
          //        Because it will automatically cancel out the basis changing and we only need to send an identity matrix here.
          if (m_pCap->camera.isLHS() && !m_pCap->camera.view.bInv) {
            xform[0][0] = -1.0;
          }
        }
        instance.lssData.xforms.push_back({ m_pCap->currentFrameNum, matrix4ToGfMatrix4d(pRtInstance->getTransform()) * xform });
        const SkinningData& skinData = pRtInstance->getBlas()->input.getSkinningState();
        if (skinData.numBones > 0) {
          instance.lssData.boneXForms.push_back({ m_pCap->currentFrameNum, matrix4VecToGfMatrix4dVec(skinData.pBoneMatrices) });
        }
      }
      instance.lssData.finalTime = m_pCap->currentFrameNum;
      instance.lssData.isSky = (pRtInstance->getBlas()->input.cameraType == CameraType::Sky);
      instance.lssData.metadata = createDrawCallMetadata(*pRtInstance);
    }
  }

  void GameCapturer::newInstance(const Rc<DxvkContext> ctx, const RtInstance& rtInstance) {
    const BlasEntry* pBlas = rtInstance.getBlas();
    assert(pBlas != nullptr);
    const XXH64_hash_t matHash = rtInstance.getMaterialDataHash();
    const XXH64_hash_t meshHash = pBlas->input.getHash(RtxOptions::geometryAssetHashRule());
    assert(meshHash != 0);

    const LegacyMaterialData& material = pBlas->getMaterialData(matHash);

    const bool bIsNewMat = (matHash != 0x0) && (m_pCap->materials.count(matHash) == 0);
    if (bIsNewMat) {
      captureMaterial(ctx, material, !rtInstance.surface.alphaState.isFullyOpaque);
    }

    bool bIsNewMesh = false;
    size_t instanceNum = 0;
    {
      std::lock_guard lock(m_meshMutex);
      bIsNewMesh = m_pCap->meshes.count(meshHash) == 0;
      if (bIsNewMesh) {
        m_pCap->meshes[meshHash] = std::make_shared<Mesh>();
        m_pCap->meshes[meshHash]->instanceCount = 0;
        m_pCap->meshes[meshHash]->matHash = matHash;
      }
      instanceNum = m_pCap->meshes[meshHash]->instanceCount++;
    }
    if (bIsNewMesh) {
      captureMesh(ctx, meshHash, *pBlas, rtInstance.getCategoryFlags(), true, true, true, true, rtInstance.isFrontFaceFlipped);
    }

    const XXH64_hash_t instanceId = rtInstance.getId();
    Instance& instance = m_pCap->instances[instanceId];
    instance.meshHash = meshHash;
    instance.matHash = matHash;
    instance.skeletonBinding = pBlas->input.dusklightSkeletonBinding;
    instance.meshInstNum = instanceNum;
    instance.lssData.firstTime = m_pCap->currentFrameNum;

    Logger::debug("[GameCapturer][" + m_pCap->idStr + "][Inst:" + hashToString(instanceId) + "] New");
  }

  void GameCapturer::captureMaterial(const Rc<DxvkContext> ctx, const LegacyMaterialData& materialData, const bool bEnableOpacity) {
    lss::Material lssMat; // to be populated

    // Resolve material name
    const std::string matName = dxvk::hashToString(materialData.getHash());
    lssMat.matName = matName;
    // Export Textures
    const std::string albedoTexFilename(matName + lss::ext::dds);

    // Dusklight HD texture packs: the material is named after the game's own texture, because that
    // is what Remix hashes, and that must not move - every tag, rtx.conf category and USD binding
    // hangs off it. What lands in the .dds is a separate question, and the answer is the texture
    // the runtime actually shows: a normal, roughness or displacement map authored from a capture
    // has to be derived from the albedo it will sit next to, and deriving it from art the player
    // never sees is silently wrong in a way that only shows up once the maps are baked.
    // rtx.dusklight.texrep.captureReplaced turns this off; the hashes are identical either way.
    Rc<DxvkImage> albedoImage = materialData.getColorTexture().getImageView()->image();
    const TextureRef* replacement = nullptr;
    const auto outcome = dusklightTexRep::resolveAlbedoForCapture(
      m_sceneManager.getAssetReplacer().get(), materialData, &replacement);
    if (outcome == dusklightTexRep::CaptureAlbedo::Substituted) {
      albedoImage = replacement->getImageView()->image();
    }
    ++m_pCap->texRepCapture[static_cast<size_t>(outcome)];

    m_exporter.dumpImageToFile(ctx, BASE_DIR + lss::commonDirName::texDir,
                               albedoTexFilename,
                               albedoImage);
    const std::string albedoTexPath = str::format(BASE_DIR + lss::commonDirName::texDir, albedoTexFilename);
    lssMat.albedoTexPath = albedoTexPath;
    // Opacity
    lssMat.enableOpacity = bEnableOpacity;
    // Collect sampler info
    const auto& samplerCreateInfo = materialData.getSampler()->info();
    lssMat.sampler.addrModeU = samplerCreateInfo.addressModeU;
    lssMat.sampler.addrModeV = samplerCreateInfo.addressModeV;
    lssMat.sampler.filter = samplerCreateInfo.magFilter;
    lssMat.sampler.borderColor = samplerCreateInfo.borderColor;

    // Set populated LSS Material in our cache
    m_pCap->materials[materialData.getHash()].lssData = lssMat;
    Logger::debug("[GameCapturer][" + m_pCap->idStr + "][Mat:" + matName + "] New");
  }

  void GameCapturer::captureMesh(const Rc<DxvkContext> ctx,
                                 const XXH64_hash_t currentMeshHash,
                                 const BlasEntry& blas,
                                 const CategoryFlags& flags,
                                 const bool bIsNewMesh,
                                 const bool bCapturePositions,
                                 const bool bCaptureNormals,
                                 const bool bCaptureIndices,
                                 const bool isLhs) {
    assert((bIsNewMesh && bCapturePositions && bCaptureNormals && bCaptureIndices) || !bIsNewMesh);
    const RaytraceGeometry& geomData = blas.modifiedGeometryData;
    const SkinningData& skinData = blas.input.getSkinningState();
    const RasterGeometry& rasterGeomData = blas.input.getGeometryData();

    // Safely get a handle to the mesh
    std::shared_ptr<Mesh> pMesh;
    {
      std::lock_guard lock(m_meshMutex);
      pMesh = m_pCap->meshes[currentMeshHash];
    }
          
    // Note: Ensures that reading a Vec3 from the position buffer will result in the proper values. This can be extended if
    // games use odd formats like R32G32B32A32 in the future, but cannot be less than 3 components unless the code is modified
    // to accomodate other strange formats.
    assert((geomData.positionBuffer.vertexFormat() == VK_FORMAT_R32G32B32_SFLOAT) ||
           (geomData.positionBuffer.vertexFormat() == VK_FORMAT_R32G32B32A32_SFLOAT));
    const size_t numVertices = geomData.vertexCount;
    assert(numVertices > 0);
    const size_t numIndices = geomData.indexCount;
    const bool isDoubleSided = geomData.cullMode == VK_CULL_MODE_NONE;
    if (bIsNewMesh) {
      assert(pMesh->lssData.buffers.positionBufs.size() == 0);
      assert(pMesh->lssData.buffers.normalBufs.size() == 0);
      assert(pMesh->lssData.buffers.idxBufs.size() == 0);
      assert(pMesh->lssData.buffers.texcoordBufs.size() == 0);
      assert(pMesh->lssData.buffers.colorBufs.size() == 0);
      pMesh->lssData.meshName = dxvk::hashToString(currentMeshHash);
      for (uint32_t i = 0; i < (uint32_t) HashComponents::Count; i++) {
        const HashComponents component = (HashComponents) i;
        pMesh->lssData.componentHashes[getHashComponentName(component)] = geomData.hashes[component];
      }
      for (uint32_t i = 0; i < (uint32_t) InstanceCategories::Count; i++) {
        const InstanceCategories flag = (InstanceCategories) i;
        pMesh->lssData.categoryFlags[getInstanceCategorySubKey(flag)] = flags.test(flag);
      }
      pMesh->lssData.numVertices = numVertices;
      pMesh->lssData.numIndices = numIndices;
      pMesh->lssData.isDoubleSided = isDoubleSided;
      pMesh->skeletonBinding = blas.input.dusklightSkeletonBinding;
      pMesh->lssData.numBones = skinData.numBones;
      pMesh->lssData.bonesPerVertex = skinData.numBonesPerVertex;
      pMesh->lssData.isLhs = isLhs;
      Logger::debug("[GameCapturer][" + m_pCap->idStr + "][Mesh:" + pMesh->lssData.meshName + "] New");
    }

    if (bCapturePositions && geomData.positionBuffer.defined()) {
      if (skinData.numBones > 0) {
        captureMeshPositions(ctx, rasterGeomData.vertexCount, rasterGeomData.positionBuffer, m_pCap->currentFrameNum, pMesh);
      } else {
        captureMeshPositions(ctx, geomData.vertexCount, geomData.positionBuffer, m_pCap->currentFrameNum, pMesh);
      }
    }
    
    if (bCaptureNormals && geomData.normalBuffer.defined()) {
      if (skinData.numBones > 0) {
        captureMeshNormals(ctx, rasterGeomData.vertexCount, rasterGeomData.normalBuffer, m_pCap->currentFrameNum, pMesh);
      } else {
        captureMeshNormals(ctx, geomData.vertexCount, geomData.normalBuffer, m_pCap->currentFrameNum, pMesh);
      }
    }
    
    if (bCaptureIndices && geomData.indexBuffer.defined()) {
      captureMeshIndices(ctx, geomData, m_pCap->currentFrameNum, m_pCap->camera, pMesh);
    }

    if (bIsNewMesh && geomData.texcoordBuffer.defined()) {
      captureMeshTexCoords(ctx, geomData, m_pCap->currentFrameNum, pMesh);
    }

    if (bIsNewMesh && geomData.color0Buffer.defined()) {
      captureMeshColor(ctx, geomData, m_pCap->currentFrameNum, pMesh);
    }

    if (bIsNewMesh && skinData.numBones > 0) {
      captureMeshBlending(ctx, rasterGeomData, m_pCap->currentFrameNum, pMesh);
      pMesh->lssData.boneXForms = matrix4VecToGfMatrix4dVec(skinData.pBoneMatrices);
    }
  }

  template <typename T>
  void GameCapturer::captureMeshPositions(const Rc<DxvkContext> ctx,
                                          const size_t numVertices,
                                          const T& inputPositionBuffer,
                                          const float currentFrameNum,
                                          std::shared_ptr<Mesh> pMesh) {
                                            
    AssetExporter::BufferCallback captureMeshPositionsAsync = [this, ctx, numVertices, inputPositionBuffer, currentFrameNum, pMesh](Rc<DxvkBuffer> posBuf) {
      // Prep helper vars
      constexpr size_t positionSubElementSize = sizeof(float);
      const size_t positionStride = inputPositionBuffer.stride() / positionSubElementSize;
      const DxvkBufferSlice positionBuffer(posBuf, 0, posBuf->info().size);
      // Ensure no reads are out of bounds
      assert(((size_t) (numVertices - 1) * (size_t)inputPositionBuffer.stride() + sizeof(pxr::GfVec3f)) <=
            (positionBuffer.length() - inputPositionBuffer.offsetFromSlice()));
      // Get copied-to-CPU GPU buffer
      const float* pVkPosBuf = (float*) positionBuffer.mapPtr((size_t)inputPositionBuffer.offsetFromSlice());
      assert(pVkPosBuf);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<pxr::GfVec3f> positions;
      positions.reserve(numVertices);
      OriginCalc originCalc;
      for (size_t idx = 0; idx < numVertices; ++idx) {
        pxr::GfVec3f pos = *reinterpret_cast<const pxr::GfVec3f*>(&pVkPosBuf[idx * positionStride]);
        if(correctBakedTransforms()) {
          originCalc.compareAndSwap(pos);
        }
        positions.push_back(pos);
      }
      if(correctBakedTransforms()) {
        pMesh->originCalc.compareAndSwap(originCalc);
      }
      assert(positions.size() > 0);
      // Create comparison function that returns float
      static auto positionsDifferentEnough = [](const pxr::GfVec3f& a, const pxr::GfVec3f& b) {
        const static float captureMeshPositionDelta = RtxOptions::captureMeshPositionDelta();
        const static float captureMeshPositionDeltaSq = captureMeshPositionDelta * captureMeshPositionDelta;
        return (a - b).GetLengthSq() > captureMeshPositionDeltaSq;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.positionBufs, positions, currentFrameNum, positionsDifferentEnough);
    };
    pMesh->meshSync.numOutstandingInc();
    m_exporter.copyBufferFromGPU(ctx, inputPositionBuffer, captureMeshPositionsAsync);
  }

  template <typename T>
  void GameCapturer::captureMeshNormals(const Rc<DxvkContext> ctx,
                                        const size_t numVertices,
                                        const T& inputNormalBuffer,
                                        const float currentFrameNum,
                                        std::shared_ptr<Mesh> pMesh) {
                                          
    AssetExporter::BufferCallback captureMeshNormalsAsync = [ctx, numVertices, inputNormalBuffer, currentFrameNum, pMesh](Rc<DxvkBuffer> norBuf) {
      assert(inputNormalBuffer.vertexFormat() == VK_FORMAT_R32G32B32_SFLOAT);
      // Prep helper vars
      constexpr size_t normalSubElementSize = sizeof(float);
      const size_t normalStride = inputNormalBuffer.stride() / normalSubElementSize;
      const DxvkBufferSlice normalBuffer(norBuf, 0, norBuf->info().size );
      // Ensure no reads are out of bounds
      assert(((size_t) (numVertices - 1) * (size_t)inputNormalBuffer.stride() + sizeof(pxr::GfVec3f)) <=
            (normalBuffer.length() - inputNormalBuffer.offsetFromSlice()));
      // Get copied-to-CPU GPU buffer
      const float* pVkNormalBuf = (float*) normalBuffer.mapPtr((size_t)inputNormalBuffer.offsetFromSlice());
      assert(pVkNormalBuf);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<pxr::GfVec3f> normals;
      normals.reserve(numVertices);
      for (size_t idx = 0; idx < numVertices; ++idx) {
        normals.push_back(pxr::GfVec3f(&pVkNormalBuf[idx * normalStride]));
      }
      assert(normals.size() > 0);
      // Create comparison function that returns float
      static auto normalsDifferentEnough = [](const pxr::GfVec3f& a, const pxr::GfVec3f& b) {
        const static float captureMeshNormalDelta = RtxOptions::captureMeshNormalDelta();
        const static float captureMeshNormalDeltaSq = captureMeshNormalDelta * captureMeshNormalDelta;
        return (a - b).GetLengthSq() > captureMeshNormalDeltaSq;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.normalBufs, normals, currentFrameNum, normalsDifferentEnough);
    };
    pMesh->meshSync.numOutstandingInc();
    m_exporter.copyBufferFromGPU(ctx, inputNormalBuffer, captureMeshNormalsAsync);
  }

  template<typename T>
  static void getIndicesFromVK(const size_t numIndices, const DxvkBufferSlice& indexBuffer, pxr::VtArray<int>& indices) {
    // Ensure no reads are out of bounds
    assert((size_t) numIndices * sizeof(T) <= indexBuffer.length());

    // Get copied-to-CPU GPU buffer
    const T* pVkIndexBuf = (T*) indexBuffer.mapPtr(0);
    assert(pVkIndexBuf);
    for (size_t idx = 0; idx < numIndices; ++idx) {
      indices.push_back(pVkIndexBuf[idx]);
    }
  }

  void GameCapturer::captureMeshIndices(const Rc<DxvkContext> ctx,
                                        const RaytraceGeometry& geomData,
                                        const float currentFrameNum,
                                        const lss::Camera& capCam,
                                        std::shared_ptr<Mesh> pMesh) {

    AssetExporter::BufferCallback captureMeshIndicesAsync = [ctx, geomData, currentFrameNum, pMesh, capCam, this](Rc<DxvkBuffer> idxBuf) {
      const size_t numIndices = geomData.indexCount;
      const DxvkBufferSlice indexBuffer(idxBuf, 0, idxBuf->info().size);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<int> indices;
      indices.reserve(numIndices);

      switch (geomData.indexBuffer.indexType()) {
      case VK_INDEX_TYPE_UINT16:
        getIndicesFromVK<uint16_t>(numIndices, indexBuffer, indices);
        break;
      case VK_INDEX_TYPE_UINT32:
        getIndicesFromVK<uint32_t>(numIndices, indexBuffer, indices);
        break;
      default:
        assert(0);
      }
      assert(indices.size() > 0);

      // Need to change winding order in 3 situations:
      // 1. The mesh is LHS
      // 2. View and Projection transformation have different handedness (because we need to add a mirror transformation in this case)
      // 3. We need to flip meshes in world space to correct upside down
      // However, swap twice is the same as original order. So, we do XOR here make sure we only swap the winding order when 1 or 3 (odd number) of these 3 situations happen.
      const bool isChangingHandedness = (capCam.proj.bInv ^ (pMesh->lssData.isLhs ^ capCam.isLHS()));
      if (isChangingHandedness){
        for (uint32_t i = 0; i < numIndices; i+=3) {
          std::swap(indices[i], indices[i + 2]);
        }
      }

      // Create comparison function that returns float
      static auto differentIndices = [](const int a, const int b) {
        return a != b;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.idxBufs, indices, currentFrameNum, differentIndices);
    };
    pMesh->meshSync.numOutstandingInc();
    m_exporter.copyBufferFromGPU(ctx, geomData.indexBuffer, captureMeshIndicesAsync);
  }

  void GameCapturer::captureMeshTexCoords(const Rc<DxvkContext> ctx,
                                          const RaytraceGeometry& geomData,
                                          const float currentFrameNum,
                                          std::shared_ptr<Mesh> pMesh) {

    AssetExporter::BufferCallback captureMeshTexCoordsAsync = [ctx, geomData, currentFrameNum, pMesh](Rc<DxvkBuffer> texBuf) {
      // Only float32 texcoord formats can be safely read as float* on the CPU.
      // Non-float32 formats (e.g. R16G16_SFLOAT) are normally converted to R32G32_SFLOAT by the
      // GPU interleaver before reaching here, but guard defensively in case that changes.
      const VkFormat texFmt = geomData.texcoordBuffer.vertexFormat();
      if (texFmt != VK_FORMAT_R32G32_SFLOAT && texFmt != VK_FORMAT_R32G32B32_SFLOAT && texFmt != VK_FORMAT_R32G32B32A32_SFLOAT) {
        Logger::err(str::format("[GameCapturer] Skipping texcoord capture for unsupported format: ", texFmt));
        return;
      }
      // Prep helper vars
      const size_t numVertices = geomData.vertexCount;
      constexpr size_t texcoordSubElementSize = sizeof(float);
      const size_t texcoordStride = geomData.texcoordBuffer.stride() / texcoordSubElementSize;
      const DxvkBufferSlice texcoordBuffer(texBuf, 0, texBuf->info().size);
      // Ensure no reads are out of bounds
      assert(((size_t) (numVertices - 1) * (size_t) geomData.texcoordBuffer.stride() + sizeof(pxr::GfVec2f)) <=
             (texcoordBuffer.length() - geomData.texcoordBuffer.offsetFromSlice()));
      // Get copied-to-CPU GPU buffer
      const float* pVkTexcoordsBuf = (float*) texcoordBuffer.mapPtr((size_t) geomData.texcoordBuffer.offsetFromSlice());
      assert(pVkTexcoordsBuf);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<pxr::GfVec2f> texcoords;
      texcoords.reserve(numVertices);
      for (size_t idx = 0; idx < numVertices; ++idx) {
        texcoords.push_back(pxr::GfVec2f(pVkTexcoordsBuf[idx * texcoordStride],
                                         1.0f - pVkTexcoordsBuf[idx * texcoordStride + 1]));
      }
      assert(texcoords.size() > 0);
      // Create comparison function that returns float
      static auto differentIndices = [](const pxr::GfVec2f& a, const pxr::GfVec2f& b) {
        const static float captureMeshTexcoordDelta = RtxOptions::captureMeshTexcoordDelta();
        const static float captureMeshTexcoordDeltaSq = captureMeshTexcoordDelta * captureMeshTexcoordDelta;
        return (a - b).GetLengthSq() > captureMeshTexcoordDeltaSq;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.texcoordBufs, texcoords, currentFrameNum, differentIndices);
    };
    pMesh->meshSync.numOutstandingInc();
    m_exporter.copyBufferFromGPU(ctx, geomData.texcoordBuffer, captureMeshTexCoordsAsync);
  }

  void GameCapturer::captureMeshColor(const Rc<DxvkContext> ctx,
                                      const RaytraceGeometry& geomData,
                                      const float currentFrameNum,
                                      std::shared_ptr<Mesh> pMesh) {

    AssetExporter::BufferCallback captureMeshColorAsync = [ctx, geomData, currentFrameNum, pMesh](Rc<DxvkBuffer> colBuf) {
      assert(geomData.color0Buffer.vertexFormat() == VK_FORMAT_B8G8R8A8_UNORM);
      // Prep helper vars
      const size_t numVertices = geomData.vertexCount;
      constexpr size_t colorSubElementSize = sizeof(uint8_t);
      const size_t colorStride = geomData.color0Buffer.stride() / colorSubElementSize;
      const DxvkBufferSlice colorBuffer(colBuf, 0, colBuf->info().size);
      // Ensure no reads are out of bounds
      assert(((size_t) (numVertices - 1) * (size_t) geomData.color0Buffer.stride() + sizeof(uint8_t) * 3) <=
             (colorBuffer.length() - geomData.color0Buffer.offsetFromSlice()));
      // Get copied-to-CPU GPU buffer
      const uint8_t* pVkColorBuf = (uint8_t*) colorBuffer.mapPtr((size_t) geomData.color0Buffer.offsetFromSlice());
      assert(pVkColorBuf);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<pxr::GfVec4f> colors;
      colors.reserve(numVertices);
      for (size_t idx = 0; idx < numVertices; ++idx) {
        colors.push_back(pxr::GfVec4f((float) pVkColorBuf[idx * colorStride + 2] / 255.f,
                                      (float) pVkColorBuf[idx * colorStride + 1] / 255.f,
                                      (float) pVkColorBuf[idx * colorStride + 0] / 255.f,
                                      (float) pVkColorBuf[idx * colorStride + 3] / 255.f));
      }
      assert(colors.size() > 0);
      // Create comparison function that returns float
      static auto colorsDifferentEnough = [](const pxr::GfVec4f& a, const pxr::GfVec4f& b) {
        const static float captureMeshColorDelta = RtxOptions::captureMeshColorDelta();
        const static float captureMeshColorDeltaSq = captureMeshColorDelta * captureMeshColorDelta;
        return (a - b).GetLengthSq() > captureMeshColorDeltaSq;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.colorBufs, colors, currentFrameNum, colorsDifferentEnough);
    };
    pMesh->meshSync.numOutstandingInc();
    m_exporter.copyBufferFromGPU(ctx, geomData.color0Buffer, captureMeshColorAsync);
  }

  void GameCapturer::captureMeshBlending(const Rc<DxvkContext> ctx,
                                         const RasterGeometry& geomData,
                                         const float currentFrameNum,
                                         std::shared_ptr<Mesh> pMesh) {
    AssetExporter::BufferCallback captureMeshBlendWeightsAsync = [ctx, geomData, currentFrameNum, pMesh](Rc<DxvkBuffer> inBuf) {
      // Prep helper vars
      const size_t numVertices = geomData.vertexCount;
      const size_t bonesPerVertex = pMesh->lssData.bonesPerVertex;
      const size_t stride = geomData.blendWeightBuffer.stride() / sizeof(float);
      const DxvkBufferSlice bufferSlice(inBuf, 0, inBuf->info().size);
      const VkFormat format = geomData.blendWeightBuffer.vertexFormat();
      if (bonesPerVertex <= 2) {
        assert(format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT || format == VK_FORMAT_R32G32B32_SFLOAT);
      } else if (bonesPerVertex == 3) {
        assert(format == VK_FORMAT_R32G32_SFLOAT || format == VK_FORMAT_R32G32B32_SFLOAT);
      } else if (bonesPerVertex == 4) {
        assert(format == VK_FORMAT_R32G32B32_SFLOAT);
      }
      // Ensure no reads are out of bounds
      assert(((size_t) (numVertices - 1) * (size_t) geomData.blendWeightBuffer.stride() + sizeof(float) * bonesPerVertex) <=
             (bufferSlice.length() - geomData.blendWeightBuffer.offsetFromSlice()));
      // Get copied-to-CPU GPU buffer
      const float* pVkBwBuf = (float*) bufferSlice.mapPtr((size_t) geomData.blendWeightBuffer.offsetFromSlice());
      assert(pVkBwBuf);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<float> targetBuffer;
      targetBuffer.reserve(numVertices * bonesPerVertex);
      for (size_t idx = 0; idx < numVertices; ++idx) {
        float lastWeight = 1.0;
        for (size_t bone_idx = 0; bone_idx < bonesPerVertex - 1; ++bone_idx) {
          float thisWeight = pVkBwBuf[idx * stride + bone_idx];
          lastWeight -= thisWeight;
          targetBuffer.push_back(thisWeight);
        }
        // D3D9 only stores bonesPerVertex - 1 weights. The last weight is 1 minus the other weights.
        targetBuffer.push_back(lastWeight);
      }
      assert(targetBuffer.size() > 0);
      // Create comparison function that returns float
      static auto weightsDifferentEnough = [](const float& a, const float& b) {
        const static float delta = RtxOptions::captureMeshBlendWeightDelta();
        return std::abs(a - b) > delta;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.blendWeightBufs, targetBuffer, currentFrameNum, weightsDifferentEnough);
    };
    AssetExporter::BufferCallback captureMeshBlendIndicesAsync = [ctx, geomData, currentFrameNum, pMesh](Rc<DxvkBuffer> inBuf) {
      assert(geomData.blendIndicesBuffer.vertexFormat() == VK_FORMAT_R8G8B8A8_USCALED);
      // Prep helper vars
      const size_t numVertices = geomData.vertexCount;
      const size_t bonesPerVertex = pMesh->lssData.bonesPerVertex;
      const size_t stride = geomData.blendIndicesBuffer.stride() / sizeof(uint8_t);
      const DxvkBufferSlice bufferSlice(inBuf, 0, inBuf->info().size);
      // Ensure no reads are out of bounds
      assert(((size_t) (numVertices - 1) * (size_t) geomData.blendIndicesBuffer.stride() + sizeof(uint8_t) * bonesPerVertex) <=
             (bufferSlice.length() - geomData.blendIndicesBuffer.offsetFromSlice()));
      // Get copied-to-CPU GPU buffer
      const uint8_t* VkBuf = (uint8_t*) bufferSlice.mapPtr((size_t) geomData.blendIndicesBuffer.offsetFromSlice());
      assert(VkBuf);
      // Copy GPU buffer to local VtArray
      pxr::VtArray<int> targetBuffer;
      targetBuffer.reserve(numVertices * bonesPerVertex);
      for (size_t idx = 0; idx < numVertices; ++idx) {
        for (size_t bone_idx = 0; bone_idx < bonesPerVertex; ++bone_idx) {
          targetBuffer.push_back(VkBuf[idx * stride + bone_idx]);
        }
      }
      assert(targetBuffer.size() > 0);
      // Create comparison function that returns float
      static auto weightsDifferentEnough = [](const int& a, const int& b) {
        return a != b;
      };
      // Cache buffer iff new buffer differs from previous buffer
      evalNewBufferAndCache(pMesh, pMesh->lssData.buffers.blendIndicesBufs, targetBuffer, currentFrameNum, weightsDifferentEnough);
    };
    pMesh->meshSync.numOutstandingInc();
    m_exporter.copyBufferFromGPU(ctx, geomData.blendWeightBuffer, captureMeshBlendWeightsAsync);
    if (geomData.blendIndicesBuffer.defined()) {
      pMesh->meshSync.numOutstandingInc();
      m_exporter.copyBufferFromGPU(ctx, geomData.blendIndicesBuffer, captureMeshBlendIndicesAsync);
    }
  }

  template <typename T, typename CompareTReturnBool>
  static void GameCapturer::evalNewBufferAndCache(std::shared_ptr<Mesh> pMesh,
                                                  std::map<float, pxr::VtArray<T>>& bufferCache,
                                                  pxr::VtArray<T>& newBuffer,
                                                  const float currentFrameNum,
                                                  CompareTReturnBool compareT) {
    std::lock_guard lock(pMesh->meshSync.mutex);
    // Discover whether the new buffer is worth cacheing
    bool bSufficientlyDifferent = false;
    if (bufferCache.size() > 0) {
      const auto& prevBuf = (--bufferCache.cend())->second;
      assert(newBuffer.size() == prevBuf.size());
      for (size_t idx = 0; idx < newBuffer.size(); ++idx) {
        const T& newVal = newBuffer[idx];
        const T& prevVal = prevBuf[idx];
        bSufficientlyDifferent = compareT(newVal, prevVal);
        if (bSufficientlyDifferent) {
          // Early out as soon as we've found enough of a difference
          break;
        }
      }
    } else {
      bSufficientlyDifferent = true;
    }
    // Cache VtArray if there is a large enough delta
    if (bSufficientlyDifferent) {
      bufferCache[currentFrameNum] = std::move(newBuffer);
    }
    pMesh->meshSync.numOutstanding--;
    pMesh->meshSync.cond.notify_all();
  }

  void GameCapturer::exportUsd(const Rc<DxvkContext> ctx) {
    assert(m_state.has<State::BeginExport>());
    assert(!m_state.has<State::PreppingExport>());
    assert(!m_state.has<State::Exporting>());
    static auto exportThreadTask = [this](const Rc<DxvkContext> ctx,
                                          std::unique_ptr<Capture> pCap,
                                          State* pState,
                                          CompletedCapture* complete,
                                          const float framesPerSecond) {
      Capture& cap = *pCap;
      const auto numTexExportsInProgress = m_exporter.getNumExportsInFlights();
      constexpr float kTimePerTexExport = 0.0050f; // Liberally decided by inspection, derived from timed out tests
      const float texExportTimeout = numTexExportsInProgress * kTimePerTexExport;
      m_exporter.waitForAllExportsToComplete(texExportTimeout);
      assert(pState->has<State::PreppingExport>());
      const auto exportPrep = prepExport(cap, framesPerSecond);
      pState->set<State::PreppingExport, false>();
      pState->set<State::Exporting, true>();

      Logger::info("[GameCapturer][" + cap.idStr + "] Begin USD export");
      const bool exportOk = lss::GameExporter::exportUsd(exportPrep);
      Logger::info("[GameCapturer][" + cap.idStr + "] End USD export");

      if (!exportOk) {
        complete->stageName = cap.instance.stageName;
        complete->stagePath = cap.instance.stagePath;
        pState->set<State::Exporting, false>();
        pState->set<State::Failed, true>();
        return;
      }

      // Necessary step for being able to properly diff and check for regressions
      const auto flattenCaptureEnvStr = env::getEnvVar("DXVK_CAPTURE_FLATTEN");
      if (!flattenCaptureEnvStr.empty()) {
        flattenExport(exportPrep);
      }

      complete->stageName = cap.instance.stageName;
      complete->stagePath = cap.instance.stagePath;
      pState->set<State::Exporting, false>();
      pState->set<State::Complete, true>();
    };

    m_state.set<State::PreppingExport, true>();
    m_state.set<State::BeginExport, false>();
    std::thread(exportThreadTask,
                ctx,
                std::move(m_pCap),
                &m_state,
                &m_completeCapture,
                static_cast<float>(m_options.fps)).detach();
  }

  lss::Export GameCapturer::prepExport(const Capture& cap,
                                             const float framesPerSecond) {
    lss::Export exportPrep;
    prepExportMetaData(cap, framesPerSecond, exportPrep);
    prepExportMaterials(cap, exportPrep);
    prepExportMeshes(cap, exportPrep);
    if (exportPrep.bExportInstanceStage) {
      prepExportInstances(cap, exportPrep);
      prepExportCharacterMerges(cap, exportPrep);
      prepExportLights(cap, exportPrep);
      exportPrep.camera = cap.camera;
    }
    // Prep global transform
    exportPrep.globalXform = pxr::GfMatrix4d{1.0};
    const bool bInvX = (!exportPrep.camera.view.bInv) && (exportPrep.camera.proj.bInv || exportPrep.camera.isLHS());
    const bool bInvY = (!exportPrep.camera.view.bInv) && exportPrep.camera.proj.bInv;
    const pxr::GfVec3d scale{ bInvX ? -1.0 : 1.0, bInvY ? -1.0 : 1.0, 1.0 };
    exportPrep.globalXform.SetScale(scale);
    if(exportPrep.meta.bCorrectBakedTransforms) {
      exportPrep.globalXform.SetTranslateOnly(-exportPrep.stageOrigin);
    }
    return exportPrep;
  }

  void GameCapturer::prepExportMetaData(const Capture& cap,
                                        const float framesPerSecond,
                                        lss::Export& exportPrep) {
    // Prep meta data
    exportPrep.meta.windowTitle = window::getWindowTitle(cap.hwnd);
    exportPrep.meta.exeName = env::getExeName();
    exportPrep.meta.iconPath = BASE_DIR + exportPrep.meta.exeName + "_icon.bmp";
    exportPrep.meta.geometryHashRule = RtxOptions::geometryAssetHashRuleString();
    exportPrep.meta.metersPerUnit = RtxOptions::sceneScale();
    exportPrep.meta.timeCodesPerSecond = framesPerSecond;
    exportPrep.meta.startTimeCode = 0.0;
    exportPrep.meta.endTimeCode = floor(static_cast<double>(cap.currentFrameNum));
    exportPrep.meta.numFramesCaptured = cap.numFramesCaptured;
    window::saveWindowIconToFile(exportPrep.meta.iconPath, cap.hwnd);
    exportPrep.meta.bReduceMeshBuffers = true;
    exportPrep.meta.isZUp = RtxOptions::zUp();
    if (s_captureRemixConfigs) {
      for (auto& pair : RtxOptionImpl::getGlobalOptionMap()) {
        exportPrep.meta.renderingSettingsDict[pair.second->getFullName()] = pair.second->getResolvedValueAsString();
      }
    }
    exportPrep.meta.bCorrectBakedTransforms = false;

    exportPrep.debugId = cap.idStr;
    exportPrep.baseExportPath = BASE_DIR;
    exportPrep.bExportInstanceStage = cap.bCaptureInstances;
    exportPrep.instanceStagePath = cap.instance.stagePath;
    exportPrep.bakedSkyProbePath = cap.bSkyProbeBaked ? (BASE_DIR + lss::commonDirName::texDir + getBakedSkyProbeName(cap.instance.stageName)) : "";
    exportPrep.bSkyFromDomeLight = cap.bSkyFromDomeLight;
    if (cap.bSkyFromDomeLight) {
      exportPrep.skyDomeLightToWorld = matrix4ToGfMatrix4d(cap.skyDomeLightToWorld);
      exportPrep.skyDomeRadiance =
        pxr::GfVec3f(cap.skyDomeRadiance.x, cap.skyDomeRadiance.y, cap.skyDomeRadiance.z);
      exportPrep.skyDomeYawDegrees = static_cast<double>(skyDomeYawDegrees());
    }
  }

  void GameCapturer::prepExportMaterials(const Capture& cap,
                                         lss::Export& exportPrep) {
    for (auto& [hash, material] : cap.materials) {
      exportPrep.materials[hash] = material.lssData;
    }

    using CaptureAlbedo = dusklightTexRep::CaptureAlbedo;
    const auto count = [&cap](const CaptureAlbedo outcome) {
      return cap.texRepCapture[static_cast<size_t>(outcome)];
    };
    // notResident and missing are the two that matter: both mean the .dds on disk is the game's own
    // art while the running game was showing the pack, so anything derived from the capture would
    // not line up. Retaking the capture a few seconds later is the fix for notResident.
    Logger::info(str::format(
      "capture.texrep enabled=", DusklightTexRep::captureReplaced() ? 1 : 0,
      " materials=", cap.materials.size(),
      " untagged=", count(CaptureAlbedo::NoReplacement),
      " replaced=", count(CaptureAlbedo::Substituted),
      " notResident=", count(CaptureAlbedo::NotResident),
      " missing=", count(CaptureAlbedo::Missing),
      "  (notResident/missing wrote the game's texture, not the pack's)"));
  }

  void GameCapturer::prepExportMeshes(const Capture& cap, lss::Export& exportPrep) {
    OriginCalc stageOriginCalc;
    for (auto& [hash, pMesh] : cap.meshes) {
      std::unique_lock lock(pMesh->meshSync.mutex);
      pMesh->meshSync.cond.wait(lock,
        [pNumOutstanding = &pMesh->meshSync.numOutstanding] { return *pNumOutstanding == 0; });
      if (pMesh->lssData.numIndices == 0 && pMesh->lssData.numVertices == 0) {
        continue;
      }
      if (cap.materials.count(pMesh->matHash) > 0) {
        pMesh->lssData.matId = pMesh->matHash;
      }
      if(correctBakedTransforms()) {
        pMesh->lssData.origin = pMesh->originCalc.calc();
        stageOriginCalc.compareAndSwap(pMesh->lssData.origin);
      }
      exportPrep.meshes[hash] = pMesh->lssData;
    }
    if(correctBakedTransforms()) {
      exportPrep.stageOrigin = stageOriginCalc.calc();
    }

    // How fragmented the capture is, and how much of a skeleton each piece got. One D3D9 draw is
    // one mesh here and one USD skeleton is written per mesh, so a character the game draws as
    // forty shape packets arrives as forty meshes and forty skeletons - and a packet that
    // references a single matrix arrives with a single bone. That is the shape of the problem, and
    // until now the only way to see it was to open the capture and count.
    //
    // The bone counts also say which of the two skinning paths a model came down: an envelope model
    // carries its whole joint palette on every draw (large, equal counts), while a matrix-palette
    // model carries only the joints that packet touched (mostly 1).
    std::array<uint32_t, 4> boneBuckets = {}; // 1, 2-4, 5-16, 17+
    uint32_t skinnedMeshes = 0;
    uint32_t maxBones = 0;
    for (const auto& [hash, mesh] : exportPrep.meshes) {
      if (mesh.numBones == 0) {
        continue;
      }
      ++skinnedMeshes;
      maxBones = std::max(maxBones, mesh.numBones);
      const size_t bucket = mesh.numBones == 1 ? 0 : (mesh.numBones <= 4 ? 1 : (mesh.numBones <= 16 ? 2 : 3));
      ++boneBuckets[bucket];
    }
    Logger::info(str::format(
      "capture.geometry meshes=", exportPrep.meshes.size(),
      " skinnedMeshes=", skinnedMeshes,
      " bones=[1:", boneBuckets[0], " 2-4:", boneBuckets[1],
      " 5-16:", boneBuckets[2], " 17+:", boneBuckets[3], "]",
      " maxBones=", maxBones,
      "  (one mesh and one skeleton per draw call; joint names and bind transforms are synthesised"
      " from vertex centroids, not the game's joint tree)"));
  }

  void GameCapturer::prepExportCharacterMerges(const Capture& cap, lss::Export& exportPrep) {
    std::vector<MergedGroup> groups = buildMergedGroups(cap);
    if (groups.empty()) {
      return;
    }

    uint32_t merged = 0;
    uint32_t rejected = 0;
    uint32_t drawsReplaced = 0;
    uint32_t largestGroup = 0;
    std::string firstRejectReason;

    for (MergedGroup& group : groups) {
      lss::Mesh mergedMesh;
      std::string rejectReason;
      if (!mergeGroup(cap, group, mergedMesh, rejectReason)) {
        ++rejected;
        if (firstRejectReason.empty()) {
          firstRejectReason = rejectReason;
        }
        continue;
      }

      // The merged instance inherits the group's transform and time span from its first member,
      // which every other member had to agree with for the merge to be accepted at all.
      const auto& firstMember = cap.instances.at(group.memberInstanceIds.front());
      lss::Instance mergedInstance = firstMember.lssData;
      mergedInstance.meshId = group.mergedMeshHash;
      mergedInstance.matId = firstMember.matHash;
      mergedInstance.instanceName = mergedMesh.meshName + "_0";
      // The members' bone samples describe their own per-draw palettes, so carrying them across
      // would animate the wrong joints. mergeGroup has already scattered them into the model's
      // joint space, so the merged instance gets one sample of that instead.
      //
      // One sample, deliberately: a merged character is captured in its bind pose with the joint
      // transforms of the group's first frame. That is what asset extraction wants and it is what
      // this feature is for. A capture taken to record *motion* should set
      // rtx.dusklight.skeleton.mergeCaptures=0 and keep the per-draw meshes, which still carry
      // their full per-frame samples. exportInstances also decides skeleton-ness from this array
      // being non-empty, so leaving it empty would silently drop the armature altogether.
      mergedInstance.boneXForms.clear();
      lss::SampledBoneXform boneSample;
      boneSample.time = firstMember.lssData.xforms.front().time;
      boneSample.xforms = mergedMesh.boneXForms;
      mergedInstance.boneXForms.push_back(std::move(boneSample));

      for (const XXH64_hash_t instanceId : group.memberInstanceIds) {
        exportPrep.instances.erase(instanceId);
      }
      exportPrep.meshes[group.mergedMeshHash] = std::move(mergedMesh);
      exportPrep.instances[group.mergedMeshHash] = std::move(mergedInstance);

      ++merged;
      drawsReplaced += static_cast<uint32_t>(group.memberInstanceIds.size());
      largestGroup = std::max(largestGroup, static_cast<uint32_t>(group.memberInstanceIds.size()));
    }

    // A member mesh is only dead once nothing still refers to it. Shared geometry - the same draw
    // appearing outside a character - has to survive, and deleting it would leave a dangling
    // meshId behind in the stage.
    std::unordered_set<lss::Id> referenced;
    for (const auto& [id, instance] : exportPrep.instances) {
      referenced.insert(instance.meshId);
    }
    for (auto it = exportPrep.meshes.begin(); it != exportPrep.meshes.end();) {
      it = referenced.count(it->first) == 0 ? exportPrep.meshes.erase(it) : std::next(it);
    }

    Logger::info(str::format(
      "capture.merge groups=", groups.size(),
      " merged=", merged,
      " rejected=", rejected,
      " drawsReplaced=", drawsReplaced,
      " largestGroup=", largestGroup,
      rejected > 0 ? str::format(" firstReject=\"", firstRejectReason, "\"") : std::string(),
      "  (a rejected group keeps its original per-draw meshes; nothing is merged approximately)"));
  }

  std::vector<GameCapturer::MergedGroup> GameCapturer::buildMergedGroups(const Capture& cap) {
    std::vector<MergedGroup> groups;
    if (!DusklightSkeleton::enable() || !DusklightSkeleton::mergeCaptures()) {
      return groups;
    }

    // Group by (model, model instance). Two Bokoblins are two groups; the two halves of one
    // Bokoblin's head are one.
    std::map<std::pair<uint64_t, uint64_t>, std::vector<XXH64_hash_t>> byModelInstance;
    for (const auto& [instanceId, instance] : cap.instances) {
      const auto& binding = instance.skeletonBinding;
      if (!binding.isValid() || instance.meshHash == 0) {
        continue;
      }
      byModelInstance[{ binding.modelKey, binding.instanceKey }].push_back(instanceId);
    }

    for (auto& [key, members] : byModelInstance) {
      // A group of one is not a merge; leaving it alone keeps its original hash, which is the one
      // any existing replacement is bound to.
      if (members.size() < 2) {
        continue;
      }
      // Sorting does two jobs. cap.instances is unordered, so without it the merged geometry would
      // come out in a different order every run - and since the merged hash is derived from the
      // member list, the hash would move too, which is exactly the instability this feature exists
      // to remove.
      std::sort(members.begin(), members.end(), [&cap](XXH64_hash_t a, XXH64_hash_t b) {
        const auto& ia = cap.instances.at(a);
        const auto& ib = cap.instances.at(b);
        if (ia.meshHash != ib.meshHash) {
          return ia.meshHash < ib.meshHash;
        }
        return a < b;
      });

      MergedGroup group;
      group.modelKey = key.first;
      group.instanceKey = key.second;
      group.memberInstanceIds = members;

      // Keyed on the model plus the exact set of draws that made it up, not on the model alone:
      // two instances of one model showing different packets (an LOD, a hidden piece) are
      // genuinely different geometry and must not collide. Two identical instances hash the same
      // and correctly share one mesh.
      XXH64_hash_t hash = XXH64(&group.modelKey, sizeof(group.modelKey), 0);
      for (const XXH64_hash_t instanceId : members) {
        const XXH64_hash_t meshHash = cap.instances.at(instanceId).meshHash;
        hash = XXH64(&meshHash, sizeof(meshHash), hash);
      }
      group.mergedMeshHash = hash;
      groups.push_back(std::move(group));
    }
    return groups;
  }

  bool GameCapturer::mergeGroup(const Capture& cap,
                                MergedGroup& group,
                                lss::Mesh& meshOut,
                                std::string& rejectReasonOut) {
    // The group's frame of reference is its first member's first transform. Everything else has to
    // agree with it: the merge concatenates vertices without rebasing them, so a member sitting on
    // a different transform would land in the wrong place. Rejecting is the right answer rather
    // than rebasing, because a character whose packets disagree here means the model identity is
    // wrong and a scattered body is a far worse outcome than an unmerged one.
    const lss::Instance& first = cap.instances.at(group.memberInstanceIds.front()).lssData;
    if (first.xforms.empty()) {
      rejectReasonOut = "no transform";
      return false;
    }
    const pxr::GfMatrix4d& groupXform = first.xforms.front().xform;

    uint32_t jointCount = 0;
    for (const XXH64_hash_t instanceId : group.memberInstanceIds) {
      const auto& instance = cap.instances.at(instanceId);
      if (instance.lssData.xforms.empty() ||
          !pxr::GfIsClose(instance.lssData.xforms.front().xform, groupXform, 1e-4)) {
        rejectReasonOut = "members disagree on the model transform";
        return false;
      }
      jointCount = std::max(jointCount, static_cast<uint32_t>(instance.skeletonBinding.jointCount));
    }
    if (jointCount == 0) {
      rejectReasonOut = "no joints";
      return false;
    }

    // Every merged mesh carries four influences per vertex whether it needs them or not. A rigid
    // packet contributes one joint at weight 1 and three at zero; a four-influence packet
    // contributes all four. Standardising here is what makes the merged mesh a single coherent
    // skinned object in Blender rather than a set of pieces with incompatible vertex groups.
    constexpr size_t kInfluences = 4;

    meshOut.meshName = dxvk::hashToString(group.mergedMeshHash);
    meshOut.isDoubleSided = false;
    meshOut.numBones = jointCount;
    meshOut.bonesPerVertex = kInfluences;
    meshOut.isLhs = false;

    pxr::VtArray<lss::Pos> positions;
    pxr::VtArray<lss::Norm> normals;
    pxr::VtArray<lss::Texcoord> texcoords;
    pxr::VtArray<lss::Color> colors;
    pxr::VtArray<lss::Index> indices;
    pxr::VtArray<lss::BlendWeight> weights;
    pxr::VtArray<lss::BlendIdx> blendIndices;
    // Identity for every joint nobody claims, so an unused joint sits at its bind pose rather than
    // collapsing the mesh to the origin.
    pxr::VtMatrix4dArray boneXForms(jointCount, pxr::GfMatrix4d(1.0));
    std::vector<bool> boneWritten(jointCount, false);

    size_t vertexBase = 0;
    for (const XXH64_hash_t instanceId : group.memberInstanceIds) {
      const auto& instance = cap.instances.at(instanceId);
      const auto meshIt = cap.meshes.find(instance.meshHash);
      if (meshIt == cap.meshes.end() || meshIt->second == nullptr) {
        rejectReasonOut = "a member mesh is missing";
        return false;
      }
      const Mesh& member = *meshIt->second;
      const lss::Mesh& src = member.lssData;
      const auto& binding = member.skeletonBinding;

      // Only the first time sample. Skinned geometry is captured in its bind pose (captureMesh
      // takes the raster buffers when there are bones), so later samples are the same points; the
      // animation lives in the bone transforms below.
      if (src.buffers.positionBufs.empty() || src.buffers.idxBufs.empty()) {
        rejectReasonOut = "a member has no geometry";
        return false;
      }
      const auto& srcPositions = src.buffers.positionBufs.begin()->second;
      const auto& srcIndices = src.buffers.idxBufs.begin()->second;
      const size_t vertexCount = srcPositions.size();

      positions.insert(positions.end(), srcPositions.begin(), srcPositions.end());

      // Every vertex needs an entry in every stream the merged mesh has, or the arrays fall out of
      // step and USD silently misreads them. Members missing a stream are padded rather than
      // skipped.
      if (!src.buffers.normalBufs.empty()) {
        const auto& srcNormals = src.buffers.normalBufs.begin()->second;
        normals.insert(normals.end(), srcNormals.begin(), srcNormals.end());
      }
      normals.resize(vertexBase + vertexCount, lss::Norm(0.f, 1.f, 0.f));

      if (!src.buffers.texcoordBufs.empty()) {
        const auto& srcTexcoords = src.buffers.texcoordBufs.begin()->second;
        texcoords.insert(texcoords.end(), srcTexcoords.begin(), srcTexcoords.end());
      }
      texcoords.resize(vertexBase + vertexCount, lss::Texcoord(0.f, 0.f));

      if (!src.buffers.colorBufs.empty()) {
        const auto& srcColors = src.buffers.colorBufs.begin()->second;
        colors.insert(colors.end(), srcColors.begin(), srcColors.end());
      }
      colors.resize(vertexBase + vertexCount, lss::Color(1.f, 1.f, 1.f, 1.f));

      const size_t startFace = indices.size() / 3;
      for (const lss::Index index : srcIndices) {
        indices.push_back(static_cast<lss::Index>(index + vertexBase));
      }
      lss::MeshMaterialRange range;
      range.matId = instance.matHash;
      range.startFace = startFace;
      range.faceCount = (indices.size() / 3) - startFace;
      meshOut.materialRanges.push_back(range);

      // Blend data, remapped from this draw's local palette into the model's joint space. This is
      // the whole point of the exercise: without it, blend index 0 of one packet and blend index 0
      // of the next are different joints and merging them is nonsense.
      const size_t srcInfluences = std::max<size_t>(src.bonesPerVertex, 1);
      const bool hasBlend = !src.buffers.blendIndicesBufs.empty() &&
                            !src.buffers.blendWeightBufs.empty() &&
                            src.numBones > 0;
      const pxr::VtArray<lss::BlendIdx>* srcBlendIndices =
        hasBlend ? &src.buffers.blendIndicesBufs.begin()->second : nullptr;
      const pxr::VtArray<lss::BlendWeight>* srcWeights =
        hasBlend ? &src.buffers.blendWeightBufs.begin()->second : nullptr;

      for (size_t v = 0; v < vertexCount; ++v) {
        for (size_t k = 0; k < kInfluences; ++k) {
          int32_t joint = 0;
          float weight = 0.f;
          if (hasBlend && k < srcInfluences) {
            const size_t offset = v * srcInfluences + k;
            if (offset < srcBlendIndices->size() && offset < srcWeights->size()) {
              const int32_t local = (*srcBlendIndices)[offset];
              weight = (*srcWeights)[offset];
              if (local < 0 || static_cast<uint32_t>(local) >= binding.blendIndexCount) {
                rejectReasonOut = "a blend index has no joint mapping";
                return false;
              }
              const uint16_t mapped = binding.blendIndexToJoint[local];
              if (mapped == dusklightSkeleton::kNoJoint || mapped >= jointCount) {
                rejectReasonOut = "a blend index maps to no joint";
                return false;
              }
              joint = static_cast<int32_t>(mapped);
            }
          } else if (k == 0) {
            // A rigid packet: one joint at full weight, taken from the single entry the game
            // published for it. Without this the piece would arrive weightless and collapse onto
            // the model origin when the armature moves.
            if (binding.blendIndexCount == 0 ||
                binding.blendIndexToJoint[0] == dusklightSkeleton::kNoJoint ||
                binding.blendIndexToJoint[0] >= jointCount) {
              rejectReasonOut = "a rigid member has no joint mapping";
              return false;
            }
            joint = static_cast<int32_t>(binding.blendIndexToJoint[0]);
            weight = 1.f;
          }
          blendIndices.push_back(joint);
          weights.push_back(weight);
        }
      }

      // Scatter this member's bone matrices into the model's joint space. Members between them
      // cover the joints the character actually uses; the rest keep the identity set above.
      const auto& memberBones = src.boneXForms;
      for (uint32_t local = 0; local < binding.blendIndexCount && local < memberBones.size(); ++local) {
        const uint16_t mapped = binding.blendIndexToJoint[local];
        if (mapped != dusklightSkeleton::kNoJoint && mapped < jointCount && !boneWritten[mapped]) {
          boneXForms[mapped] = memberBones[local];
          boneWritten[mapped] = true;
        }
      }

      vertexBase += vertexCount;
    }

    meshOut.numVertices = static_cast<uint32_t>(positions.size());
    meshOut.numIndices = static_cast<uint32_t>(indices.size());
    meshOut.boneXForms = boneXForms;
    meshOut.buffers.positionBufs[0.f] = std::move(positions);
    meshOut.buffers.normalBufs[0.f] = std::move(normals);
    meshOut.buffers.texcoordBufs[0.f] = std::move(texcoords);
    meshOut.buffers.colorBufs[0.f] = std::move(colors);
    meshOut.buffers.idxBufs[0.f] = std::move(indices);
    meshOut.buffers.blendIndicesBufs[0.f] = std::move(blendIndices);
    meshOut.buffers.blendWeightBufs[0.f] = std::move(weights);

    // The game's own joint tree, replacing the one generateSkeleton would have invented from
    // vertex centroids.
    dusklightSkeleton::Skeleton skeleton;
    if (dusklightSkeleton::findSkeleton(group.modelKey, skeleton) &&
        skeleton.jointCount >= jointCount) {
      lss::AuthoredSkeleton& authored = meshOut.authoredSkeleton;
      authored.valid = true;
      authored.name = meshOut.meshName;
      authored.jointPaths.reserve(skeleton.jointCount);
      for (uint32_t i = 0; i < skeleton.jointCount; ++i) {
        authored.jointPaths.push_back(pxr::TfToken(skeleton.jointPaths[i]));
        authored.bindTransforms.push_back(matrix4ToGfMatrix4d(skeleton.bindTransforms[i]));
        authored.restTransforms.push_back(matrix4ToGfMatrix4d(skeleton.restTransforms[i]));
      }
      // The bone array has to line up with the joint array the skeleton declares, not with the
      // subset this group happened to touch.
      meshOut.numBones = skeleton.jointCount;
      meshOut.boneXForms.resize(skeleton.jointCount, pxr::GfMatrix4d(1.0));
    }
    return true;
  }

  void GameCapturer::prepExportInstances(const Capture& cap, lss::Export& exportPrep) {
    for (auto& [hash, instance] : cap.instances) {
      if (instance.meshHash == 0) {
        continue;
      }
      auto& exportInstance = exportPrep.instances[hash];
      exportInstance = instance.lssData;

      assert(cap.meshes.count(instance.meshHash) > 0);
      exportInstance.meshId = instance.meshHash;

      const auto pMesh = cap.meshes.at(instance.meshHash);
      if (cap.materials.count(pMesh->matHash) > 0) {
        exportInstance.matId = instance.matHash;
      }
      exportInstance.instanceName = pMesh->lssData.meshName + "_" + std::to_string(instance.meshInstNum);
    }
  }

  void GameCapturer::prepExportLights(const Capture& cap,
                                      lss::Export& exportPrep) {
    // sphere lights
    for (auto& [hash, sphereLight] : cap.sphereLights) {
      exportPrep.sphereLights.emplace(hash, sphereLight);
    }
    // distant lights
    for (auto& [hash, distantLight] : cap.distantLights) {
      exportPrep.distantLights.emplace(hash, distantLight);
    }

    // One bounded line saying what lighting the capture actually ended up with. A capture that
    // opens dark in the toolkit is otherwise indistinguishable from a scene that was dark, and
    // telling those apart used to need the owner to describe what they saw.
    Logger::info(str::format(
      "capture.lights sphere=", cap.sphereLights.size(),
      " distant=", cap.distantLights.size(),
      " sky=", cap.bSkyProbeBaked ? (cap.bSkyFromDomeLight ? "domeLight" : "skyProbe") : "none",
      cap.bSkyFromDomeLight
        ? str::format(" skyRadiance=(", cap.skyDomeRadiance.x, ", ", cap.skyDomeRadiance.y, ", ",
                      cap.skyDomeRadiance.z, ") skyYawDeg=", skyDomeYawDegrees())
        : std::string(),
      "  (a capture with sky=none and no distant light is lit by nothing but its sphere lights)"));
  }

  void GameCapturer::flattenExport(const lss::Export& exportPrep) {
    Logger::info("[GameCapturer][" + exportPrep.debugId + "] Flattening USD capture.");
    const auto pStage = pxr::UsdStage::Open(exportPrep.instanceStagePath);
    assert(pStage);
    const auto flattenedStagePath = buildStagePath("flattened.usd");
    const auto pFlattenedStage = pStage->Export(flattenedStagePath, true);
    assert(pFlattenedStage);
    Logger::info("[GameCapturer][" + exportPrep.debugId + "] USD capture flattened.");
  }

  std::string GameCapturer::getCaptureInstanceStageNameWithTimestamp() {

    const std::string& stageName = RtxOptions::captureInstanceStageName();
    const auto timestampPos = stageName.find(RtxOptions::captureTimestampReplacement());
    const auto usdExtPos =
      stageName.find(lss::ext::usd, stageName.length() - lss::ext::usda.length() - 1);
    std::string stageNameWithExt = stageName + ((usdExtPos == std::string::npos) ? lss::ext::usd : "");
    
    if (timestampPos == std::string::npos) {
      return stageNameWithExt;
    }
    
    const std::time_t curTime = std::time(nullptr);
    std::tm locTime;
    // The vanilla versions of localtime are not thread safe, see:
    // https://en.cppreference.com/w/cpp/chrono/c/localtime
    localtime_s(&locTime, &curTime);
    static constexpr size_t kTimeStrLen = 19; // length of YYYY-MM-DD_HH-MM-SS
    const auto putTime = std::put_time(&locTime, "%Y-%m-%d_%H-%M-%S");
    
    std::stringstream stageNameSS;
    stageNameSS << stageNameWithExt.substr(0, timestampPos);
    stageNameSS << putTime;
    stageNameSS << stageNameWithExt.substr(timestampPos + RtxOptions::captureTimestampReplacement().length());

    return stageNameSS.str();
  }
}
