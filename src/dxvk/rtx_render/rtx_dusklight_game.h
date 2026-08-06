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

#include "rtx_option.h"
#include "../../util/util_keybind.h"

namespace dxvk {

  // Settings that belong to the game rather than to Remix, hosted here so they can be edited
  // while the game runs.
  //
  // The game these drive renders through a fixed function D3D9 path whose own debug UI is not
  // drawn at all in that mode, so its settings are otherwise reachable only by editing a config
  // file and restarting. Every one of these needs live tuning against the path traced image to
  // be worth anything. The game polls them through the getRtxOptionValue export every frame, so
  // moving a slider here takes effect on the next one; nothing here is read by Remix itself.
  //
  // These are ordinary saved options, so rtx.conf is the place to keep tuned values.
  struct DusklightGame {
    // The game's own overlay opens on F1 and is never drawn in this rendering mode, so the same key
    // opening ours keeps the muscle memory intact. Deliberately not Remix's own bind: these are two
    // separate overlays and either can be up without the other.
    inline static const VirtualKeys kDefaultDusklightMenuKeyBinds{ VirtualKey{VK_F1} };
    RTX_OPTION("rtx.dusklight", VirtualKeys, menuKeyBinds, kDefaultDusklightMenuKeyBinds,
               "Hotkey that opens the Dusklight overlay.\n"
               "example override: 'rtx.dusklight.menuKeyBinds = CTRL, D'.\n"
               "Full list of key names available in `src/util/util_keybind.h`.");

    RTX_OPTION("rtx.dusklight", bool, blockGameInput, true,
               "Stops keyboard and controller input reaching the game while either overlay is open.\n"
               "Remix's own rtx.blockInputToGameInUI cannot do this here: it works by sending a message across the 32 bit bridge, and a 64 bit game that "
               "loads this d3d9.dll directly never receives it, so input has always fallen straight through to the game. This routes the same intent through "
               "the Dusklight bridge instead, which the game is already listening to.");
    RTX_OPTION_FLAG("rtx.dusklight", bool, uiActive, false, RtxOptionFlags::NoSave,
                    "True while an overlay is up and wants the input to itself. Written by Remix, read by the game; do not set by hand.");

    // Warp, driven from the overlay's Warp tab. Indices rather than names: the destination table
    // belongs to the game, and keeping a copy here would guarantee the two drift apart. The game
    // reads these, resolves them against its own table and pushes the names back for display.
    // All NoSave - a commit counter surviving a restart would fire a warp nobody asked for.
    RTX_OPTION_FLAG("rtx.dusklight.warp", int, regionIndex, 0, RtxOptionFlags::NoSave, "Selected warp region. Set by the overlay.");
    RTX_OPTION_FLAG("rtx.dusklight.warp", int, mapIndex, 0, RtxOptionFlags::NoSave, "Selected warp level within the region. Set by the overlay.");
    RTX_OPTION_FLAG("rtx.dusklight.warp", int, roomIndex, 0, RtxOptionFlags::NoSave, "Selected room within the level. Set by the overlay.");
    RTX_OPTION_FLAG("rtx.dusklight.warp", int, pointIndex, 0, RtxOptionFlags::NoSave, "Selected spawn point within the room. Set by the overlay.");
    RTX_OPTION_FLAG("rtx.dusklight.warp", int, layer, -1, RtxOptionFlags::NoSave,
                    "Selected stage layer, which is how the game holds several versions of one place. Set by the overlay.\n"
                    "-1, the default, lets the game choose, which is what its own warp menu does. Naming a layer instead pins that version, and 0 is a "
                    "real layer rather than a 'no preference' - so leaving this at 0 would land in the wrong version of anywhere whose default is not 0.");
    RTX_OPTION_FLAG("rtx.dusklight.warp", int, commit, 0, RtxOptionFlags::NoSave,
                    "Incremented by the overlay to request a warp. The game acts on the change rather than the value, and latches the first one it sees "
                    "without acting, so connecting to a session that already has a non-zero count does not teleport anyone.");

    // Action binds, driven from the overlay's Controls tab. Indices and commit counters only: the
    // game owns the bind table, resolves conflicts, and pushes back both the resulting table and a
    // line of prose describing what happened. Nothing here decides anything - see
    // documentation/DusklightOverlay.md section 3.3.
    // All NoSave, for the same reason the warp commits are: a capture request that survived a
    // restart would arm itself on next launch.
    RTX_OPTION_FLAG("rtx.dusklight.bind", int, port, 0, RtxOptionFlags::NoSave,
                    "Controller port whose binds the Controls tab is showing, 0 to 3. Set by the overlay.");
    RTX_OPTION_FLAG("rtx.dusklight.bind", int, actionIndex, 0, RtxOptionFlags::NoSave,
                    "Which action the Controls tab has selected, as an index into rtx.dusklight.env.bindActions. Set by the overlay.");
    RTX_OPTION_FLAG("rtx.dusklight.bind", int, captureCommit, 0, RtxOptionFlags::NoSave,
                    "Incremented by the overlay to ask the game to capture the next key or button press for the selected action.\n"
                    "The game acts on the change rather than the value and latches the first one it sees without acting, so connecting to a session that "
                    "already has a non-zero count does not arm a capture nobody asked for.");
    RTX_OPTION_FLAG("rtx.dusklight.bind", int, clearCommit, 0, RtxOptionFlags::NoSave,
                    "Incremented by the overlay to unbind the selected action. Same change-not-value rule as captureCommit.");

    RTX_OPTION("rtx.dusklight.game", bool, bridgeEnable, true,
               "Whether the game feeds its environment state to Remix at all.\n"
               "Turning this off stops every push and takes the game's sun, moon and local lights with it, which makes it the "
               "one switch to reach for when deciding whether a problem belongs to the bridge or to Remix.");

    // Sun/moon distant light.
    RTX_OPTION("rtx.dusklight.game", bool, sunMoonLight, true,
               "Drives one Remix distant light from the game's own sun and moon position.\n"
               "The direction comes from the game's astronomical orbit, which depends on time of day and nothing else - deliberately "
               "not from the game's shadow casting light, which is a local light that snaps to nearby lanterns.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, sunIntensity, 5.0f,
                    "Radiance of the sun's distant light. Needs calibrating against the scene, which is easiest with auto exposure disabled.",
                    args.minValue = 0.0f,
                    args.maxValue = 50.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, moonIntensity, 0.3f,
                    "Radiance of the moon's distant light. The game's night look came mostly from its ambient colours rather than from the moon, "
                    "so this is deliberately far below the sun.",
                    args.minValue = 0.0f,
                    args.maxValue = 10.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, celestialAngle, 2.0f,
                    "Angular diameter of the sun or moon in degrees, which is what decides how sharp its shadows are.\n"
                    "The real sun is about 0.5 degrees. Larger values soften the shadow edges, which reads as more natural under a path tracer "
                    "and is also the quickest way to take the edge off a scene that has no fill light yet.",
                    args.minValue = 0.1f,
                    args.maxValue = 20.0f);
    RTX_OPTION("rtx.dusklight.game", bool, celestialFlip, false,
               "Diagnostic: negates the sun/moon direction. If shadows fall from the opposite side to the visible sun, the game and Remix "
               "disagree about handedness and this says so in one click.");
    RTX_OPTION("rtx.dusklight.game", bool, celestialLock, false,
               "Diagnostic: pins the sun/moon direction where it currently stands.\n"
               "The direction the game computes depends on nothing but time of day, so if the lighting still swings around while this is on, "
               "whatever is moving it is downstream of the game - the space Remix reads the direction in, rather than the direction itself.");

    // Fake shadows. The game draws its own approximations of shadows, all of
    // which Remix computes for real from the geometry - so drawing them puts a
    // painted shadow on top of a traced one.
    RTX_OPTION("rtx.dusklight.game", bool, blobShadows, false,
               "Let the game draw the flat circular shadows it puts under rupees, hearts, pots and small objects.\n"
               "Off, because Remix traces a real shadow for every one of those objects and the painted disc lands on "
               "top of it. Turning it on restores the game's own behaviour, which is only useful for comparison. The "
               "game reads this every frame, so it takes effect immediately - and it is suppressed at the point the "
               "shadow is registered, so no draw call is issued at all rather than one being hidden later.\n"
               "This covers the game's *simple* shadows only. Its projected shadows (Link, major actors) are a "
               "separate system and are not touched.");

    // Local point lights.
    RTX_OPTION("rtx.dusklight.game", bool, localLights, false,
               "Mirrors the game's own point lights - torches, braziers, lanterns, campfires and the dungeon lights - into Remix as sphere lights.\n"
               "The game's D3D9 path does not forward its lights, so without this Remix sees no light from the game at all: outdoors the sun covers "
               "that, but interiors and night fall through to Remix's fallback light.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, localLightIntensity, 19.0f,
                    "Scales the game's local lights.\n"
                    "At 1.0 each light is as bright as Remix's own conversion would make a legacy light that reached exactly as far as the game's "
                    "influence radius. That reading is too conservative, because the radius is not where the light ends: the game loads its attenuation "
                    "so that the radius is where brightness falls to about a ninth of peak, and the curve carries roughly four times further. Applying "
                    "Remix's own end threshold to that curve instead gives about 19, and testing picked the same number independently as the least that "
                    "lights a room usefully.\n"
                    "Set together with rtx.dusklight.game.localLightRadius: the radiance is solved so the light still reaches the same distance, so a "
                    "larger emitter needs less of it and changing one alone moves brightness as well as softness.",
                    args.minValue = 0.0f,
                    args.maxValue = 32.0f);
    RTX_OPTION("rtx.dusklight.game", bool, disableFrustumCulling, false,
               "Stops the game discarding geometry that falls outside the camera's view.\n"
               "The game culls aggressively because a rasterizer has no use for what it cannot see. A path tracer does: a wall dropped because the camera "
               "turned away stops occluding, and light spills through the gap into rooms it should never reach. Turning this off submits everything in the "
               "room every frame, which costs exactly what the culling was saving, so it is off by default. Remix's own rtx.antiCulling.object.enable is the "
               "cheaper half measure - it retains objects it has already seen rather than preventing them being dropped in the first place.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, celestialNoonElevation, 59.036f,
                    "How high the sun and moon climb at their peak, in degrees above the horizon.\n"
                    "The game's own arc tops out at 59 degrees and never higher, which reads fine against baked lighting but leaves a path tracer without a "
                    "usable overhead sun: midday shadows stretch about as far as mid-afternoon ones. Raising this tilts the orbit towards vertical, so 90 puts "
                    "the sun directly overhead at noon and drops shadows straight down.\n"
                    "The default reproduces the game's arc exactly. This moves the visible sun and moon as well as the light, so the two never disagree, and it "
                    "changes nothing about time of day - dawn, dusk, night and the palette schedule all run off the clock and never look at the orbit. Sunrise "
                    "and sunset elevations barely move either, so those transitions look the same.\n"
                    "Testing settled on 80, stopping short of 90 because the azimuth flips instantaneously at exactly 90. The recommended configuration is in "
                    "dusklight-ao/docs/dx9-fixed-function.md.",
                    args.minValue = 1.0f,
                    args.maxValue = 90.0f);
    RTX_OPTION("rtx.dusklight.game", bool, hideSkyBillboards, false,
               "Stops the game drawing its sun, moon and star billboards.\n"
               "Those are placed at a fixed offset from the camera, so they travel with the player, and any that Remix captures as ordinary world geometry "
               "become an occluder that follows you around. Tested 2026-07-29: that was the cause of shadowed areas appearing to wander at night, and this "
               "fixes it rather than masking it.\n"
               "It removes the visible stars and moon along with the occluder. rtx.dusklight.atmosphere.skyMoonEnable paints the moon back into the generated "
               "sky; these billboards also carry textures, so categorising them as Sky instead would keep all of them visible.");
    RTX_OPTION("rtx.dusklight.game", bool, perBladeGrass, false,
               "Draws each blade of grass as its own instance instead of batching a whole room into one.\n"
               "The batch is a dynamic world space vertex stream, so its asset hash churns the moment any blade sways, is cut or regrows, and Remix cannot "
               "identify it from frame to frame: no tagging, no replacement, and no denoiser or ReSTIR history, which is why grass lighting lags the scene. "
               "Per blade it is static display list geometry plus a transform, so the hash holds still.\n"
               "Off by default because it costs exactly what the batching saves - one draw call per blade in dense grass. Built 2026-07-29, untested in game; "
               "dusklight-ao/docs/remix-open-issues.md is where its state is tracked.");
    RTX_OPTION("rtx.dusklight.game", bool, hideVrbox, false,
               "Stops the game drawing its own sky dome.\n"
               "Turn this on together with rtx.dusklight.atmosphere.skyEnable, which generates a sky from the same colours the dome is painted "
               "with, or both will be visible; leave it off and the atmosphere still lights the scene but you will be looking at the game's dome.\n"
               "Also set rtx.skyAutoDetect to None, otherwise the auto detected dome keeps feeding a second, dimmer sky into the same pixels.\n"
               "Note the dome is not untaggable, which this option's rationale used to claim: it carries no texture, but rtx.skyBoxGeometries "
               "categorises by geometry hash instead. documentation/DusklightAtmosphere.md section 14.9.");
    // Clock control. Both NoSave: a frozen clock or a pinned time that survived a restart would
    // be a silent, invisible reason for the world to behave oddly, and this pair exists to make
    // comparisons repeatable rather than to configure anything.
    RTX_OPTION_ARGS("rtx.dusklight.game", float, timeOfDay, 0.0f,
                    "Where to move the game's clock to, in degrees: the whole day is 360, so 15 is an hour. 0 midnight, 90 sunrise, 180 noon, 270 sunset.\n"
                    "Setting this alone does nothing - rtx.dusklight.game.timeCommit is what asks for it to be applied. Holding the value and the request "
                    "apart is what lets the clock run on from wherever it was put instead of being pinned there, and it means pressing the same preset "
                    "twice works the second time.",
                    args.flags = RtxOptionFlags::NoSave,
                    args.minValue = 0.0f,
                    args.maxValue = 359.9f);
    RTX_OPTION_FLAG("rtx.dusklight.game", int, timeCommit, 0, RtxOptionFlags::NoSave,
                    "Incremented by the overlay to move the clock to rtx.dusklight.game.timeOfDay. The game acts on the change rather than the value, and "
                    "latches the first one it sees without acting, so connecting to a session that already has a non-zero count does not move anyone's "
                    "clock. Same shape as the warp commit, for the same reason.");
    RTX_OPTION_FLAG("rtx.dusklight.game", bool, freezeTime, false, RtxOptionFlags::NoSave,
                    "Stops the game's clock, so the sun, the moon and every palette that follows them hold still.\n"
                    "This is what makes an A/B pair worth comparing: without it the light has moved between the two shots and any difference is partly the "
                    "clock rather than the setting under test.\n"
                    "It reuses the same mechanism the game uses for a stage whose sky must not move, so it also holds the Twilight Realm's separate clock "
                    "and skips the reset to midnight that entering twilight would normally do - frozen means frozen, which is right for a comparison but "
                    "is not what the game does on its own.");
    RTX_OPTION("rtx.dusklight.game", bool, recordingMode, false,
               "The game's own recording mode: hides its HUD and silences its music.\n"
               "It is a game setting rather than a Remix one, and the game's settings screen is not drawn in the fixed function D3D9 mode, so without this it "
               "can only be changed by editing config.json and restarting - and only in one direction, since a value set there could not be turned back off "
               "while running.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, localLightRadius, 10.0f,
                    "Emitter radius of the game's local lights in world units.\n"
                    "This changes brightness as well as softness: the radiance is solved so the light still reaches the same distance, so a larger "
                    "emitter needs less of it. Large radii on lights sitting inside wall sconces will clip through the geometry, which is what bounds "
                    "this from above - 10 was tested against the Forest Temple light posts and clears them.",
                    args.minValue = 0.5f,
                    args.maxValue = 64.0f);

    // Effect lights. The replacement for the mirror above, and the reason it now defaults off.
    // Full design, citations and exclusion policy: dusklight-ao/docs/effect-lights.md.
    RTX_OPTION("rtx.dusklight.game", bool, effectLights, true,
               "Puts a sphere light at the origin of the game's own fire and glow effects - the point the flame is generated from, not the position of "
               "the light the game registered for it.\n"
               "The game's own lights are placed wherever the original per-vertex shading looked best, which was free because a GameCube point light casts "
               "no shadow. Under a path tracer the same placement is visibly wrong: the shadow comes from a point that is not the fire. This reads the "
               "emitter table instead - the game already decides every frame where fire exists and whether it is on - and keeps only the colour and reach "
               "from whatever light was authored nearby.\n"
               "Not meant to run together with rtx.dusklight.game.localLights: every fire would get two lights, one of them in the wrong place.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightIntensity, 1.0f,
                    "Master brightness for every light this system makes, whether or not it took its parameters from the game.\n"
                    "The two multipliers below scale the derived and undetermined halves separately; this one moves both at once, so it is the knob to "
                    "reach for when the whole scene is too hot or too dim.",
                    args.minValue = 0.0f,
                    args.maxValue = 8.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightDerivedIntensity, 19.0f,
                    "Scales only the lights whose reach and colour came from a light the game itself authored.\n"
                    "At 1.0 such a light is as bright as Remix's own conversion would make a legacy light reaching exactly as far as the game's influence "
                    "radius. That reading is too conservative: the game loads its attenuation so the radius is where brightness falls to about a ninth of "
                    "peak, and the curve carries roughly four times further, which puts the honest figure near 19. Testing on the local light mirror "
                    "picked the same number independently, and this starts there so that tuning carries over.\n"
                    "Separate from effectLightUndeterminedIntensity on purpose: this one maps the game's units onto Remix's scale, that one picks a size "
                    "out of nothing, and tying them together guarantees that tuning one breaks the other.",
                    args.minValue = 0.0f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightDerivedRadius, 10.0f,
                    "Emitter radius, in world units, for lights that took their reach from the game.\n"
                    "Changes brightness as well as softness - the radiance is solved so the light still reaches the same distance, so a larger emitter "
                    "needs less of it. Large radii on lights inside wall sconces clip through the geometry, which is what bounds this from above.",
                    args.minValue = 0.5f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightUndeterminedIntensity, 1.0f,
                    "Scales only the lights the game gave us nothing to go on for - a fire arrow, a torch with no registered light, any effect that simply "
                    "has no light authored beside it. Their colour still comes from the effect's own palette; only the strength is invented here.",
                    args.minValue = 0.0f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightUndeterminedReach, 400.0f,
                    "How far a light with no game-authored reach should carry, in world units.\n"
                    "For scale: the game gives a bonfire an influence radius of 500 and a dungeon torch 500, and Link stands about 150 units tall.",
                    args.minValue = 0.0f,
                    args.maxValue = 8000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightUndeterminedRadius, 8.0f,
                    "Emitter radius, in world units, for lights with no game-authored reach.",
                    args.minValue = 0.5f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightFireOffset, 15.0f,
                    "How far above the effect's origin a fire light sits, in world units.\n"
                    "An emitter is placed where the effect is generated from, which for a torch or a totem is the fuel at the base of the flame. The light "
                    "belongs a little way up inside the flame instead. The game itself does the same thing where it bothers - a Forest Temple torch offsets "
                    "its light by 10 units above the flame point.",
                    args.minValue = -200.0f,
                    args.maxValue = 200.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightGlowOffset, 0.0f,
                    "How far above the effect's origin a non-fire glow light sits, in world units. Zero because a glow is usually centred on the thing "
                    "that glows, unlike a flame which rises off its fuel.",
                    args.minValue = -200.0f,
                    args.maxValue = 200.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightMergeRadius, 60.0f,
                    "How close two effect origins must be to count as one light, in world units.\n"
                    "A single visible fire is usually several emitters at one point - a bonfire is five, a flame core plus layers plus embers. Without this "
                    "each would get its own light: five times the cost for none of the benefit, and their alphas animate independently so the sum flickers. "
                    "Too large and two neighbouring torches collapse into one.",
                    args.minValue = 0.0f,
                    args.maxValue = 500.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightAdoptRadius, 250.0f,
                    "How close one of the game's own lights must be to an effect for its colour and reach to be adopted, in world units.\n"
                    "Larger values catch lights the game deliberately offset from the flame; too large and a fire adopts the parameters of an unrelated "
                    "light across the room.",
                    args.minValue = 0.0f,
                    args.maxValue = 2000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", int, effectLightMaxLights, 32,
                    "Most lights this system will submit in one frame, brightest and nearest first.\n"
                    "A light that contributes nothing still costs a light manager entry and a slot in Remix's light sampling, so this bounds a room full of "
                    "candles rather than trusting it to be reasonable.",
                    args.minValue = 0,
                    args.maxValue = 256);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightMaxDistance, 12000.0f,
                    "Beyond this distance from the camera an effect gets no light, in world units. Zero disables the cull.",
                    args.minValue = 0.0f,
                    args.maxValue = 100000.0f);
    RTX_OPTION("rtx.dusklight.game", bool, effectLightBursts, false,
               "Give explosions and other one-shot fire their own light.\n"
               "Off because a light that appears and vanishes inside a fifth of a second is a flash, which is sometimes exactly right - a bomb should flash "
               "- and sometimes a flicker artefact. This is the exclusion most likely to be wrong for this game; turn it on and look at a bomb.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightMinChroma, 0.50f,
                    "How saturated an effect's colour has to be to read as a glow rather than as smoke or spray.\n"
                    "An effect earns a light when it is being drawn, blends additively, and its colour reads as a glow - saturated OR near white hot. This "
                    "is the saturated half; effectLightMinLuma is the white hot half. Thresholds rather than constants because they are a judgement about "
                    "this game's palette, the same reasoning as the material self-illumination thresholds - and the same two functions and the same defaults, because it is the same question asked of an effect rather than of a surface.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightMinLuma, 0.70f,
                    "How bright a desaturated effect's colour has to be to read as white hot rather than as smoke. See effectLightMinChroma.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightVolumetric, 1.0f,
                    "How much an effect light contributes to fog and haze, relative to what it contributes to surfaces.\n"
                    "Remix applies this multiplier in the volumetrics passes only, so above 1 a flame hazes the air around it without getting any brighter "
                    "on the walls - which is the cheapest way to get the glow a fire has in air with dust or smoke in it. 0 removes the light from the "
                    "volumetrics entirely while leaving it lighting surfaces normally.\n"
                    "Read when a light is created, so a change reaches existing lights on their next update rather than immediately.",
                    args.minValue = 0.0f,
                    args.maxValue = 16.0f);
    RTX_OPTION_FLAG("rtx.dusklight.game", int, effectLightReportCommit, 0, RtxOptionFlags::NoSave,
                    "Incremented by the overlay to make the game log one line per distinct effect it has seen - name, blend configuration, colours, class "
                    "and whether the rule accepted it.\n"
                    "That log is what turns 'additive blending means the effect emits light' from a reading of the format into a measurement of this game, "
                    "so one play session settles the classifier for the whole game. The game acts on the change rather than the value and latches the first "
                    "one it sees without acting, so connecting to a session that already has a non-zero count does not dump a report nobody asked for.");
  };

}
