/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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

#include <cassert>
#include <tuple>
#include <string>
#include <sstream>
#include <iomanip>
#include <optional>
#include <nvapi.h>
#include <NVIDIASansMd.ttf.h>
#include <NVIDIASansBd.ttf.h>
#include <RobotoMonoRg.ttf.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dxvk.hpp"
#include "imgui_impl_win32.h"
#include "implot.h"
#include "dxvk_imgui.h"
#include "rtx_render/rtx_imgui.h"
#include "dxvk_device.h"
#include "rtx_render/graph/rtx_graph_gui.h"
#include "rtx_render/rtx_utils.h"
#include "rtx_render/rtx_shader_manager.h"
#include "rtx_render/rtx_camera.h"
#include "rtx_render/rtx_context.h"
#include "rtx_render/rtx_hash_collision_detection.h"
#include "rtx_render/rtx_options.h"
#include "rtx_render/rtx_dusklight_env.h"
#include "rtx_render/rtx_dusklight_game.h"
// For the master switch row, which draws atmosphere.enable and atmosphere.skyEnable directly.
#include "rtx_render/rtx_dusklight_atmosphere.h"
#include "rtx_render/rtx_dusklight_texrep.h"
#include "rtx_render/rtx_dusklight_emissive.h"
#include "rtx_render/rtx_dusklight_water.h"
#include "../../d3d9/d3d9_rtx_matrep.h"
#include "../rtx_render/rtx_dusklight_catrep.h"
#include "rtx_render/rtx_global_volumetrics.h"
#include "rtx_render/rtx_bloom.h"
#include <functional>
#include <algorithm>
#include <vector>
#include "rtx_render/rtx_terrain_baker.h"
#include "rtx_render/rtx_neural_radiance_cache.h"
#include "rtx_render/rtx_ray_reconstruction.h"
#include "rtx_render/rtx_xess.h"
#include "rtx_render/rtx_rtxdi_rayquery.h"
#include "rtx_render/rtx_restir_gi_rayquery.h"
#include "rtx_render/rtx_debug_view.h"
#include "rtx_render/rtx_composite.h"
#include "rtx_render/rtx_sparse_rendering.h"
#include "dxvk_image.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/util_math.h"
#include "../util/util_global_time.h"
#include "rtx_render/rtx_opacity_micromap_manager.h"
#include "rtx_render/rtx_bridge_message_channel.h"
#include "dxvk_imgui_about.h"
#include "dxvk_imgui_splash.h"
#include "dxvk_imgui_capture.h"
#include "rtx_render/rtx_option_layer_gui.h"
#include "rtx_render/rtx_option_manager.h"
#include "dxvk_scoped_annotation.h"
#include "../../d3d9/d3d9_rtx.h"
#include "dxvk_memory_tracker.h"
#include "rtx_render/rtx_particle_system.h"
#include "rtx_render/rtx_point_instancer_system.h"
#include "rtx_render/rtx_overlay_window.h"


namespace dxvk {
  extern size_t g_streamedTextures_budgetBytes;
  extern size_t g_streamedTextures_usedBytes;
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern ImGuiKey ImGui_ImplWin32_VirtualKeyToImGuiKey(WPARAM wParam);

namespace ImGui {
  // Note: Implementation of text separators adapted from:
  // https://github.com/ocornut/imgui/issues/1643

  void CenteredSeparator(float width = 0) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
      return;
    ImGuiContext& g = *GImGui;

    // Horizontal Separator
    float x1, x2;
    if (window->DC.CurrentColumns == NULL && (width == 0)) {
      // Span whole window
      x1 = window->DC.CursorPos.x;
      // Note: Account for padding on the Window
      x2 = window->Pos.x + window->Size.x - window->WindowPadding.x;
    } else {
      // Start at the cursor
      x1 = window->DC.CursorPos.x;
      if (width != 0) {
        x2 = x1 + width;
      } else {
        x2 = window->ClipRect.Max.x;
        // Pad right side of columns (except the last one)
        if (window->DC.CurrentColumns && (window->DC.CurrentColumns->Current < window->DC.CurrentColumns->Count - 1))
          x2 -= g.Style.ItemSpacing.x;
      }
    }
    float y1 = window->DC.CursorPos.y + int(window->DC.CurrLineSize.y / 2.0f);
    float y2 = y1 + 1.0f;

    window->DC.CursorPos.x += width; //+ g.Style.ItemSpacing.x;

    const ImRect bb(ImVec2(x1, y1), ImVec2(x2, y2));
    ItemSize(ImVec2(0.0f, 0.0f)); // NB: we don't provide our width so that it doesn't get feed back into AutoFit, we don't provide height to not alter layout.
    if (!ItemAdd(bb, NULL)) {
      return;
    }

    window->DrawList->AddLine(bb.Min, ImVec2(bb.Max.x, bb.Min.y), GetColorU32(ImGuiCol_Border));
  }

  // Create a centered separator right after the current item.
  // Eg.: 
  // ImGui::PreSeparator(10);
  // ImGui::Text("Section VI");
  // ImGui::SameLineSeparator();
  void SameLineSeparator(float width = 0) {
    ImGui::SameLine();
    CenteredSeparator(width);
  }

  // Create a centered separator which can be immediately followed by a item
  void PreSeparator(float width) {
    ImGuiWindow* window = GetCurrentWindow();
    if (window->DC.CurrLineSize.y == 0)
      window->DC.CurrLineSize.y = ImGui::GetTextLineHeight();
    CenteredSeparator(width);
    ImGui::SameLine();
  }

  // The value for width is arbitrary. But it looks nice.
  void TextSeparator(const char* text, float pre_width = 10.0f) {
    ImGui::PreSeparator(pre_width);
    ImGui::Text(text);
    ImGui::SameLineSeparator();
  }
}

namespace dxvk {
  struct ImGuiTexture {
    Rc<DxvkImageView> imageView = VK_NULL_HANDLE;
    ImTextureID texID = VK_NULL_HANDLE;
    uint32_t textureFeatureFlags = 0;
  };
  std::unordered_map<XXH64_hash_t, ImGuiTexture> g_imguiTextureMap;
  fast_unordered_cache<FogState> g_imguiFogMap;
  XXH64_hash_t g_usedFogStateHash;
  std::mutex g_imguiFogMapMutex; // protects g_imguiFogMap

  struct RtxTextureOption {
    const char* uniqueId;
    const char* displayName;
    RtxOption<fast_unordered_set>* textureSetOption;
    uint32_t featureFlagMask = ImGUI::kTextureFlagsDefault;
    bool bufferToggle;
  };

  std::vector<RtxTextureOption> rtxTextureOptions = {
    {"uitextures", "UI Texture", &RtxOptions::uiTexturesObject()},
    {"worldspaceuitextures", "World Space UI Texture", &RtxOptions::worldSpaceUiTexturesObject()},
    {"worldspaceuibackgroundtextures", "World Space UI Background Texture", &RtxOptions::worldSpaceUiBackgroundTexturesObject()},
    {"skytextures", "Sky Texture", &RtxOptions::skyBoxTexturesObject()},
    {"ignoretextures", "Ignore Texture (optional)", &RtxOptions::ignoreTexturesObject()},
    {"hidetextures", "Hide Texture Instance (optional)", &RtxOptions::hideInstanceTexturesObject()},
    {"lightmaptextures","Lightmap Textures (optional)", &RtxOptions::lightmapTexturesObject()},
    {"ignorelights", "Ignore Lights (optional)", &RtxOptions::ignoreLightsObject()},
    {"particletextures", "Particle Texture (optional)", &RtxOptions::particleTexturesObject()},
    {"beamtextures", "Beam Texture (optional)", &RtxOptions::beamTexturesObject()},
    {"ignoretransparencytextures", "Ignore Transparency Layer Texture (optional)", &RtxOptions::ignoreTransparencyLayerTexturesObject()},
    {"lightconvertertextures", "Add Light to Textures (optional)", &RtxOptions::lightConverterObject()},
    {"decaltextures", "Decal Texture (optional)", &RtxOptions::decalTexturesObject()},
    {"terraintextures", "Terrain Texture", &RtxOptions::terrainTexturesObject()},
    {"watertextures", "Water Texture (optional)", &RtxOptions::animatedWaterTexturesObject()},
    {"antiCullingTextures", "Anti-Culling Texture (optional)", &RtxOptions::antiCullingTexturesObject()},
    {"motionBlurMaskOutTextures", "Motion Blur Mask-Out Textures (optional)", &RtxOptions::motionBlurMaskOutTexturesObject()},
    {"playermodeltextures", "Player Model Texture (optional)", &RtxOptions::playerModelTexturesObject()},
    {"playermodelbodytextures", "Player Model Body Texture (optional)", &RtxOptions::playerModelBodyTexturesObject()},
    {"opacitymicromapignoretextures", "Opacity Micromap Ignore Texture (optional)", &RtxOptions::opacityMicromapIgnoreTexturesObject()},
    {"ignorebakedlightingtextures","Ignore Baked Lighting Textures (optional)", &RtxOptions::ignoreBakedLightingTexturesObject()},
    {"ignorealphaontextures","Ignore Alpha Channel of Textures (optional)", &RtxOptions::ignoreAlphaOnTexturesObject()},
    {"raytracedRenderTargetTextures","Raytraced Render Target Textures (optional)", &RtxOptions::raytracedRenderTargetTexturesObject(), ImGUI::kTextureFlagsRenderTarget},
    {"particleemittertextures","Particle Emitters (optional)", &RtxOptions::particleEmitterTexturesObject()},
    {"smoothnormalstextures","Smooth Normals (optional)", &RtxOptions::smoothNormalsTexturesObject()}
  };

  RemixGui::ComboWithKey<RenderPassGBufferRaytraceMode> renderPassGBufferRaytraceModeCombo {
    "GBuffer Raytracing Mode",
    RemixGui::ComboWithKey<RenderPassGBufferRaytraceMode>::ComboEntries { {
        {RenderPassGBufferRaytraceMode::RayQuery, "RayQuery (CS)"},
        {RenderPassGBufferRaytraceMode::RayQueryRayGen, "RayQuery (RGS)"},
        {RenderPassGBufferRaytraceMode::TraceRay, "TraceRay (RGS)"}
    } }
  };

  RemixGui::ComboWithKey<RenderPassIntegrateDirectRaytraceMode> renderPassIntegrateDirectRaytraceModeCombo {
    "Integrate Direct Raytracing Mode",
    RemixGui::ComboWithKey<RenderPassIntegrateDirectRaytraceMode>::ComboEntries { {
        {RenderPassIntegrateDirectRaytraceMode::RayQuery, "RayQuery (CS)"},
        {RenderPassIntegrateDirectRaytraceMode::RayQueryRayGen, "RayQuery (RGS)"}
    } }
  };

  RemixGui::ComboWithKey<RenderPassIntegrateIndirectRaytraceMode> renderPassIntegrateIndirectRaytraceModeCombo {
    "Integrate Indirect Raytracing Mode",
    RemixGui::ComboWithKey<RenderPassIntegrateIndirectRaytraceMode>::ComboEntries { {
        {RenderPassIntegrateIndirectRaytraceMode::RayQuery, "RayQuery (CS)"},
        {RenderPassIntegrateIndirectRaytraceMode::RayQueryRayGen, "RayQuery (RGS)"},
        {RenderPassIntegrateIndirectRaytraceMode::TraceRay, "TraceRay (RGS)"}
    } }
  };

  RemixGui::ComboWithKey<CameraAnimationMode> cameraAnimationModeCombo {
    "Camera Animation Mode",
    RemixGui::ComboWithKey<CameraAnimationMode>::ComboEntries { {
        {CameraAnimationMode::CameraShake_LeftRight, "CameraShake Left-Right"},
        {CameraAnimationMode::CameraShake_FrontBack, "CameraShake Front-Back"},
        {CameraAnimationMode::CameraShake_Yaw, "CameraShake Yaw"},
        {CameraAnimationMode::CameraShake_Pitch, "CameraShake Pitch"},
        {CameraAnimationMode::YawRotation, "Camera Yaw Rotation"}
    } }
  };

  RemixGui::ComboWithKey<int> textureQualityCombo {
    "Texture Quality",
    RemixGui::ComboWithKey<int>::ComboEntries { {
        {0, "High"},
        {1, "Low"},
    } }
  };

  RemixGui::ComboWithKey<ViewDistanceMode> viewDistanceModeCombo {
    "View Distance Mode",
    RemixGui::ComboWithKey<ViewDistanceMode>::ComboEntries { {
        {ViewDistanceMode::None, "None"},
        {ViewDistanceMode::HardCutoff, "Hard Cutoff"},
        {ViewDistanceMode::CoherentNoise, "Coherent Noise"},
    } }
  };

  RemixGui::ComboWithKey<ViewDistanceFunction> viewDistanceFunctionCombo {
    "View Distance Function",
    RemixGui::ComboWithKey<ViewDistanceFunction>::ComboEntries { {
        {ViewDistanceFunction::Euclidean, "Euclidean"},
        {ViewDistanceFunction::PlanarEuclidean, "Planar Euclidean"},
    } }
  };

  static auto fusedWorldViewModeCombo = RemixGui::ComboWithKey<FusedWorldViewMode>(
  "Fused World-View Mode",
  RemixGui::ComboWithKey<FusedWorldViewMode>::ComboEntries { {
      {FusedWorldViewMode::None, "None"},
      {FusedWorldViewMode::View, "In View Transform"},
      {FusedWorldViewMode::World, "In World Transform"},
  } });

  static auto skyAutoDetectCombo = RemixGui::ComboWithKey<SkyAutoDetectMode>(
    "Sky Auto-Detect",
    RemixGui::ComboWithKey<SkyAutoDetectMode>::ComboEntries{ {
      {SkyAutoDetectMode::None, "Off"},
      {SkyAutoDetectMode::CameraPosition, "By Camera Position"},
      {SkyAutoDetectMode::CameraPositionAndDepthFlags, "By Camera Position and Depth Flags"}
  } });

  static auto upscalerNoDLSSCombo = RemixGui::ComboWithKey<UpscalerType>(
    "Upscaler Type",
    { {
      {UpscalerType::None, "None"},
      {UpscalerType::NIS, "NIS"},
      {UpscalerType::TAAU, "TAA-U"},
      {UpscalerType::XeSS, "XeSS"},
  } });

  static auto upscalerDLSSCombo = RemixGui::ComboWithKey<UpscalerType>(
    "Upscaler Type",
    { {
      {UpscalerType::None, "None"},
      {UpscalerType::DLSS, "DLSS"},
      {UpscalerType::NIS, "NIS"},
      {UpscalerType::TAAU, "TAA-U"},
      {UpscalerType::XeSS, "XeSS"},
  } });

  RemixGui::ComboWithKey<DLSSProfile> dlssProfileCombo{
    "DLSS Mode",
    RemixGui::ComboWithKey<DLSSProfile>::ComboEntries{ {
        {DLSSProfile::UltraPerf, "Ultra Performance"},
        {DLSSProfile::MaxPerf, "Performance"},
        {DLSSProfile::Balanced, "Balanced"},
        {DLSSProfile::MaxQuality, "Quality"},
        {DLSSProfile::FullResolution, "Full Resolution"},
        {DLSSProfile::Auto, "Auto"},
    } }
  };

  RemixGui::ComboWithKey<XeSSPreset> xessPresetCombo{
    "XeSS Preset",
    RemixGui::ComboWithKey<XeSSPreset>::ComboEntries{ {
        {XeSSPreset::UltraPerf, "Ultra Performance"},
        {XeSSPreset::Performance, "Performance"},
        {XeSSPreset::Balanced, "Balanced"},
        {XeSSPreset::Quality, "Quality"},
        {XeSSPreset::UltraQuality, "Ultra Quality"},
        {XeSSPreset::UltraQualityPlus, "Ultra Quality Plus"},
        {XeSSPreset::NativeAA, "Native Anti-Aliasing"},
        {XeSSPreset::Custom, "Custom"},
    } }
  };

  RemixGui::ComboWithKey<RussianRouletteMode> secondPlusBounceRussianRouletteModeCombo {
    "2nd+ Bounce Russian Roulette Mode",
    RemixGui::ComboWithKey<RussianRouletteMode>::ComboEntries { {
        {RussianRouletteMode::ThroughputBased, "Throughput Based"},
        {RussianRouletteMode::SpecularBased, "Specular Based"}
    } }
  };

  RemixGui::ComboWithKey<IntegrateIndirectMode> integrateIndirectModeCombo {
    "Integrate Indirect Illumination Mode",
    RemixGui::ComboWithKey<IntegrateIndirectMode>::ComboEntries { {
        {IntegrateIndirectMode::ImportanceSampled, "Importance Sampled",  
          "Importance Sampled. Importance sampled mode uses typical GI sampling and it is not recommended for general use as it provides the noisiest output.\n"
          "It serves as a reference integration mode for validation of other indirect integration modes." },
        {IntegrateIndirectMode::ReSTIRGI, "ReSTIR GI", 
          "ReSTIR GI provides improved indirect path sampling over \"Importance Sampled\" mode with better indirect diffuse and specular GI quality at increased performance cost."},
        {IntegrateIndirectMode::NeuralRadianceCache, "RTX Neural Radiance Cache", 
          "RTX Neural Radiance Cache (NRC). NRC is an AI based world space radiance cache. It is live trained by the path tracer\n"
          "and allows paths to terminate early by looking up the cached value and saving performance.\n"
          "NRC supports infinite bounces and often provides results closer to that of reference than ReSTIR GI\n"
          "while increasing performance in scenarios where ray paths have 2 or more bounces on average."}
    } }
  };

  static auto rayReconstructionModelCombo = RemixGui::ComboWithKey<DxvkRayReconstruction::RayReconstructionModel>(
    "Ray Reconstruction Model",
    { {
      {DxvkRayReconstruction::RayReconstructionModel::Transformer, "Transformer", "Ensures highest image quality. Can be more expensive than CNN in terms of memory and performance."},
      {DxvkRayReconstruction::RayReconstructionModel::CNN, "CNN", "Ensures great image quality"},
  } });

  RemixGui::ComboWithKey<int> dlfgMfgModeCombo {
    "DLSS Frame Generation Mode",
    RemixGui::ComboWithKey<int>::ComboEntries { {
        {1, "2x"},
        {2, "3x"},
        {3, "4x"},
        {4, "5x"},
        {5, "6x"},
    } }
  };

  RemixGui::ComboWithKey<ReflexMode> reflexModeCombo{
    "Reflex",
    RemixGui::ComboWithKey<ReflexMode>::ComboEntries{ {
        {ReflexMode::None, "Disabled"},
        {ReflexMode::LowLatency, "Enabled"},
        {ReflexMode::LowLatencyBoost, "Enabled + Boost"},
    } }
  };

#ifdef REMIX_DEVELOPMENT
  RemixGui::ComboWithKey<dxvk::RtxFramePassStage>::ComboEntries aliasingPassComboEntries = { {
      { RtxFramePassStage::FrameBegin, "FrameBegin" },
      { RtxFramePassStage::Volumetrics, "Volumetrics" },
      { RtxFramePassStage::SparseRendering, "SparseRendering" },
      { RtxFramePassStage::VolumeIntegrateRestirInitial, "VolumeIntegrateRestirInitial" },
      { RtxFramePassStage::VolumeIntegrateRestirVisible, "VolumeIntegrateRestirVisible" },
      { RtxFramePassStage::VolumeIntegrateRestirTemporal, "VolumeIntegrateRestirTemporal" },
      { RtxFramePassStage::VolumeIntegrateRestirSpatialResampling, "VolumeIntegrateRestirSpatialResampling" },
      { RtxFramePassStage::VolumeIntegrateRaytracing, "VolumeIntegrateRaytracing" },
      { RtxFramePassStage::GBufferPrimaryRays, "GBufferPrimaryRays" },
      { RtxFramePassStage::ReflectionPSR, "ReflectionPSR" },
      { RtxFramePassStage::TransmissionPSR, "TransmissionPSR" },
      { RtxFramePassStage::RTXDI_InitialTemporalReuse, "RTXDI_InitialTemporalReuse" },
      { RtxFramePassStage::RTXDI_SpatialReuse, "RTXDI_SpatialReuse" },
      { RtxFramePassStage::NEE_Cache, "NEE_Cache" },
      { RtxFramePassStage::DirectIntegration, "DirectIntegration" },
      { RtxFramePassStage::RTXDI_ComputeGradients, "RTXDI_ComputeGradients" },
      { RtxFramePassStage::IndirectIntegration, "IndirectIntegration" },
      { RtxFramePassStage::NEE_Integration, "NEE_Integration" },
      { RtxFramePassStage::NRC, "NRC" },
      { RtxFramePassStage::RTXDI_FilterGradients, "RTXDI_FilterGradients" },
      { RtxFramePassStage::RTXDI_ComputeConfidence, "RTXDI_ComputeConfidence" },
      { RtxFramePassStage::ReSTIR_GI_TemporalReuse, "ReSTIR_GI_TemporalReuse" },
      { RtxFramePassStage::ReSTIR_GI_SpatialReuse, "ReSTIR_GI_SpatialReuse" },
      { RtxFramePassStage::ReSTIR_GI_FinalShading, "ReSTIR_GI_FinalShading" },
      { RtxFramePassStage::Demodulate, "Demodulate" },
      { RtxFramePassStage::NRD, "NRD" },
      { RtxFramePassStage::CompositionAlphaBlend, "CompositionAlphaBlend" },
      { RtxFramePassStage::Composition, "Composition" },
      { RtxFramePassStage::DLSS, "DLSS" },
      { RtxFramePassStage::DLSSRR, "DLSSRR" },
      { RtxFramePassStage::NIS, "NIS" },
      { RtxFramePassStage::XeSS, "XeSS" },
      { RtxFramePassStage::TAA, "TAA" },
      { RtxFramePassStage::DustParticles, "DustParticles" },
      { RtxFramePassStage::Bloom, "Bloom" },
      { RtxFramePassStage::PostFX, "PostFX" },
      { RtxFramePassStage::AutoExposure_Histogram, "AutoExposure_Histogram" },
      { RtxFramePassStage::AutoExposure_Exposure, "AutoExposure_Exposure" },
      { RtxFramePassStage::ToneMapping, "ToneMapping" },
      { RtxFramePassStage::FrameEnd, "FrameEnd" },
  } };

  static auto aliasingBeginPassCombo = RemixGui::ComboWithKey<dxvk::RtxFramePassStage>(
    "Aliasing Begin Pass", RemixGui::ComboWithKey<dxvk::RtxFramePassStage>::ComboEntries{ aliasingPassComboEntries });

  static auto aliasingEndPassCombo = RemixGui::ComboWithKey<dxvk::RtxFramePassStage>(
    "Aliasing End Pass", RemixGui::ComboWithKey<dxvk::RtxFramePassStage>::ComboEntries { aliasingPassComboEntries });

  static auto aliasingExtentCombo = RemixGui::ComboWithKey<RtxTextureExtentType>(
    "Aliasing Extent Type",
    { {
      { RtxTextureExtentType::DownScaledExtent, "DownScaledExtent" },
      { RtxTextureExtentType::TargetExtent, "TargetExtent" },
      { RtxTextureExtentType::Custom, "Custom" },
  } } );

  static auto aliasingFormatCombo = RemixGui::ComboWithKey<RtxTextureFormatCompatibilityCategory>(
     "Aliasing Format",
     { {
      { RtxTextureFormatCompatibilityCategory::Color_Format_8_Bits, "8 Bits Color Texture" },
      { RtxTextureFormatCompatibilityCategory::Color_Format_16_Bits, "16 Bits Color Texture" },
      { RtxTextureFormatCompatibilityCategory::Color_Format_32_Bits, "32 Bits Color Texture" },
      { RtxTextureFormatCompatibilityCategory::Color_Format_64_Bits, "64 Bits Color Texture" },
      { RtxTextureFormatCompatibilityCategory::Color_Format_128_Bits, "128 Bits Color Texture" },
      { RtxTextureFormatCompatibilityCategory::Color_Format_256_Bits, "256 Bits Color Texture" },
      // All other formats
      { RtxTextureFormatCompatibilityCategory::InvalidFormatCompatibilityCategory, "Not Listed Format" },
     } }
  );

  static auto aliasingImageTypeCombo = RemixGui::ComboWithKey<VkImageType>(
     "Aliasing Image Type",
     { {
      { VK_IMAGE_TYPE_1D, "VK_IMAGE_TYPE_1D" },
      { VK_IMAGE_TYPE_2D, "VK_IMAGE_TYPE_2D" },
      { VK_IMAGE_TYPE_3D, "VK_IMAGE_TYPE_3D" },
     } }
  );

  static auto aliasingImageViewTypeCombo = RemixGui::ComboWithKey<VkImageViewType>(
     "Aliasing Image View Type",
     { {
      { VK_IMAGE_VIEW_TYPE_1D, "VK_IMAGE_VIEW_TYPE_1D" },
      { VK_IMAGE_VIEW_TYPE_1D_ARRAY, "VK_IMAGE_VIEW_TYPE_1D_ARRAY" },
      { VK_IMAGE_VIEW_TYPE_2D, "VK_IMAGE_VIEW_TYPE_2D" },
      { VK_IMAGE_VIEW_TYPE_2D_ARRAY, "VK_IMAGE_VIEW_TYPE_2D_ARRAY" },
      { VK_IMAGE_VIEW_TYPE_3D, "VK_IMAGE_VIEW_TYPE_3D" },
      { VK_IMAGE_VIEW_TYPE_CUBE, "VK_IMAGE_VIEW_TYPE_CUBE" },
      { VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, "VK_IMAGE_VIEW_TYPE_CUBE_ARRAY" },
     } }
  );
#endif

  enum class TerrainMode {
    None,
    TerrainBaker,
    AsDecals,
  };
  static auto terrainModeCombo = RemixGui::ComboWithKey<TerrainMode>(
    "Mode##terrain",
    {
      {        TerrainMode::None,              "None"},
      {TerrainMode::TerrainBaker,     "Terrain Baker"},
      {    TerrainMode::AsDecals, "Terrain-as-Decals"},
  });

  static auto themeCombo = RemixGui::ComboWithKey<ImGUI::Theme>(
    "Mode##theme",
    {
      {ImGUI::Theme::Toolkit,  "Default Theme"},
      {ImGUI::Theme::Legacy,   "Legacy Theme"},
      {ImGUI::Theme::Nvidia,   "NVIDIA Theme"},
  });

  // Styles 
  constexpr ImGuiSliderFlags sliderFlags = ImGuiSliderFlags_AlwaysClamp;
  constexpr ImGuiTreeNodeFlags collapsingHeaderClosedFlags = ImGuiTreeNodeFlags_CollapsingHeader;
  constexpr ImGuiTreeNodeFlags collapsingHeaderFlags = collapsingHeaderClosedFlags | ImGuiTreeNodeFlags_DefaultOpen;
  constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_AlwaysVerticalScrollbar;
  constexpr ImGuiWindowFlags popupWindowFlags = ImGuiWindowFlags_NoSavedSettings;

  RemixGui::ComboWithKey<UpscalerType>& getUpscalerCombo(DxvkDLSS& dlss, DxvkRayReconstruction& rayReconstruction) {
    if (dlss.supportsDLSS()) {
      return upscalerDLSSCombo;
    } else {
      // Drop DLSS item if unsupported.
      return upscalerNoDLSSCombo;
    }
  }

  bool ImGUI::showRayReconstructionEnable(bool supportsRR) {
    // Only show DLSS-RR option if "showRayReconstructionOption" is set to true.
    bool changed = false;
    bool rayReconstruction = RtxOptions::enableRayReconstruction();
    if (RtxOptions::showRayReconstructionOption()) {
      ImGui::BeginDisabled(!supportsRR);
      changed = RemixGui::Checkbox("Ray Reconstruction", &RtxOptions::enableRayReconstructionObject());

      if (RtxOptions::enableRayReconstruction()) {
        rayReconstructionModelCombo.getKey(&DxvkRayReconstruction::modelObject());
      }
      ImGui::EndDisabled();
    }

    // Disable DLSS-RR if it's unsupported.
    if (!supportsRR && RtxOptions::enableRayReconstruction()) {
      RtxOptions::enableRayReconstruction.setDeferred(false);
      changed = true;
    }
    return changed;
  }

  ImGUI::ImGUI(DxvkDevice* device)
  : m_device (device)
  , m_gameHwnd   (nullptr)
  , m_about  (new ImGuiAbout)
  , m_splash  (new ImGuiSplash)
  , m_graphGUI  (new RtxGraphGUI) {
    // Set up constant state
    m_rsState.polygonMode       = VK_POLYGON_MODE_FILL;
    m_rsState.cullMode          = VK_CULL_MODE_BACK_BIT;
    m_rsState.frontFace         = VK_FRONT_FACE_CLOCKWISE;
    m_rsState.depthClipEnable   = VK_FALSE;
    m_rsState.depthBiasEnable   = VK_FALSE;
    m_rsState.conservativeMode  = VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
    m_rsState.sampleCount       = VK_SAMPLE_COUNT_1_BIT;

    m_blendMode.enableBlending  = VK_TRUE;
    m_blendMode.colorSrcFactor  = VK_BLEND_FACTOR_ONE;
    m_blendMode.colorDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_blendMode.colorBlendOp    = VK_BLEND_OP_ADD;
    m_blendMode.alphaSrcFactor  = VK_BLEND_FACTOR_ONE;
    m_blendMode.alphaDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_blendMode.alphaBlendOp    = VK_BLEND_OP_ADD;
    m_blendMode.writeMask       = VK_COLOR_COMPONENT_R_BIT
                                | VK_COLOR_COMPONENT_G_BIT
                                | VK_COLOR_COMPONENT_B_BIT
                                | VK_COLOR_COMPONENT_A_BIT;
    
    // the size of the pool is oversized, but it's copied from imgui demo itself.
    VkDescriptorPoolSize pool_sizes[] =
    {
      { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
      { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
      { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    // ImGUI is currently using a single set per texture, and so we want this to be a big number 
    //  to support displaying texture lists in games that use a lot of textures.
    // See: 'ImGui_ImplDxvk::AddTexture(...)' for more details about how this system works.
    pool_info.maxSets = 10000;
    pool_info.poolSizeCount = std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    if (!NeuralRadianceCache::checkIsSupported(device)) {
      // Remove unsupported option
      integrateIndirectModeCombo.removeComboEntry(IntegrateIndirectMode::NeuralRadianceCache);
    }

    m_device->vkd()->vkCreateDescriptorPool(m_device->handle(), &pool_info, nullptr, &m_imguiPool);

    // Initialize the core structures of ImGui and ImPlot
    m_context = ImGui::CreateContext();
    m_plotContext = ImPlot::CreateContext();

    ImGui::SetCurrentContext(m_context);
    ImPlot::SetCurrentContext(m_plotContext);

    // Setup custom style
    setupStyle();

    // Enable keyboard nav
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    m_capture = new ImGuiCapture(this);

    if (RtxOptions::useNewGuiInputMethod()) {
      m_overlayWin = new GameOverlay("RemixGuiInputSink", this);
    }
  }

  ImGUI::~ImGUI() {
    g_imguiTextureMap.clear();

    ImGui::SetCurrentContext(m_context);
    ImPlot::SetCurrentContext(m_plotContext);

    if(m_init) {
      ImGui_ImplWin32_Shutdown();
    }

    //add the destroy the imgui created structures
    if(m_imguiPool != VK_NULL_HANDLE)
      m_device->vkd()->vkDestroyDescriptorPool(m_device->handle(), m_imguiPool, nullptr);

    if (m_init) {
      // FontView and FontImage will be released by m_fontTextureView and m_fontTexture later
      ImGuiIO& io = ImGui::GetIO();
      ImGui_ImplDxvk::Data* bd = (ImGui_ImplDxvk::Data*) io.BackendRendererUserData;
      bd->FontView = VK_NULL_HANDLE;
      bd->FontImage = VK_NULL_HANDLE;

      ImGui_ImplDxvk::Shutdown();
      m_init = false;
    }

    // Destroy the ImGui and ImPlot context
    ImPlot::DestroyContext(m_plotContext);
    ImGui::DestroyContext(m_context);
  }
  
  void ImGUI::AddTexture(const XXH64_hash_t hash, const Rc<DxvkImageView>& imageView, uint32_t textureFeatureFlags) {
    if (g_imguiTextureMap.find(hash) == g_imguiTextureMap.end()) {
      ImGuiTexture texture;
      texture.imageView = imageView; // Hold a refcount
      texture.texID = VK_NULL_HANDLE;
      texture.textureFeatureFlags = textureFeatureFlags;
      g_imguiTextureMap[hash] = texture;
    }
  }

  void ImGUI::ReleaseTexture(const XXH64_hash_t hash) {
    if (RtxOptions::keepTexturesForTagging()) {
      return;
    }
    
    // Note: Erase will do nothing if the hash does not exist in the map, and erase it if it is.
    g_imguiTextureMap.erase(hash);
  }

  void ImGUI::SetFogStates(const fast_unordered_cache<FogState>& fogStates, XXH64_hash_t usedFogHash) {
    const std::lock_guard<std::mutex> lock(g_imguiFogMapMutex);
    g_imguiFogMap = fogStates;
    g_usedFogStateHash = usedFogHash;
  }

  void ImGUI::wndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (m_overlayWin.ptr() != nullptr) {
      m_overlayWin->gameWndProcHandler(hWnd, msg, wParam, lParam);
    } else {
      // Note this is the old method for grabbing keyboard/mouse inputs which relies on hooking
      //  the wndproc from the original game, and sending that data across the x86 -> x64 bridge.  
      //  We see compatibilities in older applications with this approach that are tricky to resolve.
      //  Favour the new approach `useNewGuiInputMethod` when possible.
      ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }
  }

  void ImGUI::showMemoryStats() const {
    if (RtxOptions::Automation::disableDisplayMemoryStatistics()) {
      return;
    }

    // Gather runtime vidmem stats
    VkDeviceSize vidmemSize = 0;
    VkDeviceSize vidmemUsedSize = 0;

    DxvkAdapterMemoryInfo memHeapInfo = m_device->adapter()->getMemoryHeapInfo();
    DxvkMemoryAllocator& memoryManager = m_device->getCommon()->memoryManager();
    const VkPhysicalDeviceMemoryProperties& memoryProperties = memoryManager.getMemoryProperties();

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; i++) {
      if (memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        vidmemSize += memHeapInfo.heaps[i].memoryBudget;
        vidmemUsedSize += memHeapInfo.heaps[i].memoryAllocated;
      }
    }

    // Calculate video memory information

    constexpr float bytesPerMebibyte = 1024.f * 1024.f;
    const VkDeviceSize vidmemFreeSize = vidmemSize - std::min(vidmemUsedSize, vidmemSize);
    const float vidmemTotalSizeMB = (float)((double) vidmemSize / bytesPerMebibyte);
    const float vidmemUsedSizeMB = (float)((double) vidmemUsedSize / bytesPerMebibyte);
    const float vidmemFreeSizeMB = (float)((double) vidmemFreeSize / bytesPerMebibyte);
    const float freeVidMemRatio = (float)std::min((double) vidmemFreeSize / (double) vidmemSize, 1.0);

    // Display video memory information

#ifdef REMIX_DEVELOPMENT
    ImGui::Text("Video Memory Usage: %.f MiB / %.f MiB (%.f MiB free)", vidmemUsedSizeMB, vidmemTotalSizeMB, vidmemFreeSizeMB);
#else
    // Note: Simplify for end users, free memory is usually not as important to list and can just be observed visually with the graph.
    ImGui::Text("Video Memory Usage: %.f MiB / %.f MiB", vidmemUsedSizeMB, vidmemTotalSizeMB);
#endif

    // Note: Map the range [0.1, 0.6] to [0, 1] and clamp outside it to bias and clamp the green->red color transition more.
    const float remappedFreeVidMemRatio = std::max(std::min(freeVidMemRatio + 0.4f, 1.0f) - 0.5f, 0.0f) * 2.0f;
    ImVec4 barColor = ImVec4{ 1.0f, 1.0f, 1.0f, 1.0f };

    ImGui::ColorConvertHSVtoRGB(
      remappedFreeVidMemRatio * 0.32f, 0.717f, 0.704f,
      barColor.x, barColor.y, barColor.z);

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
    ImGui::ProgressBar(vidmemUsedSizeMB / vidmemTotalSizeMB);
    ImGui::PopStyleColor();

    ImGui::TextWrapped("RTX Remix dynamically uses available VRAM to maximize texture quality.");
    
    ImGui::Dummy(ImVec2 { 4, 0 });
  }

  void ImGUI::switchMenu(UIType type, bool force) {
    UIType oldType = RtxOptions::showUI();
    if (oldType == type && !force) {
      return;
    }
    
    if (type == UIType::None) {
      onCloseMenus();
    } else {
      onOpenMenus();
    }

    {
      // Target user layer for UI state changes (this is a user preference)
      RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);
      RtxOptions::showUI.setDeferred(type);
    }

    if (RtxOptions::blockInputToGameInUI()) {
      BridgeMessageChannel::get().send("UWM_REMIX_UIACTIVE_MSG",
                                       type != UIType::None ? 1 : 0, 0);
    }
  }
  
  void ImGUI::showMaterialOptions() {
    if (RemixGui::CollapsingHeader("Material Options (optional)", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      if (RemixGui::CollapsingHeader("Legacy Material Defaults", collapsingHeaderFlags)) {
        ImGui::Indent();

        RemixGui::Checkbox("Use Albedo/Opacity Texture (if present)", &LegacyMaterialDefaults::useAlbedoTextureIfPresentObject());
        RemixGui::Checkbox("Ignore Texture Alpha Channel", &LegacyMaterialDefaults::ignoreAlphaChannelObject());
        RemixGui::ColorEdit3("Albedo", &LegacyMaterialDefaults::albedoConstantObject());
        RemixGui::DragFloat("Opacity", &LegacyMaterialDefaults::opacityConstantObject(), 0.01f, 0.f, 1.f);
        RemixGui::ColorEdit3("Emissive Color", &LegacyMaterialDefaults::emissiveColorConstantObject());
        RemixGui::DragFloat("Emissive Intensity", &LegacyMaterialDefaults::emissiveIntensityObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
        RemixGui::DragFloat("Roughness", &LegacyMaterialDefaults::roughnessConstantObject(), 0.01f, 0.02f, 1.f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Metallic", &LegacyMaterialDefaults::metallicConstantObject(), 0.01f, 0.0f, 1.f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Anisotropy", &LegacyMaterialDefaults::anisotropyObject(), 0.01f, -1.0f, 1.f, "%.3f", sliderFlags);

        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("PBR Material Modifiers", collapsingHeaderFlags)) {
        ImGui::Indent();

        if (RemixGui::CollapsingHeader("Opaque", collapsingHeaderFlags)) {
          ImGui::Indent();

          RemixGui::SliderFloat("Albedo Scale", &OpaqueMaterialOptions::albedoScaleObject(), 0.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Albedo Bias", &OpaqueMaterialOptions::albedoBiasObject(), -1.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Metallic Scale", &OpaqueMaterialOptions::metallicScaleObject(), 0.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Metallic Bias", &OpaqueMaterialOptions::metallicBiasObject(), -1.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Roughness Scale", &OpaqueMaterialOptions::roughnessScaleObject(), 0.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Roughness Bias", &OpaqueMaterialOptions::roughnessBiasObject(), -1.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Normal Strength##1", &OpaqueMaterialOptions::normalIntensityObject(), -10.0f, 10.f, "%.3f", sliderFlags);

          RemixGui::Checkbox("Enable dual-layer animated water normal for Opaque", &OpaqueMaterialOptions::layeredWaterNormalEnableObject());

          if (OpaqueMaterialOptions::layeredWaterNormalEnable()) {
            ImGui::TextWrapped("Animated water with Opaque material is dependent on the original draw call animating using a texture transform.");
            RemixGui::SliderFloat2("Layered Motion Direction", &OpaqueMaterialOptions::layeredWaterNormalMotionObject(), -1.0f, 1.0f, "%.3f", sliderFlags);
            RemixGui::SliderFloat("Layered Motion Scale", &OpaqueMaterialOptions::layeredWaterNormalMotionScaleObject(), -10.0f, 10.0f, "%.3f", sliderFlags);
            RemixGui::SliderFloat("LOD bias", &OpaqueMaterialOptions::layeredWaterNormalLodBiasObject(), 0.0f, 16.0f, "%.3f", sliderFlags);
          }

          ImGui::Unindent();
        }

        if (RemixGui::CollapsingHeader("Translucent", collapsingHeaderFlags)) {
          ImGui::Indent();

          RemixGui::SliderFloat("Transmit. Color Scale", &TranslucentMaterialOptions::transmittanceColorScaleObject(), 0.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Transmit. Color Bias", &TranslucentMaterialOptions::transmittanceColorBiasObject(), -1.0f, 1.f, "%.3f", sliderFlags);
          RemixGui::SliderFloat("Normal Strength##2", &TranslucentMaterialOptions::normalIntensityObject(), -10.0f, 10.f, "%.3f", sliderFlags);
          RemixGui::DragFloat("IOR Scale", &TranslucentMaterialOptions::refractiveIndexScaleObject(), 0.01f, 0.1f, 3.0f);

          RemixGui::Checkbox("Enable dual-layer animated water normal for Translucent", &TranslucentMaterialOptions::animatedWaterEnableObject());
          if (TranslucentMaterialOptions::animatedWaterEnable()) {
            ImGui::TextWrapped("Animated water with Translucent materials will animate using Remix animation time.");

            RemixGui::SliderFloat2("Primary Texcoord Velocity", &TranslucentMaterialOptions::animatedWaterPrimaryNormalMotionObject(), -0.5f, 0.5f, "%.3f", sliderFlags);
            RemixGui::SliderFloat2("Secondary Normal Texcoord Velocity", &TranslucentMaterialOptions::animatedWaterSecondaryNormalMotionObject(), -0.5f, 0.5f, "%.3f", sliderFlags);
            RemixGui::SliderFloat("Secondary Normal LOD bias", &TranslucentMaterialOptions::animatedWaterSecondaryNormalLodBiasObject(), 0.0f, 16.0f, "%.3f", sliderFlags);
          }
          ImGui::Unindent();
        }

        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("PBR Material Overrides", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        if (RemixGui::CollapsingHeader("Opaque", collapsingHeaderFlags)) {
          ImGui::Indent();

          RemixGui::Checkbox("Enable Thin-Film Layer", &OpaqueMaterialOptions::enableThinFilmOverrideObject());

          if (OpaqueMaterialOptions::enableThinFilmOverride()) {
            RemixGui::SliderFloat("Thin Film Thickness", &OpaqueMaterialOptions::thinFilmThicknessOverrideObject(), 0.0f, OPAQUE_SURFACE_MATERIAL_THIN_FILM_MAX_THICKNESS, "%.1f nm", sliderFlags);
          }

          ImGui::Unindent();
        }

        if (RemixGui::CollapsingHeader("Translucent", collapsingHeaderFlags)) {
          ImGui::Indent();

          RemixGui::Checkbox("Enable Diffuse Layer", &TranslucentMaterialOptions::enableDiffuseLayerOverrideObject());

          ImGui::Unindent();
        }

        ImGui::Unindent();
      }

      ImGui::Unindent();
    }
  }

  void ImGUI::processHotkeys() {
    auto& io = ImGui::GetIO();

    if (checkHotkeyState(RtxOptions::remixMenuKeyBinds())) {
      if(RtxOptions::defaultToAdvancedUI()) {
        switchMenu(RtxOptions::showUI() != UIType::None ? UIType::None : UIType::Advanced);
      } else {
        switchMenu(RtxOptions::showUI() != UIType::None ? UIType::None : UIType::Basic);
      }
    }


    // The Dusklight overlay's own bind. Independent of Remix's on purpose: they are two overlays,
    // either can be up without the other, and the game these settings belong to opened its own
    // overlay on the same key before this rendering mode stopped drawing it.
    if (checkHotkeyState(DusklightGame::menuKeyBinds())) {
      m_dusklightWindowOpen = !m_dusklightWindowOpen;
    }

    // Toggle ImGUI mouse cursor. Alt-Del
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Delete))) {
      RtxOptions::showUICursor.setDeferred(!RtxOptions::showUICursor());
    }

    // Toggle input blocking. Alt-Backspace
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace))) {
      RtxOptions::blockInputToGameInUI.setDeferred(!RtxOptions::blockInputToGameInUI());
    }
  }

  void ImGUI::update(const Rc<DxvkContext>& ctx) {
    processHotkeys();
    updateQuickActions(ctx);

    m_splash->update(m_largeFont);

    m_about->update(ctx);
    
    m_capture->update(ctx);

    showDebugVisualizations(ctx);

    const auto showUI = RtxOptions::showUI();
    if (showUI == UIType::Advanced) {
      showMainMenu(ctx);

      // Uncomment to see the ImGUI demo, good reference!  Also, need to undefine IMGUI_DISABLE_DEMO_WINDOWS (in "imconfig.h")
      //ImGui::ShowDemoWindow();
    } else if (showUI == UIType::Basic) {
      showUserMenu(ctx);
    }
    
    // Render any blocked edit popup warnings
    RemixGui::RenderRtxOptionBlockedEditPopup();

    // Note: Only display the latency stats window when the Advanced UI is active as the Basic UI acts as a modal which blocks other
    // windows from being interacted with.
    if (showUI == UIType::Advanced && m_reflexLatencyStatsOpen) {
      showReflexLatencyStats();
    }

    // Deliberately outside the branch above: this window is not one of Remix's menus and does not
    // hide with them.
    if (m_dusklightWindowOpen) {
      showDusklightOverlay(ctx);
    }

    // Either overlay being up has to keep the cursor, or opening this one alone would leave it
    // unusable.
    const bool anyOverlayOpen = showUI != UIType::None || m_dusklightWindowOpen;

    // Published for the game to read. Remix's own input blocking sends a message across the 32 bit
    // bridge, which a 64 bit game loading this DLL directly never receives - so on this setup input
    // has always reached the game straight through an open menu. The game is already listening to
    // the Dusklight bridge, so the intent travels that way instead.
    const bool wantsInput = anyOverlayOpen && DusklightGame::blockGameInput();
    if (DusklightGame::uiActive() != wantsInput) {
      DusklightGame::uiActive.setDeferred(wantsInput);
    }

    if (!anyOverlayOpen) {
      ImGui::CloseCurrentPopup();
      ImGui::GetIO().MouseDrawCursor = false;
    } else {
      if (RtxOptions::showUICursor()) {
        ImGui::GetIO().MouseDrawCursor = true;
        // Force display counter into invisible state
        while (ShowCursor(FALSE) >= 0) { }
      } else {
        // Force display counter into visible state
        while (ShowCursor(TRUE) < 0) {  }
      }
    }

    showHudMessages(ctx);

#ifdef REMIX_DEVELOPMENT
    // Show visual indicator when crash hotkey is armed
    if (RtxOptions::enableCrashHotkey()) {
      const auto crashHotkeyStr = buildKeyBindDescriptorString(RtxOptions::crashHotkey());
      const auto warningText = str::format("!! CRASH HOTKEY ARMED (", crashHotkeyStr, ") !!");
      const ImVec2 textSize = ImGui::CalcTextSize(warningText.c_str());
      const ImGuiViewport* viewport = ImGui::GetMainViewport();
      const ImVec2 textPos(viewport->Size.x - textSize.x - 10.0f, 10.0f);
      ImGui::GetForegroundDrawList()->AddText(textPos, IM_COL32(255, 50, 50, 255), warningText.c_str());
    }
#endif

    ImGui::Render();
  }

  void ImGUI::updateQuickActions(const Rc<DxvkContext>& ctx) {
#ifdef REMIX_DEVELOPMENT
    enum RtxQuickAction : uint32_t {
      kOriginal = 0,
      kRtxOnEnhanced,
      kRtxOn,
      kCount
    };

    auto common = ctx->getCommonObjects();
    static RtxQuickAction sQuickAction = common->getSceneManager().areAllReplacementsLoaded() ? RtxQuickAction::kRtxOnEnhanced : RtxQuickAction::kRtxOn;

    if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_KeypadAdd))) {
      sQuickAction = (RtxQuickAction) ((sQuickAction + 1) % RtxQuickAction::kCount);

      // Skip over the enhancements quick option if no replacements are loaded
      if(!common->getSceneManager().areAllReplacementsLoaded() && sQuickAction == RtxQuickAction::kRtxOnEnhanced)
        sQuickAction = (RtxQuickAction) ((sQuickAction + 1) % RtxQuickAction::kCount);

      switch (sQuickAction) {
      case RtxQuickAction::kOriginal:
        RtxOptions::enableRaytracing.setDeferred(false);
        RtxOptions::enableReplacementLights.setDeferred(false);
        RtxOptions::enableReplacementMaterials.setDeferred(false);
        RtxOptions::enableReplacementMeshes.setDeferred(false);
        break;
      case RtxQuickAction::kRtxOnEnhanced:
        RtxOptions::enableRaytracing.setDeferred(true);
        RtxOptions::enableReplacementLights.setDeferred(true);
        RtxOptions::enableReplacementMaterials.setDeferred(true);
        RtxOptions::enableReplacementMeshes.setDeferred(true);
        break;
      case RtxQuickAction::kRtxOn:
        RtxOptions::enableRaytracing.setDeferred(true);
        RtxOptions::enableReplacementLights.setDeferred(false);
        RtxOptions::enableReplacementMaterials.setDeferred(false);
        RtxOptions::enableReplacementMeshes.setDeferred(false);
        break;
      case RtxQuickAction::kCount:
        assert(false && "invalid RtxQuickAction::kCount in ImGUI::updateQuickActions");
        break;
      }
    }
#endif
  }


  void ImGUI::showDebugVisualizations(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();
    common->getSceneManager().getLightManager().showImguiDebugVisualization();
  }

  void ImGUI::showMainMenu(const Rc<DxvkContext>& ctx) {
    // Target rtx.conf layer for developer menu changes
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(m_windowOnRight ? viewport->Size.x - m_windowWidth : 0.f, viewport->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(m_windowWidth, viewport->Size.y));

    // Remember switch state first, the switch UI when the curent window is finished.
    int switchUI = -1;
    bool advancedMenuOpen = RtxOptions::showUI() == UIType::Advanced;

    if (ImGui::Begin("RTX Remix Developer Menu", &advancedMenuOpen, windowFlags)) {
      // Begin handles window resize so this is fine. Do not set m_windowWidth after tabs so that tabs can modify the width
      m_windowWidth = ImGui::GetWindowWidth();

      if (ImGui::Button("Graphics Settings Menu", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
        switchUI = (int) UIType::Basic;
      }

      ImGui::SameLine();
      RemixGui::Checkbox("Default Menu", &RtxOptions::defaultToAdvancedUIObject());
      
      RemixGui::Separator();

      const static ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_NoCloseWithMiddleMouseButton;
      const static ImGuiTabItemFlags tab_item_flags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;

      // Tab Bar
      if (ImGui::BeginTabBar("Developer Tabs", tab_bar_flags)) {
        for (int n = 0; n < kTab_Count; n++) {
          auto tabItemFlags = tab_item_flags;
          if(n == m_triggerTab) {
            tabItemFlags |= ImGuiTabItemFlags_SetSelected;
            m_triggerTab = kTab_Count;
          }
          if (ImGui::BeginTabItem(tabNames[n], nullptr, tabItemFlags)) {
            const Tabs tab = (Tabs) n;
            switch (tab) {
            case kTab_Rendering:
              showRenderingSettings(ctx);
              break;
            case kTab_Setup:
              showSetupWindow(ctx);
              break;
            case kTab_Enhancements:
              showEnhancementsWindow(ctx);
              break;
            case kTab_About:
              m_about->show(ctx);
              break;
            case kTab_Development:
              showDevelopmentSettings(ctx);
              break;
            case kTab_Count:
              assert(false && "kTab_Count hit in ImGUI::showMainMenu");
              break;
            }
            m_curTab = tab;
            ImGui::EndTabItem();
          }
        }

        if (ImGui::TabItemButton(m_windowOnRight ? "<<" : ">>")) {
          m_windowOnRight = !m_windowOnRight;
        }

        ImGui::EndTabBar();
      }
    }

    ImGui::Dummy(ImVec2(0, 2));
    RemixGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));

    // Get layer pointers and check for unsaved changes
    RtxOptionLayer* rtxConfLayer = RtxOptionLayer::getRtxConfLayer();
    RtxOptionLayer* userLayer = const_cast<RtxOptionLayer*>(RtxOptionLayer::getUserLayer());
    const bool rtxHasUnsaved = rtxConfLayer && rtxConfLayer->hasUnsavedChanges();
    const bool userHasUnsaved = userLayer && userLayer->hasUnsavedChanges();
    
    // ============================================================================
    // Settings Management Section
    // ============================================================================
    if (RemixGui::CollapsingHeader("Settings Management", ImGuiTreeNodeFlags_DefaultOpen)) {

      // --- User Config Layer (higher priority, shown first) ---
      ImGui::Text("User Settings (user.conf):");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Your personal preferences. Saved to `user.conf`.\n"
          "This includes graphics preset choices, direct graphics settings,\n"
          "and other per-user preferences like UI configuration.");
      }
      if (userHasUnsaved) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "(unsaved changes)");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Changes have been made since the user.conf file was last saved.");
        }
      }

      // Show unsaved changes in a CollapsingHeader
      if (userHasUnsaved && userLayer) {
        if (RemixGui::CollapsingHeader("View Changes##User")) {
          ImGui::Indent();
          OptionLayerUI::RenderOptions renderOpts;
          renderOpts.uniqueId = "##UserLayerList";
          OptionLayerUI::renderToImGui(userLayer, renderOpts);
          ImGui::Unindent();
        }
      }
      
      OptionLayerUI::renderLayerButtons(userLayer, "User");

      ImGui::Separator();

      // --- Migration: Move miscategorized options from user.conf to rtx.conf ---
      // Get cached count of options in user.conf that don't have UserSetting flag
      const uint32_t userMiscategorizedCount = userLayer ? userLayer->countMiscategorizedOptions() : 0;

      if (userMiscategorizedCount > 0) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.75f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.55f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        
        std::string buttonLabel = str::format("Migrate ", userMiscategorizedCount, " Developer Setting", (userMiscategorizedCount > 1 ? "s" : ""), " to rtx.conf");
        if (ImGui::Button(buttonLabel.c_str(), ImVec2(-1, 0))) {
          const uint32_t migratedCount = userLayer->migrateMiscategorizedOptions();
          Logger::info(str::format("[RTX Option]: Migrated ", migratedCount, " developer settings from user.conf to rtx.conf"));
        }
        
        ImGui::PopStyleColor(4);

        // Tooltip needs to come after the PopStyleColor button to avoid having black text on a dark background
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("The user.conf file should only contain end user options (preferences and graphics quality settings).\n\n"
                            "This button will move all other settings from user.conf to rtx.conf.\n"
                            "It does not save the changes.");
        }
      }

      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::Spacing();

      // --- RTX Config Layer ---
      ImGui::Text("Remix Config (rtx.conf):");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The main place where Remix configuration is stored.\n"
          "Saves to rtx.conf. This should be where mod\n"
          "developers configure game-specific settings.");
      }
      if (rtxHasUnsaved) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "(unsaved changes)");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Changes have been made since the rtx.conf file was last saved.");
        }
      }

      // Show unsaved changes in a CollapsingHeader
      if (rtxHasUnsaved && rtxConfLayer) {
        if (RemixGui::CollapsingHeader("View Changes##RtxConf")) {
          ImGui::Indent();
          OptionLayerUI::RenderOptions renderOpts;
          renderOpts.uniqueId = "##RtxConfLayerList";
          OptionLayerUI::renderToImGui(rtxConfLayer, renderOpts);
          ImGui::Unindent();
        }
      }
      
      OptionLayerUI::renderLayerButtons(rtxConfLayer, "RtxConf");

      // --- Migration: Move miscategorized options from rtx.conf to user.conf ---
      const uint32_t rtxMiscategorizedCount = rtxConfLayer ? rtxConfLayer->countMiscategorizedOptions() : 0;
      if (rtxMiscategorizedCount > 0) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.75f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.55f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
        
        std::string buttonLabel = str::format("Migrate ", rtxMiscategorizedCount, " User Setting", (rtxMiscategorizedCount > 1 ? "s" : ""), " to user.conf");
        if (ImGui::Button(buttonLabel.c_str(), ImVec2(-1, 0))) {
          const uint32_t migratedCount = rtxConfLayer->migrateMiscategorizedOptions();
          Logger::info(str::format("[RTX Option]: Migrated ", migratedCount, " user settings from rtx.conf to user.conf"));
        }
        
        ImGui::PopStyleColor(4);

        // Tooltip needs to come after the PopStyleColor button to avoid having black text on a dark background
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("The rtx.conf file contains settings that should be in user.conf (end user options that should not be in mods).\n\n"
                            "This button will move these settings from rtx.conf to user.conf.\n"
                            "It does not save the changes.");
        }
      }

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      // --- Create .conf file for Logic ---
      ImGui::Text("Create .conf file for Logic:");
      ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(Only unsaved Remix Config changes will be exported)");
      static char exportFileName[512] = "exported_rtx.conf";
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100);
      ImGui::InputText("##ExportFileName", exportFileName, IM_ARRAYSIZE(exportFileName));
      ImGui::SameLine();
      ImGui::BeginDisabled(!rtxHasUnsaved);
      if (ImGui::Button("Create", ImVec2(-1, 0))) {
        if (rtxConfLayer) {
          std::string exportPath(exportFileName);
          rtxConfLayer->exportUnsavedChanges(exportPath);
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (rtxHasUnsaved) {
          ImGui::SetTooltip("Create a .conf file containing only the unsaved changes from rtx.conf.\nIf the file already exists, changes will be merged into it.");
        } else {
          ImGui::SetTooltip("No unsaved changes in rtx.conf to export.");
        }
      }
      ImGui::EndDisabled();

    }

    ImGui::Spacing();

    // --- Bottom Buttons ---
    const float buttonWidth = ImGui::GetContentRegionAvail().x / 2 - (ImGui::GetStyle().ItemSpacing.x / 2);
    const bool anyUnsavedChanges = rtxHasUnsaved || userHasUnsaved;
    ImGui::BeginDisabled(!anyUnsavedChanges);
    if (ImGui::Button("Revert All Unsaved Changes", ImVec2(buttonWidth, 0))) {
      // Reload all layers that have unsaved changes
      if (rtxConfLayer && rtxConfLayer->hasUnsavedChanges()) {
        rtxConfLayer->reload();
      }
      if (userLayer && userLayer->hasUnsavedChanges()) {
        userLayer->reload();
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (anyUnsavedChanges) {
        ImGui::SetTooltip("Reload rtx.conf and user.conf from disk,\ndiscarding all unsaved changes in both layers.");
      } else {
        ImGui::SetTooltip("No unsaved changes to revert.");
      }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Hide UI", ImVec2(buttonWidth, 0))) {
      switchUI = (int) UIType::None;
    }

    RemixGui::TextCentered("[Alt + Del] Toggle cursor        [Alt + Backspace] Toggle game input");
    ImGui::End();

    // Close via titlebar close button
    if (!advancedMenuOpen) {
      switchUI = (int) UIType::None;
    }

    if (switchUI >= 0) {
      switchMenu((UIType) switchUI);
    }
  }

  struct HudMessage {
    HudMessage(const std::string& text, const std::optional<std::string>& subText) : text{ text }, subText{ subText } {}

    std::string text;
    std::optional<std::string> subText;
  };

  void ImGUI::showHudMessages(const Rc<DxvkContext>& ctx) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    auto common = ctx->getCommonObjects();
    const auto& pipelineManager = common->pipelineManager();

    std::vector<HudMessage> hudMessages;

    // Add Shader Compilation HUD messages

    uint32_t asyncShaderCompilationCount = 0;
    if (RtxOptions::Shader::enableAsyncCompilation()) {
      asyncShaderCompilationCount = pipelineManager.remixShaderCompilationCount();
    }

    if (RtxOptions::Shader::enableAsyncCompilationUI() && asyncShaderCompilationCount > 0) {
      const auto compilationText = str::format("Compiling shaders (", asyncShaderCompilationCount, " remaining)");

      hudMessages.emplace_back(std::move(compilationText), "This may take some time if shaders are not cached yet.\nRemix will not render properly until compilation is finished.");
    }

    // Add Enhancement Loading HUD messages

    const auto replacementStates = common->getSceneManager().getReplacementStates();
    std::string replacementLoadingSubtext;
    std::uint32_t loadingReplacementStateCount{ 0U };

    for (std::size_t i{ 0U }; i < replacementStates.size(); ++i) {
      auto&& replacementState = replacementStates[i];

      // Add a newline when reporting on more than one mod in a loading state

      if (loadingReplacementStateCount != 0) {
        replacementLoadingSubtext += '\n';
      }

      // Hide individual mod progress messages beyond a requested amount
      // Note: This ensures if for some reason there are a significant amount of mods in place that the screen will not be filled with progress hud messages.

      constexpr std::size_t maxModProgressCount{ 4 };

      if (loadingReplacementStateCount >= maxModProgressCount) {
        replacementLoadingSubtext += str::format(replacementStates.size() - maxModProgressCount, " more hidden...");

        break;
      }

      // Set the progress message if the mod is in a loading state and increment the number of currently loading mods

      switch (replacementState.progressState) {
      case Mod::ProgressState::OpeningUSD: replacementLoadingSubtext += str::format("Opening USD"); break;
      case Mod::ProgressState::ProcessingMaterials: replacementLoadingSubtext += str::format("Processing Materials (", replacementState.progressCount, " processed)"); break;
      case Mod::ProgressState::ProcessingMeshes: replacementLoadingSubtext += str::format("Processing Meshes (", replacementState.progressCount, " processed)"); break;
      case Mod::ProgressState::ProcessingLights: replacementLoadingSubtext += str::format("Processing Lights (", replacementState.progressCount, " processed)"); break;
      default: break;
      }

      if (
        replacementState.progressState == Mod::ProgressState::OpeningUSD ||
        replacementState.progressState == Mod::ProgressState::ProcessingMaterials ||
        replacementState.progressState == Mod::ProgressState::ProcessingMeshes ||
        replacementState.progressState == Mod::ProgressState::ProcessingLights
      ) {
        ++loadingReplacementStateCount;
      }
    }

    assert((loadingReplacementStateCount == 0U) == replacementLoadingSubtext.empty());

    if (loadingReplacementStateCount != 0U) {
      hudMessages.emplace_back("Loading enhancements", replacementLoadingSubtext);
    }

    // Draw Hud Messages

    if (!hudMessages.empty()) {
      // Reset Hud Message time if needed
      // Note: This is done to minimize any potential precision issues if the game is left running for a long time.
      // Not the best solution ever, ideally just accumulating time with delta time passed in would probably be better
      // rather than querying the OS for timestamps, but getting delta time in the ImGui system is rather annoying, so
      // this is fine for now as this code isn't performance-critical anyways.

      const auto currentTime = std::chrono::steady_clock::now();

      if (!m_hudMessageTimeReset) {
        m_hudMessageStartTime = currentTime;
        m_hudMessageTimeReset = true;
      }

      // Calculate the length of the animated dot sequence based on the current time

      const auto hudMessageDisplayDuration{ currentTime - m_hudMessageStartTime };
      const auto hudMessageDisplayMilliseconds{
        std::chrono::duration_cast<std::chrono::milliseconds>(hudMessageDisplayDuration).count()
      };
      // Note: Generates a looping set of values in the range [1, 3] based on the time and the duration of each dot.
      const auto dotSequenceLength{ (hudMessageDisplayMilliseconds / hudMessageAnimatedDotDurationMilliseconds()) % 3 + 1 };

      ImGui::SetNextWindowPos(ImVec2(0, viewport->Size.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
      // Note: 368 pixels chosen as a minimum width for the message box width to ensure the current length of text has enough space
      // to render an animated dot sequence without causing the width of the window to change, as this is visually distracting.
      // If longer message box text fields are ever desired than the current ones, this number will have to be updated.
      // Hack: Currently ImGui does not properly respect the window size constraints when ImGuiWindowFlags_AlwaysAutoResize is set.
      // This call should be using the size constraints (368, -1), (-1, -1) as -1 indicates "don't care" (and we only care about setting
      // a minimum width), but for some reason that does not work as reported by this bug: https://github.com/ocornut/imgui/issues/2629
      ImGui::SetNextWindowSizeConstraints(ImVec2(368.0f, 0.0f), ImVec2(1000.0f, 1000.0f));
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.5f, 0.2f, 0.2f, 0.35f));

      const ImGuiWindowFlags hud_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
      if (ImGui::Begin("HUD", nullptr, hud_flags)) {
        for (std::size_t i{ 0U }; i < hudMessages.size(); ++i) {
          auto&& message{ hudMessages[i] };

          // Append the animated dot sequence to the message main text

          constexpr auto dotSequence{ "..." };
          std::string animatedMessageText{ message.text };

          animatedMessageText.append(dotSequence, dotSequenceLength);

          // Add a large main text and smaller sub text for each message

          ImGui::PushFont(m_largeFont);
          ImGui::Text(animatedMessageText.c_str());
          ImGui::PopFont();

          if (message.subText) {
            ImGui::Text(message.subText->c_str());
          }

          // Add a seperator between messages

          if (i != hudMessages.size() - 1) {
            RemixGui::Separator();
          }
        }
      }

      ImGui::PopStyleColor();
      ImGui::End();
    } else {
      // Note: Indicate that the Hud Message time will need to be reset the next time it is used.
      m_hudMessageTimeReset = false;
    }
  }

  void ImGUI::showDevelopmentSettings(const Rc<DxvkContext>& ctx) {
    ImGui::PushItemWidth((largeUiMode() ? m_largeWindowWidgetWidth : m_regularWindowWidgetWidth) + 50.0f);
    if (ImGui::Button("Take Screenshot")) {
      RtxContext::triggerScreenshot();
    }

    RemixGui::SetTooltipToLastWidgetOnHover("Screenshot will be dumped to, '<exe-dir>/Screenshots'");

    ImGui::SameLine(200.f);
    RemixGui::Checkbox("Include G-Buffer", &RtxOptions::captureDebugImageObject());

    RemixGui::Separator();
        
#ifdef REMIX_DEVELOPMENT
    { // Recompile Shaders button and its status information (Only available for Development Remix builds)
      const auto& shaderManager{ ShaderManager::getInstance() };
      const auto shaderReloadPhase{ shaderManager->getShaderReloadPhase() };
      const auto lastShaderReloadStatus{ shaderManager->getLastShaderReloadStatus() };

      // Note: Only allow the Recompile Shaders button to function if a shader recompile is not currently in progress (be
      // it one manually initiated by the user, or something automatic from the live shader edit mode).
      ImGui::BeginDisabled(shaderReloadPhase != ShaderManager::ShaderReloadPhase::Idle);

      if (ImGui::Button("Recompile Shaders")) {
        shaderManager->requestReloadShaders();
      }

      ImGui::EndDisabled();

      ImGui::SameLine(200.f);
      RemixGui::Checkbox("Live shader edit mode", &RtxOptions::Shader::useLiveEditModeObject());

      const char* shaderReloadPhaseText;
      const char* lastShaderReloadStatusText;
      ImVec4 shaderReloadPhaseTextColor;
      ImVec4 lastShaderReloadStatusTextColor;

      switch (shaderReloadPhase) {
      default: assert(false); [[fallthrough]];
      case ShaderManager::ShaderReloadPhase::Idle:
        shaderReloadPhaseText = "Idle";
        shaderReloadPhaseTextColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        break;
      case ShaderManager::ShaderReloadPhase::SPIRVRecompilation:
        shaderReloadPhaseText = "Working (SPIR-V Recompilation)";
        shaderReloadPhaseTextColor = ImVec4(0.73f, 0.87f, 0.54f, 1.0f);
        break;
      case ShaderManager::ShaderReloadPhase::ShaderRecreation:
        shaderReloadPhaseText = "Working (Shader Recreation)";
        shaderReloadPhaseTextColor = ImVec4(0.73f, 0.87f, 0.54f, 1.0f);
        break;
      }

      switch (lastShaderReloadStatus) {
      default: assert(false); [[fallthrough]];
      case ShaderManager::ShaderReloadStatus::Unknown:
        lastShaderReloadStatusText = "N/A";
        lastShaderReloadStatusTextColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        break;
      case ShaderManager::ShaderReloadStatus::Failure:
        lastShaderReloadStatusText = "Failure";
        lastShaderReloadStatusTextColor = ImVec4(0.83f, 0.32f, 0.32f, 1.0f);
        break;
      case ShaderManager::ShaderReloadStatus::Success:
        lastShaderReloadStatusText = "Success";
        lastShaderReloadStatusTextColor = ImVec4(0.44f, 0.81f, 0.42f, 1.0f);
        break;
      }

      ImGui::TextUnformatted("Shader Reload Phase:");
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, shaderReloadPhaseTextColor);
      ImGui::TextUnformatted(shaderReloadPhaseText);
      ImGui::PopStyleColor();

      ImGui::TextUnformatted("Last Shader Reload Status:");
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, lastShaderReloadStatusTextColor);
      ImGui::TextUnformatted(lastShaderReloadStatusText);
      ImGui::PopStyleColor();
    }

    ImGui::Separator();

    { // Crash Hotkey Feature - allows triggering a deliberate crash for testing crash handling
      const bool isArmed = RtxOptions::enableCrashHotkey();
      
      // Use warning color when armed to make it visually distinct
      if (isArmed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
      }
      
      // ImGui::Checkbox returns true when the checkbox state changes
      const bool changed = RemixGui::Checkbox("Arm Crash Hotkey", &RtxOptions::enableCrashHotkeyObject());
      
      if (isArmed) {
        ImGui::PopStyleColor();
      }
      
      const auto crashHotkeyStr = buildKeyBindDescriptorString(RtxOptions::crashHotkey());
      RemixGui::SetTooltipToLastWidgetOnHover(
        str::format("When armed, pressing ", crashHotkeyStr, " will trigger a deliberate crash.\n"
        "Useful for testing crash handling, crash dumps, and crash reporting.\n"
        "A red warning indicator will appear on screen while armed.").c_str());
      
      // Log state changes for crash dump analysis
      if (changed) {
        const bool nowArmed = RtxOptions::enableCrashHotkey();
        if (nowArmed) {
          Logger::warn(str::format("Crash hotkey ARMED - press ", crashHotkeyStr, " to trigger crash"));
        } else {
          Logger::warn("Crash hotkey disarmed");
        }
      }
    }
#endif

    RemixGui::Separator();

    showVsyncOptions(false);

    // Render GUI for memory profiler here
    GpuMemoryTracker::renderGui();

    if (RemixGui::CollapsingHeader("Camera", collapsingHeaderFlags)) {
      ImGui::Indent();

      RtCamera::showImguiSettings();

      {
        ImGui::PushID("CameraInfos");
        auto& cameraManager = ctx->getCommonObjects()->getSceneManager().getCameraManager();
        if (RemixGui::CollapsingHeader("Types", collapsingHeaderClosedFlags)) {
          ImGui::Indent();
          constexpr static std::pair<CameraType::Enum, const char*> cameras[] = {
            { CameraType::Main,             "Main" },
            { CameraType::ViewModel,        "ViewModel" },
            { CameraType::Portal0,          "Portal0" },
            { CameraType::Portal1,          "Portal1" },
            { CameraType::Sky,              "Sky" },
            { CameraType::RenderToTexture,  "RenderToTexture" },
          };
          // C++20: should be static_assert with std::ranges::find_if
          assert(
            std::find_if(
              std::begin(cameras),
              std::end(cameras),
              [](const auto& p) { return p.first == CameraType::Unknown; })
            == std::end(cameras));
          static_assert(std::size(cameras) == CameraType::Count - 1);

          static auto printCamera = [](const char* name, const RtCamera* c) {
            if (RemixGui::CollapsingHeader(name, collapsingHeaderFlags)) {
              ImGui::Indent();
              if (c) {
                ImGui::Text("Position: %.2f %.2f %.2f", c->getPosition().x, c->getPosition().y, c->getPosition().z);
                ImGui::Text("Direction: %.2f %.2f %.2f", c->getDirection().x, c->getDirection().y, c->getDirection().z);
                ImGui::Text("Vertical FOV: %.1f", c->getFov() * kRadiansToDegrees);
                ImGui::Text("Near / Far plane: %.1f / %.1f", c->getNearPlane(), c->getFarPlane());
                ImGui::Text("Projection Handedness: %s", c->isLHS() ? "Left-handed" : "Right-handed");
                ImGui::Text("Overall Handedness: %s", c->isLHS() ^ isMirrorTransform(c->getViewToWorld(false))   ? "Left-handed" : "Right-handed");
              } else {
                ImGui::Text("Position: -");
                ImGui::Text("Direction: -");
                ImGui::Text("Vertical FOV: -");
                ImGui::Text("Near / Far plane: -");
                ImGui::Text("-");
                ImGui::Text("-");
              }
              ImGui::Unindent();
            }
          };

          for (const auto& [type, name] : cameras) {
            printCamera(name, cameraManager.isCameraValid(type) ? &cameraManager.getCamera(type) : nullptr);
          }
          ImGui::Unindent();
        }
        ImGui::PopID();
      }

      if (RemixGui::CollapsingHeader("Camera Animation", collapsingHeaderClosedFlags)) {
        RemixGui::Checkbox("Animate Camera", &RtxOptions::shakeCameraObject());
        cameraAnimationModeCombo.getKey(&RtxOptions::cameraAnimationModeObject());
        RemixGui::DragFloat("Animation Amplitude", &RtxOptions::cameraAnimationAmplitudeObject(), 0.1f, 0.f, 1000.f, "%.2f", sliderFlags);
        RemixGui::DragInt("Shake Period", &RtxOptions::cameraShakePeriodObject(), 0.1f, 1, 100, "%d", sliderFlags);
      }

      if (RemixGui::CollapsingHeader("Advanced", collapsingHeaderClosedFlags)) {

        RemixGui::Checkbox("Portals: Camera History Correction", &RtxOptions::rayPortalCameraHistoryCorrectionObject());
        RemixGui::Checkbox("Portals: Camera In-Between Portals Correction", &RtxOptions::rayPortalCameraInBetweenPortalsCorrectionObject());

        if (RtxOptions::rayPortalCameraInBetweenPortalsCorrection()) {
          ImGui::Indent();

          RemixGui::DragFloat("Portals: Camera In-Between Portals Correction Threshold", &RtxOptions::rayPortalCameraInBetweenPortalsCorrectionThresholdObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", sliderFlags);

          ImGui::Unindent();
        }

        RemixGui::Checkbox("Skip Objects Rendered with Unknown Camera", &RtxOptions::skipObjectsWithUnknownCameraObject());

        RemixGui::Checkbox("Override Near Plane (if less than original)", &RtxOptions::enableNearPlaneOverrideObject());
        ImGui::BeginDisabled(!RtxOptions::enableNearPlaneOverride());
        RemixGui::DragFloat("Desired Near Plane Distance", &RtxOptions::nearPlaneOverrideObject(), 0.01f, 0.0001f, FLT_MAX, "%.3f");
        ImGui::EndDisabled();
      }
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Camera Sequence", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      RtCameraSequence::getInstance()->showImguiSettings();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Developer Options", collapsingHeaderFlags)) {
      ImGui::Indent();
      RemixGui::Checkbox("Enable Preserve Path", &RtxOptions::enablePreservePathObject());
      RemixGui::Checkbox("Enable Instance Debugging", &RtxOptions::enableInstanceDebuggingToolsObject());
      RemixGui::Checkbox("Disable Draw Calls Post RTX Injection", &RtxOptions::skipDrawCallsPostRTXInjectionObject());
      RemixGui::Checkbox("Break into Debugger On Press of Key 'B'", &RtxOptions::enableBreakIntoDebuggerOnPressingBObject());
      RemixGui::Checkbox("Block Input to Game in UI", &RtxOptions::blockInputToGameInUIObject());
      RemixGui::Checkbox("Force Camera Jitter", &RtxOptions::forceCameraJitterObject());
      RemixGui::DragInt("Camera Jitter Sequence Length", &RtxOptions::cameraJitterSequenceLengthObject());
      
      RemixGui::DragIntRange2("Draw Call Range Filter", &RtxOptions::drawCallRangeObject(), 1.f, 0, INT32_MAX, nullptr, nullptr, ImGuiSliderFlags_AlwaysClamp);
      RemixGui::InputInt("Instance Index Start", &RtxOptions::instanceOverrideInstanceIdxObject());
      RemixGui::InputInt("Instance Index Range", &RtxOptions::instanceOverrideInstanceIdxRangeObject());
      RemixGui::DragFloat3("Instance World Offset", &RtxOptions::instanceOverrideWorldOffsetObject(), 0.1f, -100.f, 100.f, "%.3f", sliderFlags);
      RemixGui::Checkbox("Instance - Print Hash", &RtxOptions::instanceOverrideSelectedInstancePrintMaterialHashObject());

      ImGui::Unindent();
      RemixGui::Checkbox("Throttle presents", &RtxOptions::enablePresentThrottleObject());
      if (RtxOptions::enablePresentThrottle()) {
        ImGui::Indent();
        RemixGui::SliderInt("Present delay", &RtxOptions::presentThrottleDelayObject(), 1, 1000, "%d ms", sliderFlags);
        ImGui::Unindent();
      }
      RemixGui::Checkbox("Hash Collision Detection", &HashCollisionDetectionOptions::enableObject());
      RemixGui::Checkbox("Validate CPU index data", &RtxOptions::validateCPUIndexDataObject());

#ifdef REMIX_DEVELOPMENT
      if (RemixGui::CollapsingHeader("Resource Aliasing Query", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        aliasingBeginPassCombo.getKey(&RtxOptions::Aliasing::beginPassObject());
        aliasingEndPassCombo.getKey(&RtxOptions::Aliasing::endPassObject());
        aliasingFormatCombo.getKey(&RtxOptions::Aliasing::formatCategoryObject());
        aliasingExtentCombo.getKey(&RtxOptions::Aliasing::extentTypeObject());
        const auto aliasingExtentType = RtxOptions::Aliasing::extentType();
        if (aliasingExtentType == RtxTextureExtentType::Custom) {
          RemixGui::DragInt("Aliasing Width", &RtxOptions::Aliasing::widthObject());
          RemixGui::DragInt("Aliasing Height", &RtxOptions::Aliasing::heightObject());
        }
        if (RtxOptions::Aliasing::imageType() == VkImageType::VK_IMAGE_TYPE_3D)
        {
          RemixGui::DragInt("Aliasing Depth", &RtxOptions::Aliasing::depthObject());
        }
        RemixGui::DragInt("Aliasing Layer", &RtxOptions::Aliasing::layerObject());
        aliasingImageTypeCombo.getKey(&RtxOptions::Aliasing::imageTypeObject());
        aliasingImageViewTypeCombo.getKey(&RtxOptions::Aliasing::imageViewTypeObject());

        if (IMGUI_ADD_TOOLTIP(ImGui::Button("Check aliasing for a new resource"),
          "Make sure to check the resources can be aliased under all major settings. For example, DLSS-RR or NRD, NRC or ReSTIR-GI.")) {
          Resources::s_queryAliasing = true;
        } else {
          Resources::s_queryAliasing = false;
        }
        std::string resourceAliasingQueryText = "Resource Aliasing Query Result: (";
        if (RtxOptions::enableRayReconstruction()) {
          resourceAliasingQueryText += "DLSS-RR, ";
        } else {
          resourceAliasingQueryText += "NRD, ";
        }
        if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache) {
          resourceAliasingQueryText += "NRC)";
        } else if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::ReSTIRGI) {
          resourceAliasingQueryText += "ReSTIR-GI)";
        } else {
          resourceAliasingQueryText += "ImportanceSampled)";
        }

        ImGui::Text(resourceAliasingQueryText.c_str());
        ImGui::Text("%s", Resources::s_resourceAliasingQueryText.c_str());

        if (IMGUI_ADD_TOOLTIP(ImGui::Button("Check aliasing for current resources"), "Make sure the resources are being active when checking for aliasing.")) {
          Resources::s_startAliasingAnalyzer = true;
        } else {
          Resources::s_startAliasingAnalyzer = false;
        }
        auto& str = Resources::s_aliasingAnalyzerResultText;
        ImGui::Text("Available Aliasing:\n%s", Resources::s_aliasingAnalyzerResultText.c_str());
        ImGui::Unindent();
      }
#endif
    }

    if (IMGUI_ADD_TOOLTIP(RemixGui::CollapsingHeader("Option Layers"), "View what options are present in each layer, and alter the blend strength and threshold for them.")) {
      ImGui::Indent();
      static char optionLayerFilter[256] = "";
      // Filter for option layer contents
      IMGUI_ADD_TOOLTIP(ImGui::InputText("RtxOption Display Filter", optionLayerFilter, IM_ARRAYSIZE(optionLayerFilter)), 
          "Filter options displayed in the Contents sections. Only options containing this text will be shown.");

          RemixGui::Checkbox("Pause Graph Execution", &GraphManager::pauseGraphUpdatesObject());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "Many graphs set `enable`, `blendStrength`, and `blendThreshold` every frame.\n"
          "Pausing the graph execution will allow controlling these values without interference.");
      }

        // Pre-compute lowercased filter once for efficiency
      std::string filterLower = optionLayerFilter;
      std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

      uint32_t optionLayerCounter = 1;
      for (auto& [layerKey, optionLayerPtr] : RtxOptionManager::getLayerRegistry()) {
        RtxOptionLayer& optionLayer = *optionLayerPtr;

        const bool isDefaultLayer = layerKey == kRtxOptionLayerDefaultKey;
        const bool isUserLayer = layerKey == kRtxOptionLayerUserKey;
        const bool isQualityLayer = layerKey == kRtxOptionLayerQualityKey;
        const bool isDxvkLayer = layerKey == kRtxOptionLayerDxvkConfKey;
        const bool isRtxConfLayer = layerKey == kRtxOptionLayerRtxConfKey;
        const bool hasSaveableConfig = optionLayer.hasSaveableConfigFile() && !isDxvkLayer;
        const bool hasUnsaved = hasSaveableConfig && optionLayer.hasUnsavedChanges();

        // Skip layers with no values (empty layers), unless they have a saveable config
        // Dynamic layers with saveable configs should still be shown even when empty/disabled
        if (!optionLayer.hasValues() && !hasSaveableConfig) {
          continue;
        }
        
        // Determine display name - system layers have proper names,
        // dynamically loaded layers have file paths as names which need shortening
        std::string displayName = optionLayer.getName();
        
        // Add config file indicator for layers with associated config files
        if (isRtxConfLayer) {
          displayName += " (rtx.conf)";
        } else if (isUserLayer) {
          displayName += " (user.conf)";
        } else if (isDxvkLayer) {
          displayName += " (dxvk.conf)";
        }
        
        // For non-system layers, shorten long file paths for display
        if (displayName.length() > 40) {
          // Try to extract just the file name from the path
          const std::string modsMarker = (std::filesystem::path("rtx-remix") / "mods" / "").string();
          size_t modsPos = displayName.find(modsMarker);
          if (modsPos != std::string::npos) {
            displayName = displayName.substr(modsPos + modsMarker.length());
          } else {
            // Just take the last portion of the path
            size_t lastSep = displayName.find_last_of("/\\");
            if (lastSep != std::string::npos) {
              displayName = displayName.substr(lastSep + 1);
            }
          }
        }
        
        // Build header text and styling
        std::string unsavedIndicator = hasUnsaved ? " *" : "";
        const std::string optionLayerText = std::to_string(optionLayerCounter++) + ". " + displayName + unsavedIndicator + "###" + displayName;
        
        // Determine if layer is active (only applies to layers with blend controls)
        bool pendingEnabled = optionLayer.getPendingEnabled();
        float pendingStrength = optionLayer.getPendingBlendStrength();
        float pendingThreshold = optionLayer.getPendingBlendThreshold();
        const bool isLayerActive = !hasSaveableConfig || (pendingEnabled && pendingStrength > pendingThreshold);
        
        // Apply header styling
        bool pushedStyle = false;
        if (!isLayerActive) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          pushedStyle = true;
        } else if (hasUnsaved) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
          pushedStyle = true;
        }
        
        // Build tooltip
        std::string tooltipText;
        if (isQualityLayer) {
          tooltipText = "Quality preset settings (highest priority). Empty when Graphics Preset is Custom.";
        } else if (isUserLayer) {
          tooltipText = "A User's local graphics settings.  Overrides all other layers except Quality Presets.";
        } else if (isDefaultLayer) {
          tooltipText = "Default values for each option, as defined in RtxOptions.md.";
        } else {
          tooltipText = optionLayer.getName();
        }
        if (!optionLayer.getFilePath().empty() && optionLayer.getFilePath() != optionLayer.getName()) {
          tooltipText += "\nFile: " + optionLayer.getFilePath();
        }
        if (hasUnsaved) {
          tooltipText += "\n[Has unsaved changes]";
        }
        
        bool headerOpen = IMGUI_ADD_TOOLTIP(RemixGui::CollapsingHeader(optionLayerText.c_str(), collapsingHeaderClosedFlags), tooltipText.c_str());
        
        if (pushedStyle) {
          ImGui::PopStyleColor();
        }
        
        if (headerOpen) {
          ImGui::Indent();
          
          // Priority display
          if (isQualityLayer) {
            ImGui::Text("Priority: MAX");
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Highest possible priority - quality preset settings control these options when preset is not Custom.\nThis layer is empty when Graphics Preset is set to Custom.");
            }
          } else if (isUserLayer) {
            ImGui::Text("Priority: MAX - 1");
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Second highest priority - user settings that override all layers except Quality Presets.\nWhen Graphics Preset is Custom, this becomes the effective highest priority layer.");
            }
          } else if (isDefaultLayer) {
            ImGui::Text("Priority: 0");
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("Lowest possible priority - every other layer will be applied on top of this layer.");
            }
          } else {
            ImGui::Text("Priority: %u", optionLayer.getLayerKey().priority);
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip(
                "Layers are applied starting with the lowest priority layer, ending with the highest.\n"
                "Each layer overrides the values written before it.\n"
                "If a layer's blendWeight is not 1 and the option is a float or Vector type,\n"
                "then the values will be calculated as LERP(previousValue, layerValue, blendWeight).");
            }
          }
          
          // Enable/blend controls only for saveable config layers (Remix Config, User, dynamically loaded mods)
          if (hasSaveableConfig && !isUserLayer) {
            const std::string optionLayerEnabledText = "Enabled###Enabled_" + displayName;
            const std::string optionLayerStrengthText = " Strength###Strength_" + displayName;
            const std::string optionLayerThresholdText = " Threshold###Threshold_" + displayName;
            
            if (IMGUI_ADD_TOOLTIP(ImGui::Checkbox(optionLayerEnabledText.c_str(), &pendingEnabled), "Check to enable the option layer. Uncheck to disable it.")) {
              optionLayer.requestEnabled(pendingEnabled);
            }

            if (IMGUI_ADD_TOOLTIP(ImGui::SliderFloat(optionLayerStrengthText.c_str(), &pendingStrength, 0.0f, 1.0f),
                                  "Adjusts the blending strength of this option layer (0 = off, 1 = full effect).")) {
              optionLayer.requestBlendStrength(pendingStrength);
            }

            if (IMGUI_ADD_TOOLTIP(ImGui::SliderFloat(optionLayerThresholdText.c_str(), &pendingThreshold, 0.0f, 1.0f),
                                  "Sets the blending strength threshold for this option layer.")) {
              optionLayer.requestBlendThreshold(pendingThreshold);
            }
          }
          
          // Action buttons only for saveable config layers
          if (hasSaveableConfig) {
            OptionLayerUI::renderLayerButtons(optionLayerPtr.get(), displayName.c_str());
          }
          
          // Contents section
          const std::string optionLayerContentsText = "Contents###Contents_" + displayName;
          if (RemixGui::CollapsingHeader(optionLayerContentsText.c_str(), collapsingHeaderClosedFlags)) {
            ImGui::Indent();
            OptionLayerUI::displayContents(optionLayer, filterLower);
            ImGui::Unindent();
          }
          
          ImGui::Unindent();
        }
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("UI Options")) {
      ImGui::Indent();

      if (m_pendingUIOptionsScroll) {
        ImGui::SetScrollHereY(0.0f);
        m_pendingUIOptionsScroll = false;
      }

      {
        if (RemixGui::Checkbox("Compact UI", &compactGuiObject())) {
          // Scroll to UI Options on the next frame
          m_pendingUIOptionsScroll = true;
        }
      }

      RemixGui::Checkbox("Always Developer Menu", &RtxOptions::defaultToAdvancedUIObject());

      if (RemixGui::SliderFloat("Background Alpha", &backgroundAlphaObject(), 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
        adjustStyleBackgroundAlpha(backgroundAlpha());
      }

      
      if (RemixGui::Checkbox("Use Large UI", &largeUiModeObject())) {
        m_pendingUIOptionsScroll = true;
      }

      {
        constexpr float indent = 60.0f;
        ImGui::PushID("gui theme");
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::Text("GUI Theme:");
        ImGui::PushItemWidth(ImGui::GetContentRegionMax().x - indent);

        if (themeCombo.getKey(&themeGuiObject())) {
          m_pendingUIOptionsScroll = true;
        }

        ImGui::PopItemWidth();
        ImGui::PopID();
      }

      ImGui::Unindent();
    }

    ImGui::PopItemWidth();
  }

  namespace {
    Vector2i tovec2i(const ImVec2& v) {
      return Vector2i { static_cast<int>(v.x), static_cast<int>(v.y) };
    };

    bool isWorldTextureSelectionAllowed() {
      // mouse cursor is not obstructed by any imgui window
      return !ImGui::GetIO().WantCaptureMouse;
    }

    bool isMaterialReplacement(SceneManager& sceneManager, XXH64_hash_t texHash) {
      return sceneManager.getAssetReplacer()->getReplacementMaterial(texHash) != nullptr;
    }

    std::string makeTextureInfo(XXH64_hash_t texHash, bool isMaterialReplacement, bool includeLayerInfo = true) {
      auto iter = g_imguiTextureMap.find(texHash);
      if (iter == g_imguiTextureMap.end()) {
        return {};
      }
      const auto& imageInfo = iter->second.imageView->imageInfo();

      const auto isRT = (imageInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

      const auto vkFormatName = (std::stringstream{} << imageInfo.format).str();
      const auto formatName = std::string_view { vkFormatName }.substr(std::string_view{"VK_FORMAT_"}.length());

      auto str = std::ostringstream {};
      str << (isMaterialReplacement ? "Replaced material" : "Legacy material") << '\n';
      str << (isRT ? "Render Target " : "Texture ") << imageInfo.extent.width << 'x' << imageInfo.extent.height << '\n';
      str << formatName << '\n';
      str << "Hash: " << hashToString(texHash) << '\n';
      
      if (!includeLayerInfo) {
        return str.str();
      }
      
      // For each category, show which layers add/remove this hash
      for (const auto& category : rtxTextureOptions) {
        if (!category.textureSetOption) {
          continue;
        }
        
        std::string layerValues = RemixGui::FormatOptionLayerValues(category.textureSetOption, texHash, true);
        if (!layerValues.empty()) {
          str << '\n' << category.displayName << ":\n" << layerValues;
        }
      }
      
      return str.str();
    }

    std::string buildTextureCategoryTooltip(const RtxTextureOption& category,
                                            std::optional<XXH64_hash_t> texHash = std::nullopt) {
      if (!category.textureSetOption) {
        assert(false && "Texture category is missing an RTX option.");
        return category.displayName;
      }

      std::string tooltipText;
      const char* description = category.textureSetOption->getDescription();
      if (description && description[0] != '\0') {
        tooltipText = description;
        tooltipText += "\n\n";
      }

      tooltipText += category.textureSetOption->getFullName();

      if (texHash.has_value()) {
        std::string layerValues = RemixGui::FormatOptionLayerValues(category.textureSetOption, texHash, false);
        if (!layerValues.empty()) {
          tooltipText += "\n\nPer-layer status for this hash:\n";
          tooltipText += layerValues;
        }
      }

      return tooltipText;
    }

    float computeTexturePopupLabelColumnWidth(uint32_t textureFeatureFlags) {
      float maxWidth = 0.0f;
      for (const auto& rtxOption : rtxTextureOptions) {
        if ((rtxOption.featureFlagMask & textureFeatureFlags) != rtxOption.featureFlagMask) {
          continue;
        }
        const std::string labelForWidth = std::string(rtxOption.displayName) + " [!]";
        maxWidth = ImMax(maxWidth, ImGui::CalcTextSize(labelForWidth.c_str()).x);
      }
      return maxWidth + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    void toggleTextureSelection(XXH64_hash_t textureHash, const char* uniqueId, RtxOption<fast_unordered_set>* textureSet) {
      if (textureHash == kEmptyHash) {
        return;
      }
      
      // Determine if user wants to add (currently unchecked) or remove (currently checked)
      const bool userWantsRemove = textureSet->containsHash(textureHash);

      // Analyze the layer state in a single pass
      const RtxOptionLayer* targetLayer = textureSet->getTargetLayer();
      const auto& targetKey = targetLayer->getLayerKey();
      
      // Track target layer's opinion and strongest weaker layer's opinion
      bool targetHasPositive = false;
      bool targetHasNegative = false;
      bool weakerLayerAddsHash = false;  // True if strongest weaker layer adds this hash
      
      textureSet->forEachLayerValue([&](const RtxOptionLayer* layer, const GenericValue& value) {
        const HashSetLayer* hashSet = value.hashSet;
        const auto& layerKey = layer->getLayerKey();
        
        if (layerKey == targetKey) {
          targetHasPositive = hashSet->hasPositive(textureHash);
          targetHasNegative = hashSet->hasNegative(textureHash);
        } else if (targetKey < layerKey) {
          // First weaker layer with an opinion - determines what happens without target layer
          weakerLayerAddsHash = hashSet->hasPositive(textureHash);
          return false; // Stop iteration
        }
        return true; // Continue
      }, textureHash);

      // Lambda to apply the user's intended action to the target layer
      auto applyAction = [textureSet, textureHash, targetLayer, userWantsRemove, weakerLayerAddsHash, uniqueId,
                          targetHasPositive, targetHasNegative]() {
        const char* action;
        if (userWantsRemove) {
          // User wants to remove this hash from the resolved set
          if (!targetHasNegative) {
            // Either the target has a positive opinion, or no opinion at all
            // In both cases, create a negative opinion to express "I don't want this"
            textureSet->removeHash(textureHash, targetLayer);
            action = "removed (negative opinion)";
          } else {
            // Already has a negative opinion - nothing to do
            action = "already removed";
          }
        } else {
          // User wants to add this hash to the resolved set
          if (!targetHasPositive) {
            // Either the target has a negative opinion, or no opinion at all
            // In both cases, create a positive opinion to express "I want this"
            textureSet->addHash(textureHash, targetLayer);
            action = "added (positive opinion)";
          } else {
            // Already has a positive opinion - nothing to do
            action = "already added";
          }
        }

        char buffer[256];
        sprintf_s(buffer, "%s - %s %016llX\n", uniqueId, action, textureHash);
        Logger::info(buffer);
      };

      // Check for blocking layers using the standardized popup system.
      // If blocked, popup is shown and applyAction will be called after user clears blockers.
      // If not blocked, apply directly.
      if (!RemixGui::CheckRtxOptionPopups(textureSet, textureHash, applyAction)) {
        applyAction();
      }
    }

    RtxOption<fast_unordered_set>* findTextureSetByUniqueId(const char* uniqueId) {
      if (uniqueId) {
        for (RtxTextureOption& category : rtxTextureOptions) {
          if (strcmp(category.uniqueId, uniqueId) == 0) {
            return category.textureSetOption;
          }
        }
      }
      return nullptr;
    }

    namespace texture_popup {
      constexpr char POPUP_NAME[] = "rtx_texture_selection_popup";

      bool lastOpenCategoryActive { false };
      std::string lastOpenCategoryId {};

      bool g_wasLeftClick { false };

      // need to keep a reference to a texture that was passed to 'open()',
      // as 'open()' is called only once, but popup needs to reference that texture throughout open-close
      std::atomic<XXH64_hash_t> g_holdingTexture {};
      bool g_openWhenAvailable {};

      void openImguiPopupOrToggle() {
        // don't show popup window and toggle the list directly,
        // if was a left mouse click in the splitted lists
        bool toggleWithoutPopup = ImGUI::showLegacyTextureGui() &&
                                  g_wasLeftClick &&
                                  !lastOpenCategoryId.empty();
        g_wasLeftClick = false;

        if (toggleWithoutPopup) {
          if (auto textureSet = findTextureSetByUniqueId(lastOpenCategoryId.c_str())) {
            toggleTextureSelection(g_holdingTexture.load(),
                                   lastOpenCategoryId.c_str(),
                                   textureSet);
          }
        } else {
          ImGui::OpenPopup(POPUP_NAME);
        }
      }

      void open(std::optional<XXH64_hash_t> texHash) {
        g_holdingTexture.exchange(texHash.value_or(kEmptyHash));
        g_openWhenAvailable = false;
        // no need to wait, open immediately
        openImguiPopupOrToggle();
      }

      void openAsync() {
        g_holdingTexture.exchange(kEmptyHash);
        g_openWhenAvailable = true;
      }

      bool isOpened() {
        return ImGui::IsPopupOpen(POPUP_NAME);
      }

      // Returns a texture hash that it holds, if the popup is opened.
      // Must be called every frame.
      std::optional<XXH64_hash_t> produce(SceneManager& sceneMgr) {
        // delayed open, if waiting async to set g_holdingTexture
        if (g_openWhenAvailable) {
          if (g_holdingTexture.load() != kEmptyHash) {
            openImguiPopupOrToggle();
            g_openWhenAvailable = false;
          }
        }

        const XXH64_hash_t texHashForSizing = g_holdingTexture.load();
        float texturePopupLabelColumnW = 0.0f;
        if (texHashForSizing != kEmptyHash) {
          uint32_t textureFeatureFlagsForSizing = 0;
          const auto pairForSizing = g_imguiTextureMap.find(texHashForSizing);
          if (pairForSizing != g_imguiTextureMap.end()) {
            textureFeatureFlagsForSizing = pairForSizing->second.textureFeatureFlags;
          }
          texturePopupLabelColumnW = computeTexturePopupLabelColumnWidth(textureFeatureFlagsForSizing);
          if (ImGui::IsPopupOpen(POPUP_NAME, ImGuiPopupFlags_None)) {
            const ImGuiStyle& sizingStyle = ImGui::GetStyle();
            const float minPopupW =
              texturePopupLabelColumnW + sizingStyle.ItemInnerSpacing.x + ImGui::GetFrameHeight() + sizingStyle.WindowPadding.x * 2.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(minPopupW, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
          }
        }

        if (ImGui::BeginPopup(POPUP_NAME)) {
          const XXH64_hash_t texHash = g_holdingTexture.load();
          if (texHash != kEmptyHash) {
            ImGui::Text("Texture Info:\n%s", makeTextureInfo(texHash, isMaterialReplacement(sceneMgr, texHash), false).c_str());
            if (ImGui::Button("Copy Texture hash##texture_popup")) {
              ImGui::SetClipboardText(hashToString(texHash).c_str());
            }
            uint32_t textureFeatureFlags = 0;
            const auto& pair = g_imguiTextureMap.find(texHash);
            if (pair != g_imguiTextureMap.end()) {
              textureFeatureFlags = pair->second.textureFeatureFlags;
            }
            RemixGui::PushLabelColumnFixedWidth(texturePopupLabelColumnW);
            for (auto& rtxOption : rtxTextureOptions) {
              rtxOption.bufferToggle = rtxOption.textureSetOption->containsHash(texHash);
              if ((rtxOption.featureFlagMask & textureFeatureFlags) != rtxOption.featureFlagMask) {
                // option requires a feature, but the texture doesn't have that feature.
                continue;
              }
              
              // Quick check for blocking layer (need this for display name)
              bool hasBlockingLayer = false;
              const RtxOptionLayer* targetLayer = rtxOption.textureSetOption->getTargetLayer();
              if (targetLayer) {
                hasBlockingLayer = rtxOption.textureSetOption->getBlockingLayer(targetLayer, texHash) != nullptr;
              }
              
              // Build display name with warning indicator if hash is blocked by higher priority layer
              std::string displayName = rtxOption.displayName;
              if (hasBlockingLayer) {
                displayName = std::string(rtxOption.displayName) + " [!]";
              }

              const bool toggleChanged = RemixGui::Checkbox(displayName.c_str(), &rtxOption.bufferToggle);
              const bool showTooltip = ImGui::IsItemHovered();

              if (toggleChanged) {
                toggleTextureSelection(texHash, rtxOption.uniqueId, rtxOption.textureSetOption);
              }
              
              // Only build the expensive tooltip when this item is actually hovered.
              if (showTooltip) {
                std::string tooltipText = buildTextureCategoryTooltip(rtxOption, texHash);
                RemixGui::SetTooltipUnformatted(tooltipText.c_str());
              }
            }
            RemixGui::PopLabelColumnFixedWidth();

            ImGui::EndPopup();
            return texHash;
          }
          ImGui::EndPopup();
          return {};
        } else {
          // popup is closed, forget texture
          g_holdingTexture.exchange(kEmptyHash);
          return {};
        }
      }
    }

    // NOTE: this is temporary, might need to show a full replacement material info
    namespace replacement_popup {
      double g_startTime { 0 };

      void open(uint32_t surfMaterialIndex) {
        g_startTime = ImGui::GetTime();
      }

      // Must be called every frame.
      std::optional< uint32_t > produce(SceneManager& sceneMgr) {
        // if mouse is now over imgui windows or there was a click, close this tooltip
        if (ImGui::GetIO().WantCaptureMouse ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          g_startTime = 0;
        }
        if (ImGui::GetTime() - g_startTime < 1.5f) {
          ImGui::SetTooltip("Replacement material");
        }
        return {};
      }
    }

    float fract(float v) {
      return v - std::floor(v);
    }

    // should be in sync with post_fx_highlight.comp.slang::highlightIntensity(),
    // so animation of post-effect highlight and UI are same
    float animatedHighlightIntensity(uint64_t timeSinceStartMS) {
      constexpr float ymax = 0.65f;
      float t10 = 1.0f - fract(static_cast<float>(timeSinceStartMS) / 1000.0f);
      return clamp(t10 > ymax ? t10 - (1.0f - ymax) : t10, 0.0f, 1.0f) / ymax;
    }

    constexpr const char* Uncategorized = "_nocategory";
  } // anonymous namespace

  void ImGUI::showTextureSelectionGrid(const Rc<DxvkContext>& ctx, const char* uniqueId, const uint32_t texturesPerRow, const float thumbnailSize, const float minChildHeight) {
    ImGui::PushID(uniqueId);
    auto common = ctx->getCommonObjects();
    uint32_t cnt = 0;
    float x = 0;
    const float startX = ImGui::GetCursorPosX();
    const float thumbnailSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float thumbnailPadding = ImGui::GetStyle().CellPadding.x;

    bool isListFiltered = false;
    RtxTextureOption listRtxOption{};

    for (auto rtxOption : rtxTextureOptions) {
      if (strcmp(rtxOption.uniqueId, uniqueId) == 0) {
        listRtxOption = rtxOption;
        isListFiltered = true;
        break;
      }
    }

    const ImVec2 availableSize = ImGui::GetContentRegionAvail();
    const float childWindowHeight = minChildHeight <= 600.0f ? minChildHeight
                                                             : availableSize.y < 600 ? 600.0f : availableSize.y;
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;
    ImGui::BeginChild(str::format("Child", uniqueId).c_str(), ImVec2(availableSize.x, childWindowHeight), false, window_flags);

    bool clickedOnTextureButton = false;
    static std::atomic<XXH64_hash_t> g_jumpto {};

    const XXH64_hash_t textureInPopup = texture_popup::g_holdingTexture.load();

    auto foundTextureHash = std::optional<XXH64_hash_t> {};
    auto highlightColor = HighlightColor::World;

    for (auto& [texHash, texImgui] : g_imguiTextureMap) {
      bool textureHasSelection = false;

      if (isListFiltered) {
        const auto& textureSet = listRtxOption.textureSetOption->get();
        textureHasSelection = listRtxOption.textureSetOption->containsHash(texHash);

        if ((listRtxOption.featureFlagMask & texImgui.textureFeatureFlags) != listRtxOption.featureFlagMask) {
          // If the list needs to be filtered by texture feature, skip it for this category.
          continue;
        }
      } else {
        for (const auto rtxOption : rtxTextureOptions) {
          textureHasSelection = rtxOption.textureSetOption->containsHash(texHash);
          if (textureHasSelection) {
            break;
          }
        }
      }

      // Only apply "show assigned only" filtering when using the legacy split texture GUI
      // When showLegacyTextureGui() is false, we want to show ALL textures in the single grid
      if (showLegacyTextureGui() && legacyTextureGuiShowAssignedOnly()) {
        if (std::string_view { uniqueId } == Uncategorized) {
          if (textureHasSelection) {
            continue; // Currently handling the uncategorized texture tab and current texture is assigned to a category -> skip
          }
        } else {
          if (!textureHasSelection) {
            continue; // Texture is not assigned to this category -> skip
          }
        }
      }

      if (texHash == textureInPopup || texHash == g_jumpto.load()) {
        const auto blueColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        const auto nvidiaColor = ImVec4(0.462745f, 0.725490f, 0.f, 1.f);

        const auto color = (texHash == textureInPopup ? blueColor : nvidiaColor);
        const float anim = animatedHighlightIntensity(GlobalTime::get().absoluteTimeMs());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(anim * color.x, anim * color.y, anim * color.z, 1.f));
      } else if (textureHasSelection) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.996078f, 0.329412f, 0.f, 1.f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 1.00f));
      }

      // Lazily create the tex ID ImGUI wants
      if (texImgui.texID == VK_NULL_HANDLE) {
        texImgui.texID = ImGui_ImplDxvk::AddTexture(nullptr, texImgui.imageView);

        if (texImgui.texID == VK_NULL_HANDLE) {
          ONCE(Logger::err("Failed to allocate ImGUI handle for texture, likely because we're trying to render more textures than VkDescriptorPoolCreateInfo::maxSets.  As such, we will truncate the texture list to show only what we can."));
          return;
        }
      }

      const auto& imageInfo = texImgui.imageView->imageInfo();

      // Calculate thumbnail extent with respect to image aspect
      const float aspect = static_cast<float>(imageInfo.extent.width) / imageInfo.extent.height;
      const ImVec2 extent {
        aspect >= 1.f ? thumbnailSize : thumbnailSize * aspect,
        aspect <= 1.f ? thumbnailSize : thumbnailSize / aspect
      };

      // Align thumbnail image button
      const float y = ImGui::GetCursorPosY();
      ImGui::SetCursorPosX(x + startX + (thumbnailSize - extent.x) / 2.f);
      ImGui::SetCursorPosY(y + (thumbnailSize - extent.y) / 2.f);

      if (ImGui::ImageButton(texImgui.texID, extent)) {
        clickedOnTextureButton = true;
        texture_popup::g_wasLeftClick = true;
      }

      if (!showLegacyTextureGui() || uniqueId == texture_popup::lastOpenCategoryId) {
        if (g_jumpto.load() == texHash) {
          ImGui::SetScrollHereY(0);
          g_jumpto.exchange(kEmptyHash);
        }
      }

      if (!texture_popup::isOpened()) {
        // if ImageButton is hovered
        if (ImGui::IsItemHovered()) {
          // imgui doesn't have right-click on a button, emulate it
          if (showLegacyTextureGui()) {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
              clickedOnTextureButton = true;
              texture_popup::g_wasLeftClick = false;
            }
          }

          foundTextureHash = texHash;
          highlightColor = HighlightColor::UI;

          // show additional info
          std::string rtxTextureSelection;
          for (auto& rtxOption : rtxTextureOptions) {
            if (rtxOption.textureSetOption->containsHash(texHash)) {
              if (rtxTextureSelection.empty()) {
                rtxTextureSelection = "\n";
              }
              rtxTextureSelection = str::format(rtxTextureSelection, " - ", rtxOption.displayName, "\n");
            }
          }
          ImGui::SetTooltip("%s\n(Left click to assign categories. Middle click to copy a texture hash.)\n\nCurrent categories:%s",
                            makeTextureInfo(texHash, isMaterialReplacement(common->getSceneManager(), texHash)).c_str(),
                            rtxTextureSelection.empty() ? "\n - None\n" : rtxTextureSelection.c_str());
          if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
            ImGui::SetClipboardText(hashToString(texHash).c_str());
          }
          texture_popup::lastOpenCategoryId = uniqueId;
        }
      }

      ImGui::PopStyleColor(1);

      if (++cnt % texturesPerRow != 0) {
        x += thumbnailSize + thumbnailSpacing + thumbnailPadding;
        ImGui::SetCursorPosY(y);
      } else {
        x = 0;
        ImGui::SetCursorPosY(y + thumbnailSize + thumbnailSpacing + thumbnailPadding);
      }
    }

    // popup for texture selection from world / ui
    // Only the "active" category is allowed to control the texture popup and highlighting logic
    if (!showLegacyTextureGui() || uniqueId == texture_popup::lastOpenCategoryId) {
      const bool wasUIClick = 
        !texture_popup::isOpened() && 
        clickedOnTextureButton;

      const bool wasWorldClick =
        isWorldTextureSelectionAllowed() &&
        !texture_popup::isOpened() &&
        !clickedOnTextureButton && 
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right));

      if (wasUIClick) {
        texture_popup::open(foundTextureHash);
      } else if (wasWorldClick) {
        texture_popup::g_wasLeftClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        // open as empty
        texture_popup::openAsync();
        // and make a request on a mouse click
        common->metaDebugView().ObjectPicking.request(
          tovec2i(ImGui::GetMousePos()),
          tovec2i(ImGui::GetMousePos()) + Vector2i { 1, 1 },

          // and callback on result:
          [](std::vector<ObjectPickingValue>&& objectPickingValues, std::optional<XXH64_hash_t> legacyTextureHash) {
            // assert(legacyTextureHash);
            // found asynchronously the legacy texture hash, place it into texture_popup; so we would highlight it
            texture_popup::g_holdingTexture.exchange(legacyTextureHash.value_or(kEmptyHash));
            // move UI menu focus
            g_jumpto.exchange(legacyTextureHash.value_or(kEmptyHash));
          });
      }

      if (wasUIClick) {
        texture_popup::lastOpenCategoryId = uniqueId;
      }

      auto texHashToHighlight = std::optional<XXH64_hash_t>{};

      // top priority for what's inside a currently open texture popup
      if (auto texInPopup = texture_popup::produce(common->getSceneManager())) {
        texHashToHighlight = *texInPopup;
        highlightColor = HighlightColor::UI;
      } else {
        if (foundTextureHash) {
          texHashToHighlight = *foundTextureHash;
        }
      }

      if (texHashToHighlight) {
        common->metaDebugView().Highlighting.requestHighlighting(*texHashToHighlight, highlightColor, ctx->getDevice()->getCurrentFrameId());
      } else {
        // if no hash to highlight: world -- highlight under a mouse cursor, ui - just desaturate
        if (isWorldTextureSelectionAllowed()) {
          common->metaDebugView().Highlighting.requestHighlighting(tovec2i(ImGui::GetMousePos()), highlightColor, ctx->getDevice()->getCurrentFrameId());
        } else {
          common->metaDebugView().Highlighting.requestHighlighting(XXH64_hash_t { kEmptyHash }, HighlightColor::UI, ctx->getDevice()->getCurrentFrameId());
        }
      }

      // checked after the last 'showTextureSelectionGrid' call to see if saved category is still active
      texture_popup::lastOpenCategoryActive = true;
    }

    ImGui::EndChild();

    ImGui::NewLine();
    ImGui::PopID();
  }

  // ---------------------------------------------------------------------------------------------
  // The Dusklight overlay.
  //
  // Restructured 2026-08-17. It was four tabs over nineteen collapsing sections, and about three
  // quarters of its vertical extent was explanatory prose rather than controls - roughly seven
  // screens of scrolling in its DEFAULT state, before anyone expanded anything, because ImGui
  // persists only a window's position, size and collapsed flag and so every section reopened to
  // its DefaultOpen state on each launch.
  //
  // Two facts made the fix cheap rather than a rewrite:
  //
  //  1. Every RemixGui widget bound to an RtxOption ALREADY renders a hover tooltip built from
  //     that option's own description (RtxOptionUxWrapper's destructor, rtx_gui_widgets.h, via
  //     RemixGui::BuildRtxOptionTooltip). All 227 rtx.dusklight.* declarations carry one, and they
  //     are consistently a superset of what this panel used to restate underneath the control. So
  //     the prose was duplicating something the reader could already get by hovering, and deleting
  //     it lost nothing. Where a sentence here said something the description did not, the
  //     sentence was moved INTO the description in the header - which is the authority, and which
  //     also carries it into RtxOptions.md at the next Windows regeneration.
  //
  //  2. What must be read WITHOUT hovering is a much smaller set than what was on screen: warnings
  //     and state, never reference. Those stayed inline. The rule applied throughout is
  //     mechanical - if the text describes what a control DOES it is a tooltip; if it describes a
  //     state that is currently WRONG it stays on screen.
  //
  // The shape that came out of it:
  //
  //   status strip     connection, protocol agreement and device registration as one coloured
  //                    token, ABOVE the tab bar so it is visible from every tab. It used to sit
  //                    mid-tab in one of the four, which is the tab this project's own notes say
  //                    to read before debugging anything else.
  //   master switches  the feature on/off toggles, one row, no expansion.
  //   alert lane       renders NOTHING when nothing is wrong. Absorbs the old "Requirements"
  //                    section, whose normal state was three static [ok] lines and no widget.
  //   tabs             each flat for its hot controls, with cold ones behind sibling collapsing
  //                    headers that are all one level deep.
  //   Readouts         the rtx.dusklight.env.* diagnostics, which are all NoSave and all
  //                    one-directional, gathered off the settings they were interleaved with.
  //
  // A standalone window rather than a tab in Remix's menu: these settings belong to the game, not
  // to this renderer, and keeping them in their own overlay means tuning the game does not require
  // Remix's menu over the top of the thing being tuned - both can be up at once, or either alone.
  // ---------------------------------------------------------------------------------------------

  namespace {
    // THE protocol the game must be speaking, and the only definition of it. The status strip is
    // the sole reader; scripts/check_dusklight_invariants.py matches this line against the prose
    // and the version registry in documentation/DusklightOverlay.md, which is what makes a
    // double-bump visible. Do not copy the literal anywhere else.
    constexpr int kRequiredProtocol = 17;

    // Matches the game's own warp menu. -1 is "you pick", and the game maps anything at or above
    // 15 to the same thing, so 14 is the last layer that means itself.
    constexpr int kMinWarpLayer = -1;
    constexpr int kMaxWarpLayer = 14;

    const ImVec4 kDusklightOkColor(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 kDusklightWarnColor(1.00f, 0.75f, 0.15f, 1.0f);
    const ImVec4 kDusklightBadColor(1.00f, 0.42f, 0.38f, 1.0f);

    // The game pushes its lists pipe delimited: one option carrying a list beats one option per
    // entry, and the destination table stays in the one place that owns it.
    std::vector<std::string> splitPipes(const std::string& packed) {
      std::vector<std::string> out;
      if (packed.empty()) {
        return out;
      }

      size_t start = 0;
      while (true) {
        const size_t next = packed.find('|', start);
        out.push_back(packed.substr(start, next == std::string::npos ? std::string::npos : next - start));
        if (next == std::string::npos) {
          break;
        }
        start = next + 1;
      }
      return out;
    }

    // ImGui's combo wants a contiguous array of pointers, and clamps the selection itself so a
    // list that shrank under it - which happens the moment the region changes - cannot index out.
    bool comboFromList(const char* label, const std::vector<std::string>& items, int& index) {
      if (items.empty()) {
        ImGui::BeginDisabled();
        int dummy = 0;
        const char* none = "(none)";
        ImGui::Combo(label, &dummy, &none, 1);
        ImGui::EndDisabled();
        return false;
      }

      index = std::clamp(index, 0, static_cast<int>(items.size()) - 1);

      std::vector<const char*> pointers;
      pointers.reserve(items.size());
      for (const std::string& item : items) {
        pointers.push_back(item.c_str());
      }

      return ImGui::Combo(label, &index, pointers.data(), static_cast<int>(pointers.size()));
    }

    // Save / discard for the rtx.conf layer, on the window's permanent header.
    //
    // The same two buttons exist in the Remix developer menu, several screens away from the
    // controls they apply to. Everything tuned in this panel is tuned while looking at the game,
    // and a setting that has to be re-found in another menu before it survives the session is a
    // setting that gets re-tuned every launch instead.
    void dusklightSaveRow() {
      RtxOptionLayer* rtxConfLayer = RtxOptionLayer::getRtxConfLayer();

      if (rtxConfLayer == nullptr) {
        return;
      }

      const bool hasUnsaved = rtxConfLayer->hasUnsavedChanges();

      ImGui::BeginDisabled(!hasUnsaved);
      if (ImGui::Button("Save Settings##dusklightSave")) {
        rtxConfLayer->save();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Write every changed Remix and Dusklight option to %s.\n"
                          "Dusklight's options are written in their own labelled block at the end of the file.",
                          rtxConfLayer->getFilePath().c_str());
      }
      ImGui::EndDisabled();

      ImGui::SameLine();

      ImGui::BeginDisabled(!hasUnsaved);
      if (ImGui::Button("Discard##dusklightDiscard")) {
        rtxConfLayer->reload();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Reload rtx.conf from disk, throwing away every change made since the last save.");
      }
      ImGui::EndDisabled();

      ImGui::SameLine();

      if (hasUnsaved) {
        ImGui::TextColored(kDusklightWarnColor, "unsaved");
      } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "saved");
      }
    }

    // A compact on/off for one option, laid out inline so several can share a row.
    //
    // RemixGui::Checkbox cannot be used for this: RtxOptionUxWrapper reserves the full width of
    // the row for its reset-button lane and hit-tests the whole row for its tooltip, so two of
    // them on one line would overlap in both. This is the same option wired by hand, carrying the
    // same tooltip the wrapper would have shown. It loses only the reset dot.
    //
    // setDeferred still lands in the rtx.conf layer, because showDusklightOverlay's
    // RtxOptionLayerTarget is in scope for everything this window draws.
    bool dusklightToggle(const char* label, RtxOption<bool>& option) {
      bool value = option();
      const bool changed = ImGui::Checkbox(label, &value);
      if (changed) {
        option.setDeferred(value);
      }
      if (ImGui::IsItemHovered()) {
        RemixGui::SetTooltipUnformatted(RemixGui::BuildRtxOptionTooltip(&option).c_str());
      }
      return changed;
    }

    // Group-level prose that belongs to no single option, attached to the collapsing header just
    // drawn rather than left as a permanent paragraph inside the group. Must be called
    // immediately after the CollapsingHeader and before its body: RemixGui::CollapsingHeader uses
    // NoTreePushOnOpen and adds no item after the header itself, so the header is the last item
    // at exactly that point and no later.
    void dusklightHeaderTip(const char* text) {
      if (ImGui::IsItemHovered()) {
        RemixGui::SetTooltipUnformatted(text);
      }
    }

    // One line of the alert lane: a Remix rendering option these features depend on but do not
    // own, and a button that sets it. Only ever drawn when the requirement is UNMET - when all of
    // them hold the lane renders nothing at all.
    //
    // This replaced a permanently open "Requirements" section at the top of the busiest tab whose
    // normal case was three static [ok] lines, a paragraph of rationale and no actionable widget.
    // The three booleans it reads are unchanged, and that matters: making this conditional is only
    // safe while the condition is exactly the one the section used, because the failure it exists
    // to prevent is an evening spent on a Dusklight switch that cannot work.
    void dusklightRequirementAlert(const char* label, const char* howToFix, const std::function<void()>& fix) {
      ImGui::TextColored(kDusklightWarnColor, "[!] %s", label);
      ImGui::SameLine();
      ImGui::PushID(label);
      if (ImGui::SmallButton("Fix")) {
        fix();
      }
      ImGui::PopID();
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", howToFix);
      }
    }

    // A diagnosis the reader must see without hovering and without having scrolled to the feature
    // it belongs to.
    void dusklightAlertText(const char* text) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDusklightWarnColor);
      ImGui::TextWrapped("%s", text);
      ImGui::PopStyleColor();
    }
  }

  void ImGUI::showDusklightOverlay(const Rc<DxvkContext>& ctx) {
    // THIS LINE IS WHY DUSKLIGHT SETTINGS PERSIST. Do not remove it, and do not assume the panel
    // works without it.
    //
    // An option edit is routed to a layer by the current edit target, and the default is Derived -
    // the layer for code-driven changes, which is never written to disk. Remix's own menus set the
    // User target (ImGUI::showMainMenu, ImGUI::switchMenu) so their edits land in the rtx.conf
    // layer; this overlay is reached from ImGUI::update instead, which does not, so every control
    // in it was writing to Derived. The values took effect, the panel read back what you set, and
    // RtxOptionImpl::writeOption then found nothing in the rtx.conf layer to serialise - so the
    // whole panel silently reset on every launch, with no error anywhere.
    //
    // Everything the window draws must stay inside this scope, the status strip and the master
    // switch row included.
    //
    // RtxOptionImpl::getTargetLayer is the routing table. Anything flagged NoSave still goes to
    // Derived regardless, which is correct: rtx.dusklight.env.* are readouts the game pushes every
    // frame and must never be baked into a config.
    RtxOptionLayerTarget layerTarget(RtxOptionEditTarget::User);

    // Wider than the 500 it was, so eight tab labels fit without the tab bar shrinking them.
    // FirstUseEver, so an existing imgui.ini keeps whatever size the window was given.
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Dusklight", &m_dusklightWindowOpen)) {
      showDusklightWindow(ctx);
    }

    ImGui::End();
  }

  void ImGUI::showDusklightWindow(const Rc<DxvkContext>& ctx) {
    ImGui::PushItemWidth(largeUiMode() ? m_largeWindowWidgetWidth : m_regularWindowWidgetWidth);

    // The permanent header. Nothing here expands, and all of it is visible from every tab.
    showDusklightStatusStrip();
    dusklightSaveRow();
    RemixGui::Separator();
    showDusklightMasterSwitches();
    showDusklightAlerts();
    RemixGui::Separator();

    if (ImGui::BeginTabBar("DusklightTabs", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton)) {
      if (ImGui::BeginTabItem("Go")) {
        showDusklightGoTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Lights")) {
        showDusklightLightsTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Sky")) {
        showDusklightSkyTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Surfaces")) {
        showDusklightSurfacesTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Scene")) {
        showDusklightSceneTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Input")) {
        showDusklightControlsTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Mods")) {
        showDusklightModsTab(ctx);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Readouts")) {
        showDusklightReadoutsTab(ctx);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::PopItemWidth();
  }

  // Connection, protocol agreement and device registration, as one coloured line above the tab
  // bar. It used to be a four-branch paragraph in the middle of one tab of four, below three
  // collapsing headers - so expanding any of those pushed it off screen, and a session sitting on
  // any other tab had no indication of protocol state at all. The Mods tab printed its own partial
  // version notice for exactly that reason.
  //
  // Skew has two directions and until 2026-08-11 only one of them was detected. The undetected one
  // is the likelier of the two in practice: a session branch bumps the protocol several times
  // while Fixed-Function-dev stays where it is, so a game built from the branch meets a d3d9.dll
  // built from the trunk far more often than the reverse. It reported "Connected" with no caveat,
  // which is the worst of the three possible answers. Both directions are reported now, and both
  // print the two numbers, because "your builds do not match" without saying which side is behind
  // still costs the rebuild-and-see round it exists to prevent.
  //
  // The colour is decided by the same two booleans that pick the text, never recomputed, so the
  // token cannot read green while the numbers differ.
  void ImGUI::showDusklightStatusStrip() {
    const bool feedLive = DusklightEnv::enable();
    const int gameProtocol = DusklightEnv::protocol();
    const bool gameTooOld = feedLive && gameProtocol < kRequiredProtocol;
    const bool remixTooOld = feedLive && gameProtocol > kRequiredProtocol;
    const bool deviceRegistered = feedLive && DusklightEnv::deviceRegistered();

    if (!feedLive) {
      ImGui::TextColored(kDusklightBadColor, "[x] Not connected - the game is not reporting anything");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "The game needs to be running on its D3D9 backend against this d3d9.dll, with the bridge\n"
          "enabled. If it was connected a moment ago and stopped, that is worth reporting: the game\n"
          "re-sends its state when it notices, so it should recover within a couple of seconds by\n"
          "itself. Settings changed here are kept and applied as soon as it connects.");
      }
    } else if (gameTooOld) {
      ImGui::TextColored(kDusklightWarnColor,
                         "[!] GAME IS OLDER - it reports protocol %d, this d3d9.dll wants %d",
                         gameProtocol, kRequiredProtocol);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "The game does not read the settings this build offers, so controls will appear to do\n"
          "nothing. The readouts are still accurate. Rebuild the game from the same commit point as\n"
          "this d3d9.dll - the two are one protocol and both sides bump together.");
      }
    } else if (remixTooOld) {
      // What actually happens, rather than a guess: the game asks for each setting by string name
      // through the getRtxOptionValue export, which returns 0 for a name it does not declare
      // (rtx_option_manager.cpp), and the game's readOption* helpers turn that into "keep the local
      // value" (remix_bridge.cpp). So nothing errors and nothing logs.
      ImGui::TextColored(kDusklightWarnColor,
                         "[!] THIS d3d9.dll IS OLDER - the game reports protocol %d, this build knows %d",
                         gameProtocol, kRequiredProtocol);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "The controls here still work, but every setting the game has gained since protocol %d is\n"
          "MISSING from this window entirely - the game keeps its config.json value for those and\n"
          "nothing here can move it - and the readouts they feed are absent for the same reason.\n"
          "Nothing errors and nothing is logged, so this notice is the only symptom. Rebuild\n"
          "d3d9.dll from the same commit point as the game.", kRequiredProtocol);
      }
    } else if (!deviceRegistered) {
      ImGui::TextColored(kDusklightWarnColor,
                         "[!] Connected, protocol %d - but its D3D9 device is NOT registered with the Remix API",
                         kRequiredProtocol);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "The environment feed is arriving, but nothing that goes through the Remix API can reach\n"
          "this runtime: effect lights, room lights and the HD texture pack all submit that way and\n"
          "will all read as doing nothing.");
      }
    } else {
      ImGui::TextColored(kDusklightOkColor, "[ok] Connected - protocol %d - device registered",
                         kRequiredProtocol);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game is feeding its environment state to Remix, both sides agree on the\n"
                          "protocol, and its D3D9 device is registered with the Remix API.");
      }
    }
  }

  // The feature on/off switches, one compact row each, always visible. Each of these used to be
  // the first control inside a collapsing header somewhere, which meant the single most-used
  // control of every feature was one expansion and several screens away.
  //
  // Each option appears HERE AND NOWHERE ELSE. Drawing one in two places would give a single
  // RtxOption two widgets with the same ImGui ID, because RtxOptionUxWrapper keys its ID off the
  // option's address.
  void ImGUI::showDusklightMasterSwitches() {
    dusklightToggle("Bridge", DusklightGame::bridgeEnableObject());
    ImGui::SameLine();
    dusklightToggle("Atmosphere", DxvkDusklightAtmosphere::enableObject());
    ImGui::SameLine();
    dusklightToggle("Sky", DxvkDusklightAtmosphere::skyEnableObject());
    ImGui::SameLine();
    dusklightToggle("Effect Lights", DusklightGame::effectLightsObject());

    dusklightToggle("Emissive", DusklightEmissive::enableObject());
    ImGui::SameLine();
    dusklightToggle("Ramps", DusklightRamp::rampMaterialsObject());
    ImGui::SameLine();
    dusklightToggle("Water", DusklightWater::enableObject());
    ImGui::SameLine();
    dusklightToggle("HD Pack", DusklightTexRep::enableObject());
  }

  // The alert lane. Renders NOTHING - not a separator, not a header, not a blank line - when
  // nothing is wrong, which is the normal case.
  //
  // Everything in it is a state that is currently wrong, which is the whole test for being here
  // rather than in a tooltip. A warning demoted to a tooltip is a warning nobody sees.
  void ImGUI::showDusklightAlerts() {
    const bool volumetricsOn = RtxGlobalVolumetrics::enable();
    const bool bloomOn = DxvkBloom::enable();
    const bool skyDetectOff = RtxOptions::skyAutoDetect() == SkyAutoDetectMode::None;
    const bool feedLive = DusklightEnv::enable();

    // Exactly the three booleans the old Requirements section read.
    const bool anyRequirementUnmet = !volumetricsOn || !bloomOn || !skyDetectOff;

    // Only complained about while the feature is actually switched on: a diagnosis of a system the
    // reader deliberately turned off is a permanent nag, not an alert.
    const bool effectLightsOn = DusklightGame::effectLights();
    const bool effectLightsSilent = feedLive && effectLightsOn && !DusklightEnv::effLightsRunning();
    const bool effectLightsRejecting = feedLive && effectLightsOn && DusklightEnv::effLightsRunning() &&
                                       DusklightEnv::effLightsCandidates() == 0 &&
                                       DusklightEnv::effLightsConsidered() > 0;
    const bool effectLightsAllOrphans = feedLive && effectLightsOn && DusklightEnv::effLightsRunning() &&
                                        DusklightEnv::effLightsOrphans() > 4 &&
                                        DusklightEnv::effLightsSites() == 0;

    if (!anyRequirementUnmet && !effectLightsSilent && !effectLightsRejecting && !effectLightsAllOrphans) {
      return;
    }

    RemixGui::Separator();

    if (!volumetricsOn) {
      dusklightRequirementAlert("Volumetrics are off - the atmosphere's fog rides on them",
                                "Sets rtx.volumetrics.enable. Without it the fog falls back to Remix's legacy depth ramp.",
                                []() { RtxGlobalVolumetrics::enableObject().setDeferred(true); });
    }
    if (!bloomOn) {
      dusklightRequirementAlert("Bloom is off - the Dusklight bloom is a mode of it",
                                "Sets rtx.bloom.enable. The Dusklight bloom is that same pass, so it cannot run with the pass off.",
                                []() { DxvkBloom::enableObject().setDeferred(true); });
    }
    if (!skyDetectOff) {
      dusklightRequirementAlert("Sky auto-detect is on - you get two skies",
                                "Sets rtx.skyAutoDetect to None. The auto detected sky keeps rasterizing behind the generated one.",
                                []() { RtxOptions::skyAutoDetect.setDeferred(SkyAutoDetectMode::None); });
    }

    if (effectLightsSilent) {
      dusklightAlertText("Effect lights: the game is not running this at all, so nothing reaches Remix. "
                         "Either the switch is not reaching the game, or its D3D9 device never registered - "
                         "the status line at the top of this window says which.");
    } else if (effectLightsRejecting) {
      dusklightAlertText("Effect lights: effects are being drawn but none passed the rule. If you are stood "
                         "at a fire, the classifier is wrong - press Log Full Effect Light Report on the "
                         "Lights tab and send the log, which names every effect it saw and why it was rejected.");
    } else if (effectLightsAllOrphans) {
      dusklightAlertText("Effect lights: this room's lights are all ones the game registered with no effect "
                         "beside them, and those are dropped by default because their placement is exactly "
                         "what this system exists to stop trusting. If the room looks under-lit, that is the "
                         "trade showing - worth reporting.");
    }
  }

  // Warp and the clock. Both answer "put me somewhere specific", and the clock is session step 1
  // in the test playbook - "do this first, it is the tool the rest want" - so it is flat here
  // rather than behind the collapsing header it used to open with.
  void ImGUI::showDusklightGoTab(const Rc<DxvkContext>& ctx) {
    if (!DusklightEnv::enable()) {
      ImGui::TextWrapped("Waiting for the game. The warp destinations and the clock both come from the "
                         "game's own tables, so nothing can be listed until it connects.");
      return;
    }

    const std::vector<std::string> regions = splitPipes(DusklightEnv::warpRegions());
    const std::vector<std::string> maps = splitPipes(DusklightEnv::warpMaps());
    const std::vector<std::string> rooms = splitPipes(DusklightEnv::warpRooms());
    const std::vector<std::string> points = splitPipes(DusklightEnv::warpPoints());

    // A branch rather than an early return: the destination list can lag a frame or two behind the
    // connection and the clock does not depend on it, so the clock below is still drawn while that
    // arrives instead of disappearing along with the combos. That used to be two call sites of
    // showDusklightTimeOfDay; it is one now, and the property is the same.
    if (regions.empty()) {
      ImGui::TextWrapped("The game has not sent its destination list yet. It arrives within a frame or two "
                         "of connecting.");
    } else {
      int regionIndex = DusklightGame::regionIndex();
      int mapIndex = DusklightGame::mapIndex();

      // Changing the region invalidates the level list, which the game rebuilds from the new index -
      // so the selection resets rather than pointing at whatever happens to sit at the same offset.
      if (comboFromList("Region", regions, regionIndex)) {
        DusklightGame::regionIndex.setDeferred(regionIndex);
        DusklightGame::mapIndex.setDeferred(0);
        DusklightGame::roomIndex.setDeferred(0);
        DusklightGame::pointIndex.setDeferred(0);
        DusklightGame::layer.setDeferred(kMinWarpLayer);
      }

      if (comboFromList("Level", maps, mapIndex)) {
        DusklightGame::mapIndex.setDeferred(mapIndex);
        DusklightGame::roomIndex.setDeferred(0);
        DusklightGame::pointIndex.setDeferred(0);
        DusklightGame::layer.setDeferred(kMinWarpLayer);
      }

      const bool canWarp = !maps.empty() && !rooms.empty() && !points.empty();

      ImGui::BeginDisabled(!canWarp);
      if (ImGui::Button("Warp", ImVec2(120, 0))) {
        // The game acts on this changing, not on its value.
        DusklightGame::commit.setDeferred(DusklightGame::commit() + 1);
      }
      ImGui::EndDisabled();

      ImGui::SameLine();
      ImGui::Text("-> %s", DusklightEnv::warpStage().empty() ? "(nothing selected)" : DusklightEnv::warpStage().c_str());
    }

    RemixGui::Separator();

    showDusklightTimeOfDay();

    RemixGui::Separator();

    const bool rarelyOpen = RemixGui::CollapsingHeader("Rarely needed", collapsingHeaderClosedFlags);
    dusklightHeaderTip("Room, point and layer, and the clock's rate.\n\n"
                       "The warp defaults land at the level's first room and entrance, which is what you "
                       "want almost every time.");
    if (rarelyOpen) {
      ImGui::Indent();

      int roomIndex = DusklightGame::roomIndex();
      if (comboFromList("Room", rooms, roomIndex)) {
        DusklightGame::roomIndex.setDeferred(roomIndex);
        DusklightGame::pointIndex.setDeferred(0);
      }

      int pointIndex = DusklightGame::pointIndex();
      if (comboFromList("Point", points, pointIndex)) {
        DusklightGame::pointIndex.setDeferred(pointIndex);
      }

      int layer = DusklightGame::layer();
      if (ImGui::InputInt("Layer", &layer)) {
        DusklightGame::layer.setDeferred(std::clamp(layer, kMinWarpLayer, kMaxWarpLayer));
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
          "How the game keeps several versions of one place - before and after a story event, say.\n"
          "-1, the default, asks the game to pick, which is nearly always what you want: 0 is a real\n"
          "layer rather than a no-preference, so pinning it lands in the wrong version of anywhere\n"
          "whose default is not 0. Clamped to -1 .. 14, because the game folds anything at or above\n"
          "15 back to -1.");
      }

      RemixGui::Separator();
      RemixGui::DragFloat("Clock Rate##dusklight", &DusklightGame::clockRateObject(), 0.05f, 0.f, 20.f, "%.2fx");

      ImGui::Unindent();
    }
  }

  // The clock. Flat, and first after the warp row: the playbook makes it session step 1 and the
  // reason is that Freeze Time is what makes an A/B pair worth comparing at all. It used to open
  // with a collapsing header of its own on a tab named Warp, one expansion down from anywhere.
  //
  // Everything below the widgets is delicate and none of it was touched by the layout change:
  // s_sliderTime is only synced from the game while the slider is not held, and s_timeRequests is
  // incremented in the UI rather than read-modify-written off the option. Both are recorded as
  // second attempts in documentation/DusklightOverlay.md 3.2.1.
  void ImGUI::showDusklightTimeOfDay() {
    const float gameTime = DusklightEnv::daytime();

    // The whole day is 360 degrees, so a degree is four minutes. Shown as a clock as well as the
    // raw number because the schedule that picks the palettes is written in hours, while
    // everything in these options is written in degrees. Integer minutes throughout, which keeps
    // this off <cmath> - the file does not include it and a transitive one is not worth relying
    // on across three compilers.
    const int totalMinutes = static_cast<int>(gameTime * 4.0f);
    const int hour = (totalMinutes / 60) % 24;
    const int minute = totalMinutes % 60;

    ImGui::Text("Game clock: %02d:%02d   (%.1f deg)%s", hour, minute, gameTime,
                DusklightGame::freezeTime() ? "   FROZEN" : "");

    // The slider follows the game whenever it is not being held, so it reads as a clock as much
    // as a control and a drag always starts from where the game actually is. It cannot simply be
    // driven from the readout every frame: the value crosses the bridge, gets applied, and comes
    // back a frame or two later, so a slider fed the returning value would fight the hand
    // holding it.
    static float s_sliderTime = 0.0f;
    static bool s_sliderHeld = false;

    if (!s_sliderHeld) {
      s_sliderTime = gameTime;
    }

    // The value and the request to apply it are separate, so dragging back onto a value the
    // option already holds still moves the clock. Pressing Noon twice in a row has to work.
    //
    // The counter is kept here rather than read back off the option and incremented, the way the
    // warp button does it: a slider fires on many consecutive frames, and read-modify-write only
    // stays monotonic if every deferred set lands before the next read. A button pressed once a
    // frame at most never tests that; a drag would.
    static int s_timeRequests = 0;
    const auto requestTime = [](float degrees) {
      DusklightGame::timeOfDay.setDeferred(degrees);
      DusklightGame::timeCommit.setDeferred(++s_timeRequests);
    };

    if (ImGui::SliderFloat("Set Time##dusklight", &s_sliderTime, 0.0f, 359.9f, "%.1f deg")) {
      requestTime(s_sliderTime);
    }
    s_sliderHeld = ImGui::IsItemActive();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
        "The day is 360 degrees: 0 midnight, 90 sunrise, 180 noon, 270 sunset, so a degree is four\n"
        "minutes. The moon to sun handover sits around 67 to 75 degrees, which is the window to sit\n"
        "in for anything about the celestial light. The physical sky blend is driven by sun\n"
        "elevation rather than by the clock, so Midday is where it is at full strength; Morning 0\n"
        "puts the sun about 15 degrees up and Evening 1 is already a little past sunset, which is\n"
        "where the game's own palette carries the look instead.");
    }

    // The game's own six time lights, at the game's own numbers. Its environment palette holds
    // six time-of-day slots and its light schedule (l_time_attribute, d_kankyo_data.cpp:212)
    // cross-fades between them; the values below are the ones the game's own debug time-fix
    // menu pins (d_kankyo.cpp, dScnKy_env_light_c::setDaytime), each chosen so the schedule
    // resolves to exactly one slot with no blend. So each button shows one authored palette
    // entry rather than a point part-way between two.
    //
    // The names are the game's too, romanized: asa 0/1 (morning), hiru (midday - not afternoon,
    // and not noon: it is pinned at 11:00), yuu 0/1 (evening), yoru (night).
    //
    // Three of the six are reachable ONLY at their exact value. The schedule holds midday over
    // 135-240, evening 0 over 255-270 and night over 300-75, but morning 0, morning 1 and
    // evening 1 sit on a single instant each (90, 105, 285) with a cross-fade either side. A
    // slider cannot realistically be dragged onto them, which is most of why these buttons are
    // worth having.
    //
    // This replaced four clock quarter points (0/90/180/270). Those were not wrong - each did
    // resolve to one pure slot (night, morning 0, midday, evening 0) - but they reached only
    // four of the six, and the two they missed are two of the three a slider cannot reach.
    //
    // Dropping 0 and 180 costs nothing measurable for the sun and moon, which are on their own
    // orbit rather than on the palette schedule: setSunpos peaks at 59.0 deg of elevation at
    // 0/180 and gives 57.4 deg at 345/165, so Night and Midday stand in for midnight and noon
    // to within about 1.6 deg. Use the slider if an exact solar extreme is what is wanted.
    //
    // Buttons rather than a combo because the whole value of these is landing on the same number
    // twice. Two rows so six fit without the window widening.
    struct TimePreset {
      const char* label;
      float degrees;
    };
    static constexpr TimePreset kPresets[] = {
      { "Morning 0", 90.0f },  { "Morning 1", 105.0f }, { "Midday", 165.0f },
      { "Evening 0", 255.0f }, { "Evening 1", 285.0f }, { "Night", 345.0f },
    };
    constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
    constexpr size_t kPresetsPerRow = 3;

    static constexpr const char* kPresetHelp =
      "The game's own six time-of-day palette entries, at the six clock values the game itself uses\n"
      "to show one of them cleanly. Between them the schedule is cross-fading two entries, and\n"
      "Morning 0, Morning 1 and Evening 1 exist for one instant each - the slider cannot land on\n"
      "them, and without Freeze Time the clock walks straight back off them. So an A/B of anything\n"
      "palette driven belongs on a button, frozen.";

    for (size_t i = 0; i < kPresetCount; i++) {
      if (i % kPresetsPerRow != 0) {
        ImGui::SameLine();
      }
      if (ImGui::Button(kPresets[i].label)) {
        s_sliderTime = kPresets[i].degrees;
        requestTime(kPresets[i].degrees);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kPresetHelp);
      }
    }

    RemixGui::Checkbox("Freeze Time", &DusklightGame::freezeTimeObject());
  }

  void ImGUI::showDusklightLightsTab(const Rc<DxvkContext>& ctx) {
    // The four controls a test session actually reaches for, flat. The playbook's own instruction
    // is narrower than the section used to be: "If it is too bright or too dim, move Master
    // Intensity first - it scales everything. Only reach for Derived Intensity / Undetermined
    // Intensity once you can see which half is wrong."
    RemixGui::DragFloat("Master Intensity##dusklight", &DusklightGame::effectLightIntensityObject(), 0.02f, 0.f, 8.f, "%.2f");
    RemixGui::DragFloat("Master Reach##dusklight", &DusklightGame::effectLightReachScaleObject(), 0.02f, 0.f, 16.f, "%.2fx");
    RemixGui::DragFloat("Master Radius##dusklight", &DusklightGame::effectLightRadiusScaleObject(), 0.02f, 0.01f, 8.f, "%.2fx");
    RemixGui::Checkbox("Infinite Lantern Oil", &DusklightGame::lanternInfiniteOilObject());

    // An action, so NoSave and a counter rather than a flag: a persisted request would fire on the
    // next launch, and the game latches the first count it sees without acting so that connecting
    // to a Remix that outlived a game restart does not dump a report nobody asked for.
    if (ImGui::Button("Log Full Effect Light Report", ImVec2(-1, 0))) {
      DusklightGame::effectLightReportCommit.setDeferred(DusklightGame::effectLightReportCommit() + 1);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
        "One press, five sections, every open question answered - send the log and nothing else is\n"
        "needed.\n\n"
        "COUNTERS: the whole chain plus the bridge's own create/destroy counts.\n"
        "EFFECTS: one line per distinct effect seen since the last press - name, blend\n"
        "  configuration, colours, the MEASURED chroma and luma the rule cut on, which keyword\n"
        "  picked its class, and a verdict that names the clause that refused it rather than just\n"
        "  saying no. Plus whether its colour is even capable of animating, and how long it lives.\n"
        "SITES: every light this frame - where it is, how many emitters merged into it, and how far\n"
        "  it was from the game light it adopted.\n"
        "GAME LIGHTS: every light the game registered and which effect took it. Adoption is\n"
        "  exclusive, so this is what shows a short-lived effect stealing a torch's light.\n"
        "TRACE: a rolling window of how each light changed over the last few seconds. It is\n"
        "  RETROSPECTIVE - do the thing you want to look at first, THEN press this.");
    }

    const bool tuningOpen = RemixGui::CollapsingHeader("Effect light tuning", collapsingHeaderClosedFlags);
    dusklightHeaderTip("Everything the classifier and the solver take from the game, split by where the\n"
                       "number came from. None of it is named in a test procedure or an open issue; the\n"
                       "three Master multipliers above reach all of it at once.");
    if (tuningOpen) {
      ImGui::Indent();
      RemixGui::DragFloat("Mass Exponent##dusklight", &DusklightGame::effectLightMassExponentObject(), 0.01f, 0.f, 2.f, "%.2f");

      RemixGui::Separator();
      ImGui::TextUnformatted("From the game (a light was authored beside the effect)");
      RemixGui::DragFloat("Derived Intensity##dusklight", &DusklightGame::effectLightDerivedIntensityObject(), 0.05f, 0.f, 64.f, "%.2f");
      RemixGui::DragFloat("Derived Radius##dusklight", &DusklightGame::effectLightDerivedRadiusObject(), 0.1f, 0.5f, 64.f, "%.1f units");

      RemixGui::Separator();
      ImGui::TextUnformatted("Invented (nothing authored - fire arrows, unlit torches)");
      RemixGui::DragFloat("Undetermined Intensity##dusklight", &DusklightGame::effectLightUndeterminedIntensityObject(), 0.05f, 0.f, 64.f, "%.2f");
      RemixGui::DragFloat("Undetermined Reach##dusklight", &DusklightGame::effectLightUndeterminedReachObject(), 5.f, 0.f, 8000.f, "%.0f units");
      RemixGui::DragFloat("Undetermined Radius##dusklight", &DusklightGame::effectLightUndeterminedRadiusObject(), 0.1f, 0.5f, 64.f, "%.1f units");

      RemixGui::Separator();
      ImGui::TextUnformatted("What the effect's own artists authored");
      RemixGui::Checkbox("Colour From The Effect's Authored Ramp", &DusklightGame::effectLightAuthoredColorObject());
      RemixGui::Checkbox("Grow The Sphere To The Authored Size", &DusklightGame::effectLightAuthoredRadiusObject());

      RemixGui::Separator();
      ImGui::TextUnformatted("Link's lantern");
      RemixGui::Checkbox("Give The Lantern Its Own Settings", &DusklightGame::effectLightLanternSeparateObject());
      ImGui::BeginDisabled(!DusklightGame::effectLightLanternSeparate());
      RemixGui::DragFloat("Lantern Intensity##dusklight", &DusklightGame::effectLightLanternIntensityObject(), 0.05f, 0.f, 64.f, "%.2f");
      RemixGui::DragFloat("Lantern Reach##dusklight", &DusklightGame::effectLightLanternReachObject(), 5.f, 0.f, 8000.f, "%.0f units");
      RemixGui::DragFloat("Lantern Radius##dusklight", &DusklightGame::effectLightLanternRadiusObject(), 0.1f, 0.5f, 64.f, "%.1f units");
      ImGui::EndDisabled();

      RemixGui::Separator();
      ImGui::TextUnformatted("Placement, grouping and budget");
      RemixGui::DragFloat("Fire Height Offset##dusklight", &DusklightGame::effectLightFireOffsetObject(), 0.5f, -200.f, 200.f, "%.1f units");
      RemixGui::DragFloat("Glow Height Offset##dusklight", &DusklightGame::effectLightGlowOffsetObject(), 0.5f, -200.f, 200.f, "%.1f units");
      RemixGui::DragFloat("Merge Radius##dusklight", &DusklightGame::effectLightMergeRadiusObject(), 1.f, 0.f, 500.f, "%.0f units");
      RemixGui::DragFloat("Adopt Radius##dusklight", &DusklightGame::effectLightAdoptRadiusObject(), 5.f, 0.f, 2000.f, "%.0f units");
      RemixGui::DragFloat("Volumetric Boost##dusklight", &DusklightGame::effectLightVolumetricObject(), 0.05f, 0.f, 16.f, "%.2f");
      RemixGui::DragInt("Max Lights (0 = no limit)##dusklight", &DusklightGame::effectLightMaxLightsObject(), 1.f, 0, 256);
      RemixGui::DragFloat("Max Distance##dusklight", &DusklightGame::effectLightMaxDistanceObject(), 50.f, 0.f, 100000.f, "%.0f units");
      RemixGui::Checkbox("Light Explosions and One-Shots", &DusklightGame::effectLightBurstsObject());

      // The Shadow Insect controls. Deliberately a pair - the switch answers "I do not like
      // this", the hold answers "it flickers" - because they are different complaints with
      // different fixes and one control could only have served one of them.
      RemixGui::Checkbox("Light Sparks (Shadow Insect, glitter)", &DusklightGame::effectLightSparksObject());
      RemixGui::DragInt("Spark Hold (extra frames)##dusklight", &DusklightGame::effectLightSparkHoldObject(), 1.f, 0, 120);

      RemixGui::Separator();
      ImGui::TextUnformatted("What counts as a glow");
      RemixGui::DragFloat("Min Chroma##dusklight", &DusklightGame::effectLightMinChromaObject(), 0.01f, 0.f, 1.f, "%.2f");
      RemixGui::DragFloat("Min Luminance##dusklight", &DusklightGame::effectLightMinLumaObject(), 0.01f, 0.f, 1.f, "%.2f");
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Sun and moon", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      RemixGui::Checkbox("Sun/Moon Light Enabled", &DusklightGame::sunMoonLightObject());
      RemixGui::DragFloat("Sun Intensity##dusklight", &DusklightGame::sunIntensityObject(), 0.05f, 0.f, 50.f, "%.2f");
      RemixGui::DragFloat("Moon Intensity##dusklight", &DusklightGame::moonIntensityObject(), 0.01f, 0.f, 10.f, "%.2f");
      RemixGui::DragFloat("Angular Diameter##dusklight", &DusklightGame::celestialAngleObject(), 0.05f, 0.1f, 20.f, "%.2f deg");
      RemixGui::DragFloat("Noon Elevation##dusklight", &DusklightGame::celestialNoonElevationObject(), 0.25f, 1.f, 90.f, "%.1f deg");
      RemixGui::Separator();
      RemixGui::Checkbox("Flip Direction (diagnostic)", &DusklightGame::celestialFlipObject());
      RemixGui::Checkbox("Lock Direction (diagnostic)", &DusklightGame::celestialLockObject());

      if (DusklightEnv::enable() && !DusklightEnv::sunActive()) {
        ImGui::TextUnformatted("Not drawing: the game reports no sun or moon in this area.");
      }
      ImGui::Unindent();
    }

    const bool roomOpen = RemixGui::CollapsingHeader("Room lights (authored, experimental)", collapsingHeaderClosedFlags);
    dusklightHeaderTip("The lights the room itself was built with, out of its stage file - not the torches\n"
                       "and lanterns actors register. These are what light a dungeon corridor with no fire\n"
                       "in it, and they are the only lights in the game carrying a direction and a cone.\n\n"
                       "EXPERIMENTAL and off by default on purpose: these are authored positions, the exact\n"
                       "property the effect lights exist because they distrust. Two things to look for with\n"
                       "this on. Every fire gaining a second light, offset from the first, means it is\n"
                       "double counting with Effect Lights. Shadows arriving from somewhere that is not a\n"
                       "visible light means the placements do not survive the path tracer, and the answer is\n"
                       "to leave this off rather than to tune it.");
    if (roomOpen) {
      ImGui::Indent();
      RemixGui::Checkbox("Room Lights Enabled", &DusklightGame::roomLightsObject());
      RemixGui::DragFloat("Room Intensity##dusklight", &DusklightGame::roomLightIntensityObject(), 0.05f, 0.f, 64.f, "%.2f");
      RemixGui::DragFloat("Room Radius##dusklight", &DusklightGame::roomLightRadiusObject(), 0.1f, 0.5f, 64.f, "%.1f units");
      RemixGui::DragFloat("Cone Softness##dusklight", &DusklightGame::roomLightConeSoftnessObject(), 0.01f, 0.f, 4.f, "%.2f");

      // Kept beside the controls rather than moved to the alert lane: this feature is off by
      // default, so a diagnosis of it is only meaningful to someone who has come here and turned
      // it on. The counts themselves are on the Readouts tab.
      if (DusklightEnv::enable() && DusklightGame::roomLights()) {
        if (!DusklightEnv::roomLightsRunning()) {
          ImGui::TextWrapped(
            "The game is not running its room light submission, so nothing here can reach Remix. Either "
            "this switch is not reaching the game, or its D3D9 device never registered - the status "
            "line at the top of this window says which.");
        } else if (DusklightEnv::roomLightsFound() == 0) {
          ImGui::TextWrapped(
            "This room was authored with no lights of its own. Expected outdoors, where the sun and moon "
            "take the first two slots, and in any room lit only by its palette.");
        } else if (DusklightEnv::roomLightsDrawn() == 0) {
          ImGui::TextWrapped(
            "The room has authored lights but none reached Remix. Every one of them is either switched "
            "off by a game switch, or the palette has taken its colour to black - both of which the game "
            "does too, so this is more likely correct than broken.");
        }
      }
      ImGui::Unindent();
    }
  }

  void ImGUI::showDusklightSkyTab(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();

    const bool volumetricsOnForFog = RtxGlobalVolumetrics::enable();
    const bool bloomPassOn = DxvkBloom::enable();

    // Each BeginDisabled sits INSIDE its header rather than around the lot, because a
    // CollapsingHeader inside BeginDisabled refuses the click that opens it - so wrapping the
    // whole group would make the greyed controls unreachable rather than merely inert.
    if (!volumetricsOnForFog) {
      ImGui::TextWrapped(
        "Greyed out: Remix's volumetrics are off, and the fog half of this rides on them. The alert "
        "at the top of this window has a button for it, or it is under Rendering > Volumetrics.");
    }

    ImGui::BeginDisabled(!volumetricsOnForFog);
    common->metaDusklightAtmosphere().showImguiHot();
    ImGui::EndDisabled();

    const bool fogOpen = RemixGui::CollapsingHeader("Fog medium", collapsingHeaderClosedFlags);
    dusklightHeaderTip("The one participating medium the whole atmosphere is derived from. Ramp Owns\n"
                       "decides which part of the distance the game's own fog ramp is responsible for;\n"
                       "everything else shapes what the medium does with the light.");
    if (fogOpen) {
      ImGui::Indent();
      ImGui::BeginDisabled(!volumetricsOnForFog);
      common->metaDusklightAtmosphere().showImguiFog();
      ImGui::EndDisabled();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Froxel grid", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      ImGui::BeginDisabled(!volumetricsOnForFog);
      common->metaDusklightAtmosphere().showImguiFroxelGrid();
      ImGui::EndDisabled();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Sky shape and moon", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      ImGui::BeginDisabled(!volumetricsOnForFog);
      common->metaDusklightAtmosphere().showImguiSkyShape();
      ImGui::EndDisabled();
      ImGui::Unindent();
    }

    const bool physicalOpen = RemixGui::CollapsingHeader("Physical sky", collapsingHeaderClosedFlags);
    dusklightHeaderTip("The blend follows the sun's height because that is where the two skies actually\n"
                       "disagree. At midday both are a plain blue gradient and the change is nearly\n"
                       "invisible, while everything it brings - sky fill in shadow, haze with distance - is\n"
                       "not. At dusk the game's version is deliberately more saturated than physics\n"
                       "produces, so it keeps the bottom of the arc. Night and the Twilight Realm are the\n"
                       "game's outright.");
    if (physicalOpen) {
      ImGui::Indent();
      ImGui::BeginDisabled(!volumetricsOnForFog);
      common->metaDusklightAtmosphere().showImguiPhysicalSky();
      ImGui::EndDisabled();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Bloom", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      if (!bloomPassOn) {
        ImGui::TextWrapped(
          "Greyed out: Remix's bloom pass is off, and the Dusklight bloom is a mode of that same "
          "pass. The alert at the top of this window has a button for it, or it is under "
          "Rendering > Post-Processing > Bloom.");
      }
      ImGui::BeginDisabled(!bloomPassOn);
      common->metaBloom().showDusklightImguiSettings();
      ImGui::EndDisabled();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Ambient grade", collapsingHeaderClosedFlags)) {
      common->metaDusklightGrade().showImguiSettings();
    }
  }

  // Material translation, water and the HD texture pack. The first is Remix's own half of the
  // wire and works whether or not the game is connected; the other two are read out of the D3D9
  // stream. They share a tab because all three answer "what does this surface look like".
  void ImGUI::showDusklightSurfacesTab(const Rc<DxvkContext>& ctx) {
    RemixGui::Combo("Emitted Colour", &DusklightEmissive::colorSourceObject(),
                    "Reconstructed Albedo\0Albedo Texture\0Presented Colour\0");
    RemixGui::DragFloat("Emissive Brightness", &DusklightEmissive::brightnessObject(), 0.02f, 0.f, 50.f);

    const bool emissiveOpen = RemixGui::CollapsingHeader("Emissive tuning", collapsingHeaderClosedFlags);
    dusklightHeaderTip("Should not need touching.\n\n"
                       "Normalise By Own Brightness is what makes Emissive Brightness a target rather than a\n"
                       "multiplier, and it is the reason a dropped rupee once came out brighter than the\n"
                       "lava. The two glow thresholds decide whether an authored colour counts as a glow at\n"
                       "all, and either one alone is enough - a glow is a strong colour or it is\n"
                       "near-white-hot, while a muted mid-tone is a surface colour.");
    if (emissiveOpen) {
      ImGui::Indent();
      RemixGui::DragFloat("Normalise By Own Brightness", &DusklightEmissive::brightnessLumaWeightObject(), 0.01f, 0.f, 1.f);
      RemixGui::DragFloat("Radiance Ceiling", &DusklightEmissive::maxRadianceObject(), 0.5f, 0.f, 256.f, "%.1f");
      RemixGui::DragFloat("Saturation Counts As Glow", &DusklightEmissive::glowChromaObject(), 0.01f, 0.f, 1.f);
      RemixGui::DragFloat("Brightness Counts As Glow", &DusklightEmissive::glowLumaObject(), 0.01f, 0.f, 1.f);
      ImGui::Unindent();
    }

    const bool waterOpen = RemixGui::CollapsingHeader("Water", collapsingHeaderClosedFlags);
    dusklightHeaderTip("The game marks its own water draws by material name and carries the mark per draw.\n"
                       "Transmittance Distance is the one to tune first: it sets how far light travels\n"
                       "before reaching the transmittance colour, so it decides how quickly water reads as\n"
                       "deep. The layer names are the game's own, romanized from the Japanese original.");
    if (waterOpen) {
      ImGui::Indent();
      // Everything below is inert while the master Water switch is off, and the pair stays wholly
      // inside this header.
      ImGui::BeginDisabled(!DusklightWater::enable());
      RemixGui::DragFloat("Index of Refraction", &DusklightWater::refractiveIndexObject(), 0.005f, 1.0f, 3.0f);
      RemixGui::ColorEdit3("Transmittance Color", &DusklightWater::transmittanceColorObject());
      RemixGui::DragFloat("Transmittance Distance", &DusklightWater::transmittanceMeasurementDistanceObject(), 1.0f, 0.001f, 65504.0f);
      RemixGui::Checkbox("Thin Walled", &DusklightWater::thinWalledObject());
      RemixGui::DragFloat("Thin Wall Thickness", &DusklightWater::thinWallThicknessObject(), 0.01f, 0.001f, 65504.0f);
      RemixGui::Checkbox("Animate Texcoords", &DusklightWater::animateTexcoordsObject());
      RemixGui::DragFloat("UV Tiling", &DusklightWater::uvTilingObject(), 0.05f, 0.01f, 256.0f);
      RemixGui::DragFloat2("Scroll Speed", &DusklightWater::scrollSpeedObject(), 0.001f, -1.0f, 1.0f);
      RemixGui::DragFloat("Normal Intensity", &TranslucentMaterialOptions::normalIntensityObject(), 0.01f, 0.0f, 4.0f);

      RemixGui::Separator();
      ImGui::TextUnformatted("Hide water layers");
      RemixGui::Checkbox("Shimmer (mera)", &DusklightWater::hideShimmerLayerObject());
      RemixGui::Checkbox("Waves (nami)", &DusklightWater::hideWavesLayerObject());
      RemixGui::Checkbox("Shoreline (mizugiwa)", &DusklightWater::hideShorelineLayerObject());
      RemixGui::Checkbox("Murk (nigori)", &DusklightWater::hideMurkLayerObject());
      RemixGui::Checkbox("Additive passes (kasan)", &DusklightWater::hideAdditiveLayerObject());

      RemixGui::Separator();
      ImGui::TextUnformatted("Exceptions");
      RemixGui::Checkbox("Shoreline Keeps Its Blend", &DusklightWater::shorelineAsBlendObject());
      RemixGui::Checkbox("Waves Keep Their Blend", &DusklightWater::wavesAsBlendObject());
      RemixGui::Checkbox("Apply To Replaced Materials", &DusklightWater::applyToReplacementsObject());
      RemixGui::Checkbox("Hide Projected Reflection Layer", &DusklightWater::hideProjectedLayerObject());
      ImGui::EndDisabled();
      ImGui::Unindent();
    }

    const bool packOpen = RemixGui::CollapsingHeader("HD texture pack", collapsingHeaderClosedFlags);
    dusklightHeaderTip("The pack never travels through D3D9, so the game's own textures are what Remix\n"
                       "hashes and what the texture categorization list shows. Tags and rtx.conf categories\n"
                       "are unaffected by installing, changing or removing a pack.");
    if (packOpen) {
      ImGui::Indent();
      RemixGui::Checkbox("Apply To HUD", &DusklightTexRep::applyToRasterObject());
      RemixGui::Checkbox("Hold At Full Resolution", &DusklightTexRep::forceFullMipsObject());

      // "Handed over" and "substituted" failing separately are quite different bugs and read
      // identically as "the pack does nothing", so each is named rather than left to be inferred.
      // The counters these read from are on the Readouts tab; only the diagnosis is here, because
      // the diagnosis is a state that is wrong and the counters are reference.
      const auto& texRepStats = dusklightTexRep::stats();
      if (!DusklightEnv::texrepEnabled()) {
        ImGui::TextWrapped(
          "The game is not handing a pack over. Either texture replacements are off in its config, its "
          "texture_replacements directory is empty, or the game build predates this feature - the status "
          "line at the top of this window says whether it is connected at all.");
      } else if (DusklightEnv::texrepCreated() == 0 && DusklightEnv::texrepEntries() > 0) {
        ImGui::TextWrapped(
          "The game selected replacements but has handed none over yet. It spreads creation over frames "
          "at launch; if this stays at zero, its D3D9 device never registered with Remix.");
      } else if (texRepStats.handlesSeen == 0 && DusklightEnv::texrepCreated() > 0) {
        ImGui::TextWrapped(
          "Materials were handed over but no draw is tagged with one, so the D3D9 stream is not carrying "
          "the index. That is an aurora older than this build of Remix, or a scene whose textures simply "
          "have no replacements in the pack.");
      } else if (texRepStats.missing > 0) {
        ImGui::TextWrapped(
          "Some draws are tagged with an index Remix has no material for. The two sides disagree about "
          "the pack - most likely the game reloaded its registry after handing it over.");
      }
      ImGui::Unindent();
    }

    const bool reportsOpen = RemixGui::CollapsingHeader("Reports", collapsingHeaderClosedFlags);
    dusklightHeaderTip("Every log this tab can ask for, in one place. All of them are bounded and all of\n"
                       "them are written to the session log, which is what a test session sends back\n"
                       "instead of a description.");
    if (reportsOpen) {
      ImGui::Indent();
      RemixGui::Checkbox("Log Emissive Candidates", &DusklightEmissive::logObject());
      RemixGui::Checkbox("Log Material Translation Report", &DusklightMatrep::matrepObject());
      RemixGui::Checkbox("Log Water Materials", &DusklightWater::logObject());
      RemixGui::Checkbox("Log HD Pack Report Next Frame", &DusklightTexRep::reportObject());
      if (ImGui::Button("Log Texture Category Report", ImVec2(-1, 0))) {
        DusklightCatrep::catrepCommit.setDeferred(DusklightCatrep::catrepCommit() + 1);
      }
      if (ImGui::IsItemHovered()) {
        RemixGui::SetTooltipUnformatted(
          RemixGui::BuildRtxOptionTooltip(&DusklightCatrep::catrepCommitObject()).c_str());
      }
      ImGui::Unindent();
    }
  }

  // The game's own geometry and tuning switches. Flat, no headers at all - eleven controls, one
  // screen, and every one of them is a thing you turn on to look at something.
  void ImGUI::showDusklightSceneTab(const Rc<DxvkContext>& ctx) {
    RemixGui::Checkbox("Disable Frustum Culling", &DusklightGame::disableFrustumCullingObject());
    RemixGui::Checkbox("Hide Sky Billboards (diagnostic)", &DusklightGame::hideSkyBillboardsObject());
    ImGui::Indent();
    RemixGui::Checkbox("...Including The Stars", &DusklightGame::hideStarBillboardsObject());
    ImGui::Unindent();
    RemixGui::Checkbox("Hide Game Sky Dome", &DusklightGame::hideVrboxObject());
    RemixGui::Checkbox("Per-Blade Grass", &DusklightGame::perBladeGrassObject());
    RemixGui::Checkbox("Per-Flower Blossoms", &DusklightGame::perBladeFlowersObject());
    RemixGui::Checkbox("Hide Epona Dash Effect", &DusklightGame::hideDashEffectObject());
    RemixGui::Checkbox("Game's Blob Shadows", &DusklightGame::blobShadowsObject());
    RemixGui::Checkbox("Recording Mode", &DusklightGame::recordingModeObject());

    RemixGui::Separator();
    // Three values the game's original artists had sliders for, at the ranges they worked in.
    // Their tuning panel is compiled out of every build of this port, so these have not been
    // reachable by anyone since the game shipped; each starts at the value the game ships.
    // Clock Rate, the third of them, is on the Go tab beside the clock it scales.
    ImGui::TextUnformatted("The game's own tuning values");
    RemixGui::DragFloat("Water Surface Gloss##dusklight", &DusklightGame::waterSurfaceShineObject(), 0.01f, 0.f, 1.f, "%.2f");
    RemixGui::DragFloat("Grass Light Influence##dusklight", &DusklightGame::grassLightInfluenceObject(), 0.01f, 0.f, 2.f, "%.2f");
  }

  void ImGUI::showDusklightControlsTab(const Rc<DxvkContext>& ctx) {
    // This tab decides nothing. It sends indices and two commit counters; the game captures the
    // press, resolves the conflict and pushes back both the resulting table and a line describing
    // what it did. Everything below is display.
    //
    // The ownership is the whole design and it is worth stating where it is easiest to break.
    // The overlay cannot see the game's whole input picture - only the binds it is handed - so a
    // check made here would allow a conflict with anything outside that list. Checking in both
    // places would be worse: two rules that can disagree now and will drift the first time one is
    // edited.
    const bool feedLive = DusklightEnv::enable();
    if (!feedLive) {
      ImGui::TextWrapped("Waiting for the game's environment feed. The bind table lives in the game, so there is "
                         "nothing to show until it connects.");
      return;
    }

    const std::vector<std::string> actions = splitPipes(DusklightEnv::bindActions());
    const std::vector<std::string> buttons = splitPipes(DusklightEnv::bindButtons());

    if (actions.empty()) {
      ImGui::TextWrapped("The game has not sent its bind table yet. A frame or two of lag after connecting is "
                         "expected; longer than that means the game build predates this tab.");
      return;
    }

    int port = std::clamp(DusklightGame::port(), 0, 3);
    static const char* kPorts[] = { "Port 1", "Port 2", "Port 3", "Port 4" };
    if (RemixGui::Combo("Controller##dusklightBind", &port, kPorts, IM_ARRAYSIZE(kPorts))) {
      DusklightGame::port.setDeferred(port);
    }
    ImGui::TextWrapped(DusklightEnv::bindKeyboard()
                       ? "This port is driven by a keyboard, so binds are keys."
                       : "This port is driven by a gamepad, so binds are controller buttons.");

    RemixGui::Separator();

    const int selected = std::clamp(DusklightGame::actionIndex(), 0, static_cast<int>(actions.size()) - 1);
    const bool capturing = DusklightEnv::bindCapturing();

    // Whichever action is selected is the one Rebind and Clear act on, so the selection has to be
    // visible at a glance rather than inferred from a dropdown somewhere else.
    for (size_t i = 0; i < actions.size(); ++i) {
      const bool isSelected = static_cast<int>(i) == selected;
      const std::string bound = i < buttons.size() ? buttons[i] : std::string("?");

      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(actions[i].c_str(), isSelected, 0, ImVec2(220.0f, 0.0f))) {
        DusklightGame::actionIndex.setDeferred(static_cast<int>(i));
      }
      ImGui::SameLine(240.0f);
      ImGui::TextUnformatted(bound.c_str());
      ImGui::PopID();
    }

    RemixGui::Separator();

    ImGui::BeginDisabled(capturing);
    if (ImGui::Button("Rebind")) {
      DusklightGame::captureCommit.setDeferred(DusklightGame::captureCommit() + 1);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(
        "Binding DISPLACES rather than refuses: the key you press is always taken, and whatever else\n"
        "held it on this port is left unbound and named in the status line. Refusing instead would\n"
        "leave you pressing a key and watching nothing happen, with no way to tell why.\n\n"
        "Escape unbinds, matching the game's own controller config screen.\n\n"
        "Capture keeps working while this overlay is blocking input from the game, because the game\n"
        "reads the device directly for this one purpose - no carve-out in the input blocking was\n"
        "needed, which is worth knowing before anyone 'fixes' that asymmetry.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
      DusklightGame::clearCommit.setDeferred(DusklightGame::clearCommit() + 1);
    }
    ImGui::EndDisabled();

    if (capturing) {
      ImGui::SameLine();
      ImGui::TextUnformatted("listening...");
    }

    // A line of prose the game wrote about what it just did. It is state, so it stays on screen.
    const std::string status = DusklightEnv::bindStatus();
    if (!status.empty()) {
      ImGui::TextWrapped("%s", status.c_str());
    }
  }

  void ImGUI::showDusklightModsTab(const Rc<DxvkContext>& ctx) {
    // This tab loads nothing and runs nothing. The game owns its mod loader; all that crosses is
    // an inventory in one direction and a list of ids in the other. That split is deliberate -
    // a runtime that could activate game code from a config string would be a much larger thing
    // than a checkbox.
    //
    // The game's own mod window is never drawn under the fixed function backend, which is the
    // whole reason this exists.
    const bool feedLive = DusklightEnv::enable();
    if (!feedLive) {
      ImGui::TextWrapped("Waiting for the game's environment feed. The mod list lives in the game, so there is "
                         "nothing to show until it connects.");
      return;
    }

    if (!DusklightEnv::modsRunning()) {
      // This used to name a protocol number of its own because it could not reach the real one.
      // It can now: the status line at the top of the window reports the actual agreement, in
      // both directions, from the single kRequiredProtocol definition.
      ImGui::TextWrapped("The game has not published a mod inventory. Either its discovery has not run yet - a "
                         "frame or two after connecting is expected - or this game build predates the mod wire, "
                         "which the status line at the top of this window would show as a protocol skew.");
      return;
    }

    ImGui::TextWrapped("Mods are ALWAYS off when the game starts, whatever was ticked last session and whatever "
                       "the game's own config.json says. Nothing here is written to rtx.conf.");
    RemixGui::Separator();

    if (DusklightEnv::modCount() <= 0) {
      ImGui::TextWrapped("Discovery ran and found no mods. That is a different state from the one above: the "
                         "loader is working, the mods directory is empty or nothing in it parsed.");
      return;
    }

    // Records are ';' delimited, fields within a record '|' delimited:
    //   id | display name | native status | active | failed
    const std::string packed = DusklightEnv::modList();

    // What is currently ticked. The sentinel is "nothing", not the empty string - see
    // kModsNoneSentinel in rtx_dusklight_game.h for why an empty one could never arrive.
    std::string enabledPacked = DusklightGame::modsEnabled();
    if (enabledPacked == kModsNoneSentinel) {
      enabledPacked.clear();
    }
    std::vector<std::string> enabled = splitPipes(enabledPacked);

    const auto isEnabled = [&enabled](const std::string& id) {
      return std::find(enabled.begin(), enabled.end(), id) != enabled.end();
    };

    bool changed = false;
    size_t shown = 0;

    size_t recordStart = 0;
    while (recordStart <= packed.size()) {
      const size_t recordEnd = packed.find(';', recordStart);
      const std::string record =
        packed.substr(recordStart, recordEnd == std::string::npos ? std::string::npos : recordEnd - recordStart);

      if (!record.empty()) {
        const std::vector<std::string> fields = splitPipes(record);

        if (fields.size() >= 2) {
          const std::string& id = fields[0];
          const std::string& name = fields[1];
          const std::string nativeStatus = fields.size() > 2 ? fields[2] : std::string("unknown");
          const bool active = fields.size() > 3 && fields[3] == "1";
          const bool failed = fields.size() > 4 && fields[4] == "1";

          ++shown;
          ImGui::PushID(static_cast<int>(shown));

          bool ticked = isEnabled(id);
          if (ImGui::Checkbox(name.empty() ? id.c_str() : name.c_str(), &ticked)) {
            changed = true;
            if (ticked) {
              enabled.push_back(id);
            } else {
              enabled.erase(std::remove(enabled.begin(), enabled.end(), id), enabled.end());
            }
          }

          ImGui::Indent();
          ImGui::TextWrapped("id %s - game reports %s%s", id.c_str(),
                             failed ? "LOAD FAILED" : (active ? "running" : "not running"),
                             ticked && !active && !failed ? ", request pending" : "");

          // The reason mods were switched off entirely under this backend, shown next to the mod
          // it applies to rather than as a banner nobody reads. The D3D9 path never initializes
          // WebGPU, so a native mod that reaches for the renderer takes the process down as it
          // loads - and the moment it loads is the moment the box below is ticked.
          if (nativeStatus != "none") {
            ImGui::TextWrapped("NATIVE (%s). The fixed function backend never initializes WebGPU, so a native "
                               "mod that touches the renderer can take the process down the instant it is "
                               "enabled. Save first.", nativeStatus.c_str());
          }
          ImGui::Unindent();
          ImGui::PopID();
        }
      }

      if (recordEnd == std::string::npos) {
        break;
      }
      recordStart = recordEnd + 1;
    }

    if (shown == 0) {
      ImGui::TextWrapped("The game reported %d mod(s) but none of the records parsed. That is a wire mismatch "
                         "rather than a mod problem - the expected shape is id|name|native|active|failed, "
                         "records separated by ';'.", DusklightEnv::modCount());
    }

    if (changed) {
      std::string packedEnabled;
      for (const std::string& id : enabled) {
        if (!packedEnabled.empty()) {
          packedEnabled += '|';
        }
        packedEnabled += id;
      }
      // Never the empty string: it would not cross, and the last mod would never turn off.
      DusklightGame::modsEnabled.setDeferred(packedEnabled.empty() ? std::string(kModsNoneSentinel)
                                                                   : packedEnabled);
    }

    RemixGui::Separator();
    if (ImGui::Button("Disable All")) {
      DusklightGame::modsEnabled.setDeferred(std::string(kModsNoneSentinel));
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
        "Turning a mod off asks the game to shut it down and unload it. Some mods cannot be unloaded\n"
        "cleanly once resident - the game reports what actually happened in the line under each one,\n"
        "which is the state to trust rather than the checkbox.");
    }
  }

  // Every rtx.dusklight.env.* diagnostic the window shows, and nothing else. They were interleaved
  // with the settings in seven places, one of the blocks was open by default at the bottom of the
  // busiest tab, and the colpat crossfade was printed twice in two different sections.
  //
  // They belong together because they are all the same kind of thing: pushed by the game, all
  // NoSave, all one-directional, and none of them editable. Separating them by a tab boundary
  // rather than by a header means a diagnostic can never be mistaken for a control.
  //
  // What this costs is adjacency, and it is a real cost rather than a free win: the effect-light
  // chain is read while moving Master Intensity, and the HD pack's two counter rows are the first
  // step of the pack's own test recipe. The diagnoses that go WITH those numbers stayed where the
  // controls are - only the numbers moved - and whether that is the right split is the one part of
  // this restructure a test session should decide rather than a document.
  void ImGUI::showDusklightReadoutsTab(const Rc<DxvkContext>& ctx) {
    auto common = ctx->getCommonObjects();

    if (!DusklightEnv::enable()) {
      ImGui::TextWrapped("Nothing reported yet - the game is not connected. Every line on this tab is a value "
                         "the game pushes, so none of them exist until it is.");
      return;
    }

    ImGui::TextUnformatted("Sun and moon");
    if (DusklightEnv::sunActive()) {
      // Azimuth and elevation are functions of the game's time of day and nothing else, so they
      // answer "is the light following the player?" at a glance - which is the first thing anyone
      // asks of a sun that was just made distant. If they hold still while the player runs in a
      // circle, the direction is not tied to the player.
      ImGui::Text("Drawing: %s   azimuth %6.1f deg   elevation %5.1f deg   fade %.2f",
                  DusklightEnv::sunIsDay() ? "SUN" : "MOON",
                  DusklightEnv::sunAzimuth(), DusklightEnv::sunElevation(),
                  DusklightEnv::sunFade());
    } else {
      ImGui::TextUnformatted("Not drawing: the game reports no sun or moon in this area.");
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Effect lights");
    // The chain, in the order a light can be lost: alive -> drawn in a world pass -> passed the
    // rule -> merged into a site -> reached Remix. Printing all of it means the step something
    // was lost at is visible without anyone having to describe a scene.
    ImGui::Text("emitters %d  ->  considered %d  ->  candidates %d  ->  sites %d  ->  drawn %d",
                DusklightEnv::effLightsEmitters(), DusklightEnv::effLightsConsidered(),
                DusklightEnv::effLightsCandidates(), DusklightEnv::effLightsSites(),
                DusklightEnv::effLightsDrawn());
    ImGui::Text("from the game: %d    game lights with no effect: %d    culled: %d    refused by name: %d",
                DusklightEnv::effLightsDerived(), DusklightEnv::effLightsOrphans(),
                DusklightEnv::effLightsCulled(), DusklightEnv::effLightsExcluded());
    ImGui::Text("game lights available to copy (point/spot): %s",
                DusklightEnv::effLightsVanilla().c_str());
    // What each light was made of, and what kind of thing each one is.
    ImGui::Text("solved from authored values: %s", DusklightEnv::effLightsAuthored().c_str());
    ImGui::Text("by class: %s", DusklightEnv::effLightsClasses().c_str());
    // The Shadow Insect answer, without a log. seen>0 lit==0 is the negative result.
    ImGui::Text("sparks: %s", DusklightEnv::effLightsSparks().c_str());

    RemixGui::Separator();
    ImGui::TextUnformatted("Room lights");
    ImGui::Text("Authored in this room: %d   drawn this frame: %d   tracked: %d",
                DusklightEnv::roomLightsFound(), DusklightEnv::roomLightsDrawn(),
                DusklightEnv::roomLightsTracked());
    ImGui::Text("Of those drawn, %d carry a cone; %d used a ring cone that had to be dropped",
                DusklightEnv::roomLightsShaped(), DusklightEnv::roomLightsUnshapeable());

    RemixGui::Separator();
    ImGui::TextUnformatted("HD texture pack");
    // The split is the point: the first row is what the GAME says it did, the second is what
    // REMIX did with it. "The game never handed it over" and "the fork ignored it" are different
    // bugs that both read as "the pack does nothing".
    {
      const auto& texRepStats = dusklightTexRep::stats();
      ImGui::Text("Game: %d selected, %d handed over, %d skipped",
                  DusklightEnv::texrepEntries(), DusklightEnv::texrepCreated(),
                  DusklightEnv::texrepSkipped());
      ImGui::Text("Remix: %u draws tagged, %u substituted (%u of them HUD), %u still loading, %u unknown",
                  texRepStats.handlesSeen, texRepStats.applied, texRepStats.appliedRaster,
                  texRepStats.pending, texRepStats.missing);
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Environment response");
    ImGui::Text("Bloom: %s   threshold %.3f   blur %.0f / %.0f",
                DusklightEnv::bloomEnable() ? "on" : "off", DusklightEnv::bloomThreshold(),
                DusklightEnv::bloomBlurSize(), DusklightEnv::bloomBlurRatio());
    {
      const Vector3 actorAmbient = DusklightEnv::actorAmbient();
      const Vector3 bgAmbient = DusklightEnv::bgAmbient();
      ImGui::Text("Actor ambient: %.3f, %.3f, %.3f", actorAmbient.x, actorAmbient.y, actorAmbient.z);
      ImGui::Text("BG ambient:    %.3f, %.3f, %.3f", bgAmbient.x, bgAmbient.y, bgAmbient.z);
    }

    // The three background alphas, carried since protocol 13. They share a struct with the BG
    // ambient above and are not ambient at all - the game uses them as material constants on
    // its water, murk and faked-fog surfaces. Shown here because nothing consumes them yet, so
    // this readout is the only way to learn what an area asks for. A negative value is the
    // game not reporting, deliberately distinct from a reported 0, which is authored.
    {
      const float bgWaterA = DusklightEnv::bgWaterAlpha();
      if (bgWaterA < 0.0f) {
        ImGui::TextUnformatted("BG alphas:     not reported - this game build predates protocol 13");
      } else {
        ImGui::Text("BG alphas:     water %.3f   aux %.3f   fake fog %.3f",
                    bgWaterA, DusklightEnv::bgAuxAlpha(), DusklightEnv::bgFakeFogAlpha());
      }
    }

    ImGui::Text("Mono overlay:  %.2f", DusklightEnv::monoAmount());

    // The game's fog numbers live in stage data rather than in its code, so this readout is the
    // only way to learn what an area actually asks for. Stand somewhere that looks wrong and read
    // them.
    {
      const Vector3 fogColor = DusklightEnv::fogColor();
      ImGui::Text("Fog: %s   %.0f .. %.0f units",
                  DusklightEnv::fogActive() ? "on" : "off",
                  DusklightEnv::fogStartZ(), DusklightEnv::fogEndZ());
      ImGui::Text("Fog colour:    %.3f, %.3f, %.3f", fogColor.x, fogColor.y, fogColor.z);
    }

    {
      const Vector3 skyColor = DusklightEnv::skyColor();
      const Vector3 kasumiInner = DusklightEnv::kasumiInner();
      const Vector3 kasumiOuter = DusklightEnv::kasumiOuter();
      // colpat is a crossfade, not a state: the game holds an outgoing pattern, an incoming one
      // and a 0..1 ratio, and every colour above is that lerp. Shown as "prev -> curr @ ratio".
      // Outside a transition the two are equal and the ratio is 1.00, so anything else on screen
      // means a weather, room or event change is in progress right now.
      //
      // THE ONLY COPY. The atmosphere's resolved block below printed the same three values until
      // 2026-08-17, roughly 700 source lines away, with a different set of neighbouring fields.
      ImGui::Text("Sky: %s   colpat %d -> %d @ %.2f   moya %d @ %.0f",
                  DusklightEnv::skyHidden() ? "none (interior)" : "present",
                  DusklightEnv::colpatPrev(), DusklightEnv::colpat(), DusklightEnv::colpatBlend(),
                  DusklightEnv::moyaMode(), DusklightEnv::moyaCount());
      ImGui::Text("Sky colour:    %.3f, %.3f, %.3f", skyColor.x, skyColor.y, skyColor.z);
      ImGui::Text("Haze in / out: %.3f, %.3f, %.3f  /  %.3f, %.3f, %.3f",
                  kasumiInner.x, kasumiInner.y, kasumiInner.z,
                  kasumiOuter.x, kasumiOuter.y, kasumiOuter.z);
    }

    RemixGui::Separator();
    ImGui::TextUnformatted("Atmosphere, resolved");
    common->metaDusklightAtmosphere().showImguiReadouts();
  }

  void ImGUI::showEnhancementsWindow(const Rc<DxvkContext>& ctx) {
    ImGui::PushItemWidth(largeUiMode() ? m_largeWindowWidgetWidth : m_regularWindowWidgetWidth);

    m_capture->show(ctx);
    
    if(RemixGui::CollapsingHeader("Enhancements", collapsingHeaderFlags | ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      showEnhancementsTab(ctx);
      ImGui::Unindent();
    }
    
    // Graph Visualization Section
    RemixGui::Separator();
    if (RemixGui::CollapsingHeader("Remix Logic", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Indent();
      m_graphGUI->showGraphVisualization(ctx);
      ImGui::Unindent();
    }
  }
  
  void ImGUI::showEnhancementsTab(const Rc<DxvkContext>& ctx) {
    if (!ctx->getCommonObjects()->getSceneManager().areAllReplacementsLoaded()) {
      ImGui::Text("No USD enhancements detected, the following options have been disabled.  See documentation for how to use enhancements with Remix.");
    }

    ImGui::BeginDisabled(!ctx->getCommonObjects()->getSceneManager().areAllReplacementsLoaded());
    RemixGui::Checkbox("Enable Enhanced Assets", &RtxOptions::enableReplacementAssetsObject());
    {
      ImGui::Indent();
      ImGui::BeginDisabled(!RtxOptions::enableReplacementAssets());

      RemixGui::Checkbox("Enable Enhanced Materials", &RtxOptions::enableReplacementMaterialsObject());
      RemixGui::Checkbox("Enable Enhanced Meshes", &RtxOptions::enableReplacementMeshesObject());
      RemixGui::Checkbox("Enable Enhanced Lights", &RtxOptions::enableReplacementLightsObject());

      ImGui::EndDisabled();
      ImGui::Unindent();
    }
    ImGui::EndDisabled();
    RemixGui::Separator();
    RemixGui::Checkbox("Highlight Legacy Materials (flash red)", &RtxOptions::useHighlightLegacyModeObject());

  }

  namespace {
    std::optional<float> calculateTextureCategoryHeight(bool onlySelected, const char* uniqueId,
                                                        uint32_t numThumbnailsPerRow, float thumbnailSize) {
      constexpr float HeightLimit = 600;
      if (strcmp(uniqueId, Uncategorized) == 0) {
        return HeightLimit;
      }

      const RtxOption<fast_unordered_set>* selected = nullptr;
      if (onlySelected) {
        const auto found = std::find_if(rtxTextureOptions.begin(), rtxTextureOptions.end(),
          [&](const RtxTextureOption& o) {
            return strcmp(o.uniqueId, uniqueId) == 0;
          });
        if (found == rtxTextureOptions.end() || !found->textureSetOption) {
          assert(0);
          return {};
        }
        selected = found->textureSetOption;
      }

      float height = -1;
      uint32_t textureCount = 0;
      for (const auto& [texHash, texImgui] : g_imguiTextureMap) {
        if (selected) {
          if (!selected->containsHash(texHash)) {
            continue;
          }
        }
        textureCount++;

        uint32_t rows = (textureCount + numThumbnailsPerRow - 1) / numThumbnailsPerRow;
        height = rows * thumbnailSize + 16.0f;
        if (height >= HeightLimit) {
          return HeightLimit;
        }
      }
      if (height <= 0) {
        return {};
      }
      assert(height >= thumbnailSize);
      return height;
    }
  }

  void ImGUI::showSetupWindow(const Rc<DxvkContext>& ctx) {
    static auto spacing = []() {
      ImGui::Dummy({ 0,2 });
    };
    static auto separator = []() {
      spacing();
      RemixGui::Separator();
      spacing();
    };

    constexpr ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_NoCloseWithMiddleMouseButton;
    constexpr ImGuiTabItemFlags tab_item_flags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;
    if (!ImGui::BeginTabBar("##showSetupWindow", tab_bar_flags)) {
      return;
    }
    ImGui::PushItemWidth(largeUiMode() ? m_largeWindowWidgetWidth : m_regularWindowWidgetWidth);

    texture_popup::lastOpenCategoryActive = false;

    const float thumbnailScale = RtxOptions::textureGridThumbnailScale();
    const float thumbnailSize = (120.f * thumbnailScale);
    const float thumbnailSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float thumbnailPadding = ImGui::GetStyle().CellPadding.x;
    const uint32_t numThumbnailsPerRow = uint32_t(std::max(1.f, (m_windowWidth - 18.f) / (thumbnailSize + thumbnailSpacing + thumbnailPadding * 2.f)));

    if (IMGUI_ADD_TOOLTIP(ImGui::BeginTabItem("Step 1: Categorize Textures", nullptr, tab_item_flags), "Select texture definitions for Remix")) {
      spacing();
      RemixGui::Checkbox("Preserve discarded textures", &RtxOptions::keepTexturesForTaggingObject());
      separator();

      // set thumbnail size
      {
        constexpr int step = 25;
        int percentage = static_cast<int>(round(100.f * RtxOptions::textureGridThumbnailScale()));
        bool changed = false;

        float buttonsize = ImGui::GetFont() ? ImGui::GetFont()->FontSize * 1.3f : 4;
        if (ImGui::Button("-##thumbscale", { buttonsize, buttonsize })) {
          percentage = std::max(25, percentage - step);
          changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("+##thumbscale", { buttonsize, buttonsize })) {
          percentage = std::min(300, percentage + step);
          changed = true;
        }
        ImGui::SameLine();
        ImGui::Text("Texture Thumbnail Scale: %d%%", percentage);
        if (ImGui::IsItemHovered()) {
          RemixGui::SetTooltipUnformatted(RemixGui::BuildRtxOptionTooltip(&RtxOptions::textureGridThumbnailScale).c_str());
        }

        if (changed) {
          RemixGui::CheckRtxOptionPopups(&RtxOptions::textureGridThumbnailScaleObject());
          RtxOptions::textureGridThumbnailScale.setDeferred(static_cast<float>(percentage) / 100.f);
        }
      }

      RemixGui::Checkbox("Split Texture Category List", &showLegacyTextureGuiObject());
      ImGui::BeginDisabled(!showLegacyTextureGui());
      RemixGui::Checkbox("Only Show Assigned Textures in Category Lists", &legacyTextureGuiShowAssignedOnlyObject());
      ImGui::EndDisabled();

      separator();

      // One-time migration button: only show if there are texture hashes incorrectly stored in user.conf
      {
        const RtxOptionLayer* userLayer = RtxOptionLayer::getUserLayer();
        bool hasTexturesInUserConf = false;
        if (userLayer) {
          for (const auto& rtxOption : rtxTextureOptions) {
            if (rtxOption.textureSetOption && rtxOption.textureSetOption->hasValueInLayer(userLayer)) {
              hasTexturesInUserConf = true;
              break;
            }
          }
        }
        
        if (hasTexturesInUserConf) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.4f, 0.1f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.5f, 0.2f, 1.0f));
          if (IMGUI_ADD_TOOLTIP(ImGui::Button("Migrate user.conf Textures"), 
              "One-time fix: Move texture hashes from user.conf to rtx.conf.\n\n"
              "In previous versions, texture categories were incorrectly saved to user.conf.\n"
              "This button migrates them to rtx.conf where they belong.\n\n"
              "After clicking, use 'Save rtx.conf Layer' to write changes to disk.")) {
            RtxOptionLayer* rtxConfLayer = RtxOptionLayer::getRtxConfLayer();
            if (rtxConfLayer) {
              for (auto& rtxOption : rtxTextureOptions) {
                if (rtxOption.textureSetOption) {
                  rtxOption.textureSetOption->moveLayerValue(userLayer, rtxConfLayer);
                }
              }
              Logger::info("[RTX Option]: Migrated texture hashes from user.conf to rtx.conf");
            }
          }
          ImGui::PopStyleColor(2);
          separator();
        }
      }

      if (showLegacyTextureGui()) {
        ImGui::TextUnformatted(
          "Hover over an object on screen, or an icon in the grid below.\n"
          "Left click to toggle the currently active category.\n"
          "Right click to open a category selection window.");
      } else {
        ImGui::TextUnformatted(
          "Hover over an object on screen, or an icon in the grid below.\n"
          "Left click to open a category selection window.");
      }

      spacing();

      if (!showLegacyTextureGui()) {
        showTextureSelectionGrid(ctx, Uncategorized, numThumbnailsPerRow, thumbnailSize);
      } else {

        auto showLegacyGui = [&](const char* uniqueId, const char* displayName, const char* description) {
          const bool countOnlySelected = legacyTextureGuiShowAssignedOnly() && !(strcmp(uniqueId, Uncategorized) == 0);
          const auto height = calculateTextureCategoryHeight(countOnlySelected, uniqueId, numThumbnailsPerRow, thumbnailSize);
          if (!height.has_value()) {
            ImGui::BeginDisabled(true);
            const auto label = displayName + std::string { " [Empty]" };
            RemixGui::CollapsingHeader(label.c_str(), collapsingHeaderClosedFlags);
            ImGui::EndDisabled();
            return;
          }
          const bool isForToggle = (texture_popup::lastOpenCategoryId == uniqueId);
          if (isForToggle) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4 { 0.996078f, 0.329412f, 0.f, 1.f });
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4 { 0.996078f, 0.329412f, 0.f, 1.f });
          }
          if (IMGUI_ADD_TOOLTIP(RemixGui::CollapsingHeader(displayName, collapsingHeaderClosedFlags), description)) {
            if (height) {
              if (ImGui::IsItemToggledOpen() || texture_popup::lastOpenCategoryId.empty()) {
                // Update last opened category ID if texture category (RemixGui::CollapsingHeader) was just toggled open or if ID is empty
                texture_popup::lastOpenCategoryId = uniqueId;
              }

              showTextureSelectionGrid(ctx, uniqueId, numThumbnailsPerRow, thumbnailSize, *height);
            }
          }
          if (isForToggle) {
            ImGui::PopStyleColor(2);
          }
        };

        if (legacyTextureGuiShowAssignedOnly()) {
          showLegacyGui(Uncategorized, "Uncategorized", "Textures that are not assigned to any category");
          spacing();
        }
        for (const RtxTextureOption& category : rtxTextureOptions) {
          std::string categoryTooltip = buildTextureCategoryTooltip(category);
          showLegacyGui(category.uniqueId, category.displayName, categoryTooltip.c_str());
        }

        // Check if last saved category was closed this frame
        if (!texture_popup::lastOpenCategoryActive) {
          texture_popup::lastOpenCategoryId.clear();
        }
      }

      //separator();
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Step 2: Parameter Tuning", nullptr, tab_item_flags)) {
      spacing();
      RemixGui::DragFloat("Scene Unit Scale", &RtxOptions::sceneScaleObject(), 0.00001f, 0.00001f, FLT_MAX, "%.5f", sliderFlags);
      ImGui::Indent();
      ImGui::TextWrapped("1 cm  =  %.2f game units", RtxOptions::sceneScale());
      ImGui::Unindent();
      RemixGui::Checkbox("Scene Z-Up", &RtxOptions::zUpObject());
      RemixGui::Checkbox("Scene Left-Handed Coordinate System", &RtxOptions::leftHandedCoordinateSystemObject());
      fusedWorldViewModeCombo.getKey(&RtxOptions::fusedWorldViewModeObject());
      RemixGui::Separator();

      RemixGui::DragFloat("Unique Object Search Distance", &RtxOptions::uniqueObjectDistanceObject(), 0.01f, FLT_MIN, FLT_MAX, "%.3f", sliderFlags);
      RemixGui::Separator();

      RemixGui::DragFloat("Vertex Color Strength", &RtxOptions::vertexColorStrengthObject(), 0.001f, 0.0f, 1.0f);
      RemixGui::Checkbox("Vertex Color Is Baked Lighting", &RtxOptions::vertexColorIsBakedLightingObject());
      RemixGui::Checkbox("Ignore All Baked Lighting", &RtxOptions::ignoreAllVertexColorBakedLightingObject());
      RemixGui::Separator();

      if (RemixGui::CollapsingHeader("Heuristics", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::Checkbox("Orthographic Is UI", &D3D9Rtx::orthographicIsUIObject());
        RemixGui::Checkbox("Pre-Transformed Vertices Is UI", &D3D9Rtx::preTransformedVerticesIsUIObject());
        RemixGui::Checkbox("Allow Cubemaps", &D3D9Rtx::allowCubemapsObject());
        RemixGui::Checkbox("Always Calculate AABB (For Instance Matching)", &RtxOptions::enableAlwaysCalculateAABBObject());
        RemixGui::Checkbox("Skip Sky Fog Values", &RtxOptions::fogIgnoreSkyObject());
        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("Texture Parameters", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::DragFloat("Force Cutout Alpha", &RtxOptions::forceCutoutAlphaObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("World Space UI Background Offset", &RtxOptions::worldSpaceUiBackgroundOffsetObject(), 0.01f, -FLT_MAX, FLT_MAX, "%.3f", sliderFlags);
        RemixGui::Checkbox("Ignore last texture stage", &RtxOptions::ignoreLastTextureStageObject());
        RemixGui::Checkbox("Enable Multiple Stage Texture Factor Blending", &RtxOptions::enableMultiStageTextureFactorBlendingObject());
        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("Shader Support (Experimental)", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::Checkbox("Capture Vertices from Shader", &D3D9Rtx::useVertexCaptureObject());
        RemixGui::Checkbox("Capture Normals from Shader", &D3D9Rtx::useVertexCapturedNormalsObject());
        RemixGui::Separator();
        RemixGui::Checkbox("Use World Transforms", &D3D9Rtx::useWorldMatricesForShadersObject());
        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("View Model", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::Checkbox("Enable View Model", &RtxOptions::ViewModel::enableObject());
        RemixGui::SliderFloat("Max Z Threshold", &RtxOptions::ViewModel::maxZThresholdObject(), 0.0f, 1.0f);
        RemixGui::Checkbox("Virtual Instances", &RtxOptions::ViewModel::enableVirtualInstancesObject());
        RemixGui::Checkbox("Perspective Correction", &RtxOptions::ViewModel::perspectiveCorrectionObject());
        RemixGui::DragFloat("Scale", &RtxOptions::ViewModel::scaleObject(), 0.01f, 0.01f, 2.0f);
        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("Sky Tuning", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::DragFloat("Sky Brightness", &RtxOptions::skyBrightnessObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
        RemixGui::InputInt("First N Untextured Draw Calls", &RtxOptions::skyDrawcallIdThresholdObject(), 1, 1, 0);
        RemixGui::SliderFloat("Sky Min Z Threshold", &RtxOptions::skyMinZThresholdObject(), 0.0f, 1.0f);
        skyAutoDetectCombo.getKey(&RtxOptions::skyAutoDetectObject());

        if (RemixGui::CollapsingHeader("Advanced", collapsingHeaderClosedFlags)) {
          ImGui::Indent();

          RemixGui::Checkbox("Reproject Sky to Main Camera", &RtxOptions::skyReprojectToMainCameraSpaceObject());
          {
            ImGui::BeginDisabled(!RtxOptions::skyReprojectToMainCameraSpace());
            RemixGui::DragFloat("Reprojected Sky Scale", &RtxOptions::skyReprojectScaleObject(), 1.0f, 0.1f, 1000.0f);
            RemixGui::Checkbox("Force Auto-Detected Sky to Reproject", &RtxOptions::skyForceAutoDetectedToReprojectObject());
            ImGui::EndDisabled();
          }
          RemixGui::DragFloat("Sky Auto-Detect Unique Camera Search Distance", &RtxOptions::skyAutoDetectUniqueCameraDistanceObject(), 1.0f, 0.1f, 1000.0f);

          RemixGui::Checkbox("Force HDR sky", &RtxOptions::skyForceHDRObject());

          static const char* exts[] = { "256 (1.5MB vidmem)", "512 (6MB vidmem)", "1024 (24MB vidmem)",
            "2048 (96MB vidmem)", "4096 (384MB vidmem)", "8192 (1.5GB vidmem)" };

          static int extIdx;
          extIdx = std::clamp(bit::tzcnt(RtxOptions::skyProbeSide()), 8u, 13u) - 8;

          if (RemixGui::Combo("Sky Probe Extent", &extIdx, exts, IM_ARRAYSIZE(exts))) {
            RemixGui::CheckRtxOptionPopups(&RtxOptions::skyProbeSideObject());
          }
          RtxOptions::skyProbeSide.setDeferred(1 << (extIdx + 8));

          ImGui::Unindent();
        }
        ImGui::Unindent();
      }

      if (RtxOptions::Eye::showOptions() && RemixGui::CollapsingHeader("Eyes", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::Checkbox("Enable eye shading", &RtxOptions::Eye::enableObject());
        ImGui::BeginDisabled(!RtxOptions::Eye::enable());
        RemixGui::Checkbox("Detect texture-generation draw call as Eye", &RtxOptions::Eye::assumeViewTexgenModeAsEyeObject());
        RemixGui::DragFloat("Eye Whites Albedo Scale", &RtxOptions::Eye::eyeWhitesAlbedoScaleObject(), 0.01F);
        RemixGui::DragFloat("Normals: Eyeball Offset", &RtxOptions::Eye::eyeballSphereOffsetObject(), 0.001F);
        RemixGui::DragFloat("Normals: Cornea Offset", &RtxOptions::Eye::corneaSphereOffsetObject(), 0.001F);
        RemixGui::DragFloat("Iris Radius", &RtxOptions::Eye::irisRadiusObject(), 0.001F);
        RemixGui::DragFloat("Iris Depth", &RtxOptions::Eye::irisDepthObject(), 0.001F);
        ImGui::EndDisabled();
        ImGui::Unindent();
      }

      auto common = ctx->getCommonObjects();
      common->getSceneManager().getLightManager().showImguiSettings();

      showMaterialOptions();

      if (RemixGui::CollapsingHeader("Fog Tuning", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        ImGui::PushID("FogInfos");
        if (RemixGui::CollapsingHeader("Explanation", collapsingHeaderClosedFlags)) {
          ImGui::Indent();
          ImGui::TextWrapped("In D3D9, every draw call comes with its own fog settings."
            " In Remix pathtracing, all rays need to use the same fog setting."
            " So Remix will choose the earliest valid non-sky fog to use."

            "\n\nIn some games, fog can be used to indicate the player is inside some "
            "translucent medium, like being underwater.  In path tracing this is "
            "better represented as starting inside a translucent material.  To "
            "support this, you can copy one or more of the fog hashes listed below, "
            "and specify a translucent replacement material in your mod.usda."

            "\n\nThis replacement material should share transmittance and ior properties"
            " with your water material, but does not need any textures set."

            "\n\nReplacing a given fog state with a translucent material will disable that "
            "fog."
          );
          ImGui::Unindent();
        }

        constexpr static const char* fogModes[] = {
          "D3DFOG_NONE",
          "D3DFOG_EXP",
          "D3DFOG_EXP2",
          "D3DFOG_LINEAR",
        };

        {
          const std::lock_guard<std::mutex> lock(g_imguiFogMapMutex);
          for (const auto& pair : g_imguiFogMap) {
            const std::string hashString = hashToString(pair.first);
            const char* replaced = ctx->getCommonObjects()->getSceneManager().getAssetReplacer()->getReplacementMaterial(pair.first) ? 
              " (Replaced)" : "";
            const char* usedAsMain = (g_usedFogStateHash == pair.first) ? " (Used for Rendering)" : "";
            ImGui::Text("Hash: %s%s%s", hashString.c_str(), replaced, usedAsMain);
            const FogState& fog = pair.second;
            ImGui::Indent();

            if (ImGui::Button(str::format("Copy hash to clipboard##fog_list", hashString).c_str())) {
              ImGui::SetClipboardText(hashString.c_str());
            }
            if (uint32_t(fog.mode) < 4) {
              ImGui::Text("Mode: %s", fogModes[uint32_t(fog.mode)]);
            } else {
              ImGui::Text("Mode: unknown enum value: %u", uint32_t(fog.mode));
            }
            ImGui::Text("Color: %.2f %.2f %.2f", fog.color.r, fog.color.g, fog.color.b);
            ImGui::Text("Scale: %.2f", fog.scale);
            ImGui::Text("End: %.2f", fog.end);
            ImGui::Text("Density: %.2f", fog.density);
            
            ImGui::Unindent();
          }
        }
        ImGui::PopID();
        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("Input", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::Checkbox("Restore Mouse Position on Remix UI Close", &RtxOptions::restoreCursorPositionObject());
        ImGui::Unindent();
      }

      //separator();
      ImGui::EndTabItem();
    }

    ImGui::PopItemWidth();
    ImGui::EndTabBar();
  }

  void ImGUI::adjustStyleBackgroundAlpha(const float& alpha, ImGuiStyle* dst) {
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();
    ImVec4& currColor = style->Colors[ImGuiCol_WindowBg];
    currColor.w = alpha;
  }

  void ImGUI::updateWindowWidths() {
    // Developer menu
    m_windowWidth = largeUiMode() ? m_largeWindowWidth : m_regularWindowWidth + (compactGui() ? 0.0f : 42.0f);

    // User menu popup
    m_userWindowWidth = largeUiMode() ? m_largeUserWindowWidth : m_regularUserWindowWidth;
    m_userWindowHeight = largeUiMode() ? m_largeUserWindowHeight : m_regularUserWindowHeight;
  }

  void ImGUI::setToolkitStyle(ImGuiStyle* dst) {
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();

    style->Alpha = 1.0f;
    style->DisabledAlpha = 0.5f;

    style->WindowPadding = ImVec2(8.0f, 10.0f);
    style->FramePadding = compactGui() ? ImVec2(4.0f, 3.0f) : ImVec2(5.0f, 4.0f);
    style->CellPadding = ImVec2(5.0f, 4.0f);
    style->ItemSpacing = compactGui() ? ImVec2(8.0f, 4.0f) : ImVec2(3.0f, 5.0f);
    style->ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style->IndentSpacing = 10.0f;
    style->ColumnsMinSpacing = 10.0f;
    style->ScrollbarSize = 15.0f;
    style->GrabMinSize = 10.0f;

    style->WindowBorderSize = 1.5f;
    style->ChildBorderSize = 1.5f;
    style->PopupBorderSize = 1.5f;
    style->FrameBorderSize = 1.5f;
    style->TabBorderSize = 0.0f;

    style->WindowRounding = 0.0f;
    style->ChildRounding = 2.0f;
    style->FrameRounding = 3.0f;
    style->PopupRounding = 2.0f;
    style->ScrollbarRounding = 2.0f;
    style->GrabRounding = 1.0f;
    style->TabRounding = 2.0f;
    style->WindowMenuButtonPosition = ImGuiDir_None;

    style->WindowMinSize = ImVec2(32, 32);
    style->TouchExtraPadding = ImVec2(0, 0);
    style->LogSliderDeadzone = 4.0f;
    style->TabMinWidthForCloseButton = 0.0f;
    style->DisplayWindowPadding = ImVec2(19, 19);
    style->DisplaySafeAreaPadding = ImVec2(3, 3);
    style->MouseCursorScale = 1.0f;

    style->Colors[ImGuiCol_WindowBg] = ImVec4(0.188f, 0.188f, 0.188f, backgroundAlpha());
    style->Colors[ImGuiCol_PopupBg] = ImVec4(0.188f, 0.188f, 0.188f, 1.00f);
    style->Colors[ImGuiCol_Text] = ImVec4(0.8f, 0.8f, 0.8f, 1.00f);
    style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);
    style->Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.16f, 0.16f, 0.86f);
    style->Colors[ImGuiCol_Border] = ImVec4(0.34f, 0.34f, 0.34f, 1.0f);
    style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style->Colors[ImGuiCol_FrameBg] = ImVec4(0.188f, 0.188f, 0.188f, 1.00f);
    style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.30f, 0.35f, 1.00f);
    style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.10f, 0.15f, 0.16f, 0.59f);
    style->Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
    style->Colors[ImGuiCol_MenuBarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style->Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.31f, 0.31f, 0.78f);
    style->Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.78f, 0.78f, 0.78f, 0.33f);
    style->Colors[ImGuiCol_CheckMark] = ImVec4(0.53f, 0.53f, 0.53f, 1.00f);
    style->Colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.39f);
    style->Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.10f, 0.46f, 0.56f, 1.00f);
    style->Colors[ImGuiCol_Button] = ImVec4(0.121f, 0.129f, 0.141f, 1.00f);
    style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
    style->Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.44f, 0.45f, 1.00f);
    style->Colors[ImGuiCol_Header] = ImVec4(0.125f, 0.125f, 0.125f, 1.00f);
    style->Colors[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.25f, 0.27f, 0.78f);
    style->Colors[ImGuiCol_HeaderActive] = ImVec4(0.17f, 0.25f, 0.27f, 0.78f);
    style->Colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    style->Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.15f, 0.52f, 0.66f, 0.30f);
    style->Colors[ImGuiCol_SeparatorActive] = ImVec4(0.30f, 0.69f, 0.84f, 0.39f);
    style->Colors[ImGuiCol_ResizeGrip] = ImVec4(0.43f, 0.43f, 0.43f, 0.51f);
    style->Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.07f, 0.39f, 0.47f, 0.59f);
    style->Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.30f, 0.69f, 0.84f, 0.39f);
    style->Colors[ImGuiCol_Tab] = ImVec4(0.00f, 0.00f, 0.00f, 0.37f);
    style->Colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.33f, 0.36f, 1.00f);
    style->Colors[ImGuiCol_TabActive] = ImVec4(0.11f, 0.42f, 0.51f, 1.00f);
    style->Colors[ImGuiCol_TabUnfocused] = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
    style->Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.24f);
    style->Colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
    style->Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
    style->Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    style->Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_TableBorderLight] = ImVec4(0.00f, 0.00f, 0.00f, 0.54f);
    style->Colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.39f);
    style->Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.11f, 0.42f, 0.51f, 0.35f);
    style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_DragDropTarget] = ImVec4(0.00f, 0.51f, 0.39f, 0.31f);
    style->Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    style->Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.56f);
  }

  void ImGUI::setLegacyStyle(ImGuiStyle* dst) {
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();

    // Original ImGui theme from ImGuiStyle::ImGuiStyle()
    style->Alpha = 1.0f;
    style->DisabledAlpha = 0.60f;
    style->WindowPadding = ImVec2(8, 8);
    style->WindowRounding = 0.0f;
    style->WindowBorderSize = 1.0f;
    style->WindowMinSize = ImVec2(32, 32);
    style->WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style->WindowMenuButtonPosition = ImGuiDir_Left;
    style->ChildRounding = 0.0f;
    style->ChildBorderSize = 1.0f;
    style->PopupRounding = 0.0f;
    style->PopupBorderSize = 1.0f;
    style->FramePadding = compactGui() ? ImVec2(4, 3) : ImVec2(7, 5);
    style->FrameRounding = 0.0f;
    style->FrameBorderSize = 0.0f;
    style->ItemSpacing = compactGui() ? ImVec2(8, 4) : ImVec2(3, 5);
    style->ItemInnerSpacing = compactGui() ? ImVec2(4, 4) : ImVec2(3, 8);
    style->CellPadding = ImVec2(4, 2);
    style->TouchExtraPadding = ImVec2(0, 0);
    style->IndentSpacing = 21.0f;
    style->ColumnsMinSpacing = 6.0f;
    style->ScrollbarSize = 14.0f;
    style->ScrollbarRounding = 9.0f;
    style->GrabMinSize = 10.0f;
    style->GrabRounding = 0.0f;
    style->LogSliderDeadzone = 4.0f;
    style->TabRounding = 4.0f;
    style->TabBorderSize = 0.0f;
    style->TabMinWidthForCloseButton = 0.0f;
    style->ColorButtonPosition = ImGuiDir_Right;
    style->ButtonTextAlign = ImVec2(0.5f, 0.5f);
    style->SelectableTextAlign = ImVec2(0.0f, 0.0f);
    style->DisplayWindowPadding = ImVec2(19, 19);
    style->DisplaySafeAreaPadding = ImVec2(3, 3);
    style->MouseCursorScale = 1.0f;
    style->AntiAliasedLines = true;
    style->AntiAliasedLinesUseTex = true;
    style->AntiAliasedFill = true;
    style->CurveTessellationTol = 1.25f;
    style->CircleTessellationMaxError = 0.30f;
    ImGui::StyleColorsDark(style);

    // Remix changes
    style->Colors[ImGuiCol_WindowBg] = ImVec4(0.f, 0.f, 0.f, backgroundAlpha());
    style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.4f);
    style->TabRounding = 1;
  }

  void ImGUI::setNvidiaStyle(ImGuiStyle* dst) {
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();

    // Based on legacy theme
    setLegacyStyle(style);

    style->Colors[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.91f, 1.00f);
    style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);
    style->Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.90f);
    style->Colors[ImGuiCol_ChildBg] = ImVec4(0.19f, 0.19f, 0.19f, 0.80f);
    style->Colors[ImGuiCol_PopupBg] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    style->Colors[ImGuiCol_Border] = ImVec4(0.31f, 0.31f, 0.31f, 0.20f);
    style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.23f);
    style->Colors[ImGuiCol_FrameBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.33f, 0.47f, 0.08f, 1.00f);
    style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.46f, 0.73f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.98f);
    style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
    style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
    style->Colors[ImGuiCol_MenuBarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style->Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.39f);
    style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.54f, 0.54f, 0.54f, 0.47f);
    style->Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.78f, 0.78f, 0.78f, 0.33f);
    style->Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.39f);
    style->Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.31f);
    style->Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.47f, 0.08f, 1.00f);
    style->Colors[ImGuiCol_ButtonActive] = ImVec4(0.46f, 0.73f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_Header] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
    style->Colors[ImGuiCol_HeaderHovered] = ImVec4(0.33f, 0.47f, 0.08f, 1.00f);
    style->Colors[ImGuiCol_HeaderActive] = ImVec4(0.33f, 0.47f, 0.08f, 1.00f);
    style->Colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    style->Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.32f, 0.46f, 0.06f, 1.00f);
    style->Colors[ImGuiCol_SeparatorActive] = ImVec4(0.46f, 0.73f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_ResizeGrip] = ImVec4(0.43f, 0.43f, 0.43f, 0.51f);
    style->Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.32f, 0.46f, 0.06f, 1.00f);
    style->Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.46f, 0.73f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_Tab] = ImVec4(0.00f, 0.00f, 0.00f, 0.37f);
    style->Colors[ImGuiCol_TabHovered] = ImVec4(0.32f, 0.46f, 0.06f, 1.00f);
    style->Colors[ImGuiCol_TabActive] = ImVec4(0.46f, 0.73f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_TabUnfocused] = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
    style->Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.24f);
    style->Colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
    style->Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);
    style->Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style->Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    style->Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_TableBorderLight] = ImVec4(0.00f, 0.00f, 0.00f, 0.54f);
    style->Colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.39f);
    style->Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.46f, 0.73f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style->Colors[ImGuiCol_DragDropTarget] = ImVec4(0.00f, 0.51f, 0.39f, 0.31f);
    style->Colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style->Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    style->Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.56f);
  }

  void ImGUI::onThemeChange(DxvkDevice* device) {
    if (GImGui != nullptr) {
      ImGUI& gui = device->getCommon()->getImgui();
      gui.setupStyle();

    }
  }

  void ImGUI::onBackgroundAlphaChange(DxvkDevice* device) {
    if (GImGui != nullptr) {
      ImGUI& gui = device->getCommon()->getImgui();
      gui.adjustStyleBackgroundAlpha(backgroundAlpha());
    }
  }

  void ImGUI::setupStyle(ImGuiStyle* dst) {
    ImGui::GetIO().FontDefault = largeUiMode() ? m_largeFont : m_regularFont;
    updateWindowWidths();
   
    ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();
    switch (themeGui())
    {
    default:
    case Theme::Toolkit:
      setToolkitStyle(style);
      break;

    case Theme::Legacy:
      setLegacyStyle(style);
      break;
    
    case Theme::Nvidia:
      setNvidiaStyle(style);
      break;
    }
  }

  void ImGUI::showVsyncOptions(bool enableDLFGGuard) {
    // we should never get here without a swapchain, so we must have latched the vsync value already
    assert(RtxOptions::enableVsyncState != EnableVsync::WaitingForImplicitSwapchain);
    
    if (enableDLFGGuard && DxvkDLFG::enable()) {
      ImGui::BeginDisabled();
    }

    bool vsyncEnabled = RtxOptions::enableVsyncState == EnableVsync::On;
    bool changed = RemixGui::Checkbox("Enable V-Sync", &vsyncEnabled);
    if (changed) {
      // option has been toggled manually, so we need to actually store the value in the option.
      // RtxOptions::enableVsyncState will be changed by the onChange handler at the end of the frame.
      RemixGui::CheckRtxOptionPopups(&RtxOptions::enableVsyncObject());
      RtxOptions::enableVsync.setDeferred(vsyncEnabled ? EnableVsync::On : EnableVsync::Off);
    }

    ImGui::BeginDisabled();
    ImGui::Indent();
    ImGui::TextWrapped("This setting overrides the native game's V-Sync setting.");
    ImGui::Unindent();
    ImGui::EndDisabled();
    
    if (enableDLFGGuard && DxvkDLFG::enable()) {
      ImGui::Indent();
      ImGui::TextWrapped("When Frame Generation is active, V-Sync is automatically disabled.");
      ImGui::Unindent();

      ImGui::EndDisabled();
    }
  }

  void ImGUI::showDLFGOptions(const Rc<DxvkContext>& ctx) {
    const bool supportsDLFG = ctx->getCommonObjects()->metaNGXContext().supportsDLFG() && !ctx->getCommonObjects()->metaDLFG().hasDLFGFailed();
    const uint32_t maxInterpolatedFrames = ctx->getCommonObjects()->metaNGXContext().dlfgMaxInterpolatedFrames();
    const bool supportsMultiFrame = maxInterpolatedFrames > 1;

    if (!supportsDLFG) {
      ImGui::BeginDisabled();
    }

    bool dlfgChanged = RemixGui::Checkbox("Enable DLSS Frame Generation", &DxvkDLFG::enableObject());
    if (supportsMultiFrame) {
      dlfgMfgModeCombo.getKey(&DxvkDLFG::maxInterpolatedFramesObject());
    }

    const auto& reason = ctx->getCommonObjects()->metaNGXContext().getDLFGNotSupportedReason();
    if (reason.size()) {
      RemixGui::SetTooltipToLastWidgetOnHover(reason.c_str());
      ImGui::TextWrapped(reason.c_str());
    }

    if (!supportsDLFG) {
      ImGui::EndDisabled();
    }

    // Need to change Reflex in sync with DLFG, not on the next frame.
    if (dlfgChanged) {
      if (!supportsDLFG) {
        DxvkDLFG::enable.setDeferred(false);
      } else if (!DxvkDLFG::enable()){
        // DLFG was just enabled.  force Reflex to Low Latency.
        RtxOptions::reflexMode.setDeferred(ReflexMode::LowLatency);
      }
    }

  }

  void ImGUI::showReflexOptions(const Rc<DxvkContext>& ctx, bool displayStatsWindowToggle) {
    RtxReflex& reflex = m_device->getCommon()->metaReflex();

    // Note: Skip Reflex ImGUI options if Reflex is not initialized (either fully disabled or failed to be initialized).
    if (!reflex.reflexInitialized()) {
      return;
    }

    // Display Reflex mode selector

    {
      bool disableReflexUI = ctx->isDLFGEnabled();
      ImGui::BeginDisabled(disableReflexUI);
      reflexModeCombo.getKey(&RtxOptions::reflexModeObject());
      ImGui::EndDisabled();
    }

    // Add a button to toggle the Reflex latency stats Window if requested

    if (displayStatsWindowToggle) {
      if (ImGui::Button("Toggle Reflex Stats Window", ImVec2(ImGui::GetContentRegionAvail().x - GImGui->Style.FramePadding.x * 2, 0))) {
        m_reflexLatencyStatsOpen = !m_reflexLatencyStatsOpen;
      }
    }

  }

  void ImGUI::showReflexLatencyStats() {
    // Set up the latency stats Window

    ImGui::SetNextWindowSize(ImVec2(m_reflexLatencyStatsWindowWidth, m_reflexLatencyStatsWindowHeight), ImGuiCond_Once);

    if (!ImGui::Begin("Reflex Latency Stats", &m_reflexLatencyStatsOpen, popupWindowFlags)) {
      ImGui::End();

      return;
    }

    RtxReflex& reflex = m_device->getCommon()->metaReflex();
    const auto latencyStats = reflex.getLatencyStats();

    constexpr ImPlotFlags druationGraphFlags = ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
    constexpr ImPlotAxisFlags graphFrameAxisFlags = ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_Lock;
    constexpr ImPlotAxisFlags graphDurationAxisFlags = ImPlotAxisFlags_Lock;

    constexpr ImPlotFlags timingGraphFlags = ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoLegend;
    constexpr ImPlotAxisFlags graphTimeAxisFlags = ImPlotAxisFlags_Lock;
    constexpr ImPlotAxisFlags graphRegionAxisFlags = ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks;

    // Update Reflex stat ranges

    const auto& interpolationRate = reflexStatRangeInterpolationRate();
    const auto& paddingRatio = reflexStatRangePaddingRatio();

    const auto newCurrentGameToRenderDurationMin = std::max(latencyStats.gameToRenderDurationMin - latencyStats.gameToRenderDurationMin * paddingRatio, 0.0f);
    const auto newCurrentGameToRenderDurationMax = latencyStats.gameToRenderDurationMax + latencyStats.gameToRenderDurationMax * paddingRatio;
    const auto newCurrentCombinedDurationMin = std::max(latencyStats.combinedDurationMin - latencyStats.combinedDurationMin * paddingRatio, 0.0f);
    const auto newCurrentcombinedDurationMax = latencyStats.combinedDurationMax + latencyStats.combinedDurationMax * paddingRatio;

    // Note: Check if the various range members have been initialized yet to allow the first frame to set them rather than interpolate (since they are
    // left as undefined values right now, and even if they were set to 0 or some other value it might give slightly jarring initial behavior).
    if (m_reflexRangesInitialized) {
      // Note: Exponential-esque moving averages.
      m_currentGameToRenderDurationMin = lerp(m_currentGameToRenderDurationMin, newCurrentGameToRenderDurationMin, interpolationRate);
      m_currentGameToRenderDurationMax = lerp(m_currentGameToRenderDurationMax, newCurrentGameToRenderDurationMax, interpolationRate);
      m_currentCombinedDurationMin = lerp(m_currentCombinedDurationMin, newCurrentCombinedDurationMin, interpolationRate);
      m_currentCombinedDurationMax = lerp(m_currentCombinedDurationMax, newCurrentcombinedDurationMax, interpolationRate);
    } else {
      m_currentGameToRenderDurationMin = newCurrentGameToRenderDurationMin;
      m_currentGameToRenderDurationMax = newCurrentGameToRenderDurationMax;
      m_currentCombinedDurationMin = newCurrentCombinedDurationMin;
      m_currentCombinedDurationMax = newCurrentcombinedDurationMax;

      m_reflexRangesInitialized = true;
    }

    // Draw Total Duration Plot

    if (ImPlot::BeginPlot("Total Duration", ImVec2(-1, 200), druationGraphFlags)) {
      ImPlot::SetupAxes("Frame", "Duration (ms)", graphFrameAxisFlags, graphDurationAxisFlags);
      ImPlot::SetupAxisLimits(ImAxis_X1, static_cast<double>(latencyStats.frameIDMin), static_cast<double>(latencyStats.frameIDMax), ImGuiCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, static_cast<double>(m_currentGameToRenderDurationMin), static_cast<double>(m_currentGameToRenderDurationMax), ImPlotCond_Always);

      ImPlot::PlotLine("Game to Render", latencyStats.frameID, latencyStats.gameToRenderDuration, LatencyStats::statFrames, 0, 0);

      ImPlot::EndPlot();
    }

    ImGui::Text("Game to Render Duration: %.2f ms", latencyStats.gameToRenderDuration[LatencyStats::statFrames - 1]);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    RemixGui::SetTooltipToLastWidgetOnHover("This measures the time from the start of the simulation to the end of the GPU rendering as a total game to render latency.");

    RemixGui::Separator();

    // Draw Region Duration Plot

    if (ImPlot::BeginPlot("Region Durations", ImVec2(-1, 250), druationGraphFlags)) {
      ImPlot::SetupAxes("Frame", "Duration (ms)", graphFrameAxisFlags, graphDurationAxisFlags);
      ImPlot::SetupAxisLimits(ImAxis_X1, static_cast<double>(latencyStats.frameIDMin), static_cast<double>(latencyStats.frameIDMax), ImGuiCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, static_cast<double>(m_currentCombinedDurationMin), static_cast<double>(m_currentCombinedDurationMax), ImPlotCond_Always);

      ImPlot::PlotLine("Simulation", latencyStats.frameID, latencyStats.simDuration, LatencyStats::statFrames, 0, 0);
      ImPlot::PlotLine("Render Submit", latencyStats.frameID, latencyStats.renderSubmitDuration, LatencyStats::statFrames, 0, 0);
      ImPlot::PlotLine("Present", latencyStats.frameID, latencyStats.presentDuration, LatencyStats::statFrames, 0, 0);
      ImPlot::PlotLine("Driver", latencyStats.frameID, latencyStats.driverDuration, LatencyStats::statFrames, 0, 0);
      ImPlot::PlotLine("OS Queue", latencyStats.frameID, latencyStats.osRenderQueueDuration, LatencyStats::statFrames, 0, 0);
      ImPlot::PlotLine("GPU Render", latencyStats.frameID, latencyStats.gpuRenderDuration, LatencyStats::statFrames, 0, 0);

      ImPlot::EndPlot();
    }

    ImGui::Text("Simulation Duration: %.2f ms", latencyStats.simDuration[LatencyStats::statFrames - 1]);
    ImGui::Text("Render Submit Duration: %.2f ms", latencyStats.renderSubmitDuration[LatencyStats::statFrames - 1]);
    ImGui::Text("Present Duration: %.2f ms", latencyStats.presentDuration[LatencyStats::statFrames - 1]);
    ImGui::Text("Driver Duration: %.2f ms", latencyStats.driverDuration[LatencyStats::statFrames - 1]);
    ImGui::Text("OS Queue Duration: %.2f ms", latencyStats.osRenderQueueDuration[LatencyStats::statFrames - 1]);
    ImGui::Text("GPU Render Duration: %.2f ms", latencyStats.gpuRenderDuration[LatencyStats::statFrames - 1]);

    RemixGui::Separator();

    // Draw Region Timing Plot

    constexpr float microsecondsPerMillisecond { 1000.0f };

    if (ImPlot::BeginPlot("Region Timings", ImVec2(-1, 150), timingGraphFlags)) {
      ImPlot::SetupAxes(nullptr, nullptr, graphTimeAxisFlags, graphRegionAxisFlags);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0.0f, static_cast<double>(static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond), ImPlotCond_Always);

      constexpr const char* regions[]{ "Simulation", "Render Submit", "Present", "Driver", "OS Queue", "GPU Render" };
      ImPlot::SetupAxisTicks(ImAxis_Y1, 0, 5, 6, regions, false);

      // Note: Name needed to color label.
      constexpr const char* labels[]{ "", "Time", "" };
      const float timeData[]{
        // Pre-Span
        static_cast<float>(latencyStats.simCurrentStartTime - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.renderSubmitCurrentStartTime - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.presentCurrentStartTime - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.driverCurrentStartTime - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.osRenderQueueCurrentStartTime - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.gpuRenderCurrentStartTime - latencyStats.combinedCurrentTimeMin) / microsecondsPerMillisecond,
        // Current Span
        latencyStats.simDuration[LatencyStats::statFrames - 1],
        latencyStats.renderSubmitDuration[LatencyStats::statFrames - 1],
        latencyStats.presentDuration[LatencyStats::statFrames - 1],
        latencyStats.driverDuration[LatencyStats::statFrames - 1],
        latencyStats.osRenderQueueDuration[LatencyStats::statFrames - 1],
        latencyStats.gpuRenderDuration[LatencyStats::statFrames - 1],
        // Post-Span
        static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.simCurrentEndTime) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.renderSubmitCurrentEndTime) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.presentCurrentEndTime) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.driverCurrentEndTime) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.osRenderQueueCurrentEndTime) / microsecondsPerMillisecond,
        static_cast<float>(latencyStats.combinedCurrentTimeMax - latencyStats.gpuRenderCurrentEndTime) / microsecondsPerMillisecond,
      };

      ImPlot::PlotBarGroups(labels, timeData, 3, 6, 0.75, 0, ImPlotBarGroupsFlags_Stacked | ImPlotBarGroupsFlags_Horizontal);

      ImPlot::EndPlot();
    }

    ImGui::End();
  }

  void ImGUI::showRenderingSettings(const Rc<DxvkContext>& ctx) {
    ImGui::PushItemWidth(largeUiMode() ? m_largeWindowWidgetWidth : m_regularWindowWidgetWidth);
    auto common = ctx->getCommonObjects();

    ImGui::Text("Disclaimer: The following settings are intended for developers,\nchanging them may introduce instability.");
    RemixGui::Separator();

    // Always display memory stats to user.
    showMemoryStats();

    RemixGui::Separator();

    if (RemixGui::CollapsingHeader("General", collapsingHeaderFlags)) {
      auto& dlss = common->metaDLSS();
      auto& rayReconstruction = common->metaRayReconstruction();
      ImGui::Indent();

      if (RtxOptions::showRaytracingOption()) {
        RemixGui::Checkbox("Raytracing Enabled", &RtxOptions::enableRaytracingObject());

        renderPassGBufferRaytraceModeCombo.getKey(&RtxOptions::renderPassGBufferRaytraceModeObject());
        renderPassIntegrateDirectRaytraceModeCombo.getKey(&RtxOptions::renderPassIntegrateDirectRaytraceModeObject());
        renderPassIntegrateIndirectRaytraceModeCombo.getKey(&RtxOptions::renderPassIntegrateIndirectRaytraceModeObject());

        RemixGui::Separator();
      }

      showDLFGOptions(ctx);

      RemixGui::Separator();

      showReflexOptions(ctx, true);

      RemixGui::Separator();

      if (ctx->getCommonObjects()->metaDLSS().supportsDLSS()) {
        // Show upscaler and DLSS-RR option.
        auto oldUpscalerType = RtxOptions::upscalerType();
        bool oldDLSSRREnabled = RtxOptions::enableRayReconstruction();
        getUpscalerCombo(dlss, rayReconstruction).getKey(&RtxOptions::upscalerTypeObject());
        showRayReconstructionEnable(rayReconstruction.supportsRayReconstruction());

        // Update path tracer settings when upscaler is changed or DLSS-RR is toggled.
        if (oldUpscalerType != RtxOptions::upscalerType() || oldDLSSRREnabled != RtxOptions::enableRayReconstruction()) {
          RtxOptions::updateLightingSetting();
        }
      } else {
        getUpscalerCombo(dlss, rayReconstruction).getKey(&RtxOptions::upscalerTypeObject());
      }

      RtxOptions::updatePresetFromUpscaler();

      if (RtxOptions::upscalerType() == UpscalerType::DLSS && !ctx->getCommonObjects()->metaDLSS().supportsDLSS()) {
        RtxOptions::upscalerType.setDeferred(UpscalerType::TAAU);
      }

      if (RtxOptions::isRayReconstructionEnabled()) {
        dlssProfileCombo.getKey(&RtxOptions::qualityDLSSObject());
        rayReconstruction.showRayReconstructionImguiSettings(false);
      } else if (RtxOptions::upscalerType() == UpscalerType::DLSS) {
        dlssProfileCombo.getKey(&RtxOptions::qualityDLSSObject());
        dlss.showImguiSettings();
      } else if (RtxOptions::upscalerType() == UpscalerType::NIS) {
        RemixGui::SliderFloat("Resolution scale", &RtxOptions::resolutionScaleObject(), 0.5f, 1.0f);
        RemixGui::SliderFloat("Sharpness", &ctx->getCommonObjects()->metaNIS().m_sharpness, 0.1f, 1.0f);
        RemixGui::Checkbox("Use FP16", &ctx->getCommonObjects()->metaNIS().m_useFp16);
      } else if (RtxOptions::upscalerType() == UpscalerType::XeSS) {
          xessPresetCombo.getKey(&DxvkXeSS::XessOptions::presetObject());

          // Show resolution slider only for Custom preset
          if (DxvkXeSS::XessOptions::preset() == XeSSPreset::Custom) {
            RemixGui::SliderFloat("Resolution Scale", &RtxOptions::resolutionScaleObject(), 0.1f, 1.0f, "%.2f");
          }

          // Display XeSS internal resolution
          auto& xess = ctx->getCommonObjects()->metaXeSS();

          uint32_t inputWidth;
          uint32_t inputHeight;
          xess.getInputSize(inputWidth, inputHeight);
          ImGui::TextWrapped(str::format("Render Resolution: ", inputWidth, "x", inputHeight).c_str());
        } else if (RtxOptions::upscalerType() == UpscalerType::TAAU) {
        RemixGui::SliderFloat("Resolution scale", &RtxOptions::resolutionScaleObject(), 0.5f, 1.0f);
      }

      RemixGui::Separator();

      RemixGui::Checkbox("Allow Full Screen Exclusive?", &RtxOptions::allowFSEObject());

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Pathtracing", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      RemixGui::Checkbox("RNG: seed with frame index", &RtxOptions::rngSeedWithFrameIndexObject());
      RemixGui::Checkbox("Advance time", &RtxOptions::advanceTimeObject());

      if (RemixGui::CollapsingHeader("Resolver", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        RemixGui::DragInt("Max Primary Interactions", &RtxOptions::primaryRayMaxInteractionsObject(), 1.0f, 1, 255, "%d", sliderFlags);
        RemixGui::DragInt("Max PSR Interactions", &RtxOptions::psrRayMaxInteractionsObject(), 1.0f, 1, 255, "%d", sliderFlags);
        RemixGui::DragInt("Max Secondary Interactions", &RtxOptions::secondaryRayMaxInteractionsObject(), 1.0f, 1, 255, "%d", sliderFlags);
        RemixGui::Checkbox("Separate Unordered Approximations", &RtxOptions::enableSeparateUnorderedApproximationsObject());
        RemixGui::Checkbox("Direct Translucent Shadows", &RtxOptions::enableDirectTranslucentShadowsObject());
        RemixGui::Checkbox("Direct Alpha Blended Shadows", &RtxOptions::enableDirectAlphaBlendShadowsObject());
        RemixGui::Checkbox("Indirect Translucent Shadows", &RtxOptions::enableIndirectTranslucentShadowsObject());
        RemixGui::Checkbox("Indirect Alpha Blended Shadows", &RtxOptions::enableIndirectAlphaBlendShadowsObject());
        RemixGui::Checkbox("Decal Material Blending", &RtxOptions::enableDecalMaterialBlendingObject());
        RemixGui::Checkbox("Billboard Orientation Correction", &RtxOptions::enableBillboardOrientationCorrectionObject());
        if (RtxOptions::enableBillboardOrientationCorrection()) {
          ImGui::Indent();
          RemixGui::Checkbox("Dev: Use i-prims on primary rays", &RtxOptions::useIntersectionBillboardsOnPrimaryRaysObject());
          ImGui::Unindent();
        }
        RemixGui::Checkbox("Track Particle Object", &RtxOptions::trackParticleObjectsObject());

        RemixGui::SliderFloat("Resolve Transparency Threshold", &RtxOptions::resolveTransparencyThresholdObject(), 0.0f, 1.0f);
        RemixGui::SliderFloat("Resolve Opaqueness Threshold", &RtxOptions::resolveOpaquenessThresholdObject(), 0.0f, 1.0f);

        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("PSR", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        RemixGui::Checkbox("Reflection PSR Enabled", &RtxOptions::enablePSRRObject());
        RemixGui::Checkbox("Transmission PSR Enabled", &RtxOptions::enablePSTRObject());
        // # bounces limitted by 8b allocation in payload
        // Note: value of 255 effectively means unlimited bounces, and we don't want to allow that
        RemixGui::DragInt("Max Reflection PSR Bounces", &RtxOptions::psrrMaxBouncesObject(), 1.0f, 1, 254, "%d", sliderFlags);
        RemixGui::DragInt("Max Transmission PSR Bounces", &RtxOptions::pstrMaxBouncesObject(), 1.0f, 1, 254, "%d", sliderFlags);
        RemixGui::Checkbox("Outgoing Transmission Approx Enabled", &RtxOptions::enablePSTROutgoingSplitApproximationObject());
        RemixGui::Checkbox("Incident Transmission Approx Enabled", &RtxOptions::enablePSTRSecondaryIncidentSplitApproximationObject());
        RemixGui::DragFloat("Reflection PSR Normal Detail Threshold", &RtxOptions::psrrNormalDetailThresholdObject(), 0.001f, 0.f, 1.f);
        RemixGui::DragFloat("Transmission PSR Normal Detail Threshold", &RtxOptions::pstrNormalDetailThresholdObject(), 0.001f, 0.f, 1.f);

        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("Integrator", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        RemixGui::Checkbox("Enable Secondary Bounces", &RtxOptions::enableSecondaryBouncesObject());
        RemixGui::Checkbox("Enable Russian Roulette", &RtxOptions::enableRussianRouletteObject());
        RemixGui::Checkbox("Enable Probability Dithering Filtering for Primary Bounce", &RtxOptions::enableFirstBounceLobeProbabilityDitheringObject());
        RemixGui::Checkbox("Unordered Resolve in Indirect Rays", &RtxOptions::enableUnorderedResolveInIndirectRaysObject());
        ImGui::BeginDisabled(!RtxOptions::enableUnorderedResolveInIndirectRays());
        RemixGui::Checkbox("Probabilistic Unordered Resolve in Indirect Rays", &RtxOptions::enableProbabilisticUnorderedResolveInIndirectRaysObject());
        ImGui::EndDisabled();
        RemixGui::Checkbox("Unordered Emissive Particles in Indirect Rays", &RtxOptions::enableUnorderedEmissiveParticlesInIndirectRaysObject());
        RemixGui::Checkbox("Transmission Approximation in Indirect Rays", &RtxOptions::enableTransmissionApproximationInIndirectRaysObject());
        // # bounces limitted by 4b allocation in payload
        // Note: It's possible get up to 16 bounces => will require logic adjustment
        RemixGui::DragInt("Minimum Path Bounces", &RtxOptions::pathMinBouncesObject(), 1.0f, 0, 15, "%d", sliderFlags);
        RemixGui::DragInt("Maximum Path Bounces", &RtxOptions::pathMaxBouncesObject(), 1.0f, RtxOptions::pathMinBounces(), 15, "%d", sliderFlags);
        RemixGui::DragFloat("Firefly Filtering Luminance Threshold", &RtxOptions::fireflyFilteringLuminanceThresholdObject(), 0.1f, 0.0f, FLT_MAX, "%.3f", sliderFlags);
        RemixGui::DragFloat("Secondary Specular Firefly Filtering Threshold", &RtxOptions::secondarySpecularFireflyFilteringThresholdObject(), 0.1f, 0.0f, FLT_MAX, "%.3f", sliderFlags);
        RemixGui::DragFloat("Opaque Diffuse Lobe Probability Zero Threshold", &RtxOptions::opaqueDiffuseLobeSamplingProbabilityZeroThresholdObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Min Opaque Diffuse Lobe Probability", &RtxOptions::minOpaqueDiffuseLobeSamplingProbabilityObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Opaque Specular Lobe Probability Zero Threshold", &RtxOptions::opaqueSpecularLobeSamplingProbabilityZeroThresholdObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Min Opaque Specular Lobe Probability", &RtxOptions::minOpaqueSpecularLobeSamplingProbabilityObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Opaque Opacity Transmission Lobe Probability Zero Threshold", &RtxOptions::opaqueOpacityTransmissionLobeSamplingProbabilityZeroThresholdObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Min Opaque Opacity Transmission Lobe Probability", &RtxOptions::minOpaqueOpacityTransmissionLobeSamplingProbabilityObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Diffuse Transmission Lobe Probability Zero Threshold", &RtxOptions::opaqueDiffuseTransmissionLobeSamplingProbabilityZeroThresholdObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Min Diffuse Transmission Lobe Probability", &RtxOptions::minOpaqueDiffuseTransmissionLobeSamplingProbabilityObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Translucent Specular Lobe Probability Zero Threshold", &RtxOptions::translucentSpecularLobeSamplingProbabilityZeroThresholdObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Min Translucent Specular Lobe Probability", &RtxOptions::minTranslucentSpecularLobeSamplingProbabilityObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Translucent Transmission Lobe Probability Zero Threshold", &RtxOptions::translucentTransmissionLobeSamplingProbabilityZeroThresholdObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Min Translucent Transmission Lobe Probability", &RtxOptions::minTranslucentTransmissionLobeSamplingProbabilityObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);
        RemixGui::DragFloat("Indirect Ray Spread Angle Factor", &RtxOptions::indirectRaySpreadAngleFactorObject(), 0.001f, 0.0f, 1.0f, "%.3f", sliderFlags);

        if (RtxOptions::enableRussianRoulette() && RemixGui::CollapsingHeader("Russian Roulette", collapsingHeaderClosedFlags)) {
          ImGui::Indent();

          RemixGui::DragFloat("1st bounce: Min Continue Probability", &RtxOptions::russianRoulette1stBounceMinContinueProbabilityObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
          RemixGui::DragFloat("1st bounce: Max Continue Probability", &RtxOptions::russianRoulette1stBounceMaxContinueProbabilityObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
          
          secondPlusBounceRussianRouletteModeCombo.getKey(&RtxOptions::russianRouletteModeObject());
          if (RtxOptions::russianRouletteMode() == RussianRouletteMode::ThroughputBased)
          {
            RemixGui::DragFloat("2nd+ bounce: Max Continue Probability", &RtxOptions::russianRouletteMaxContinueProbabilityObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
          }
          else
          {
            RemixGui::DragFloat("2nd+ bounce: Diffuse Continue Probability", &RtxOptions::russianRouletteDiffuseContinueProbabilityObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
            RemixGui::DragFloat("2nd+ bounce: Specular Continue Probability", &RtxOptions::russianRouletteSpecularContinueProbabilityObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
            RemixGui::DragFloat("2nd+ bounce: Distance Factor", &RtxOptions::russianRouletteDistanceFactorObject(), 0.01f, 0.0f, 1.0f, "%.3f", sliderFlags);
          }
          
          ImGui::Unindent();
        }
        ImGui::Unindent();
      }

      if (RtxOptions::getIsOpacityMicromapSupported() && 
          RemixGui::CollapsingHeader("Opacity Micromap", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        RemixGui::Checkbox("Enable Opacity Micromap", &RtxOptions::OpacityMicromap::enableObject());
        
        if (common->getOpacityMicromapManager()) {
          common->getOpacityMicromapManager()->showImguiSettings();
        }
        ImGui::Unindent();
      }

      const VkPhysicalDeviceProperties& props = m_device->adapter()->deviceProperties();
      const NV_GPU_ARCHITECTURE_ID archId = RtxOptions::getNvidiaArch();

      // Shader Execution Reordering
      if (RtxOptions::isShaderExecutionReorderingSupported()) {
        if (RemixGui::CollapsingHeader("Shader Execution Reordering", collapsingHeaderClosedFlags)) {
          ImGui::Indent();

          if (RtxOptions::renderPassIntegrateIndirectRaytraceMode() == DxvkPathtracerIntegrateIndirect::RaytraceMode::TraceRay)
            RemixGui::Checkbox("Enable In Integrate Indirect Pass", &RtxOptions::enableShaderExecutionReorderingInPathtracerIntegrateIndirectObject());

          ImGui::Unindent();
        }
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Lighting", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      common->getSceneManager().getLightManager().showImguiLightOverview();

      if (RemixGui::CollapsingHeader("Effect Light", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        ImGui::TextWrapped("These settings control the effect lights, which are created by Remix, and attached to objects tagged using the rtx.lightConverter option (found in the texture tagging menu as 'Add Light to Texture').");

        RemixGui::DragFloat("Light Intensity", &RtxOptions::effectLightIntensityObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", sliderFlags);
        RemixGui::DragFloat("Light Radius", &RtxOptions::effectLightRadiusObject(), 0.01f, 0.01f, FLT_MAX, "%.3f", sliderFlags);
        // Plasma ball has first priority
        RemixGui::Checkbox("Plasma Ball Effect", &RtxOptions::effectLightPlasmaBallObject());
        ImGui::BeginDisabled(RtxOptions::effectLightPlasmaBall());
        RemixGui::ColorPicker3("Light Color", &RtxOptions::effectLightColorObject());
        ImGui::EndDisabled();
        ImGui::Unindent();
      }

      RemixGui::DragFloat("Emissive Intensity", &RtxOptions::emissiveIntensityObject(), 0.01f, 0.0f, FLT_MAX, "%.3f", sliderFlags);
      RemixGui::Separator();
      RemixGui::SliderInt("RIS Light Sample Count", &RtxOptions::risLightSampleCountObject(), 0, 64);
      RemixGui::Separator();
      RemixGui::Checkbox("Direct Lighting Enabled", &RtxOptions::enableDirectLightingObject());
      RemixGui::Checkbox("Indirect Lighting Enabled", &RtxOptions::enableSecondaryBouncesObject());

      if (RemixGui::CollapsingHeader("RTXDI", collapsingHeaderClosedFlags)) {
        ImGui::Indent();

        RemixGui::Checkbox("Enable RTXDI", &RtxOptions::useRTXDIObject());

        auto& rtxdi = common->metaRtxdiRayQuery();
        rtxdi.showImguiSettings();
        ImGui::Unindent();
      }

      // Indirect Illumination Integration Mode
      if (RemixGui::CollapsingHeader("Indirect Illumination", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        integrateIndirectModeCombo.getKey(&RtxOptions::integrateIndirectModeObject());

        if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::ReSTIRGI) {
          if (RemixGui::CollapsingHeader("ReSTIR GI", collapsingHeaderClosedFlags)) {
            ImGui::Indent();
            ImGui::PushID("ReSTIR GI");
            auto& restirGI = common->metaReSTIRGIRayQuery();
            restirGI.showImguiSettings();
            ImGui::PopID();
            ImGui::Unindent();
          }
        } else if (RtxOptions::integrateIndirectMode() == IntegrateIndirectMode::NeuralRadianceCache) {
          if (RemixGui::CollapsingHeader("RTX Neural Radiance Cache", collapsingHeaderClosedFlags)) {

            ImGui::Indent();
            ImGui::PushID("Neural Radiance Cache");
            NeuralRadianceCache& nrc = common->metaNeuralRadianceCache();
            nrc.showImguiSettings(*ctx);
            ImGui::PopID();
            ImGui::Unindent();
          }
        }

        ImGui::Unindent();
      }

      if (RemixGui::CollapsingHeader("NEE Cache", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        ImGui::PushID("NEE Cache");
        auto& neeCache = common->metaNeeCache();
        neeCache.showImguiSettings();
        ImGui::PopID();
        ImGui::Unindent();
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Sparse Rendering", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      common->metaSparseRendering().showImguiSettings();
      ImGui::Unindent();
    }

    RtxParticleSystemManager::showImguiSettings();

    RtxPointInstancerSystem::showImguiSettings();

    if (RemixGui::CollapsingHeader("RTX Volumetrics (Global)", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      common->metaGlobalVolumetrics().showImguiSettings();

      common->metaDustParticles().showImguiSettings();

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Subsurface Scattering", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      RemixGui::Checkbox("Enable Thin Opaque", &RtxOptions::SubsurfaceScattering::enableThinOpaqueObject());
      RemixGui::Checkbox("Enable Texture Maps", &RtxOptions::SubsurfaceScattering::enableTextureMapsObject());

      RemixGui::Checkbox("Enable Diffusion Profile SSS", &RtxOptions::SubsurfaceScattering::enableDiffusionProfileObject());

      if (RtxOptions::SubsurfaceScattering::enableDiffusionProfile()) {
        RemixGui::SliderFloat("SSS Scale", &RtxOptions::SubsurfaceScattering::diffusionProfileScaleObject(), 0.0f, 100.0f);

        RemixGui::Checkbox("Enable SSS Transmission", &RtxOptions::SubsurfaceScattering::enableTransmissionObject());
        if (RtxOptions::SubsurfaceScattering::enableTransmission()) {
          RemixGui::Checkbox("Enable SSS Transmission Single Scattering", &RtxOptions::SubsurfaceScattering::enableTransmissionSingleScatteringObject());
          RemixGui::Checkbox("Enable Transmission Diffusion Profile Correction [Experimental]", &RtxOptions::SubsurfaceScattering::enableTransmissionDiffusionProfileCorrectionObject());
          RemixGui::DragInt("SSS Transmission BSDF Sample Count", &RtxOptions::SubsurfaceScattering::transmissionBsdfSampleCountObject(), 0.1f, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
          RemixGui::DragInt("SSS Transmission Single Scattering Sample Count", &RtxOptions::SubsurfaceScattering::transmissionSingleScatteringSampleCountObject(), 0.1f, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        }
      }

      RemixGui::DragInt2("Diffusion Profile Sampling Debugging Pixel Position", &RtxOptions::SubsurfaceScattering::diffusionProfileDebugPixelPositionObject(), 0.1f, 0, INT32_MAX, "%d", ImGuiSliderFlags_AlwaysClamp);

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Alpha Test/Blending", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      RemixGui::Checkbox("Render Alpha Blended", &RtxOptions::enableAlphaBlendObject());
      RemixGui::Checkbox("Render Alpha Tested", &RtxOptions::enableAlphaTestObject());
      RemixGui::Separator();

      RemixGui::Checkbox("Emissive Blend Translation", &RtxOptions::enableEmissiveBlendModeTranslationObject());

      RemixGui::Checkbox("Emissive Blend Override", &RtxOptions::enableEmissiveBlendEmissiveOverrideObject());
      RemixGui::DragFloat("Emissive Blend Override Intensity", &RtxOptions::emissiveBlendOverrideEmissiveIntensityObject(), 0.001f, 0.0f, FLT_MAX, "%.3f", sliderFlags);

      RemixGui::Separator();
      RemixGui::SliderFloat("Particle Softness", &RtxOptions::particleSoftnessFactorObject(), 0.f, 0.5f);
      RemixGui::Separator();
      if (RemixGui::CollapsingHeader("Weighted Blended OIT", collapsingHeaderClosedFlags)) {
        RemixGui::Checkbox("Enable", &RtxOptions::wboitEnabledObject());
        ImGui::BeginDisabled(!RtxOptions::wboitEnabled());
        RemixGui::SliderFloat("Energy Compensation", &RtxOptions::wboitEnergyLossCompensationObject(), 1.f, 10.f);
        RemixGui::SliderFloat("Depth Weight Tuning", &RtxOptions::wboitDepthWeightTuningObject(), 0.01f, 10.f);
        ImGui::EndDisabled();
      }
      common->metaComposite().showStochasticAlphaBlendImguiSettings();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Denoising", collapsingHeaderClosedFlags)) {
      bool isRayReconstructionEnabled = RtxOptions::isRayReconstructionEnabled();
      bool useNRD = !isRayReconstructionEnabled || common->metaRayReconstruction().enableNRDForTraining();
      ImGui::Indent();
      ImGui::BeginDisabled(!useNRD);
      RemixGui::Checkbox("Denoising Enabled", &RtxOptions::useDenoiserObject());
      RemixGui::Checkbox("Reference Mode | Accumulation", &RtxOptions::useDenoiserReferenceModeObject());

      if (RtxOptions::useDenoiserReferenceMode()) {
        common->metaComposite().showAccumulationImguiSettings();
      }

      ImGui::EndDisabled();

      if(RemixGui::CollapsingHeader("Settings", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::Checkbox("Separate Primary Direct/Indirect Denoiser", &RtxOptions::denoiseDirectAndIndirectLightingSeparatelyObject());
        RemixGui::Checkbox("Reset History On Settings Change", &RtxOptions::resetDenoiserHistoryOnSettingsChangeObject());
        RemixGui::Checkbox("Replace Direct Specular HitT with Indirect Specular HitT", &RtxOptions::replaceDirectSpecularHitTWithIndirectSpecularHitTObject());
        RemixGui::Checkbox("Use Virtual Shading Normals", &RtxOptions::useVirtualShadingNormalsForDenoisingObject());
        RemixGui::Checkbox("Adaptive Resolution Denoising", &RtxOptions::adaptiveResolutionDenoisingObject());
        RemixGui::Checkbox("Adaptive Accumulation", &RtxOptions::adaptiveAccumulationObject());
        common->metaDemodulate().showImguiSettings();
        common->metaComposite().showDenoiseImguiSettings();
        ImGui::Unindent();
      }
      bool useDoubleDenoisers = RtxOptions::denoiseDirectAndIndirectLightingSeparately();
      if (isRayReconstructionEnabled) {
        if (RemixGui::CollapsingHeader("DLSS-RR", collapsingHeaderClosedFlags)) {
          ImGui::Indent();
          ImGui::PushID("DLSS-RR");
          common->metaRayReconstruction().showRayReconstructionImguiSettings(true);
          ImGui::PopID();
          ImGui::Unindent();
        }
      }
      
      if (useNRD)
      {
        if (useDoubleDenoisers) {
          if (RemixGui::CollapsingHeader("Primary Direct Light Denoiser", collapsingHeaderClosedFlags)) {
            ImGui::Indent();
            ImGui::PushID("Primary Direct Light Denoiser");
            common->metaPrimaryDirectLightDenoiser().showImguiSettings();
            ImGui::PopID();
            ImGui::Unindent();
          }

          if (RemixGui::CollapsingHeader("Primary Indirect Light Denoiser", collapsingHeaderClosedFlags)) {
            ImGui::Indent();
            ImGui::PushID("Primary Indirect Light Denoiser");
            common->metaPrimaryIndirectLightDenoiser().showImguiSettings();
            ImGui::PopID();
            ImGui::Unindent();
          }
        } else {
          if (RemixGui::CollapsingHeader("Primary Direct/Indirect Light Denoiser", collapsingHeaderClosedFlags)) {
            ImGui::Indent();
            ImGui::PushID("Primary Direct/Indirect Light Denoiser");
            common->metaPrimaryCombinedLightDenoiser().showImguiSettings();
            ImGui::PopID();
            ImGui::Unindent();
          }
        }

        if (RemixGui::CollapsingHeader("Secondary Direct/Indirect Light Denoiser", collapsingHeaderClosedFlags)) {
          ImGui::Indent();
          ImGui::PushID("Secondary Direct/Indirect Light Denoiser");
          common->metaSecondaryCombinedLightDenoiser().showImguiSettings();
          ImGui::PopID();
          ImGui::Unindent();
        }
      }

      // Show secondary denoiser settings when RR is enabled and secondary signal uses external denoiser
      if (!useNRD && isRayReconstructionEnabled && common->metaRayReconstruction().denoiseSecondarySignalWithExternalDenoiser()) {
        if (RemixGui::CollapsingHeader("Secondary Direct/Indirect Light Denoiser", collapsingHeaderClosedFlags)) {
          ImGui::Indent();
          ImGui::PushID("Secondary Direct/Indirect Light Denoiser");
          common->metaSecondaryCombinedLightDenoiser().showImguiSettings();
          ImGui::PopID();
          ImGui::Unindent();
        }
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Post-Processing", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      if (RemixGui::CollapsingHeader("Composition", collapsingHeaderClosedFlags))
        common->metaComposite().showImguiSettings();

      if (RtxOptions::upscalerType() == UpscalerType::TAAU) {
        if (RemixGui::CollapsingHeader("TAA-U", collapsingHeaderClosedFlags))
          common->metaTAA().showImguiSettings();
      }

      if (RemixGui::CollapsingHeader("Bloom", collapsingHeaderClosedFlags))
        common->metaBloom().showImguiSettings();


      if (RemixGui::CollapsingHeader("Auto Exposure", collapsingHeaderClosedFlags))
        common->metaAutoExposure().showImguiSettings();

      if (RemixGui::CollapsingHeader("Tonemapping", collapsingHeaderClosedFlags))
      {
        RemixGui::SliderInt("User Brightness", &RtxOptions::userBrightnessObject(), 0, 100, "%d");
        RemixGui::DragFloat("User Brightness EV Range", &RtxOptions::userBrightnessEVRangeObject(), 0.5f, 0.f, 10.f, "%.1f");
        RemixGui::Separator();
        RemixGui::Combo("Tonemapping Mode", &RtxOptions::tonemappingModeObject(), "Global\0Local\0");
        if (RtxOptions::tonemappingMode() == TonemappingMode::Global) {
          common->metaToneMapping().showImguiSettings();
        } else {
          common->metaLocalToneMapping().showImguiSettings();
        }
        if (RtxOptions::showLegacyACESOption()) {
          RemixGui::Separator();
          RemixGui::Checkbox("Use Legacy ACES", &RtxOptions::useLegacyACESObject());
          if (!RtxOptions::useLegacyACES()) {
            ImGui::Indent();
            ImGui::TextWrapped("WARNING: Non-legacy ACES is currently experimental and the implementation is a subject to change.");
            ImGui::Unindent();
          }
        }
      }

      if (RemixGui::CollapsingHeader("Post FX", collapsingHeaderClosedFlags))
        common->metaPostFx().showImguiSettings();

      if (RemixGui::CollapsingHeader("sRGB + Dither", collapsingHeaderClosedFlags))
        common->metaSRGBDither().showImguiSettings();

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Debug", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      common->metaDebugView().showImguiSettings();
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Geometry", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      RemixGui::Checkbox("Enable Triangle Culling (Globally)", &RtxOptions::enableCullingObject());
      RemixGui::Checkbox("Enable Triangle Culling (Override Secondary Rays)", &RtxOptions::enableCullingInSecondaryRaysObject());
      RemixGui::Separator();
      RemixGui::DragInt("Min Prims in Dynamic BLAS", &RtxOptions::minPrimsInDynamicBLASObject(), 1.f, 100, 0);
      RemixGui::DragInt("Max Prims in Merged BLAS", &RtxOptions::maxPrimsInMergedBLASObject(), 1.f, 100, 0);
      RemixGui::Checkbox("Force Merge All Meshes", &RtxOptions::forceMergeAllMeshesObject());
      RemixGui::Checkbox("Minimize BLAS Merging", &RtxOptions::minimizeBlasMergingObject());
      RemixGui::Separator();
      RemixGui::Checkbox("Portals: Virtual Instance Matching", &RtxOptions::useRayPortalVirtualInstanceMatchingObject());
      RemixGui::Checkbox("Portals: Fade In Effect", &RtxOptions::enablePortalFadeInEffectObject());
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Shadow Terminator Fix", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      RemixGui::Checkbox("Enable Terminator Offset", &RtxOptions::ShadowTerminator::enableOffsetObject());
      ImGui::Indent();
      ImGui::BeginDisabled(!RtxOptions::ShadowTerminator::enableOffset());
      ImGui::TextWrapped("NOTE: The options below are metric (ensure a correct scene scale).");
      RemixGui::DragFloat("Area Threshold (in meters^2)", &RtxOptions::ShadowTerminator::maxAreaObject(), 0.01f, 0.f, 100.f);
      RemixGui::DragFloat("Max Offset Length (in meters)", &RtxOptions::ShadowTerminator::maxLengthObject(), 0.01f, 0.f, 1.f);
      ImGui::EndDisabled();
      ImGui::Unindent();

      RemixGui::Separator();
      RemixGui::Checkbox("Terminator Transition Softening", &RtxOptions::ShadowTerminator::softenObject());

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Texture Streaming [Experimental]", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      if (RtxOptions::TextureManager::hotReload()) {
        ImGui::TextColored(ImVec4{ 250 / 255.F, 176 / 255.F, 50 / 255.F, 1.F }, "Hot-reloading active.");
        ImGui::Dummy({ 0, 2 });
      }
      ImGui::BeginDisabled(!RtxOptions::TextureManager::samplerFeedbackEnable());
      {
        if (RtxOptions::TextureManager::fixedBudgetEnable() && RtxOptions::TextureManager::samplerFeedbackEnable()) {
          if (RemixGui::DragFloatMB_showGB("Texture Budget##1",
                                        &RtxOptions::TextureManager::fixedBudgetMiBObject(),
                                        0.5f, 1.f, 32.f, "%.1f GB", ImGuiSliderFlags_NoRoundToFormat)) {
            ctx->getCommonObjects()->getSceneManager().requestVramCompaction();
          }
        } else {
          // always disabled drag float just to show the available texture cache budget
          ImGui::BeginDisabled(true);
          const char* formatstr = RtxOptions::TextureManager::samplerFeedbackEnable()
            ? "%.1f GB"
            : "UNB%0.0fUND";
          static float s_dummy{};
          s_dummy = RtxOptions::TextureManager::samplerFeedbackEnable()
            ? float(g_streamedTextures_budgetBytes) / 1024.F / 1024.F / 1024.F
            : 0.F;
          RemixGui::DragFloat("Texture Cache##2", &s_dummy, 0.5f, 1.f, 32.f, formatstr, ImGuiSliderFlags_NoRoundToFormat);
          ImGui::EndDisabled();
        }
      }
      {
        ImGui::BeginDisabled(RtxOptions::TextureManager::fixedBudgetEnable());
        if (RemixGui::DragInt("of VRAM is dedicated to Textures",
                            &RtxOptions::TextureManager::budgetPercentageOfAvailableVramObject(),
                            10.F,
                            10,
                            100,
                            "%d%%")) {
          ctx->getCommonObjects()->getSceneManager().requestVramCompaction();
        }
        ImGui::EndDisabled();
      }
      if (RemixGui::Checkbox("Force Fixed Texture Budget", &RtxOptions::TextureManager::fixedBudgetEnableObject())) {
        // budgeting technique changed => ask DXVK to return unused VRAM chunks to OS to better represent consumption
        ctx->getCommonObjects()->getSceneManager().requestVramCompaction();
      }
      ImGui::EndDisabled();

      ImGui::Dummy({ 0, 2 });
      if (RemixGui::CollapsingHeader("Advanced##texstream", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        ImGui::Text("Streamed Texture VRAM usage: %.1f GB", float(g_streamedTextures_usedBytes) / 1024.F / 1024.F / 1024.F);
        ImGui::Dummy({ 0, 2 });
        RemixGui::Separator();
        ImGui::Dummy({ 0, 2 });
        ImGui::TextUnformatted("Warning: toggling this option will enforce a full texture reload.");
        if (RemixGui::Checkbox("Sampler Feedback", &RtxOptions::TextureManager::samplerFeedbackEnableObject())) {
          // sampler feedback ON/OFF changed => free all to refit textures in VRAM
          ctx->getCommonObjects()->getSceneManager().requestTextureVramFree();
        }
        ImGui::Dummy({ 0, 2 });
        RemixGui::Separator();
        ImGui::Dummy({ 0, 2 });
        if (ImGui::Button("Demote All Textures")) {
          ctx->getCommonObjects()->getSceneManager().requestTextureVramFree();
        }
        RemixGui::Checkbox("Reload Textures on Window Resize", &RtxOptions::reloadTextureWhenResolutionChangedObject());
        ImGui::Unindent();
      }
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Terrain [Experimental]")) {
      ImGui::Indent();

      {
        TerrainMode mode = TerrainMode::None;
        if (TerrainBaker::enableBaking()) {
          mode = TerrainMode::TerrainBaker;
        } else {
          if (RtxOptions::terrainAsDecalsEnabledIfNoBaker()) {
            mode = TerrainMode::AsDecals;
          }
        }

        bool terrainModeChanged = IMGUI_ADD_TOOLTIP(
          terrainModeCombo.getKey(&mode),
          "\'Terrain Baker\': rasterize the draw calls marked as \'Terrain\' into a single mesh that would be used for ray tracing.\n"
          "\n"
          "\'Terrain-as-Decals\': draw calls marked as 'Terrain' are ray traced as decals.");

        if (terrainModeChanged) {
          if (mode == TerrainMode::TerrainBaker) {
            RemixGui::CheckRtxOptionPopups(&TerrainBaker::enableBakingObject());
          } else if (mode == TerrainMode::AsDecals) {
            RemixGui::CheckRtxOptionPopups(&RtxOptions::terrainAsDecalsEnabledIfNoBakerObject());
          }
        }

        switch (mode) {
        case TerrainMode::None: {
          TerrainBaker::enableBaking.setDeferred(false);
          RtxOptions::terrainAsDecalsEnabledIfNoBaker.setDeferred(false);
          break;
        }
        case TerrainMode::TerrainBaker: {
          TerrainBaker::enableBaking.setDeferred(true);
          RtxOptions::terrainAsDecalsEnabledIfNoBaker.setDeferred(false);
          break;
        }
        case TerrainMode::AsDecals: {
          TerrainBaker::enableBaking.setDeferred(false);
          RtxOptions::terrainAsDecalsEnabledIfNoBaker.setDeferred(true);
          break;
        }
        default: break;
        }
      }

      RemixGui::Separator();

      if (TerrainBaker::enableBaking()) {
        common->getTerrainBaker().showImguiSettings();
      } else if (RtxOptions::terrainAsDecalsEnabledIfNoBaker()) {
        RemixGui::Checkbox("Over-modulate Blending", &RtxOptions::terrainAsDecalsAllowOverModulateObject());
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Player Model", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      RemixGui::Checkbox("Primary Shadows", &RtxOptions::PlayerModel::enablePrimaryShadowsObject());
      RemixGui::Checkbox("Show in Primary Space", &RtxOptions::PlayerModel::enableInPrimarySpaceObject());
      RemixGui::Checkbox("Create Virtual Instances", &RtxOptions::PlayerModel::enableVirtualInstancesObject());
      if (RemixGui::CollapsingHeader("Calibration", collapsingHeaderClosedFlags)) {
        ImGui::Indent();
        RemixGui::DragFloat("Backward Offset", &RtxOptions::PlayerModel::backwardOffsetObject(), 0.01f, 0.f, 100.f);
        RemixGui::DragFloat("Horizontal Detection Distance", &RtxOptions::PlayerModel::horizontalDetectionDistanceObject(), 0.01f, 0.f, 100.f);
        RemixGui::DragFloat("Vertical Detection Distance", &RtxOptions::PlayerModel::verticalDetectionDistanceObject(), 0.01f, 0.f, 100.f);
        ImGui::Unindent();
      }
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Displacement [Experimental]", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      ImGui::TextWrapped("Warning: This is currently implemented using POM with a simple height map, displacing inwards.  The implementation may change in the future, which could include changes to the texture format or displacing outwards.\nRaymarched POM will use a simple raymarch algorithm, and will show artifacts on thin features and at oblique angles.\nQuadtree POM depends on custom mipmaps with maximums instead of averages, which can be generated using `generate_max_mip.py`.");
      RemixGui::Combo("Mode", &RtxOptions::Displacement::modeObject(), "Off\0Raymarched POM\0Quadtree POM\0");
      RemixGui::Checkbox("Enable Direct Lighting", &RtxOptions::Displacement::enableDirectLightingObject());
      RemixGui::Checkbox("Enable Indirect Lighting", &RtxOptions::Displacement::enableIndirectLightingObject());
      RemixGui::Checkbox("Enable Indirect Hit", &RtxOptions::Displacement::enableIndirectHitObject());
      RemixGui::Checkbox("Enable NEE Cache", &RtxOptions::Displacement::enableNEECacheObject());
      RemixGui::Checkbox("Enable ReSTIR_GI", &RtxOptions::Displacement::enableReSTIRGIObject());
      RemixGui::Checkbox("Enable PSR", &RtxOptions::Displacement::enablePSRObject());
      RemixGui::DragFloat("Global Displacement Factor", &RtxOptions::Displacement::displacementFactorObject(), 0.01f, 0.0f, 20.0f);
      RemixGui::DragFloat("Displacement In Factor", &RtxOptions::Displacement::displacementInFactorObject(), 0.01f, 0.0f, 20.0f);
      RemixGui::DragFloat("Displacement Out Factor", &RtxOptions::Displacement::displacementOutFactorObject(), 0.01f, 0.0f, 20.0f);
      RemixGui::DragInt("Max Iterations", &RtxOptions::Displacement::maxIterationsObject(), 1.f, 1, 256, "%d", sliderFlags);
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Raytraced Render Target [Experimental]", collapsingHeaderClosedFlags)) {
      ImGui::Indent();
      ImGui::TextWrapped("When a screen in-game is displaying the rasterized results of another camera, this can be used to raytrace that scene.\nNote that the render target texture containing the rasterized results needs to be set to `raytracedRenderTargetTextures` in the texture selection menu.");

      RemixGui::Checkbox("Enable Raytraced Render Targets", &RtxOptions::RaytracedRenderTarget::enableObject());
      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("View Distance", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      viewDistanceModeCombo.getKey(&ViewDistanceOptions::distanceModeObject());

      if (ViewDistanceOptions::distanceMode() != ViewDistanceMode::None) {
        viewDistanceFunctionCombo.getKey(&ViewDistanceOptions::distanceFunctionObject());

        if (ViewDistanceOptions::distanceMode() == ViewDistanceMode::HardCutoff) {
          RemixGui::DragFloat("Distance Threshold", &ViewDistanceOptions::distanceThresholdObject(), 0.1f, 0.0f, 0.0f, "%.2f", sliderFlags);
        } else if (ViewDistanceOptions::distanceMode() == ViewDistanceMode::CoherentNoise) {
          RemixGui::DragFloat("Distance Fade Min", &ViewDistanceOptions::distanceFadeMinObject(), 0.1f, 0.0f, ViewDistanceOptions::distanceFadeMax(), "%.2f", sliderFlags);
          RemixGui::DragFloat("Distance Fade Max", &ViewDistanceOptions::distanceFadeMaxObject(), 0.1f, ViewDistanceOptions::distanceFadeMin(), 0.0f, "%.2f", sliderFlags);
          RemixGui::DragFloat("Noise Scale", &ViewDistanceOptions::noiseScaleObject(), 0.1f, 0.0f, 0.0f, "%.2f", sliderFlags);
        }
      }

      ImGui::Unindent();
    }

    if (RemixGui::CollapsingHeader("Material Filtering", collapsingHeaderClosedFlags)) {
      ImGui::Indent();

      RemixGui::Checkbox("Use White Material Textures", &RtxOptions::useWhiteMaterialModeObject());
      RemixGui::Separator();
      constexpr float kMipBiasRange = 32;
      RemixGui::DragFloat("Mip LOD Bias", &RtxOptions::nativeMipBiasObject(), 0.01f, -kMipBiasRange, kMipBiasRange, "%.2f", sliderFlags);
      RemixGui::DragFloat("Upscaling LOD Bias", &RtxOptions::upscalingMipBiasObject(), 0.01f, -kMipBiasRange, kMipBiasRange, "%.2f", sliderFlags);
      RemixGui::Separator();
      RemixGui::Checkbox("Use Anisotropic Filtering", &RtxOptions::useAnisotropicFilteringObject());
      if (RtxOptions::useAnisotropicFiltering()) {
        RemixGui::DragFloat("Max Anisotropy Samples", &RtxOptions::maxAnisotropySamplesObject(), 0.5f, 1.0f, 16.f, "%.3f", sliderFlags);
      }
      RemixGui::DragFloat("Translucent Decal Albedo Factor", &RtxOptions::translucentDecalAlbedoFactorObject(), 0.01f);
      ImGui::Unindent();
    }

    if (!RtCamera::enableFreeCamera() &&
        RemixGui::CollapsingHeader("Anti-Culling", collapsingHeaderClosedFlags)) {

      ImGui::Indent();

      if (ctx->getCommonObjects()->getSceneManager().isAntiCullingSupported()) {
        RemixGui::Checkbox("Anti-Culling Objects", &RtxOptions::AntiCulling::Object::enableObject());
        if (RtxOptions::AntiCulling::Object::enable()) {
          RemixGui::Checkbox("High precision Anti-Culling", &RtxOptions::AntiCulling::Object::enableHighPrecisionAntiCullingObject());
          if (RtxOptions::AntiCulling::Object::enableHighPrecisionAntiCulling()) {
            RemixGui::Checkbox("Infinity Far Frustum", &RtxOptions::AntiCulling::Object::enableInfinityFarFrustumObject());
          }
          RemixGui::Checkbox("Enable Bounding Box Hash For Duplication Check", &RtxOptions::AntiCulling::Object::hashInstanceWithBoundingBoxHashObject());
          RemixGui::InputInt("Instance Max Size", &RtxOptions::AntiCulling::Object::numObjectsToKeepObject(), 1, 1, 0);
          RemixGui::DragFloat("Anti-Culling Fov Scale", &RtxOptions::AntiCulling::Object::fovScaleObject(), 0.01f, 0.1f, 2.0f);
          RemixGui::DragFloat("Anti-Culling Far Plane Scale", &RtxOptions::AntiCulling::Object::farPlaneScaleObject(), 0.1f, 0.1f, 10000.0f);
        }
        RemixGui::Separator();
        RemixGui::Checkbox("Anti-Culling Lights", &RtxOptions::AntiCulling::Light::enableObject());
        if (RtxOptions::AntiCulling::Light::enable()) {
          RemixGui::InputInt("Max Number Of Lights", &RtxOptions::AntiCulling::Light::numLightsToKeepObject(), 1, 1, 0);
          RemixGui::InputInt("Max Number of Frames to keep lights", &RtxOptions::AntiCulling::Light::numFramesToExtendLightLifetimeObject(), 1, 1, 0);
          RemixGui::DragFloat("Anti-Culling Lights Fov Scale", &RtxOptions::AntiCulling::Light::fovScaleObject(), 0.01f, 0.1f, 2.0f);
        }
      } else {
        ImGui::Text("The game doesn't set up the View Matrix, \nAnti-Culling is disabled to prevent visual corruption.");
      }

      ImGui::Unindent();
    }

    ImGui::PopItemWidth();
  }

  void ImGUI::render(const Rc<DxvkContext>& ctx, VkExtent2D surfaceSize) {
    ScopedGpuProfileZone(ctx, "ImGUI Render");

    const HWND gameHwnd = ctx->getCommonObjects()->getLastKnownWindowHandle();
    
    // We need a window to render the GUI and for input to work correctly
    if (gameHwnd == 0) {
      return;
    }

    if (m_overlayWin.ptr() != nullptr) {
      m_overlayWin->update(gameHwnd);
    }

    ImGui::SetCurrentContext(m_context);
    ImPlot::SetCurrentContext(m_plotContext);

    // Sometimes games can change windows on us, so we need to check that here and tell ImGUI
    if (m_gameHwnd != gameHwnd) {
      m_gameHwnd = gameHwnd;

      if (m_init) {
        ImGui_ImplWin32_Shutdown();
      }

      ImGui_ImplWin32_Init(gameHwnd);
    }

    if (!m_init) {
      ImGui_ImplDxvk::Init(m_device);

      //execute a gpu command to upload imgui font textures
      createFontsTexture(ctx);

      m_init = true;
    }

    ImGui_ImplDxvk::NewFrame();
    ImGui_ImplWin32_NewFrame(); 

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float) surfaceSize.width, (float) surfaceSize.height);

    ImGui::NewFrame();

    update(ctx);

    ImGui_ImplDxvk::RenderDrawData(ImGui::GetDrawData(), ctx.ptr(), surfaceSize.width, surfaceSize.height);
  }

  void ImGUI::createFontsTexture(const Rc<DxvkContext>& ctx) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplDxvk::Data* bd = (ImGui_ImplDxvk::Data*)io.BackendRendererUserData;
    
    // Range of characters we want to use the primary font
    ImVector<ImWchar> characterRange;
    {
      ImFontGlyphRangesBuilder builder;
      builder.AddText("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$ % &\'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c");
      builder.BuildRanges(&characterRange);
    }

    // Range of characters we want to use the second (monospaced) font for
    ImVector<ImWchar> numericalRange;
    {
      ImFontGlyphRangesBuilder builder;
      builder.AddText("0123456789");
      builder.BuildRanges(&numericalRange);
    }

    // Build a second font, where all characters are consistent.  This will be used for non-field/title text
    ImVector<ImWchar> allRange;
    {
      ImFontGlyphRangesBuilder builder;
      builder.AddText("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$ % &\'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c");
      builder.BuildRanges(&allRange);
    }

    // Normal Size Font (Default)

    ImFontConfig normalFontCfg = ImFontConfig();
    normalFontCfg.SizePixels = 16.f;
    normalFontCfg.FontDataOwnedByAtlas = false;

    const size_t nvidiaSansLength = sizeof(___NVIDIASansMd) / sizeof(___NVIDIASansMd[0]);
    const size_t nvidiaSansBdLength = sizeof(___NVIDIASansBd) / sizeof(___NVIDIASansBd[0]);
    const size_t robotoMonoLength = sizeof(___RobotoMonoRg) / sizeof(___RobotoMonoRg[0]);

    {
      // Add letters/symbols (NVIDIA-Sans)
      m_regularFont = io.Fonts->AddFontFromMemoryTTF(&___NVIDIASansMd[0], nvidiaSansLength, 0, &normalFontCfg, characterRange.Data);

      // Enable merging
      normalFontCfg.MergeMode = true;

      // Add numbers (Roboto-Mono)
      io.Fonts->AddFontFromMemoryTTF(&___RobotoMonoRg[0], robotoMonoLength, 0, &normalFontCfg, numericalRange.Data);

      normalFontCfg.MergeMode = false;
      m_boldFont = io.Fonts->AddFontFromMemoryTTF(&___NVIDIASansBd[0], nvidiaSansBdLength, 0, &normalFontCfg, allRange.Data);
    }

    // Large Size Font

    ImFontConfig largeFontCfg = ImFontConfig();
    largeFontCfg.SizePixels = 24.f;
    largeFontCfg.FontDataOwnedByAtlas = false;

    {
      // Add letters/symbols (NVIDIA-Sans)
      m_largeFont = io.Fonts->AddFontFromMemoryTTF(&___NVIDIASansBd[0], nvidiaSansLength, 0, &largeFontCfg, characterRange.Data);

      // Enable merging
      largeFontCfg.MergeMode = true;

      // Add numbers (Roboto-Mono)
      io.Fonts->AddFontFromMemoryTTF(&___RobotoMonoRg[0], robotoMonoLength, 0, &largeFontCfg, numericalRange.Data);
    }

    // Build the fonts

    io.Fonts->Build();

    // Apply the correct default font based on largeUiMode setting
    io.FontDefault = largeUiMode() ? m_largeFont : m_regularFont;


    // Allocate/upload glyph cache...

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    size_t row_pitch = (size_t)width * 4 * sizeof(char);
    size_t upload_size = height * row_pitch;

    VkResult err;

    // Create the Image:
    {
      DxvkImageCreateInfo info = {};
      info.type = VK_IMAGE_TYPE_2D;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent.width = width;
      info.extent.height = height;
      info.extent.depth = 1;
      info.mipLevels = 1;
      info.numLayers = 1;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.layout = VK_IMAGE_LAYOUT_GENERAL;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      m_fontTexture = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, DxvkMemoryStats::Category::AppTexture, "imgui font texture");
      bd->FontImage = m_fontTexture->handle();
    }

    // Create the Image View:
    {
      DxvkImageViewCreateInfo info = {};
      info.type = VK_IMAGE_VIEW_TYPE_2D;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
      info.numLevels = 1;
      info.numLayers = 1;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      m_fontTextureView = m_device->createImageView(m_fontTexture, info);
      bd->FontView = m_fontTextureView->handle();
    }

    ctx->updateImage(m_fontTexture,
      VkImageSubresourceLayers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      VkOffset3D{ 0, 0, 0 },
      m_fontTexture->mipLevelExtent(0),
      pixels, row_pitch, upload_size);

    Rc<DxvkSampler> sampler = m_device->getCommon()->getResources().getSampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // Store our identifier
    io.Fonts->SetTexID(ImGui_ImplDxvk::AddTexture(sampler, m_fontTextureView));
  }

  bool ImGUI::checkHotkeyState(const VirtualKeys& virtKeys, const bool allowContinuousPress) {
    bool result = false;
    if(virtKeys.size() > 0) {
      auto& io = ImGui::GetIO();
      result = true;
      for(const auto& vk : virtKeys) {
        if(vk.val == VK_SHIFT) {
          result = result && io.KeyShift;
        } else if(vk.val == VK_CONTROL) {
          result = result && io.KeyCtrl;
        } else if(vk.val == VK_MENU) {
          result = result && io.KeyAlt;
        } else {
          ImGuiKey key = ImGui::GetKeyIndex(ImGui_ImplWin32_VirtualKeyToImGuiKey(vk.val));
          if (allowContinuousPress) {
            result = result && ImGui::IsKeyDown(key);
          } else {
            result = result && ImGui::IsKeyPressed(key, false);
          }
        }
      }
    }
    return result;
  }

  void ImGUI::onCloseMenus() {
    // When closing the menus, try and free up some extra memory, just in case
    //  the user has toggled a bunch of systems while in menus causing an artificial
    //  inflation.
    freeUnusedMemory();

    ::ShowCursor(m_prevCursorVisible);
    if (RtxOptions::restoreCursorPosition()) {
      ::SetPhysicalCursorPos(m_cachedGameCursorX, m_cachedGameCursorY);
    }
  }

  void ImGUI::onOpenMenus() {
    // Before opening the menus, try free some memory, the idea being the 
    //  user may want to make some changes to various settings and so they
    //  should have all available memory to do so.
    freeUnusedMemory();

    CURSORINFO info;
    GetCursorInfo(&info);
    m_prevCursorVisible = info.flags == CURSOR_SHOWING;

    // Use physical cursor position to avoid DPI scaling issues
    POINT pt;
    ::GetPhysicalCursorPos(&pt);
    m_cachedGameCursorX = pt.x;
    m_cachedGameCursorY = pt.y;
  }

  void ImGUI::freeUnusedMemory() {
    if (!m_device) {
      return;
    }

    m_device->getCommon()->getSceneManager().requestVramCompaction();
  }

}
