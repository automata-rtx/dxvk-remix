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
      // Solved so the medium carries mediumFraction of the game's own opacity at the far edge of the
      // froxel grid, and no more. It is deliberately an under-run: the composite can only ever add
      // fog, so anything the medium overshoots by is an error nothing downstream can take back.
      float   sigma = 0.0f;
      // Linear radiance the fog tends towards. The game authors this as a display colour.
      Vector3 fogRadiance = Vector3(0.0f, 0.0f, 0.0f);

      // The game's own ramp, in world units. rampStart is routinely negative: scripted fog banks
      // set it that way so the ramp is already well underway at the camera.
      float   rampStart = 0.0f;
      float   rampEnd = 0.0f;

      // How far the froxel grid reaches this frame, in world units, after smoothing.
      float   froxelMaxDistance = 0.0f;

      // What the game's ramp asks for at the far edge of the grid, and what the medium will actually
      // deliver there. Reported rather than used: they are the two numbers that say whether the
      // split between the medium and the composite's correction is where it was meant to be, and
      // having them resolved once keeps the readout and the log from re-deriving them differently.
      float   rampOpacityAtReach = 0.0f;
      float   mediumOpacityAtReach = 0.0f;

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

    // One line per distinct derivation, capped. Rule 4 of the project notes: the medium and the
    // game's ramp agreeing is a numeric question, and it should be answerable from a log rather
    // than by asking someone whether the fog looks right.
    void logFog(const Derived& d) const;

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

    // Fog log budget. A fixed array rather than a set, so the cap is structural rather than
    // remembered - a log that can fill a disk is worse than no log.
    static constexpr uint32_t kMaxFogLogLines = 24;
    mutable uint64_t m_fogLogKeys[kMaxFogLogLines] = {};
    mutable uint32_t m_fogLogCount = 0;
    mutable bool m_fogLogCapped = false;

    // Owned once and kept alive for the process, not rebuilt per frame: the dome light holds a
    // bindless index into it, and dropping the image for even one frame drops the sky back to
    // Remix's own probe.
    Resources::Resource m_skyTexture;
    uint32_t m_skyTextureIndex = UINT32_MAX;

    Resources::Resource m_transmittanceLut;
    Resources::Resource m_multiScatterLut;
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
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, mediumFraction, 0.5f,
                    "How much of the game's fog the volumetric medium carries, with the composite making up the rest, 0..1.\n"
                    "The medium and the game's fog are different functions and cannot be made equal: the game's ramp is flat zero out to its start distance and "
                    "then linear in opacity, while an exponential medium begins accumulating at the camera and never quite closes. Whatever the medium overshoots "
                    "by is permanent - the composite can add fog to a pixel but cannot take it back out - so the medium is run deliberately thin and the shortfall "
                    "is paid back per pixel, which lands the total on the game's ramp exactly at every distance.\n"
                    "This is the knob for that trade and it costs no accuracy either way. At 0 the fog is the game's ramp and nothing else: exactly right, with no "
                    "light shafts in it. At 1 the medium carries as much as it can and near objects pick up haze the original did not have. The error it buys is "
                    "worst just inside the ramp's start distance and scales with this number.\n"
                    "UNVALIDATED: derived from the two functions, never measured against a running build.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, zHalfMin, 100.0f,
                    "Smallest half density distance the medium is allowed, in world units - one metre at this game's scale.\n"
                    "A ceiling on density, reached when a scripted fog bank asks for a whiteout that is already opaque at the camera. Raising it thins the very "
                    "densest fog; lowering it lets a whiteout close in harder. Only the medium is clamped: the composite still corrects to the game's ramp, so "
                    "this changes how much of a whiteout is volumetric rather than how thick it looks.\n"
                    "UNVALIDATED: chosen analytically, never measured against a running build. See documentation/DusklightAtmosphere.md.",
                    args.minValue = 1.0f,
                    args.maxValue = 10000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, densityScale, 1.0f,
                    "Scales the derived extinction, after rtx.dusklight.atmosphere.mediumFraction. Above 1 the medium can overshoot the game's ramp, which the "
                    "composite cannot correct - prefer mediumFraction unless you are deliberately looking for that.",
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
                    "How much of the game's fog colour the medium reproduces on its own, before any real light reaches it.\n"
                    "At 1 an unlit patch of the medium settles at exactly the colour the palette authored, which is the point: it is the same colour the "
                    "composite's correction blends towards, so the two halves of the fog agree and there is no seam between them. The game's fog is never black - "
                    "it was a colour blend rather than a simulation - so without this a dark interior's fog goes black while the correction's half stays orange.\n"
                    "Divided through by the single scattering albedo on the way in, so the settled colour is that albedo's business and this number stays "
                    "readable as a fraction of the authored colour. Light that genuinely reaches the medium adds on top, which is what makes a shaft a shaft.\n"
                    "Was 0.25 before 2026-08-06, which left the medium's own colour at 0.225 of the authored one - a fog that read four times too dark wherever "
                    "nothing was lighting it. documentation/DusklightAtmosphere.md §5.3.",
                    args.minValue = 0.0f,
                    args.maxValue = 4.0f);
    RTX_OPTION_ARGS("rtx.dusklight.atmosphere", float, froxelRangeScale, 0.6f,
                    "How much of the game's fog range the froxel grid is sized to cover.\n"
                    "The grid gets a fixed number of depth slices wherever it is pointed, so sizing it from the game's own fog range is what puts them where the "
                    "fog actually is: tight inside a dense interior, wide across an open field. Costs nothing - the slice count does not change, only its reach.\n"
                    "Below 1 to spend the slices on the near field, where shafts live and where the grid's resolution is visible; the composite's correction "
                    "reaches any distance and closes the fog exactly where the original did, so nothing is lost past the last slice. Until 2026-08-06 this had a "
                    "second job - the composite only added fog beyond the grid, so a reach of 1 left it with nothing to do and the fog never closed - and that "
                    "reason no longer applies. documentation/DusklightAtmosphere.md section 5.2.\n"
                    "It also sets where the medium's density is solved: sigma is chosen so the medium hits mediumFraction of the game's opacity at this reach.\n"
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
    RTX_OPTION("rtx.dusklight.atmosphere", bool, fogLog, true,
               "Logs one line per distinct fog derivation, so whether the medium and the game's ramp agree is answerable from a log rather than from looking at "
               "pixels.\n"
               "Each line carries the game's ramp, the solved density, and the two opacity curves sampled across the froxel grid's reach: what the game asks for "
               "at that distance, what the medium delivers, and what the composite has to make up.\n"
               "The field to read first is nearHaze: the worst fog the original did not have, at the ramp's own start distance, which is the one error the "
               "correction cannot reach. It is what rtx.dusklight.atmosphere.mediumFraction trades, and documentation/DusklightAtmosphere.md section 5.1 tabulates "
               "what to expect from it.\n"
               "Bounded: at most 24 lines per run, deduplicated on the ramp and the settings that decide the split, with a single notice when the cap is hit.");

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
