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
               "Let the game draw the flat circular ground shadow it puts under an actor.\n"
               "Off, because Remix traces a real shadow for every one of those objects and the painted disc lands on "
               "top of it. Turning it on restores the game's own behaviour, which is only useful for comparison. The "
               "game reads this every frame, so it takes effect immediately - and it is suppressed at the point the "
               "shadow is registered, so no draw call is issued at all rather than one being hidden later.\n"
               "SCOPE - wider than this description used to say. It drops the ground shadow of EVERY actor that "
               "registers one, not just dropped items: pots, insects, enemies, NPCs and cutscene actors too. The "
               "suppression sits inside dDlst_shadowControl_c::setSimple, which is the single funnel behind all 50 "
               "dComIfGd_setSimpleShadow call sites in the game, and two of those are shared base classes that carry "
               "most of the reach - daNpcT_c::draw (51 derived NPC classes) and daItemBase_c::setShadow. The "
               "2026-08-06 in-game test that confirmed this correct only looked at dropped items, so the tested part "
               "is much narrower than the changed part; the reasoning holds for anything whose caster geometry "
               "reaches Remix, but an NPC losing its ground shadow is expected behaviour here, not a new bug.\n"
               "This covers the game's *simple* shadow class only. Its projected shadows (Link, major actors, "
               "dDlst_shadowReal_c) are a separate system and are not touched. Naming, since it confuses people: the "
               "game has a word for the projected class and calls it \"riaru kage\" (real shadow) in its own debug "
               "labels, but it has no name at all for the simple class - \"blob shadow\" is this project's coinage.");

    // Local point lights - superseded 2026-08-06 by rtx.dusklight.game.effectLights, kept as
    // the comparison path. Its description has to say so: this option's tooltip and its row in
    // RtxOptions.md are where somebody setting the game up will read about it, and until
    // 2026-08-07 both still told them to turn it on.
    RTX_OPTION("rtx.dusklight.game", bool, localLights, false,
               "Mirrors the point lights the game's ACTORS register - torches, braziers, lanterns, campfires, Midna, bomb flashes - into Remix as sphere "
               "lights, at the positions the game gave them.\n"
               "It does not cover the lights a room is authored with; those are a separate registry with its own switch, rtx.dusklight.game.roomLights. "
               "(This description used to say it covered \"the dungeon lights\", which read as if it did.)\n"
               "SUPERSEDED by rtx.dusklight.game.effectLights, which is on by default. Those positions are the problem: a GameCube point light casts no "
               "shadow, so the artists could put one wherever the shading looked best - offset from the flame, sunk into geometry, one light standing in "
               "for three - and none of it reads as wrong until a path tracer casts a real shadow from the exact point it occupies. The replacement puts "
               "the light at the origin of the effect that draws the fire and keeps only the game's colour and reach.\n"
               "Kept so the two can be compared. Running both gives every fire two lights, one of them in the old place - which looks exactly like the new "
               "placement being broken, so the Dusklight tab warns when both are on.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, localLightIntensity, 19.0f,
                    "Scales the game's local lights. Applies to the superseded mirror only - the equivalent for effect lights is "
                    "rtx.dusklight.game.effectLightDerivedIntensity, which starts from this same 19 and for the same reason.\n"
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
               "sky. Categorising as Sky instead is an option for the sun and moon, which carry textures, and is not one for the stars, which are drawn with "
               "no texture bound at all - there is no hash to categorise. rtx.dusklight.game.hideStarBillboards splits the star half back out.");
    RTX_OPTION("rtx.dusklight.game", bool, hideStarBillboards, true,
               "Whether Hide Sky Billboards also removes the star field. A sub-switch: with that option off, this one does nothing.\n"
               "On by default, which is exactly what Hide Sky Billboards has always done - the two were a single switch until 2026-08-11, and this default "
               "keeps every existing rtx.conf behaving identically.\n"
               "Turn it OFF, with Hide Sky Billboards left ON, to get the stars back while the sun and moon billboards stay hidden. That is the A/B that "
               "isolates which packet is actually the occluder, and it has never been run. Only the sun packet draws the moon: dKyr_drawSun hangs an "
               "8000-unit quad 80000 units out along the direction the moon light arrives from, and that is the geometry the 2026-07-29 test measured. "
               "dKyr_drawStar emits up to 1200 scattered triangles covering roughly 0.05% of the sky, fades out any that fall near the moon, and has never "
               "been shown to occlude anything - but no test has separated the two, so that is arithmetic rather than a measurement.\n"
               "The stars are not only decoration: the first 13 are a constellation the original team placed by hand. dusklight-ao/docs/remix-test-playbook.md "
               "section 4b is the recipe.");
    RTX_OPTION("rtx.dusklight.game", bool, hideDashEffect, false,
               "Stops the game drawing the speed effect it spawns while Epona dashes.\n"
               "daHorse_c::setDashEffect places JPA particle 0x8657 at a computed offset in front of the *camera* rather "
               "than in the world, so it is a screen-covering translucent quad that travels with the view. That is the "
               "same shape of problem as the sky billboards above: a rasterizer composites it over the frame, but Remix "
               "captures it as ordinary world geometry sitting directly in front of the camera, where it veils everything "
               "behind it and is path traced as if it were real.\n"
               "Off by default. It shipped on, encoding the hypothesis that this effect was why water changed appearance "
               "while dashing, and the 2026-08-08 log refuted it: the effect was suppressed for that entire session and "
               "the water still changed, new material shapes were not clustered on the nine dashes (2 within 250ms where "
               "chance predicts ~10), and the gallop covered 29% of the session while producing 6% of them. The cause was "
               "the projective texture transform on the water's reflection layer, which this runtime was discarding; that "
               "is now implemented, so this option is back to preserving the game's behaviour.\n"
               "Still worth turning on to see the scene without a translucent quad travelling with the camera, which "
               "remains a real thing to do to a path tracer even though it was not this defect. Suppressed at the point "
               "the emitter is created, so no draw call is issued. The game reads this every frame.");
    RTX_OPTION("rtx.dusklight.game", bool, perBladeGrass, false,
               "Draws each blade of grass as its own instance instead of batching a whole room into one.\n"
               "The batch is a dynamic world space vertex stream, so its asset hash churns the moment any blade sways, is cut or regrows, and Remix cannot "
               "identify it from frame to frame: no tagging, no replacement, and no denoiser or ReSTIR history, which is why grass lighting lags the scene. "
               "Per blade it is static display list geometry plus a transform, so the hash holds still.\n"
               "Off by default because it costs exactly what the batching saves - one draw call per blade in dense grass. Built 2026-07-29, untested in game; "
               "dusklight-ao/docs/remix-open-issues.md is where its state is tracked.\n"
               "GRASS ONLY, which the name hides. The actor that plants grass plants flowers too - one kind spawns kusa (grass) into dGrass_packet_c, two "
               "more spawn hana (flower) into dFlower_packet_c - and this switch reaches the grass packet alone. The flower packet batches in exactly the "
               "same way, into the same kind of dynamic world-space stream with the same churning hash. Its switch is Per-Flower Blossoms below, added at "
               "protocol 13; before that it had none. So if a grass-like symptom is showing on flowers, this control will not move it and that one will.");
    RTX_OPTION("rtx.dusklight.game", bool, perBladeFlowers, false,
               "Draws each flower as its own instance instead of batching a whole room into one.\n"
               "The flower half of Per-Blade Grass above, over the other packet the same actor feeds. Identical mechanism: the batch is a dynamic vertex "
               "stream under an identity position matrix, so its asset hash churns whenever any flower sways or is cut and Remix cannot identify the patch "
               "from frame to frame - no tagging, no replacement, no denoiser or ReSTIR history. Per flower it is static display list geometry plus a "
               "transform, so the hash holds still.\n"
               "Off by default, for the same reason: it costs one draw call per flower where the batch cost a few per room. A separate switch rather than a "
               "widening of Per-Blade Grass on purpose - a flower is a bigger template than a blade so the cost differs, and NEITHER half has been run in "
               "game even once, so keeping them apart lets one session answer both questions instead of confounding them.\n"
               "Built 2026-08-11, protocol 13, untested in game. The game reads this every frame; dusklight-ao/docs/remix-open-issues.md issue 7 tracks its "
               "state. If the flowers vanish or draw untextured with this on, the vertex descriptor or the ambient reuse is wrong, not the idea.");
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

    // Room lights - the room's OWN authored lights, a third registry from either of the two
    // above and the only one in the game that carries a cone. Off by default until one log says
    // whether they double-count with the effect lights. dusklight-ao/docs/effect-lights.md 8.1.
    RTX_OPTION("rtx.dusklight.game", bool, roomLights, false,
               "Forwards the lights the room itself was built with - the ones in its stage file, placed by whoever laid the room out - into Remix as sphere "
               "lights, with their cones.\n"
               "These are NOT the torches and lanterns rtx.dusklight.game.localLights mirrors. Those are registered by actors; these are authored per room, "
               "are what lights a dungeon corridor with no fire in it, and are the only lights in the game with a direction and a cutoff angle at all. "
               "Nothing in this project read them until now.\n"
               "OFF BY DEFAULT, and the reason is the same one that turned the local light mirror off: these are authored positions, and a GameCube light "
               "casts no shadow, so a room light could be sunk in a wall or floating over a doorway and nothing would have looked wrong at the time. A path "
               "tracer casts a real shadow from exactly where it sits. Turn this on, look at where the shadows come from, and read the counters below - if "
               "every fire ends up with two lights it is double counting with rtx.dusklight.game.effectLights, and if the shadows come from nowhere "
               "sensible then the placements do not survive the path tracer and the honest answer is to leave this off.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, roomLightIntensity, 19.0f,
                    "Scales the room's authored lights.\n"
                    "Starts at the same 19 as rtx.dusklight.game.localLightIntensity, but for a weaker reason. That one converts a radius the game really "
                    "does treat as a reach. This one starts from the room light's authored radius, which the game loads into its hardware with a reference "
                    "brightness of 0.99999 - so the light is still at full strength AT that radius and would need thousands of times it to fade out. In "
                    "other words the original room lights barely fall off at all, and the number here is a nominal size being used as a reach because it is "
                    "the only distance the authors wrote down.\n"
                    "Expect to move this. A path-traced sphere light falls off physically whatever the setting, so a bright spot near the light where the "
                    "game had an even wash is the predicted behaviour rather than a bug.",
                    args.minValue = 0.0f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, roomLightRadius, 10.0f,
                    "Emitter radius of the room's authored lights in world units.\n"
                    "Changes brightness as well as softness, the same way rtx.dusklight.game.localLightRadius does: the radiance is solved so the light "
                    "still reaches the same distance, so a larger emitter needs less of it.",
                    args.minValue = 0.5f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, roomLightConeSoftness, 1.0f,
                    "How soft the edge of a room spotlight's cone is, as a multiple of the widest softening that still fits inside the cone.\n"
                    "Read this as a knob, not as a conversion. The cone's DIRECTION and its ANGLE are transcribed exactly from the game. The SHAPE of the "
                    "falloff between the edge and the axis is not: the game has four different curves for it and Remix has one, so every curve except the "
                    "hard-edged one is approximated by the same smooth ramp and this scales how far in that ramp reaches. 0 gives a hard edge, 1 softens "
                    "across the whole cone.\n"
                    "The game's two ring-shaped spot functions - dark on axis, brightest partway out - cannot be expressed by Remix's shaping at all. Those "
                    "lights are sent with no cone rather than dropped, and rtx.dusklight.env.roomLightsUnshapeable counts them.",
                    args.minValue = 0.0f,
                    args.maxValue = 4.0f);

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
    // THE THREE GLOBAL MULTIPLIERS - one per value this system derives from the game, all
    // defaulting to 1.0, all applied at one point to both the derived and the undetermined
    // branch. They exist so a value the artists authored can be corrected without a rebuild
    // when it comes out too weak or too strong. The whole chain from an authored value to a
    // final radiance is written out once, in dusklight-ao/docs/effect-lights.md section 5, and
    // the classification report prints it back with the live numbers substituted in.
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightIntensity, 1.0f,
                    "Global multiplier on the BRIGHTNESS of every light this system makes, whether or not it took its parameters from the game.\n"
                    "The derived and undetermined intensities below scale the two halves separately; this one moves both at once, so it is the knob to "
                    "reach for when the whole scene is too hot or too dim.\n"
                    "It does NOT reach a lantern that is being solved separately - see rtx.dusklight.game.effectLightLanternSeparate, which is exactly "
                    "what 'its own settings' has to mean to be useful.",
                    args.minValue = 0.0f,
                    args.maxValue = 8.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightReachScale, 1.0f,
                    "Global multiplier on the REACH of every light this system makes - the distance the light is solved to carry to.\n"
                    "This replaced rtx.dusklight.game.effectLightDerivedReach, which did the same job with the same default for the derived half only. "
                    "A derived light's reach is the game's own LIGHT_INFLUENCE::mPow, which is the single genuinely photometric number the original "
                    "artists left anywhere; an undetermined light's is effectLightUndeterminedReach. This scales whichever it was.\n"
                    "Reach and radius pull in opposite directions and only one of them changes the shape of the light: reach is the distance the light "
                    "is solved to carry to, radius is the physical size of the sphere. Growing the radius is what makes a light inside a sconce clip "
                    "through the geometry, so raising this is the way to push light further without that happening.\n"
                    "Worth knowing before tuning: reach feeds exactly two things, the solved radiance and the budget sort. Radiance goes as the square "
                    "of it, so 2.0 here is the same brightness as 4.0 on an intensity. The one place they differ is priority - a light with more reach "
                    "outranks a dimmer one when maxLights is binding, and intensity does not enter that.\n"
                    "If you set effectLightDerivedReach in an rtx.conf it is now inert; move the value here.",
                    args.minValue = 0.0f,
                    args.maxValue = 16.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightRadiusScale, 1.0f,
                    "Global multiplier on the RADIUS of every light this system makes - the size of the emitting sphere.\n"
                    "Radiance is solved so that the light still carries to the same distance whatever its radius is, so this changes softness and "
                    "near-field falloff rather than how far the light travels. Larger spheres inside wall sconces clip through the geometry, which is "
                    "what bounds it from above.",
                    args.minValue = 0.01f,
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
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightMassExponent, 0.5f,
                    "How much a light grows with the amount of fire actually standing at it.\n"
                    "Every site sums the emitters merged into it, weighted by their alpha, into a mass: a five-emitter bonfire at full alpha "
                    "measures 5, a single candle measures 1, a fire fading out measures less as it fades. Reach is then multiplied by mass "
                    "raised to this power.\n"
                    "0 disables it exactly - mass to the power 0 is 1, so every light behaves as it did before this existed. 0.5, the default, "
                    "makes RADIANCE proportional to mass, because radiance goes as the square of reach: twice the fire, twice the light. 1.0 "
                    "makes reach itself proportional to mass, which is much stronger and grows quadratically in brightness.\n"
                    "This exists because nothing previously scaled a light by how much fire was there - a roaring bonfire and a guttering "
                    "candle emitted identically, which is why large fires read as underwhelming. A single full-alpha emitter measures 1 and is "
                    "therefore unchanged at any exponent, so existing tuning for torches and candles survives.",
                    args.minValue = 0.0f,
                    args.maxValue = 2.0f);
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

    // WHAT THE ARTISTS AUTHORED. Both read the loaded JPA blocks - immutable for the session -
    // rather than the live emitter fields, which are overwritten every frame by key blocks and
    // by several hundred actor setter calls. Neither of them invents a brightness: nothing in
    // the JPA format is photometric, and that is why radiance still comes from the game's own
    // light registry and from the settings above.
    RTX_OPTION("rtx.dusklight.game", bool, effectLightAuthoredColor, true,
               "Take a light's colour from the effect's own authored colour ramp instead of from the registers its emitter happens to be holding.\n"
               "The authored ramp is written into the .jpa at load and never changes; the live registers do, in three ways that are none of them the "
               "fire changing colour. A global colour animation walks its key frame every frame. The game multiplies a time-of-day tint into any effect "
               "whose authored user-work word carries bit 0x20 or 0x40, so a torch's colour drifts from dawn to dusk. And the shared emitter behind every "
               "'simple' effect - which is most torches and candles - is made continuous when it is created, so its colour cycle free-runs from level "
               "load and every torch in the world reads the same unrelated phase of it.\n"
               "This changes HUE ONLY. Radiance is normalised by its brightest channel, so no light gets brighter or dimmer from this.\n"
               "It also stops a light re-entering Remix's light manager every frame: a colour that moves more than 2% re-creates the light and costs it "
               "its temporal history, and a fixed hue simply does not move.\n"
               "A light that adopted one of the game's own lights still takes THAT colour - the artists chose it for the light rather than for the sprite, "
               "and it wins over both.");
    RTX_OPTION("rtx.dusklight.game", bool, effectLightAuthoredRadius, false,
               "Grow a light's sphere to the effect's own authored extent, where the artists made the effect bigger than the configured radius.\n"
               "The extent is the authored particle size, or the authored spawn volume where that is larger - both immutable, both in the effect's own "
               "units. It can only ever GROW the sphere: the two configured radii are what all existing tuning was done against, and it is capped at 64 "
               "units so that switching it on cannot leave that envelope.\n"
               "Radiance is solved so the light still carries to the same distance whatever its radius is, so this changes softness and near-field "
               "falloff rather than how far the light travels. Off by default because that is a judgement about how a fire should look rather than a "
               "correctness fix, and a silent change to the look of every fire is the one thing that cannot be un-seen.");

    // LINK'S LANTERN. The one class that can be given settings of its own, because it is the one
    // light the player carries and therefore the one whose brightness is a gameplay decision
    // rather than a scene decision. It is identified by name and the identification is exact:
    // "kantera" matches five names in the game's whole 3205-entry effect table, two of which are
    // Link's still and swung lantern flames and three of which have no caller anywhere in the
    // game. Both of Link's are covered, which matters - the game destroys one emitter and creates
    // the other every time he swings the lamp.
    RTX_OPTION("rtx.dusklight.game", bool, effectLightLanternSeparate, false,
               "Give Link's lantern its own reach, radius and brightness, separately from every other light this system makes.\n"
               "OFF, the default, is today's behaviour exactly: the lantern is classified, merged, adopted and solved like any other fire, and every "
               "global multiplier reaches it.\n"
               "ON, the three settings below replace whatever the shared chain would have produced, RAW - the global intensity, reach and radius "
               "multipliers do not apply to it, and neither does the mass boost. That is what makes them independent rather than merely additional: a "
               "lantern you have tuned stays where you put it while you tune the rest of the world around it.\n"
               "Its COLOUR is not overridden either way. The game registers a real lamp light at the flame point and the lantern adopts that colour, "
               "which is the artists' own.\n"
               "The three defaults below are the undetermined branch's own values, so flipping this on and changing nothing else leaves the lantern where "
               "it was apart from dropping the multipliers.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightLanternIntensity, 1.0f,
                    "Brightness of Link's lantern, when rtx.dusklight.game.effectLightLanternSeparate is on. Ignored entirely when it is off.\n"
                    "rtx.dusklight.game.effectLightIntensity does NOT multiply this.",
                    args.minValue = 0.0f,
                    args.maxValue = 64.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightLanternReach, 400.0f,
                    "How far Link's lantern carries, in world units, when it is being solved separately. Ignored when it is not.\n"
                    "For scale: the game gives a dungeon torch an influence radius of 500, and Link stands about 150 units tall. "
                    "rtx.dusklight.game.effectLightReachScale does NOT multiply this.",
                    args.minValue = 0.0f,
                    args.maxValue = 8000.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, effectLightLanternRadius, 8.0f,
                    "Emitter radius of Link's lantern, in world units, when it is being solved separately. Ignored when it is not.\n"
                    "rtx.dusklight.game.effectLightRadiusScale does NOT multiply this.",
                    args.minValue = 0.5f,
                    args.maxValue = 64.0f);
    RTX_OPTION("rtx.dusklight.game", bool, lanternInfiniteOil, false,
               "Keep Link's lantern permanently fuelled.\n"
               "Tops the oil back to full whenever it is below, and stops the per-frame burn while the lantern is lit. An empty "
               "lantern refills rather than going out.\n"
               "Here rather than in the game's own menu because that menu is never drawn in the fixed-function D3D9 mode. It exists "
               "because enclosed rooms currently have very little light of their own and the lantern is the only portable source, so "
               "testing interior lighting otherwise means managing fuel instead of looking at the room. It is a gameplay change and it "
               "is off by default.");

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
                    "candles rather than trusting it to be reasonable.\n"
                    "0 means no limit, matching rtx.dusklight.game.effectLightMaxDistance where 0 disables the distance cull. To turn the system off, use "
                    "rtx.dusklight.game.effectLights - dragging this to 0 does the opposite of what it looks like.",
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
    RTX_OPTION("rtx.dusklight.game", bool, effectLightSparks, true,
               "Give the game's spark effects their own light - kirakira glitter, and the Shadow Insect's electric spark.\n"
               "ON is what the game already did. Every one of the 29 spark effects was lit before this switch existed, so turning it on changes nothing "
               "and turning it OFF is the visible change. It is here as an undo: the Shadow Insect (闇虫 yami mushi, the twilight bug Wolf Link hunts) "
               "sparks in short bursts driven by its behaviour rather than on a timer, and if that reads as flicker rather than as a spark this is the "
               "one checkbox that removes it without a rebuild.\n"
               "The bug's own spark is ZI_S_ym_elecAt_a..d and the large one's is ZI_S_yb_elec_a..d. Turning this off also removes the 21 kirakira "
               "glitter effects, which are a different thing that happens to share the class - check rtx.dusklight.env.effLightsSparks before deciding.");
    RTX_OPTION_ARGS("rtx.dusklight.game", int, effectLightSparkHold, 12,
                    "Extra frames a spark's light is held after its last particle, on top of the six every light already gets. At the game's 30Hz sim "
                    "pace 12 frames is 0.4 seconds.\n"
                    "This is a renderer setting, not a look setting. The Shadow Insect's shortest spark window is 5 to 15 frames - shorter than the base "
                    "hold - so without this a bug bouncing around a room repeatedly destroys and re-creates its light, and a re-created light is a new "
                    "hash that has to accumulate its denoising history again from nothing. The hold bridges the gaps INSIDE a burst; it does not keep a "
                    "light alive after the sparking genuinely stops. Set 0 for the old behaviour.",
                    args.minValue = 0,
                    args.maxValue = 120);
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

    // Three values the game's original artists had sliders for and nobody since has been able to
    // reach. Their debug panel is compiled out of every build of this port (one #if DEBUG around
    // the whole of d_kankyo.cpp's genMessage functions, and DEBUG is 0), so the bindings survive
    // only as a specification - a label the authors wrote, the exact field, and the range they
    // worked in. docs/kankyo-tuning-surface.md in the game repo has the extraction.
    //
    // These three and no others because these three are the only environment fields that are set
    // once per scene rather than rebuilt every frame by the palette blend, which is what lets the
    // game apply them at all. Each defaults to the game's own value, so nothing changes until a
    // slider moves.
    RTX_OPTION_ARGS("rtx.dusklight.game", float, waterSurfaceShine, 1.0f,
                    "How glossy a water surface reads. The game's own slider for this is labelled 'tera-tera' - the Japanese mimetic for a wet, "
                    "glistening sheen - and its range is this one.\n"
                    "It moves two things together, which is why it is worth a control rather than a constant: the konstant colour the water surface's "
                    "shading is built from, and the speed its ripple texture animates at. Lower is duller and slower; 1 is what the game ships and is "
                    "the default here.\n"
                    "Applies to the MA09 water surface, which is one of the several stacked layers a body of water is drawn from - see the Water "
                    "section for the others.",
                    args.minValue = 0.0f,
                    args.maxValue = 1.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, grassLightInfluence, 1.0f,
                    "How much the room's light colours the grass, on the game's own 0 to 2 scale with 1 as shipped.\n"
                    "The game tints every blade from the room's first light before drawing it, and that tint is one of the colours that does reach "
                    "Remix - so this is the dial for grass reading too dark or too flat against terrain the path tracer has lit for real. It reaches "
                    "the flowers as well, which are planted by the same actor through a second batch.",
                    args.minValue = 0.0f,
                    args.maxValue = 2.0f);
    RTX_OPTION_ARGS("rtx.dusklight.game", float, clockRate, 1.0f,
                    "How fast the game's clock runs, as a multiplier on its normal speed. 1 is normal, 0 stops it, 10 makes a day take a tenth of the time.\n"
                    "Slow is as useful as fast here: three of the six time-of-day palettes the game authors exist for a single instant each with a "
                    "cross-fade either side, and a low rate is the only way to watch one of those transitions happen rather than land on it.\n"
                    "Freeze Time is still the right tool for an A/B pair - it also holds the Twilight Realm's separate clock. This only scales the "
                    "normal advance, and it deliberately keeps its hands off the wolf's howl-to-dawn skip while that is running.",
                    args.minValue = 0.0f,
                    args.maxValue = 20.0f);
  };

}
