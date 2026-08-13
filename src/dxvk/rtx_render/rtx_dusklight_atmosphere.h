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

#include <vector>

// Note: Forward declared rather than included. This header reaches every translation unit through
// dxvk_objects.h, and neither the shader side argument struct nor the fog state needs to come along
// for that ride.
struct DusklightCompositeArgs;

namespace dxvk {

  class DxvkDevice;
  class SceneManager;
  class RtxContext;
  struct FogState;

  // One participating medium per frame, derived from the game's own environment feed and shared by
  // everything with an opinion about the air: the volumetrics, the fog term in the composite, and
  // the sky.
  //
  // Deriving it once is the correctness argument, not an optimisation. The game authors its fog
  // colour, its fog distances and every one of its sky colours in the same palette entry, picks
  // them with the same time of day and weather indices and blends them in the same call, so its fog
  // colour *is* its sky colour; physics agrees from the other end, aerial perspective and sky colour
  // being one scattering integral over different path lengths. Derive them separately and you have
  // computed one quantity twice and will get two answers.
  //
  // Covers the fog (linear ramp -> extinction), the palette gradient sky dome, and the Hillaire
  // physical sky that blends over it by sun elevation. Design, measurements and the blend weight:
  // documentation/DusklightAtmosphere.md - its §12 is what is tested and what is not.
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
      // one scalar ramp, so it dims every channel by the same fraction; a per channel extinction
      // pulled out of the fog colour would make the fog's *strength* channel dependent, which the
      // original never does. The colour arrives through scattering instead. DusklightAtmosphere.md §5.1.
      //
      // In top up mode this is capped so the medium can never be denser than the game's own ramp by
      // more than clearZoneTolerance - see sigmaMatched below for what it would otherwise have been.
      float   sigma = 0.0f;
      // What sigma would be from the half density match alone, before the clear zone cap. Equal to
      // sigma whenever the cap is not binding, which is what makes the pair readable in a log.
      float   sigmaMatched = 0.0f;
      // Linear radiance the fog tends towards. The game authors this as a display colour.
      Vector3 fogRadiance = Vector3(0.0f, 0.0f, 0.0f);
      // What the medium's ambient in-scatter and the ramp both blend towards: fogRadiance steered
      // towards the generated sky dome's mean radiance by skyAmbientWeight. Outdoors that turns the
      // fog's colour into an actual radiance measured off the sky the scene is standing under,
      // rather than a palette colour authored against a display; indoors, where there is no dome, it
      // stays the palette colour.
      Vector3 fogAmbient = Vector3(0.0f, 0.0f, 0.0f);
      // How far fogAmbient came from the dome rather than the palette, after the sky and validity
      // checks. The composite gets the same number so the near and far halves cannot disagree.
      float   skyAmbientWeight = 0.0f;
      // What the palette's fog colour was divided by to turn a display colour into a radiance, or
      // 1.0 when exposureFogMode leaves it alone. This is 1/exposure, so a value below 1 means the
      // scene is bright and the fog was scaled up to match; above 1 means a dark scene and the fog
      // was scaled down so it does not lift the blacks.
      float   exposureCorrection = 1.0f;

      // Peak amount, 0..1, by which the medium out-fogs the game's ramp, and the distance it happens
      // at. Zero means the medium is nowhere thicker than the original's fog, which is the condition
      // the top up ramp needs in order to be exact rather than clamped.
      float   excessPeak = 0.0f;
      float   excessPeakDistance = 0.0f;

      // Forward scattering of the fog medium, handed to the volumetrics in place of
      // rtx.volumetrics.anisotropy. Not the same quantity as the sky model's mieAnisotropy, which
      // shapes the glow around the sun in the generated dome and never reaches the froxel grid.
      float   fogAnisotropy = 0.0f;

      // The game's own ramp, in world units. rampStart is routinely negative: scripted fog banks
      // set it that way so the ramp is already well underway at the camera.
      float   rampStart = 0.0f;
      float   rampEnd = 0.0f;

      // How far the froxel grid reaches this frame, in world units, after smoothing.
      float   froxelMaxDistance = 0.0f;

      // 0 is the game's own gradient, 1 is the scattering model. See resolvePhysicalWeight for why
      // it is shaped the way it is.
      float   physicalWeight = 0.0f;
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
    float resolvePhysicalWeight() const;

    // Peak amount by which a homogeneous medium of the given extinction is thicker than the game's
    // linear ramp, searched over the whole ramp. Positive means the medium out-fogs the original
    // somewhere - almost always just before fogStartZ, where the original is still perfectly clear
    // and an exponential has already been extinguishing since the camera.
    static float rampExcessPeak(float sigma, float start, float end, float& peakDistance);
    // Largest extinction whose peak excess stays within the tolerance. Monotone in sigma, so a
    // bisection is exact to within its own resolution and needs no starting guess.
    static float solveSigmaWithinTolerance(float sigmaMatched, float start, float end, float tolerance);

    // 1 / the exposure the tonemapper is about to apply, under the selected mode; 1.0 when the mode
    // is off or the area is not one it covers. See the exposureFogMode option for why this is the
    // faithful reading of the game's fog colour rather than a correction bolted onto it.
    float resolveExposureCorrection(bool outdoor) const;

    // One line per distinct fog state, capped. Everything the fog's level and shape depend on, in
    // one place, so a play session settles it without anyone being asked to judge a colour.
    void logFogStateOnce(const Derived& d) const;

    // Instrumentation only, and deliberately narrow: it reports which colour patterns actually
    // reach the Palace of Twilight bypass in resolvePhysicalWeight, so one play session can settle
    // whether that bypass has any target at all. See DusklightAtmosphere.md 8.6 for what each
    // possible result means and what will be done about it.
    void logColpatOnce(int colpat, bool guardFired) const;

    // The two lookup tables depend on the medium and on nothing else - not the sun, not the view -
    // so they are rebuilt only when the medium actually changes. That is what makes a physically
    // based sky affordable per frame at all.
    bool mediumChangedSince(const Vector3& skyColor, float influence) const;

    DxvkDevice* m_device;

    mutable Derived m_derived;
    mutable uint32_t m_resolvedFrame = UINT32_MAX;
    // Smoothed grid extent carried across frames. The game eases its own fog transitions, so
    // following them rather than snapping is faithful as well as cheap on the denoiser. A large
    // jump - a room change or an area load - snaps instead; see resolve().
    mutable float m_smoothedFroxelMaxDistance = 0.0f;

    // One bit per colour pattern already reported by logColpatOnce. The game's own pattern switch
    // (d_kankyo.cpp:1896-1990) accepts 0-63 and nothing wider, so a 64 bit mask caps the whole
    // instrument at 64 lines for a session - in practice a handful - with one more line if a value
    // ever arrives outside that range.
    mutable uint64_t m_loggedColpatMask = 0;
    mutable bool m_loggedColpatOutOfRange = false;

    // One entry per distinct fog state already reported, and a flag for the notice printed when the
    // cap is reached. Bounded by construction: a session that wanders through more than
    // kMaxLoggedFogStates distinct ramps gets one line saying so and then silence, rather than a
    // line per area transition for as long as the session lasts.
    mutable std::vector<uint64_t> m_loggedFogStates;
    mutable bool m_loggedFogStateOverflow = false;

    // Owned once and kept alive for the process, not rebuilt per frame: the dome light holds a
    // bindless index into it, and dropping the image for even one frame drops the sky back to
    // Remix's own probe.
    Resources::Resource m_skyTexture;
    uint32_t m_skyTextureIndex = UINT32_MAX;

    Resources::Resource m_transmittanceLut;
    Resources::Resource m_multiScatterLut;

    // The dome reduced to one radiance, and the ring that carries it back to the host. Written by
    // the reduction pass into the device local buffer, copied into a host visible ring, and read
    // kMaxFramesInFlight frames later so nothing ever waits on the GPU. The lag is frames; the sky
    // moves over minutes. rtx/pass/dusklight/dusklight_sky_stats.h says why the fog needs this.
    Rc<DxvkBuffer> m_skyStatsGpu;
    Rc<DxvkBuffer> m_skyStatsHost;
    Vector3 m_skyAmbient = Vector3(0.0f, 0.0f, 0.0f);
    bool m_skyAmbientValid = false;
    // What the tables were last built for. A rebuild is triggered by a change here, not by a frame
    // boundary.
    Vector3 m_lutSkyColor = Vector3(-1.0f, -1.0f, -1.0f);
    float m_lutPaletteInfluence = -1.0f;

    RTX_OPTION("rtx.dusklight.atmosphere", bool, enable, false,
               "Derives one participating medium from the game's environment feed and gives it to the volumetrics, the fog and the sky together.\n"
               "The game authors its fog colour and its sky colours in the same palette entry and blends them in the same call, so they are one system in the "
               "original and splitting them here is what makes fog and sky disagree. Off by default, and inert unless the game's bridge is running "
               "(rtx.dusklight.env.enable). The fog half additionally waits on rtx.dusklight.env.fogActive; the sky half does not, since an area can have a "
               "sky and no haze in it.");
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
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, multiScatteringScale, 1.0f,
                    "How strongly the fog's own colour is injected as an ambient in-scatter term in the medium.\n"
                    "It has to exist because Remix's froxel grid is lit by next event estimation over the RTXDI light list, and that list holds five light "
                    "types - sphere, rect, disk, cylinder, distant. There is no dome light in it. So the fog is lit by the sun and by analytic lights and by "
                    "nothing else: outdoors, fog standing in shadow receives nothing at all from the sky. This term is what it receives instead.\n"
                    "1.0 is not a taste setting, it is the value at which the near half of the fog and the far ramp reach the same colour: the estimate is "
                    "divided by the single scattering albedo on the way in, so the integral of the ambient in-scatter lands on exactly the radiance the ramp "
                    "blends towards. Together with rtx.dusklight.atmosphere.fogRampMode = 1 that makes the composite's output algebraically equal to the "
                    "original's fog formula. Moving it away from 1.0 breaks that identity and makes the fog change colour with distance.\n"
                    "It defaulted to 0.25 before 2026-08-13, and the albedo division did not exist, so the near half of the fog reached 0.225 of the colour "
                    "the far half reached - a 4.4x step at the handover that no other setting could correct.",
                    args.minValue = 0.0f,
                    args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyAmbientWeight, 1.0f,
                    "How far the fog's colour is taken from the generated sky dome rather than from the game's palette fog colour, 0..1.\n"
                    "The game authors its fog colour and its sky colours in the same palette entry, so they are the same colour by construction - but the "
                    "palette one is a *display* colour, authored to be blended over a finished image, while the dome holds an actual radiance in the same "
                    "linear frame the scene is rendered in. Taking the fog's colour from the dome is therefore both more faithful and better scaled: it is "
                    "bright when the scene is bright and dark when the scene is dark, without anyone tuning a level per area.\n"
                    "The near half uses the dome's mean radiance over the sphere, because the term it feeds is isotropic; the far ramp samples the dome in the "
                    "view direction, which is aerial perspective proper. One weight drives both.\n"
                    "Inert with no sky: indoors, or with rtx.dusklight.atmosphere.skyEnable off, the palette colour is used whatever this says.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", int, exposureFogMode, 1,
                    "Treats the game's fog colour as the display colour it actually is, by dividing it by the exposure the tonemapper is about to apply.\n"
                    "This is the last of the reasons dark scenes read grey. The game blended fog over a finished, already-exposed image, so fog_col means "
                    "'what the screen should show there' - it is not a quantity of light. Used raw in a linear frame it is a fixed radiance, which is far too "
                    "much light for a dark cave and not enough for a sunlit field, and no single value can serve both. Dividing by the exposure makes the fog "
                    "land at the display colour the original intended, whatever the scene's brightness.\n"
                    "It cannot run away: the fog's contribution after tonemapping is (colour / exposure) * exposure, so its *display* value is invariant to "
                    "exposure by construction and adds no gain to the eye adaptation loop.\n"
                    "Only the palette-derived part of the fog colour is corrected. Anything taken from the sky dome "
                    "(rtx.dusklight.atmosphere.skyAmbientWeight) is already a real radiance measured in the renderer's own units and must not be rescaled, so "
                    "outdoors with the dome supplying the colour this does almost nothing either way.\n"
                    "0: Off. The palette colour is used as a radiance, which is the behaviour before 2026-08-13.\n"
                    "1: Indoors only. Applied where the game reports no sky - which is exactly where there is no dome to take a real radiance from, and so "
                    "exactly where the problem still bites. The default.\n"
                    "2: Always. Also applied outdoors, to whatever share of the fog colour still comes from the palette.\n"
                    "Reads the auto exposure multiplier only, not rtx.tonemap.exposureBias: a manual bias is a deliberate look adjustment to the whole image "
                    "and the fog should ride along with it, whereas eye adaptation is an automatic normalisation the original never had.",
                    args.minValue = 0,
                    args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyAmbientScale, 1.0f,
                    "Trim on the dome-derived fog colour, applied before it is blended in by rtx.dusklight.atmosphere.skyAmbientWeight.\n"
                    "The dome average is a measurement rather than a choice, so this exists only for taste - raise it for a hazier, more luminous distance, "
                    "lower it if outdoor fog reads brighter than the terrain it sits in front of.",
                    args.minValue = 0.0f,
                    args.maxValue = 8.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, fogAnisotropy, 0.6f,
                    "Forward scattering of the fog medium, -1..1. Higher pulls in-scattered light into a tighter glow around whatever is casting it.\n"
                    "This is what makes a torch in fog read as a torch in fog and a sunbeam read as a beam. Upstream's rtx.volumetrics.anisotropy defaults to "
                    "0 - perfectly isotropic - which spreads every light's contribution evenly in all directions and leaves shafts flat and shapeless. Real "
                    "atmospheric haze is strongly forward scattering.\n"
                    "Not the same setting as rtx.dusklight.atmosphere.mieAnisotropy, which shapes the sun's glow inside the generated sky image and never "
                    "reaches the froxel grid. Both exist and they are easy to confuse.",
                    args.minValue = -0.95f,
                    args.maxValue = 0.95f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", int, fogRampMode, 1,
                    "Which part of the distance the game's own fog ramp is responsible for.\n"
                    "0: Handover. The froxel grid owns everything within its reach and the ramp owns everything past it, rebased at the handover. This makes "
                    "the medium responsible for how thick the fog looks in the near field, which a homogeneous medium cannot do faithfully - the original's "
                    "ramp is exactly zero before fogStartZ and an exponential starts extinguishing at the camera, so the near field arrives hazed where the "
                    "original is crisp. Kept as the A/B baseline.\n"
                    "1: Top up. The ramp owns the fog's appearance at every distance, added as the residual against whatever the medium already achieved. The "
                    "surface then arrives attenuated by exactly the game's ramp - this is an algebraic identity, not an approximation, and it holds for any "
                    "density the medium happens to have. The medium's density is therefore free to be set for the quality of light shafts rather than pinned "
                    "to reproducing an opacity curve, which is the whole point.",
                    args.minValue = 0,
                    args.maxValue = 1);
    RTX_OPTION("rtx.dusklight.atmosphere", bool, limitDensityToRamp, true,
               "Caps the medium's extinction so it is never thicker than the game's own fog.\n"
               "The top up ramp can add fog but cannot take it away - a medium denser than the original's ramp leaves the ramp with a negative residual, which "
               "is clamped to zero rather than subtracted back out, because dividing integrated light back out amplifies the froxel grid's noise. So the "
               "condition is enforced ahead of time here instead, by lowering sigma until the medium fits under the ramp with at most "
               "rtx.dusklight.atmosphere.clearZoneTolerance to spare.\n"
               "The atmosphere panel reports both the matched and the capped extinction, so it is visible when this is binding and by how much. Turning it off "
               "restores the pure half-density match and lets the near field haze over.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, clearZoneTolerance, 0.08f,
                    "How much fog the medium is allowed to add where the game's own ramp has not started yet, 0..0.5.\n"
                    "This is the one real trade in the fog system. The original leaves everything before fogStartZ perfectly clear; a homogeneous medium cannot, "
                    "and the medium is what carries light shafts. At 0 the near field is exactly as crisp as the original and there is almost no medium left to "
                    "scatter anything; higher values buy shaft presence with a thin haze over near geometry.\n"
                    "Read only when rtx.dusklight.atmosphere.limitDensityToRamp is on.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.5f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelRangeScale, 0.6f,
                    "How much of the game's fog range the froxel grid is sized to cover.\n"
                    "The grid gets a fixed number of depth slices wherever it is pointed, so sizing it from the game's own fog range is what puts them where the "
                    "fog actually is: tight inside a dense interior, wide across an open field. Costs nothing - the slice count does not change, only its reach.\n"
                    "Deliberately below 1. At 1 the grid swallows the whole ramp and the composite's far half has nothing left to do, so the fog never closes to "
                    "fully opaque the way the original does - an exponential medium only asymptotes towards that. Leaving the last stretch to the ramp buys the "
                    "closure and spends the slices on the near field where shafts live. documentation/DusklightAtmosphere.md section 5.2.\n"
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
               "The dome is painted by setting a handful of colours per frame rather than by drawing a texture, so generating the sky from those same colours is "
               "the direct translation of it. The result is high dynamic range, non-occluding by construction, and is the same image the physical sky writes into.\n"
               "Not because the alternatives are impossible: the untextured dome can be categorised as sky by geometry hash (rtx.skyBoxGeometries), and "
               "rtx.skyForceHDR lifts the auto detected probe's inherited 8 bit clamp. Both were once claimed otherwise here - see "
               "documentation/DusklightAtmosphere.md sections 14.9 and 14.10.\n"
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
    RTX_OPTION("rtx.dusklight.atmosphere", bool, physicalSky, false,
               "Computes the sky by simulating how light scatters through air, instead of reading the game's gradient off its palette.\n"
               "What this buys is structure the palette cannot describe: the sky correctly brightening towards the horizon and around the sun and darkening "
               "overhead, all of it changing as the sun moves, without anyone tuning it - and, most usefully, shadows filled with a blue that is right rather "
               "than chosen.\n"
               "It does not replace the game's look everywhere, and is not meant to. The blend is driven by how high the sun is, because that is where the two "
               "actually disagree: a real midday sky and this game's midday sky are both a plain blue gradient, while its dusk is deliberately more saturated "
               "than physics would ever produce. See rtx.dusklight.atmosphere.physicalMaxWeight.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, physicalMaxWeight, 1.0f,
                    "How far towards the simulated sky the blend is allowed to go, at its strongest. 0 leaves the game's gradient untouched.\n"
                    "Lower this if midday looks right but you want the game's palette to keep more of a say.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, physicalElevationLowDegrees, 2.0f,
                    "Sun elevation below which the sky is entirely the game's own, in degrees.\n"
                    "The two descriptions diverge most at a low sun: the game's dusk is authored, saturated and unmistakably its own, and a physical one is "
                    "quieter and more graduated. Holding the game's version at the bottom of the arc keeps sunsets recognisable.",
                    args.minValue = -10.0f,
                    args.maxValue = 45.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, physicalElevationHighDegrees, 28.0f,
                    "Sun elevation above which the sky is entirely simulated, in degrees.\n"
                    "Safe to be aggressive here: by this height the game's sky is a plain blue gradient and so is a real one, so the change is nearly invisible "
                    "while everything it brings with it - correct sky fill in shadow, correct haze with distance - is not.",
                    args.minValue = 0.0f,
                    args.maxValue = 90.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, physicalWeatherWeight, 0.25f,
                    "How much simulation survives under the game's non-clear weather patterns, 0..1.\n"
                    "A clear-sky model has nothing to say about an overcast one; the palette does, because someone painted it. This keeps the palette in charge "
                    "when the weather is doing something the physics cannot describe.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, paletteInfluence, 0.5f,
                    "How far the simulated air is steered towards the game's own colours, 0..1.\n"
                    "Applied to the scattering coefficients rather than to the finished image. That distinction is the whole idea: tinting the coefficients means "
                    "the sky and the light it casts stay the same sky, whereas tinting the output would leave a scene lit by one colour and looking at another. "
                    "At 0 the air is Earth's; at 1 its blue is whatever the artists picked.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, mieAnisotropy, 0.76f,
                    "How strongly haze scatters light forwards, 0..0.95. Higher pulls the glow tighter around the sun.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.95f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, multiScatterScale, 1.0f,
                    "Scales light that bounced more than once before reaching the eye.\n"
                    "At 0 you get single scattering only, which is both too dark and too saturated - in a blue-scattering medium the photons that bounce most are "
                    "exactly the blue ones, so throwing them away skews the colour as well as the level.",
                    args.minValue = 0.0f,
                    args.maxValue = 4.0f);

    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyGroundFraction, 0.35f,
                    "How much of the lower hemisphere is treated as ground bounce rather than sky, 0..1.\n"
                    "A dome light wraps the whole sphere, so without this the scene is lit from below by a copy of the sky and everything loses its sense of "
                    "sitting on the ground.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);

    RTX_OPTION("rtx.dusklight.atmosphere", bool, skyMoonEnable, true,
               "Paints the moon into the generated sky.\n"
               "The game hangs its moon on a billboard anchored to the camera, which under a path tracer becomes an occluder that travels with the "
               "player - the measured cause of shadow coverage appearing to wander at night. rtx.dusklight.game.hideSkyBillboards removes it and fixes "
               "that, and takes the visible moon with it. This gives the moon back without giving the problem back: a disc painted into the dome is "
               "correctly placed, moves with the sky rather than with the camera, and cannot cast a shadow because it is not geometry.\n"
               "Only the moon. The sun stays out of this image deliberately - it is an analytic distant light, and baking something that bright into a "
               "dome only ever reached by ray miss would double count it and sample it badly.\n"
               "Inert unless rtx.dusklight.atmosphere.skyEnable is on, and drawn only while the game's celestial body is the moon.\n"
               "The billboard cause was confirmed by testing on 2026-07-29; this replacement is built and CI-green but has never been run in game.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyMoonAngularDiameterDegrees, 5.7f,
                    "Apparent size of the painted moon, in degrees.\n"
                    "The default matches the game's own: its moon quad is 8000 units across at an orbit radius of 80000, which subtends about 5.7 "
                    "degrees - roughly eleven times the real moon, and what a player of this game is used to seeing.",
                    args.minValue = 0.1f,
                    args.maxValue = 30.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyMoonIntensity, 4.0f,
                    "Radiance of the painted moon.\n"
                    "Absolute rather than a multiple of the sky's brightness, so it does not swing with the palette. This is appearance only - the "
                    "moonlight itself comes from the distant light the bridge drives (rtx.dusklight.game.moonIntensity), so raising this makes the moon "
                    "brighter to look at without making the night any brighter to stand in.",
                    args.minValue = 0.0f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyMoonEdgeSoftness, 0.15f,
                    "Softness of the moon's edge, as a fraction of its radius.\n"
                    "A hard edge aliases badly here: the dome is a lat-long map, so its angular sampling rate varies with latitude and a crisp circle "
                    "crawls as the moon moves.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);

    // Two candidate fixes for the same defect, kept side by side deliberately so they can be
    // compared in game rather than argued about. One of them is meant to be deleted once the
    // comparison has been made. Both are untested in game; DusklightAtmosphere.md §12.
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", int, skyFogMode, 1,
                    "How much of the fog a ray that hits nothing - the visible sky - is allowed to pick up.\n"
                    "The distance ramp already refuses to run on the sky; the froxel grid was never told the same thing, so it dims the sky by the "
                    "transmittance of its whole depth and adds that depth's fog colour on top. The sky then reads dim and dingy while the terrain in "
                    "front of it, correctly fading towards the sky's own colour, does not - a horizon seam that worsens with density. It bites harder "
                    "here than elsewhere because this fog is an artistic quantity rather than air: it closes over tens of metres, and all of that lands "
                    "on the sky.\n"
                    "0: Off. The untreated behaviour, kept so the defect can be seen on demand.\n"
                    "1: Exempt. The sky ignores the fog entirely, which is what the original did - it drew its sky with fog switched off at any density. "
                    "Costs light shafts that would have been visible against the sky, since those are the same in-scatter.\n"
                    "2: Weighted. The sky picks up rtx.dusklight.atmosphere.skyFogAmount of the fog, so a genuinely foggy day still veils it and shafts "
                    "against the sky survive in proportion.",
                    args.minValue = 0,
                    args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyFogAmount, 0.15f,
                    "In Weighted mode, how much of the fog the sky picks up. 0 matches Exempt, 1 matches Off.\n"
                    "Low by construction: the point is that the sky should be veiled by weather rather than erased by a medium calibrated to close in "
                    "tens of metres. Read only when rtx.dusklight.atmosphere.skyFogMode is 2.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
  };

}
