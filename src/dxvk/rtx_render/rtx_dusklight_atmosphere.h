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
#pragma once

#include "dxvk_include.h"
#include "dxvk_context.h"
#include "rtx_resources.h"
#include "rtx_dusklight_env.h"

// Note: Forward declared rather than included. This header reaches every translation unit through
// dxvk_objects.h, and neither the shader side argument struct nor the fog state needs to come along
// for that ride.
struct DusklightCompositeArgs;

namespace dxvk {

  class DxvkDevice;
  class SceneManager;
  class RtxContext;
  struct FogState;

  // One participating medium per frame, derived from the game's own environment feed, shared by
  // everything that has an opinion about the air: the volumetrics, the fog term in the composite,
  // and the sky.
  //
  // The unification is not an optimisation, it is the correctness argument. This game authors its
  // fog colour, its fog distances and every one of its sky colours in the same palette entry,
  // picks them with the same time of day and weather indices, and blends them in the same call -
  // so its fog colour *is* its sky colour and distant terrain dissolves into the sky because
  // somebody drew it that way. Physics says the same thing from the other end: aerial perspective
  // and sky colour are one scattering integral evaluated over different path lengths. A renderer
  // that derives the two separately is computing the same quantity twice and will get two answers.
  //
  // Deriving once also means the froxel grid is already integrating the medium the sky is made of,
  // so aerial perspective costs nothing extra and needs no lookup table of its own.
  //
  // Scope: this is the fog half (linear ramp -> extinction) plus a gradient sky dome built from the
  // palette colours. The physically based atmosphere that replaces the gradient, and the stylised
  // versus physical blend weight that governs it, are a later phase; the option surface here is
  // shaped so that landing them does not move anything already in a config file.
  class DxvkDusklightAtmosphere: public RtxPass {

  public:
    explicit DxvkDusklightAtmosphere(DxvkDevice* device);
    ~DxvkDusklightAtmosphere();

    DxvkDusklightAtmosphere(const DxvkDusklightAtmosphere&) = delete;
    DxvkDusklightAtmosphere(DxvkDusklightAtmosphere&&) noexcept = delete;
    DxvkDusklightAtmosphere& operator=(const DxvkDusklightAtmosphere&) = delete;
    DxvkDusklightAtmosphere& operator=(DxvkDusklightAtmosphere&&) noexcept = delete;

    // Everything resolved from one frame of the game's environment feed. Consumers read this and
    // derive nothing themselves; that is what stops the fog and the sky drifting apart.
    struct Derived {
      // The fog derivation is usable this frame.
      bool    fogValid = false;
      // The area has a sky, so a sky light belongs here. False indoors, which is most dungeons.
      bool    outdoor = false;

      // Extinction, per world unit. Deliberately scalar: the game's fog is a colour lerp driven by
      // a single scalar ramp, so it dims every channel by the same fraction. A per channel
      // extinction derived from the fog colour would make the fog's *strength* depend on channel,
      // which the original never does. The colour arrives through scattering instead.
      float   sigma = 0.0f;
      // Linear radiance the fog tends towards. The game authors this as a display colour.
      Vector3 fogRadiance = Vector3(0.0f, 0.0f, 0.0f);

      // The game's own ramp, in world units. rampStart is routinely negative: scripted fog banks
      // set it that way so the ramp is already well underway at the camera.
      float   rampStart = 0.0f;
      float   rampEnd = 0.0f;

      // How far the froxel grid reaches this frame, in world units, after smoothing.
      float   froxelMaxDistance = 0.0f;
    };

    // Resolved lazily and latched per frame, so callers do not have to agree about ordering.
    const Derived& derived() const;

    // The last resolved state, without driving the latch. The UI runs on the presenting thread, and
    // resolving from there would both race the render thread's reads and steal a smoothing step
    // from it, so the panel reads whatever the last rendered frame settled on.
    const Derived& peek() const { return m_derived; }

    // True while the game's feed is driving the medium. Everything guarded by this must be a no-op
    // when it is false, so that turning the bridge off leaves a build that behaves as upstream.
    bool active() const;
    bool outdoor() const;
    bool skyActive() const;

    // A1. Replaces the fog state Remix picked from the draw stream. The game sets fog per object
    // and Remix keeps whichever it happened to see first, so the captured value is decided by
    // submission order; the game's global environment fog is the room's actual answer.
    void applyFogOverride(FogState& fog, bool fogReplacedByMaterial) const;

    // A2/A4. Overwrites the medium and the grid extent the volumetrics would otherwise derive.
    void applyVolumeArgs(Vector3& attenuationCoefficient,
                         Vector3& scatteringCoefficient,
                         float& transmittanceMeasurementDistance,
                         Vector3& multiScatteringEstimate) const;

    // A3. Hands the composite the far half of the fog - everything past where the froxel grid
    // stops, which the volumetric integration cannot reach.
    void fillCompositeArgs(DusklightCompositeArgs& args) const;

    // B1. Regenerates the sky image and re-arms the dome light. Must run every frame: the light
    // manager clears the active dome light at the top of each one.
    void prepareSceneData(Rc<RtxContext> ctx, SceneManager& sceneManager);

    void showImguiSettings();

  private:
    virtual bool isEnabled() const override;
    virtual void releaseTargetResource() override;

    void resolveIfStale() const;
    Derived resolve() const;

    DxvkDevice* m_device;

    mutable Derived m_derived;
    mutable uint32_t m_resolvedFrame = UINT32_MAX;
    // Smoothed grid extent carried across frames. The game eases its own fog transitions, so
    // following them rather than snapping is faithful as well as cheap on the denoiser.
    mutable float m_smoothedFroxelMaxDistance = 0.0f;

    // Owned once and kept alive for the process, not rebuilt per frame: the dome light holds a
    // bindless index into it, and dropping the image for even one frame drops the sky back to
    // Remix's own probe.
    Resources::Resource m_skyTexture;
    uint32_t m_skyTextureIndex = UINT32_MAX;

    RTX_OPTION("rtx.dusklight.atmosphere", bool, enable, false,
               "Derives one participating medium from the game's environment feed and gives it to the volumetrics, the fog and the sky together.\n"
               "The game authors its fog colour and its sky colours in the same palette entry and blends them in the same call, so they are one system in the "
               "original and splitting them here is what makes fog and sky disagree. Off by default; has no effect unless the game's bridge is running "
               "(rtx.dusklight.env.enable) and reporting fog (rtx.dusklight.env.fogActive).");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, zHalfMin, 100.0f,
                    "Smallest half density distance the medium is allowed, in world units - one metre at this game's scale.\n"
                    "The medium is solved by matching the game's linear ramp at the point where it is half opaque. Scripted fog banks put that point behind the "
                    "camera, which would send the density to infinity, so it is clamped here. Raising it thins the very densest fog; lowering it lets a whiteout "
                    "close in harder.\n"
                    "UNVALIDATED: chosen analytically, never measured against a running build. See documentation/DusklightAtmosphere.md.",
                    args.minValue = 1.0f,
                    args.maxValue = 10000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, densityScale, 1.0f,
                    "Scales the derived extinction. 1.0 reproduces the density of the game's own ramp at its half opaque point; higher is thicker.",
                    args.minValue = 0.0f,
                    args.maxValue = 8.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, fogRadianceScale, 1.0f,
                    "Scales the game's fog colour on its way to becoming radiance.\n"
                    "The game authored that colour to be blended over a finished, display referred image; here it is a quantity of light in a linear frame that "
                    "is still going to be tone mapped. The conversion is principled but the overall level is not something the original had an opinion about, so "
                    "this is the knob for it. Calibrate with auto exposure off.",
                    args.minValue = 0.0f,
                    args.maxValue = 16.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", Vector3, singleScatteringAlbedo, Vector3(0.9f, 0.9f, 0.9f),
                    "How much of what the medium extinguishes is scattered rather than absorbed, per channel.\n"
                    "Near 1 gives a bright fog that carries light shafts well; lower values give a sooty, absorbing haze. The medium's hue comes from the game's "
                    "fog colour rather than from here, so this stays close to neutral by default.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, multiScatteringScale, 0.25f,
                    "How strongly the game's fog colour is injected as a constant ambient term in the medium.\n"
                    "Without it the fog is only as bright as the lights reaching it, which in an unlit interior is nothing - the game's fog is never black, "
                    "because it was a colour blend rather than a simulation. This is what keeps a dark room's fog the colour the palette asked for.",
                    args.minValue = 0.0f,
                    args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelRangeScale, 0.6f,
                    "How much of the game's fog range the froxel grid is sized to cover.\n"
                    "The grid gets a fixed number of depth slices wherever it is pointed, so sizing it from the game's own fog range is what puts them where the "
                    "fog actually is: tight inside a dense interior, wide across an open field. Costs nothing - the slice count does not change, only its reach.\n"
                    "Deliberately below 1. At 1 the grid swallows the whole ramp, the composite's far half has nothing left to do, and the fog never closes to "
                    "fully opaque the way the original does - an exponential medium only ever asymptotes towards that. Leaving the last stretch to the ramp is "
                    "what buys the closure, and it also spends the fixed slice count on the near field where light shafts actually live.\n"
                    "UNVALIDATED: never measured against a running build.",
                    args.minValue = 0.1f,
                    args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelMaxDistanceMinMeters, 5.0f,
                    "Floor on the froxel grid's reach, in metres. Stops a scripted whiteout shrinking the grid to nothing.",
                    args.minValue = 0.5f,
                    args.maxValue = 100.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelMaxDistanceMaxMeters, 120.0f,
                    "Ceiling on the froxel grid's reach, in metres.\n"
                    "Past this the far slices get thick enough that light shafts in them go blocky, and the distance is better served by the ramp in the composite "
                    "that takes over beyond the grid anyway.",
                    args.minValue = 5.0f,
                    args.maxValue = 1000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelSmoothingRate, 0.08f,
                    "How quickly the froxel grid's reach follows the game's fog range, per frame, 0..1. 1 snaps immediately.\n"
                    "Moving it invalidates nothing - the previous extent is carried explicitly so reprojection stays correct - but a fast move still drags the "
                    "temporal history through a changing grid, and the game eases its own fog transitions anyway.",
                    args.minValue = 0.005f,
                    args.maxValue = 1.0f);

    RTX_OPTION("rtx.dusklight.atmosphere", bool, skyEnable, false,
               "Builds the sky from the colours the game paints its own sky dome with, and hands it to Remix as a dome light.\n"
               "The game's dome carries no texture - it is painted by setting a handful of colours per frame - so there is nothing for Remix to hash and it can "
               "never be tagged as sky. Generating the sky from those same colours sidesteps that permanently, and unlike the auto detected sky probe, which is "
               "rasterized into the game's own 8 bit target and so can never be brighter than 1, this is real high dynamic range: it can light the scene at the "
               "intensity an actual sky does.\n"
               "Turn on rtx.dusklight.game.hideVrbox with it, and set rtx.skyAutoDetect to None, or you will be looking at three skies at once.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyIntensity, 6.0f,
                    "Radiance of the generated sky, as a multiplier on the game's sky colours.\n"
                    "This is the sky's brightness *and* how strongly it lights the world - one number, because they are the same thing. A dome at radiance L and "
                    "a distant light at radiance R deposit light in the same proportion as L to R, and a real midday sky is about a fifth of its sun.\n"
                    "The multiplier is large because the palette colours it scales are small by the time they get here: they are authored as display colours, so "
                    "they are decoded out of gamma first, which takes a mid blue of 0.5 down to about 0.2. Against the default sunIntensity of 5, landing the "
                    "sky's share near a fifth therefore wants a multiplier around 6, not around 1. If you change sunIntensity, scale this with it to hold the "
                    "ratio - the two are one relationship, not two settings.",
                    args.minValue = 0.0f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyHorizonSharpness, 2.5f,
                    "How tightly the horizon haze hugs the horizon.\n"
                    "The game's gradient is carried by the vertices of a dome model we do not have, so this stands in for its shape. Higher values keep the haze "
                    "in a thin band and let the sky colour own most of the dome; lower values bleed it further up.",
                    args.minValue = 0.25f,
                    args.maxValue = 16.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyGroundFraction, 0.35f,
                    "How much of the lower hemisphere is treated as ground bounce rather than sky, 0..1.\n"
                    "A dome light wraps the whole sphere, so without this the scene is lit from below by a copy of the sky and everything loses its sense of "
                    "sitting on the ground.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
  };

}
