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
struct VolumeArgs;

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
      // The palette's fog colour under each of the two conversions, before fogRadianceScale and the
      // exposure correction are applied. Nothing decides on these; they exist so one log line can
      // state what each convention produces for the same input, which is the only way anyone settles
      // which one the fog should be using without being asked to describe a colour.
      Vector3 paletteRaw = Vector3(0.0f, 0.0f, 0.0f);
      Vector3 paletteDecoded = Vector3(0.0f, 0.0f, 0.0f);
      // What the medium's ambient in-scatter and the ramp both blend towards: fogRadiance steered
      // towards the generated sky dome's mean radiance by skyAmbientWeight. Outdoors that turns the
      // fog's colour into an actual radiance measured off the sky the scene is standing under,
      // rather than a palette colour authored against a display; indoors, where there is no dome, it
      // stays the palette colour.
      Vector3 fogAmbient = Vector3(0.0f, 0.0f, 0.0f);
      // How far fogAmbient came from the dome rather than the palette, after the sky and validity
      // checks. The composite gets the same number so the near and far halves cannot disagree.
      float   skyAmbientWeight = 0.0f;
      // What the dome's radiance was multiplied by before the fog took its colour from it. 1.0 in
      // Full radiance mode; in Hue only mode it is the ratio that normalises the dome to the
      // palette's own level, so a value far from 1 is a direct measure of how far apart the two
      // descriptions of the same sky had drifted. Also handed to the composite, so that when
      // fogColorDirectional is on and the far ramp takes its own view-direction dome sample, it
      // normalises that sample by exactly the same amount the near half's sphere average got.
      float   skyLevelScale = 1.0f;
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
      // The other direction, and the one that answers "what did the composite's ramp actually have
      // to do": the peak amount by which the medium falls *short* of the game's ramp, which is the
      // largest residual the top-up has to supply.
      //
      // In Exact ramp mode it is not a peak but a constant, and not always zero: the medium can only
      // start at the camera, so a ramp that starts behind the camera leaves exactly
      // -rampStart / (rampEnd - rampStart) for the composite to add at every distance. Zero whenever
      // the ramp starts at or beyond the camera. "Flat at this value" is then the check - but only
      // out to rampEnd - rampMinRemaining, and only inside froxelMaxDistance. Past the divergence
      // clamp the residual climbs to 1 by construction, and past the grid's reach the raymarch closes
      // against the scalar medium instead, which outdoors is most of the ramp. See resolve().
      float   residualPeak = 0.0f;
      // Meaningless in Exact ramp mode, where the residual is constant over the stretch the medium
      // owns and its two remaining variations are both at known distances rather than at a searched
      // one; left at 0 there rather than filled with a number a reader would take literally.
      float   residualPeakDistance = 0.0f;
      // The excess budget actually used, after clearZoneVeilTarget weighted clearZoneTolerance by
      // how bright the medium is. Equal to clearZoneTolerance wherever the weighting is not binding,
      // which is what makes the pair readable in a log the same way sigma and sigmaMatched are.
      float   clearZoneToleranceUsed = 0.0f;

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

      // Floor on (rampEnd - d) handed to the medium in Exact ramp mode, in world units. Zero in
      // every other mode. The medium's transmittance bottoms out at this divided by the ramp's range
      // IN FRONT OF THE CAMERA, rampEnd - max(rampStart, 0), and not by its full span - which is
      // what makes it exactly fogRampFloor whether or not the ramp starts behind the viewer. So it
      // is the "closes to 99% instead of 100%" number stated as a distance.
      float   rampMinRemaining = 0.0f;

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

    // Whether the call above actually wrote the fog state, as opposed to leaving the game's own raw
    // D3D9 colour in place. rtx_composite.cpp gates rtx.fogColorScale on this rather than on
    // active(), which was the 2026-08-17 shape and left one gap: applyFogOverride also declines when
    // a translucent material has already replaced the fog (SceneManager's
    // m_fogStartInMediumMaterialIndex_inCache), and there the composite would have been handed the
    // game's palette colour with the scale silently switched off - a 4x brightness change to a path
    // this change never meant to touch.
    //
    // Ordering, since this is a latch rather than a pure function: SceneManager::prepareSceneData
    // calls applyFogOverride once per frame and dominates every reader of the fog state, the
    // composite included, so the value read is this frame's. If the scene never resolved, the fog
    // state the composite reads is stale in exactly the same way and by the same amount.
    bool fogColorOverridden() const { return m_fogColorOverridden; }

    // A2/A4. Overwrites the medium and the grid extent the volumetrics would otherwise derive.
    void applyVolumeArgs(Vector3& attenuationCoefficient,
                         Vector3& scatteringCoefficient,
                         float& transmittanceMeasurementDistance,
                         Vector3& multiScatteringEstimate) const;

    // A2b. The game's ramp as an extinction field, for fogRampMode 2. Separate from applyVolumeArgs
    // above because it writes into VolumeArgs itself rather than into the four medium quantities
    // that function negotiates, and because it must run for every frame - it zeroes the mode when
    // the bridge is off, which is what keeps the shader on upstream's homogeneous path there.
    void fillVolumeRampArgs(VolumeArgs& args) const;

    // A3. Hands the composite the far half of the fog - everything past where the froxel grid
    // stops, which the volumetric integration cannot reach.
    void fillCompositeArgs(DusklightCompositeArgs& args) const;

    // B1. Regenerates the sky image and re-arms the dome light. Must run every frame: the light
    // manager clears the active dome light at the top of each one.
    void prepareSceneData(Rc<RtxContext> ctx, SceneManager& sceneManager);

    // The overlay draws this panel as one flat row of hot controls plus four collapsing groups on
    // its Sky tab, so the groups are separate calls rather than one function with labels in it.
    // The headers and the volumetrics BeginDisabled/EndDisabled pairs are the overlay's, which is
    // what lets a group be expanded while volumetrics are off - one function wrapped in
    // BeginDisabled cannot be, because a disabled CollapsingHeader refuses the click that opens it.
    //
    // Atmosphere Enabled and Generate Sky are NOT drawn here: they are on the window's master
    // switch row. Drawing them in both places would give one RtxOption two widgets with the same
    // ImGui ID, since RtxOptionUxWrapper keys its ID off the option's address.
    void showImguiHot();
    void showImguiFog();
    void showImguiFroxelGrid();
    void showImguiSkyShape();
    void showImguiPhysicalSky();
    // Twelve lines of resolved state and no controls, so it lives on the overlay's Readouts tab.
    void showImguiReadouts();

  private:
    virtual bool isEnabled() const override;
    // Everything this pass owns is released here rather than from releaseTargetResource(), which
    // RtxPass also calls on every target resize. Nothing here has an extent to resize to: the sky
    // image is a fixed kSkyWidth x kSkyHeight and both lookup tables are compile-time sized. Moved
    // 2026-08-16 - see the note on the definition for what the old placement cost.
    virtual void onDeactivation() override;

    void resolveIfStale() const;
    Derived resolve() const;
    float resolvePhysicalWeight() const;

    // Peak amount by which a homogeneous medium of the given extinction is thicker than the game's
    // linear ramp, searched over the whole ramp. Positive means the medium out-fogs the original
    // somewhere - almost always just before fogStartZ, where the original is still perfectly clear
    // and an exponential has already been extinguishing since the camera.
    static float rampExcessPeak(float sigma, float start, float end, float& peakDistance);
    // The mirror of the above: peak amount by which the medium is *thinner* than the ramp, which is
    // the largest top-up residual the composite has to supply. The same pair of curves, so the same
    // interval endpoints are candidates - but NOT the same stationary point, which is a maximum of
    // the excess and therefore a minimum of the shortfall. See the definition, where that was
    // written down backwards until 2026-08-18 and where the seed it cost is described.
    static float rampShortfallPeak(float sigma, float start, float end, float& peakDistance);
    // Largest extinction whose peak excess stays within the tolerance. Monotone in sigma, so a
    // bisection is exact to within its own resolution and needs no starting guess.
    static float solveSigmaWithinTolerance(float sigmaMatched, float start, float end, float tolerance);
    // Extinction of a homogeneous medium that reproduces the game's ramp at one anchor distance.
    // Not "half density" - it matches whatever opacity the game's own ramp actually has there, which
    // is 0.5 only when the anchor lands on the ramp's midpoint. See the definition.
    static float sigmaMatchingRampAt(float anchorDistance, float start, float end);

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
    // Two budgets, not one. Outdoor fog states drift continuously with the time of day and interior
    // ones do not, so a single shared budget is always spent by the sky before anyone reaches a
    // dungeon - measured on 2026-08-15, where 32 states went in six and a half minutes of Hyrule
    // Field and Goron Mines and Arbiter's Grounds then logged nothing for the next 36.
    mutable std::vector<uint64_t> m_loggedFogStates;
    mutable bool m_loggedFogStateOverflow = false;
    mutable std::vector<uint64_t> m_loggedFogStatesIndoor;
    mutable bool m_loggedFogStateOverflowIndoor = false;
    // Frame the last fog line was emitted on, so a transition cannot spend the whole budget.
    mutable uint32_t m_lastFogLogFrame = UINT32_MAX;

    // Set by applyFogOverride for exactly the frames on which it wrote the fog state. See
    // fogColorOverridden() above for what reads it and why active() alone was not enough.
    mutable bool m_fogColorOverridden = false;

    // Owned across frames and across target resizes, not rebuilt per frame: the dome light holds a
    // bindless index into it, and dropping the image for even one frame drops the sky back to
    // Remix's own probe. Released when the pass deactivates, and with the object at device teardown.
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
    // Consecutive frames the reduction has been dispatched for. The readback reads a slot written
    // kMaxFramesInFlight frames ago, so for that many frames after the dome comes back the slot
    // still holds whatever the ring had from the last time it ran - a different area's sky. That is
    // not hypothetical: a session log showed a bright outdoor 5.43,3.83,2.24 served for three frames
    // in a dark interior, twice. Counting is exact and costs nothing; clearing the ring is not, since
    // copies queued before the dome went away can still land after the memset.
    uint32_t m_skyStatsFramesActive = 0;
    // What the tables were last built for. A rebuild is triggered by a change here, not by a frame
    // boundary.
    Vector3 m_lutSkyColor = Vector3(-1.0f, -1.0f, -1.0f);
    float m_lutPaletteInfluence = -1.0f;

    RTX_OPTION("rtx.dusklight.atmosphere", bool, enable, false,
               "Derives one participating medium from the game's environment feed and gives it to the volumetrics, the fog and the sky together.\n"
               "The game authors its fog colour and its sky colours in the same palette entry and blends them in the same call, so they are one system in the "
               "original and splitting them here is what makes fog and sky disagree. Off by default, and inert unless the game's bridge is running "
               "(rtx.dusklight.env.enable). The fog half additionally waits on rtx.dusklight.env.fogActive; the sky half does not, since an area can have a "
               "sky and no haze in it.\n"
               "WHAT THIS OVERRIDES IN REMIX, moved here from the overlay's own section 2026-08-17 because it is reference rather than state, and "
               "'I changed it and nothing happened' is the most expensive kind of bug here. While this is on it takes over "
               "rtx.volumetrics.froxelMaxDistanceMeters (sized from the game's fog range instead), rtx.volumetrics.transmittanceColor and "
               "transmittanceMeasurementDistanceMeters, rtx.volumetrics.singleScatteringAlbedo (use the atmosphere's own instead), "
               "rtx.volumetrics.enableFogRemap and enableFogColorRemap (bypassed entirely, so a value there is a false lead), and "
               "rtx.volumetrics.enableAtmosphere (forced on outdoors, since infinite lights need it). While the generated sky is on, "
               "rtx.skyBrightness stops mattering as well: it scales the probe the dome light replaces.\n"
               "rtx.fogColorScale IS NOW ALSO OVERRIDDEN, updated 2026-08-18 - this used to say it merely belonged to a path volumetrics skip. Whenever this "
               "atmosphere writes the fog state it writes its own resolved linear radiance there, and the composite stops applying rtx.fogColorScale to it, "
               "because that option was calibrated against a different game's fog and applying it is what left the legacy depth path and the volumetric path "
               "aiming at two different colours for one fog. rtx.dusklight.atmosphere.fogRadianceScale is the level knob for both. The scale does still apply "
               "in the one case this atmosphere declines to write the fog: when a translucent material has already replaced it, where the colour in the state "
               "is the game's own. rtx.maxFogDistance is untouched and belongs to the legacy depth path, which is skipped whenever volumetrics are running.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, zHalfMin, 100.0f,
                    "Nearest distance the scalar medium is allowed to be matched to the game's ramp at, in world units - one metre at this game's scale.\n"
                    "The medium is solved by matching the game's linear ramp at one anchor distance, which is the ramp's own midpoint. Scripted fog banks put "
                    "that midpoint behind the camera, which would send the density to infinity, so the anchor is clamped here. Raising it thins the very densest "
                    "fog; lowering it lets a whiteout close in harder.\n"
                    "CORRECTED 2026-08-17: the match is to the game's ramp at the anchor, not to half opacity there. The two are the same thing only when the "
                    "clamp is not binding, which is exactly the case scripted fog banks are not - at the Lost Woods mist tag's -2000..200 ramp the anchor "
                    "clamps to 100 units where the game is 95.5% opaque, and asserting 50% there made every scripted fog bank several times too thin.\n"
                    "READ IN EVERY MODE, INCLUDING 2 - corrected 2026-08-18, where this said it was read only in modes 0 and 1 and the panel greyed it out "
                    "accordingly. Mode 2 gives the *view ray* the ramp itself, but the scalar extinction this sets is still what light visibility through the "
                    "fog is attenuated by and what the transmittance measurement distance is derived from, so this is one of only two controls over the one "
                    "quantity sigma still governs there. What it does not do in mode 2 is change how thick the fog looks along the view ray.\n"
                    "A ramp ending at or below this distance puts the anchor at or past the ramp's end, where the game is already fully opaque; the medium is "
                    "then capped at a 0.999 opacity match rather than matched to anything, so for a whiteout closing in over a metre or two it is this number "
                    "and that cap, not the ramp, that decide the density.\n"
                    "UNVALIDATED: chosen analytically, never measured against a running build. See documentation/DusklightAtmosphere.md.",
                    args.minValue = 1.0f,
                    args.maxValue = 10000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, densityScale, 1.0f,
                    "Scales the derived scalar extinction. 1.0 reproduces the opacity of the game's own ramp at the anchor distance "
                    "(rtx.dusklight.atmosphere.zHalfMin); higher is thicker.\n"
                    "READ IN EVERY MODE, INCLUDING 2 - corrected 2026-08-18, where this said it was read only in modes 0 and 1 and the panel greyed it out. In "
                    "mode 2 the view ray's opacity comes from the ramp field and this cannot change it, but the scalar it scales is still what light visibility "
                    "through the fog is attenuated by, so lowering it makes a light shining through the far end of the ramp read brighter without altering how "
                    "thick the fog looks.\n"
                    "In mode 2 the value that reaches the medium is floored just above zero. A scalar of exactly zero there is not 'no attenuation', it is no "
                    "medium at all - the extinction and the scattering coefficient both vanish and the exact ramp becomes a no-op with every readout still "
                    "reporting it as active. The floor is a half-density distance of several hundred thousand units, so nothing measurable is attenuated by it.",
                    args.minValue = 0.0f,
                    args.maxValue = 8.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, fogRadianceScale, 1.0f,
                    "Scales the game's fog colour on its way to becoming radiance.\n"
                    "The game authored that colour to be blended over a finished, display referred image; here it is a quantity of light in a linear frame that "
                    "is still going to be tone mapped. The conversion is principled but the overall level is not something the original had an opinion about, so "
                    "this is the knob for it. Calibrate with auto exposure off.\n"
                    "SINCE 2026-08-17 THIS IS THE LEVEL KNOB FOR BOTH FOG PATHS. rtx.fogColorScale used to set the legacy depth fog's level and this one "
                    "set the volumetric fog's, which is how the two ended up aiming at different colours; wherever this atmosphere writes the fog state "
                    "rtx.fogColorScale is bypassed and this is the only level control. If the fog changed brightness when that landed, this is the number to "
                    "move. The one exception is a fog already replaced by a translucent material, which this atmosphere leaves alone entirely.",
                    args.minValue = 0.0f,
                    args.maxValue = 16.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", int, fogColorSpace, 1,
                    "How the game's palette fog colour is read into the renderer's linear frame. Both fog paths use the answer, so this cannot make them disagree.\n"
                    "WHAT IS ACTUALLY CORRECT, stated plainly because the default is not it: Decoded is. fog_col is a display value - the console clamps it to "
                    "0..255 and blends it into an already-shaded framebuffer with no tone mapping anywhere downstream - and Remix injects into a pre-tonemap "
                    "linear radiance buffer. A gamma-encoded triple used as a radiance is a category error, and it is not only a level error: decoding changes "
                    "the ratios between the channels too, so Raw reads less saturated than the artists' colour actually is.\n"
                    "The default is Raw anyway, and the reason is honest rather than principled. Raw is what the legacy depth path has always done and therefore "
                    "what has been looked at and judged for months; Decoded is about 2.3x darker at a mid grey (0.5 becomes 0.214) and no level calibration ever "
                    "existed to absorb that. Switching the default to Decoded is a look change dressed up as a correctness fix, so it is offered rather than "
                    "imposed - and rtx.dusklight.atmosphere.fogRadianceScale is the number that makes Decoded land where Raw did.\n"
                    "0: Decoded. sRGB gamma decode, which is what the volumetric path did alone before 2026-08-17. The right answer, and the one to move to once "
                    "the level has been recalibrated.\n"
                    "1: Raw. The palette value used directly as a radiance, which is what the depth path did alone. THE DEFAULT.\n"
                    "The fog log prints the palette colour under both conversions on every distinct fog state, so the size of the difference in a given area is "
                    "readable from a session log rather than from a screenshot.",
                    args.minValue = 0,
                    args.maxValue = 1);
    RTX_OPTION("rtx.dusklight.atmosphere", bool, fogColorDirectional, false,
               "Lets the far half of the fog sample the sky dome in the view direction instead of using the same sphere average the near half does.\n"
               "This is aerial perspective proper - distant things fade towards whatever sky is behind them, so fog towards the sun reads warmer than fog away "
               "from it - and it was the behaviour before 2026-08-17. It is off by default because it breaks the top-up identity: the medium's ambient "
               "in-scatter is isotropic and can only be given one colour per frame (the dome's sphere mean), so if the ramp blends towards a different colour "
               "than the medium does, the two halves of the fog are different colours and the algebra in "
               "documentation/DusklightAtmosphere.md section 5.2 no longer holds. With this off both halves use the sphere mean and the identity is exact.\n"
               "WHAT TURNING IT OFF COSTS, stated so it is recognised rather than discovered: the distance loses its directional tint. Fog is one colour across "
               "the frame at a given distance, so a sunset no longer warms the haze on the sun's side. That is a real loss and it is the reason this switch "
               "exists rather than the directional path simply being deleted.\n"
               "Inert with no sky, or with rtx.dusklight.atmosphere.skyAmbientWeight at 0: there is only the palette colour then, and both halves already agree.");
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
                    "blends towards. Together with rtx.dusklight.atmosphere.fogRampMode 1 or 2 that makes the composite's output algebraically equal to the "
                    "original's fog formula. Moving it away from 1.0 breaks that identity and makes the fog change colour with distance.\n"
                    "THE ALBEDO DIVISION IS CORRECT, re-derived 2026-08-17 against the raymarch itself and recorded here because reading one line of it "
                    "suggests otherwise. The march accumulates inScattering = SH + multiScatteringEstimate and only then multiplies by the scattering "
                    "coefficient, so the estimate is added to the RADIANCE rather than to a coefficient, and a constant estimate M over a path of opacity "
                    "(1 - T) contributes M * albedo * (1 - T). Undoing the albedo on the way in is what lands that on fogAmbient * (1 - T). It holds for the "
                    "heterogeneous field of fogRampMode 2 as well, since the per-step density divides the reference extinction back out of both products.\n"
                    "The identity also needs rtx.dusklight.atmosphere.fogColorDirectional off, which is its default. With it on the far ramp deliberately "
                    "blends towards the dome sampled in the view direction while this term can only ever be given one colour per frame, so the two halves of "
                    "the fog aim at different colours and no value here reconciles them.\n"
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
                    "Both halves of the fog use the dome's MEAN RADIANCE OVER THE SPHERE - corrected 2026-08-18, where this said the far ramp sampled the dome "
                    "in the view direction. It did, and that was the defect: the near half's in-scatter term is isotropic and can only be given one colour per "
                    "frame, so a directional far half meant the two halves blended towards different colours wherever the sky is not uniform, which outdoors is "
                    "always. The blend now happens once, here, and the ramp is handed the result already blended. "
                    "rtx.dusklight.atmosphere.fogColorDirectional puts the view-direction sample back for anyone who would rather have aerial perspective's "
                    "tint than the identity; this weight drives both arrangements.\n"
                    "Inert with no sky: indoors, or with rtx.dusklight.atmosphere.skyEnable off, the palette colour is used whatever this says.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", int, exposureFogMode, 2,
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
                    "1: Indoors only. Applied where the game reports no sky.\n"
                    "2: Always. Applied everywhere. THE DEFAULT since 2026-08-13, and the change followed from skyAmbientMode: in Hue only mode the palette "
                    "supplies the fog's level everywhere rather than only indoors, so the correction that gives that level a meaning has to run everywhere "
                    "too. With skyAmbientMode set to Full radiance, Indoors only is the matching choice.\n"
                    "Reads the auto exposure multiplier only, not rtx.tonemap.exposureBias: a manual bias is a deliberate look adjustment to the whole image "
                    "and the fog should ride along with it, whereas eye adaptation is an automatic normalisation the original never had.",
                    args.minValue = 0,
                    args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", int, skyAmbientMode, 1,
                    "What the fog takes from the generated sky dome: its colour, or its colour and its brightness.\n"
                    "The distinction is not a nicety. rtx.dusklight.atmosphere.skyIntensity is a *lighting* calibration - it sets how strongly the dome lights "
                    "the world relative to the sun, and it is 6 because the palette colours it scales are small once decoded out of gamma. Handing that same "
                    "radiance to the fog as its colour makes the fog about six times brighter than the colour the game authored, and the medium's ambient term "
                    "is not shadowed by anything, so an interior gets full open-sky in-scatter inside a sealed room. That was measured, not guessed: with the "
                    "palette's fog colour at 0.12 the dome-derived ambient read 0.71.\n"
                    "0: Off. The palette's colour only. Fog in shadow loses the sky's hue.\n"
                    "1: Hue only. The dome decides what colour the sky is and how it varies across the frame; the palette, after the exposure correction, "
                    "decides how bright the fog reads. The default, and the one that cannot wash out an interior.\n"
                    "2: Full radiance. The dome supplies both. Physically the more correct answer for an open sky - distant fog really should approach the "
                    "sky's own radiance - and the one to try if terrain now reads darker than the sky behind it.",
                    args.minValue = 0,
                    args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, skyAmbientScale, 1.0f,
                    "Trim on the dome-derived fog colour, applied before it is blended in by rtx.dusklight.atmosphere.skyAmbientWeight.\n"
                    "The dome average is a measurement rather than a choice, so this exists only for taste - raise it for a hazier, more luminous distance, "
                    "lower it if outdoor fog reads brighter than the terrain it sits in front of.",
                    args.minValue = 0.0f,
                    args.maxValue = 8.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, fogAnisotropy, 0.0f,
                    "Forward scattering of the fog medium, -1..1. Higher pulls in-scattered light into a tighter glow around whatever is casting it.\n"
                    "DEFAULTED TO 0.6 ON 2026-08-13 AND BACK TO 0 THE SAME DAY, after the fog was reported reading blocky and inconsistent across the screen. "
                    "The froxel grid stores each cell's in-scatter as a first-order spherical harmonic, and the composite's raymarch evaluates it through the "
                    "Henyey-Greenstein phase function with no filtering - upstream's own other consumer of that data applies a Hanning filter first, with a "
                    "comment saying it was tuned until ringing was minimised. An L1 harmonic cannot represent a sharply forward-scattering lobe, so raising "
                    "this makes each froxel ring independently, which reads exactly as a grid. UNCONFIRMED as the cause of that report - it is one of two "
                    "candidates and the other was fog brightness - but it is the one this fork changed, so it goes back to upstream's value.\n"
                    "Raising it is still the right way to give shafts and torch glow directional shape. Try 0.2-0.3 first and watch for blockiness before "
                    "going further.\n"
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
                    "to reproducing an opacity curve, which is the whole point. THE DEFAULT.\n"
                    "2: Exact ramp. The medium itself is given the game's fog curve as its extinction field - sigma(d) = 1 / (end - d) on the ramp and exactly "
                    "zero before it - so the medium can never out-fog the original and the clear-zone cap that modes 0 and 1 need has nothing to do. That field "
                    "is not a fit: requiring a medium to reproduce T(d) = 1 - f(d) determines it uniquely, which also says the "
                    "console's fog always was a participating medium, just a heterogeneous one. What this buys over Top up is the medium's *shape*: the fog is "
                    "genuinely clear where the original is clear and genuinely thick where it is thick, so light shafts sit where the artists put the haze "
                    "instead of being spread evenly and then held down by rtx.dusklight.atmosphere.clearZoneTolerance to stop them hazing the near field.\n"
                    "WHAT THE RESIDUAL DOES HERE, because the obvious guess is wrong and it was written down wrong once already. It is exactly zero when the "
                    "game's ramp starts at or beyond the camera. When the ramp starts *behind* the camera - which scripted fog banks do routinely, the Lost "
                    "Woods mist tag at -2000..200 being the extreme - the original was already -start / (end - start) fogged at the camera itself, and no "
                    "medium beginning at the camera can reproduce fog applied before it. The residual then comes out as exactly that constant, at every "
                    "distance, and the composite adds it as a flat veil. The overall result is still algebraically the game's own formula. The panel and the "
                    "log print the constant, so 'zero, or flat at the value shown' is the check.\n"
                    "WHERE THAT CHECK DOES NOT APPLY, and this needs saying because the default outdoor configuration is one of them. The residual is flat only "
                    "over the stretch the medium actually owns, and there are two places it is expected to vary. First, past the froxel grid's reach: "
                    "rtx.dusklight.atmosphere.froxelMaxDistanceMaxMeters is 120 m = 12000 units and the open world's fogEndZ has been measured around 33500, so "
                    "outdoors the grid covers roughly a third of the ramp and beyond it the raymarch closes against the scalar medium instead, exactly as it "
                    "does in Top up. Second, over the last rtx.dusklight.atmosphere.fogRampFloor fraction of the ramp, where the divergence clamp freezes the "
                    "medium and the residual climbs off the constant to 1 by construction. The panel and the log both print the grid's reach against the ramp's "
                    "end and the clamp's distance, so both cases are readable rather than reportable. The defect is a residual that varies *inside* the grid's "
                    "reach and before the clamp.\n"
                    "Two things this mode changes that are not the ramp. The froxel grid is sized to cover the whole ramp rather than "
                    "rtx.dusklight.atmosphere.froxelRangeScale of it, because that setting's sub-1 default exists to leave the fog's final closure to the "
                    "composite - which an exponential needs and this field does not. And light *visibility* through the fog is still computed against the "
                    "scalar extinction, because that code path has no camera to measure a radial distance from; see the note in "
                    "rtx/algorithm/volume_lighting.slangh.\n"
                    "UNTESTED IN GAME. Built and invariant-checked on 2026-08-17, never run.",
                    args.minValue = 0,
                    args.maxValue = 2);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, fogRampFloor, 0.01f,
                    "How much transmittance the Exact ramp mode's medium is allowed to keep at the end of the ramp, 0..0.25. Read only when "
                    "rtx.dusklight.atmosphere.fogRampMode is 2.\n"
                    "The exact extinction field sigma(d) = 1 / (end - d) diverges at the ramp's end, so a raymarch step landing there would ask for an infinite "
                    "optical depth. The remaining distance is floored at this fraction of the ramp's range *in front of the camera*, which bounds the worst "
                    "step at ln(1 / this) and lands the medium's transmittance at exactly this value instead of zero.\n"
                    "Measured against the visible range rather than against (end - start), deliberately: a scripted fog bank's start is often far behind the "
                    "camera, and the Lost Woods mist tag's 2200-unit span has only 200 units of it in front of anyone - so a fraction of the span would have "
                    "frozen the medium over the last 11% of what is actually on screen instead of the last 1%.\n"
                    "WHAT IT COSTS: the medium closes the ramp to 99% at the default rather than to 100%, and stops thickening over the last 1% of the visible "
                    "ramp. The composite's top-up residual closes that last sliver, so nothing is visibly missing - what is lost is the medium's own density "
                    "there, and therefore any light shaft in the very last stretch of the fog. Lower it for a harder closure and a longer worst-case step; "
                    "raise it if the far end of a scripted whiteout reads noisy.\n"
                    "0 IS LEGAL and means 'as hard a closure as the maths allows'. It is floored internally at 1e-4 of the visible range, which bounds the worst "
                    "step at ln(10000) and leaves the medium's transmittance at 0.0001 - four decades below anything visible. Until 2026-08-18 a 0 here instead "
                    "turned Exact ramp off silently: the medium fell back to the uncapped homogeneous one while the composite went on running mode 2's residual "
                    "and every readout went on saying 'exact ramp'. If a config from before that date sets 0 and the near field looked hazy, this was why.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.25f);
    RTX_OPTION("rtx.dusklight.atmosphere", bool, limitDensityToRamp, true,
               "Caps the medium's extinction so it is never thicker than the game's own fog.\n"
               "The top up ramp can add fog but cannot take it away - a medium denser than the original's ramp leaves the ramp with a negative residual, which "
               "is clamped to zero rather than subtracted back out, because dividing integrated light back out amplifies the froxel grid's noise. So the "
               "condition is enforced ahead of time here instead, by lowering sigma until the medium fits under the ramp with at most "
               "rtx.dusklight.atmosphere.clearZoneTolerance to spare.\n"
               "The atmosphere panel reports both the matched and the capped extinction, so it is visible when this is binding and by how much. Turning it off "
               "restores the pure anchor match and lets the near field haze over.\n"
               "Read only in fogRampMode 1. Mode 2's medium follows the game's ramp exactly, so it can never be denser than it and there is nothing to cap.");
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, clearZoneTolerance, 0.08f,
                    "How much fog the medium is allowed to add where the game's own ramp has not started yet, 0..0.5.\n"
                    "This is the one real trade in the fog system. The original leaves everything before fogStartZ perfectly clear; a homogeneous medium cannot, "
                    "and the medium is what carries light shafts. At 0 the near field is exactly as crisp as the original and there is almost no medium left to "
                    "scatter anything; higher values buy shaft presence with a thin haze over near geometry.\n"
                    "Read only when rtx.dusklight.atmosphere.limitDensityToRamp is on.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.5f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, clearZoneVeilTarget, 0.01f,
                    "How bright a near-field veil the clear zone may show, as luminance rather than as coverage. 0 disables the weighting.\n"
                    "clearZoneTolerance on its own is a coverage number, and coverage is only half of what a viewer sees: the same 8% of a "
                    "dim grey cave is invisible, and 8% of Goron Mines' lava-lit orange is a haze hanging in front of the player. That is "
                    "measured, not supposed - in the 2026-08-15 log the outdoor ambient came to luminance 0.085 and the Goron Mines ambient "
                    "to 0.357, a factor of four, both spending the same budget.\n"
                    "So the budget is spent in luminance: the effective tolerance is this divided by the medium's own ambient luminance, "
                    "capped by clearZoneTolerance, which stays the ceiling and keeps its meaning. Bright interiors tighten; dim and outdoor "
                    "scenes are left exactly where they were, because there the quotient lands above the ceiling anyway.\n"
                    "The atmosphere panel and the fog log report the tolerance actually used beside the ceiling, so it is visible when this "
                    "is binding and by how much.",
                    args.minValue = 0.0f,
                    args.maxValue = 0.5f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelRangeScale, 0.6f,
                    "How much of the game's fog range the froxel grid is sized to cover.\n"
                    "The grid gets a fixed number of depth slices wherever it is pointed, so sizing it from the game's own fog range is what puts them where the "
                    "fog actually is: tight inside a dense interior, wide across an open field. Costs nothing - the slice count does not change, only its reach.\n"
                    "Deliberately below 1. At 1 the grid swallows the whole ramp and the composite's far half has nothing left to do, so the fog never closes to "
                    "fully opaque the way the original does - an exponential medium only asymptotes towards that. Leaving the last stretch to the ramp buys the "
                    "closure and spends the slices on the near field where shafts live. documentation/DusklightAtmosphere.md section 5.2.\n"
                    "That reason is specific to an exponential, so fogRampMode 2 ignores anything below 1 here: its extinction field closes the ramp on its own, "
                    "and leaving the far stretch outside the grid there would attenuate the surface correctly and never deposit the fog's colour on it.\n"
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
