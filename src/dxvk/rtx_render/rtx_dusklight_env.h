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

namespace dxvk {

  // Raw environment state fed by the game's kankyo bridge (see dusklight-ao
  // docs/kankyo-remix.md). "kankyo" is the game's own name for its environment system,
  // romanized Japanese rather than an acronym - like most identifiers in the game, which
  // the decompilation preserves from the original Japanese team. It is spelled that way in
  // every option description below on purpose; dusklight-ao docs/japanese-naming.md is the
  // reference. The game pushes these through the Remix API every frame it is
  // running under Remix with the bridge enabled; they describe what the game's environment
  // system computed, not how strongly Remix should respond to it. Response knobs live with
  // the passes that consume them (e.g. rtx.bloom.dusklightThresholdScale). All options here
  // are NoSave so a settings save never bakes one moment's weather into a config file.
  struct DusklightEnv {
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, enable, false, RtxOptionFlags::NoSave,
                    "True while the game's kankyo bridge is pushing environment state into Remix.\n"
                    "Set by the game itself; do not set by hand. Consumers only honour the other rtx.dusklight.env options while this is true.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, protocol, 0, RtxOptionFlags::NoSave,
                    "Which revision of the game side bridge is running, so the Dusklight tab can tell an out of date game build from a broken one.\n"
                    "0 means the game predates rtx.dusklight.game.*, and every control in that tab will appear to do nothing because nothing is reading them. "
                    "Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, daytime, 0.0f, RtxOptionFlags::NoSave,
                    "The game's clock in degrees, 0 to 360 over a whole day, so 15 is an hour. Written by the game's kankyo bridge.\n"
                    "Quantized to a quarter of a degree, one in-game minute, because it changes every frame. This is the readout; "
                    "rtx.dusklight.game.timeOfDay is the control.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, bloomEnable, true, RtxOptionFlags::NoSave,
                    "Whether the game's current environment palette wants bloom at all. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomThreshold, 0.5f, RtxOptionFlags::NoSave,
                    "The game's bloom threshold (its 0..255 'point' value normalized to 0..1). Written by the game's kankyo bridge.\n"
                    "Consumed by the Dusklight bloom pass scaled by rtx.bloom.dusklightThresholdScale.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomBlurSize, 64.0f, RtxOptionFlags::NoSave,
                    "The game's bloom blur size in its native 0..255 range. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomBlurRatio, 128.0f, RtxOptionFlags::NoSave,
                    "The game's bloom brightness in its native 0..255 range. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, bloomTint, Vector3(1.0f, 1.0f, 1.0f), RtxOptionFlags::NoSave,
                    "The game's bloom blend colour, normalized to 0..1. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomBaseWeight, 1.0f, RtxOptionFlags::NoSave,
                    "How much of the base image the game's bloom composite keeps (its blend alpha normalized to 0..1). "
                    "Twilight dims the scene to about 0.82 through this. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, bloomScreenBlend, false, RtxOptionFlags::NoSave,
                    "Whether the game's bloom composite uses the screen style blend. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, monoColor, Vector3(1.0f, 1.0f, 1.0f), RtxOptionFlags::NoSave,
                    "The game's full-screen mono overlay tint, normalized to 0..1. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, monoAmount, 0.0f, RtxOptionFlags::NoSave,
                    "Strength of the game's full-screen mono (desaturate + tint) overlay, 0..1. Twilight runs this at about 0.38. "
                    "Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, actorAmbient, Vector3(1.0f, 1.0f, 1.0f), RtxOptionFlags::NoSave,
                    "The ambient colour the game's environment system is currently applying to actors, normalized to 0..1. Written by the game's kankyo bridge.\n"
                    "This is the ambient the original fixed function pipeline tinted every character and object with; path tracing replaces that lighting, "
                    "so rtx.dusklight.grade.* uses it to put the mood back.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, bgAmbient, Vector3(1.0f, 1.0f, 1.0f), RtxOptionFlags::NoSave,
                    "The ambient colour the game's environment system is currently applying to room and terrain geometry, normalized to 0..1. "
                    "Written by the game's kankyo bridge. The counterpart to rtx.dusklight.env.actorAmbient for everything that is not an actor.");

    // Fog. The game authors these in the same palette entry as the sky colours below, selected by
    // the same time of day and weather indices and blended by the same call, so its fog colour is
    // its sky colour - distant terrain dissolves into the sky because it was authored to. They are
    // pushed together for that reason and consumed together by rtx.dusklight.atmosphere.
    //
    // These are the game's *global* environment fog. The game also sets fog per object, which is
    // what the D3D9 capture sees, and Remix keeps only the first such state it encounters in a
    // frame - so the captured value is decided by submission order. These replace it.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, fogActive, false, RtxOptionFlags::NoSave,
                    "True while the game's environment system has fog enabled. Written by the game's kankyo bridge.\n"
                    "When false the atmosphere leaves Remix's own fog handling alone.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, fogColor, Vector3(1.0f, 1.0f, 1.0f), RtxOptionFlags::NoSave,
                    "The game's fog colour, normalized to 0..1. Written by the game's kankyo bridge.\n"
                    "This is the final value after every modifier the game applies - the palette blend, the additive offset, the colour "
                    "ratio that lightning pulses, and the second 'gather' blend that the fog bank tags drive - so nothing needs "
                    "reimplementing on this side to stay faithful.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, fogStartZ, 0.0f, RtxOptionFlags::NoSave,
                    "Distance in world units at which the game's fog begins. Written by the game's kankyo bridge.\n"
                    "The game's fog is a linear ramp, not extinction: nothing before this, fully opaque at rtx.dusklight.env.fogEndZ. "
                    "Scripted fog banks pass a negative value here deliberately, which means the ramp is already well underway at the camera.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, fogEndZ, 0.0f, RtxOptionFlags::NoSave,
                    "Distance in world units at which the game's fog reaches full opacity. Written by the game's kankyo bridge.\n"
                    "Ranges over three orders of magnitude across the game - a couple of metres inside a scripted fog bank, hundreds "
                    "of metres in an open field - which is why the froxel grid has to follow it rather than sit at a fixed size.");

    // Sky. Fed to the dome light, which replaces both Remix's auto detected sky probe and the
    // game's own sky dome. The game paints its dome by handing the hardware these few colours per
    // frame rather than by drawing a texture, so generating the sky from the same colours is the
    // direct translation of it.
    //
    // Not because the dome is untaggable - that claim was checked and is false. Texture hashing is
    // only one of three category routes; rtx.skyBoxGeometries tags by geometry hash and needs no
    // texture. documentation/DusklightAtmosphere.md §14.9.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, skyHidden, false, RtxOptionFlags::NoSave,
                    "True when the game's current area has no sky at all - interiors and most dungeons. Written by the game's kankyo bridge.\n"
                    "The game decides this itself by checking whether its sky colours sum to zero, so this is its own answer rather than a guess, "
                    "and it is what stops a sky light being added indoors.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, skyColor, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's sky colour at the zenith, normalized to 0..1. Written by the game's kankyo bridge.");
    // Corrected 2026-08-10: these were described as the haze "on the sun's side" and "away from
    // the sun". Nothing in the game relates either field to sun position. The split is front/back,
    // and the game says so in three places - the palette CSV exporter's Japanese header
    // (d_kankyo.cpp:6582) labels kasumi_outer as the near band and kasumi_inner as the far one,
    // the debug view (d_kankyo_debug.cpp:301,306) prints them as kasumiF and kasumiB, and the two
    // dome actors paint one band each. Note this makes "outer" the NEAR band, opposite to what the
    // English reads like. dusklight-ao docs/japanese-naming.md section 6 carries the derivation.
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kasumiInner, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's far horizon haze band, normalized to 0..1. Written by the game's kankyo bridge.\n"
                    "'Kasumi' is the game's own name for horizon haze; the game labels this one the back band.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kasumiOuter, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's near horizon haze band, normalized to 0..1. Written by the game's kankyo bridge.\n"
                    "Despite the name this is the front band, the one nearer the viewer; the game labels it so.");
    // The two kasumi bands' alphas, and the cloud layer's. Added at protocol 12. Authored per
    // palette entry and blended every frame exactly like the RGBs (d_kankyo.cpp:2827, 2847, 2775),
    // with a slider each in the original team's own panel. -1 means the game has not reported it -
    // a build older than protocol 12, or the bridge not running. Distinguishing that from a
    // genuine 0 matters: 0 is a real authored value, and treating "silent" as 0 is how a readout
    // that is not arriving turns into a look change nobody can trace.
    RTX_OPTION_FLAG("rtx.dusklight.env", float, kasumiInnerAlpha, -1.0f, RtxOptionFlags::NoSave,
                    "Alpha of the game's far horizon haze band, 0..1, or -1 when the game has not reported it. Written by the game's kankyo bridge.\n"
                    "Pushed and displayed only. What a TEV colour register's alpha does depends on the alpha stages inside vrbox_sora.bmd, and no "
                    ".bmd exists in any of the three checkouts, so calling this 'the haze's opacity' would be inference rather than something read.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, kasumiOuterAlpha, -1.0f, RtxOptionFlags::NoSave,
                    "Alpha of the game's near horizon haze band, 0..1, or -1 when the game has not reported it. Written by the game's kankyo bridge.\n"
                    "This is the one alpha the fork consumes, and only in one place: it supplies rtx.dusklight.atmosphere.kasumiFrontWeight, the "
                    "near band's share in the fixed-composite haze blend. That blend is off by default, so nothing reads this unless "
                    "kasumiBlendMode is set to 1. See documentation/DusklightAtmosphere.md section 12.2.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, kumoAlpha, -1.0f, RtxOptionFlags::NoSave,
                    "Alpha of the game's cloud layer, 0..1, or -1 when the game has not reported it. Written by the game's kankyo bridge.\n"
                    "Named for the layer rather than for a band on purpose. The game keeps it in vrbox_kumo_top_col.a, but its palette source is "
                    "kumo_shadow_col.a, its CSV column is the lower-cloud alpha, its slider sits under the lower-cloud-shadow heading, and the "
                    "debug view prints it as 'Cloud A' rather than 'CloudU A'. It is the layer's alpha, not the upper band's. Not consumed.");

    // Corrected 2026-08-11: these were described as the "lit" cloud colour, the "shaded cloud
    // underside", and a generic "cloud shadow". The game's labels say upper, lower, and the LOWER
    // cloud's shadow - d_kankyo.cpp:6302, 6324, 6346 - and the one site that consumes the pair
    // lerps them by horizontal distance from the camera (d_kankyo_rain.cpp:5026-5039), which is a
    // zenith-to-horizon gradient across the cloud field rather than a lighting term. The old
    // wording is the spec a clouds phase would have built from, and it would have led to a
    // physically-lit cloud model where the game means a distance gradient it already ships a
    // recipe for. Same shape of error as the kasumi pair, in the adjacent fields.
    // dusklight-ao/docs/japanese-naming.md section 6; the recipe is DusklightAtmosphere.md C3.
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kumoTop, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's upper cloud band colour, normalized to 0..1. Written by the game's kankyo bridge. Not consumed yet - clouds are a later phase.\n"
                    "'Upper' is positional, not a lighting term: this is the end of a gradient the game runs across its cloud field by distance, not the lit side of a cloud.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kumoBottom, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's lower cloud band colour, normalized to 0..1. Written by the game's kankyo bridge. Not consumed yet.\n"
                    "The far end of that same gradient - what the cloud field fades towards near the horizon - rather than a shaded underside.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kumoShadow, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's LOWER cloud's shadow colour, normalized to 0..1. Written by the game's kankyo bridge. Not consumed yet.\n"
                    "The game labels it for the lower band specifically, not as a generic cloud shadow.");

    // Scene classification, used to decide how stylised the atmosphere should be.
    RTX_OPTION_FLAG("rtx.dusklight.env", int, colpat, 0, RtxOptionFlags::NoSave,
                    "Which of the game's colour patterns is currently active. Written by the game's kankyo bridge.\n"
                    "0 is clear weather; others are weather, story and area variants. Pattern 9 is the Palace of Twilight, whose sky has no "
                    "physical description at all - there is no sun and the look is authored - so it is the clearest signal to stop trying to "
                    "model the sky and reproduce the palette instead.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, moyaMode, 0, RtxOptionFlags::NoSave,
                    "Which of the game's haze particle modes is running, 0 for none. Written by the game's kankyo bridge.\n"
                    "These are billboard particles rather than fog, but they are already never drawn on the D3D9 backend "
                    "(dKankyo_cloud_Packet::draw returns early), so there is nothing to double count against the medium. Pushed as a signal of how "
                    "much haze an area wants folded into the medium; only displayed so far, not consumed by the atmosphere.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, moyaCount, 0.0f, RtxOptionFlags::NoSave,
                    "How strongly the game's haze particles are running, in its native 0..50 range. Written by the game's kankyo bridge.");

    // Light status, reported so the Dusklight tab can show what the game is actually doing.
    // The game's own debug UI is not drawn in its D3D9 mode, so this is the only place these
    // are visible.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, sunActive, false, RtxOptionFlags::NoSave,
                    "True while the game is drawing its sun/moon distant light. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, sunIsDay, false, RtxOptionFlags::NoSave,
                    "True when the game's celestial light is currently the sun rather than the moon. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, sunAzimuth, 0.0f, RtxOptionFlags::NoSave,
                    "Compass bearing of the game's sun or moon in degrees, about the world's up axis (0 = +Z, 90 = +X). Written by the game's kankyo bridge.\n"
                    "This is a function of the game's time of day and nothing else, so it must not move while the player does - which makes it the quickest "
                    "answer to whether the light is following the player around.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, sunElevation, 0.0f, RtxOptionFlags::NoSave,
                    "Height of the game's sun or moon above the horizon in degrees. Written by the game's kankyo bridge. "
                    "Like the azimuth, a function of time of day alone.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, sunFade, 0.0f, RtxOptionFlags::NoSave,
                    "How far the game's celestial light is faded in, 0..1. Falls to zero across the dawn and dusk handovers, where the direction jumps "
                    "between the sun's and the moon's. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, deviceRegistered, false, RtxOptionFlags::NoSave,
                    "True once the game has registered its D3D9 device with the Remix API, which everything that submits lights depends on. "
                    "Written by the game's kankyo bridge.");
    // Warp destinations, resolved by the game from the indices the overlay selected. Pipe
    // delimited, because a list crossing as one value beats one option per entry.
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, warpRegions, "", RtxOptionFlags::NoSave,
                    "Every warp region the game knows, in plain English, pipe delimited. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, warpMaps, "", RtxOptionFlags::NoSave,
                    "Levels in the selected region, in plain English, pipe delimited. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, warpRooms, "", RtxOptionFlags::NoSave,
                    "Room numbers in the selected level, pipe delimited. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, warpPoints, "", RtxOptionFlags::NoSave,
                    "Spawn points in the selected room, pipe delimited. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, warpStage, "", RtxOptionFlags::NoSave,
                    "The stage file the current selection resolves to - the name the warp actually travels on. Written by the game's kankyo bridge.");

    // Action binds, pushed by the game for the Controls tab to display. The game is the only side
    // that can name a bind correctly - the stored value is an SDL scancode on a keyboard driven
    // port and a native gamepad button otherwise - so it sends finished strings rather than raw
    // values, and the overlay never has to know the difference.
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, bindActions, "", RtxOptionFlags::NoSave,
                    "Pipe delimited names of the game's rebindable actions, in the order the overlay's indices refer to.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, bindButtons, "", RtxOptionFlags::NoSave,
                    "Pipe delimited display names of what each action is currently bound to on the selected port, aligned with bindActions.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, bindStatus, "", RtxOptionFlags::NoSave,
                    "What the game did with the last bind request, in prose - including which action lost its bind when one was displaced.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, bindCapturing, false, RtxOptionFlags::NoSave,
                    "True while the game is waiting for a key or button press to bind.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, bindKeyboard, false, RtxOptionFlags::NoSave,
                    "True when the selected port is driven by a keyboard, which is what decides whether a bind is a scancode or a gamepad button.");

    RTX_OPTION_FLAG("rtx.dusklight.env", bool, localLightsRunning, false, RtxOptionFlags::NoSave,
                    "True when the game got past every gate and actually ran its light submission loop. Written by the game's kankyo bridge.\n"
                    "Without this an option that reads false and an area with no lights in it are indistinguishable from the other side, since both report zero.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, localLightsFound, 0, RtxOptionFlags::NoSave,
                    "How many point lights the game itself had registered this frame, before any filtering on our side. Written by the game's kankyo bridge.\n"
                    "This is what tells a room with no lights in it apart from a bridge that is failing to submit them - two states that otherwise both read as "
                    "zero drawn, which is what made the first attempt at this hard to diagnose.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, localLightsDrawn, 0, RtxOptionFlags::NoSave,
                    "How many of the game's own point lights were submitted to Remix this frame. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, localLightsTracked, 0, RtxOptionFlags::NoSave,
                    "How many of the game's own point lights currently hold a live Remix light. Written by the game's kankyo bridge.");

    // Effect lights. The chain reads emitters -> considered -> candidates -> sites -> drawn, so
    // whichever step a light was lost at is visible without asking anyone to describe a scene.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, effLightsRunning, false, RtxOptionFlags::NoSave,
                    "True when the effect light system got past every gate and actually ran. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsEmitters, 0, RtxOptionFlags::NoSave,
                    "How many particle emitters the game had alive this frame, before any filtering on our side.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsConsidered, 0, RtxOptionFlags::NoSave,
                    "How many of those were being drawn in a world space pass, and so reached the rule at all.\n"
                    "A large gap between this and effLightsEmitters is normal - most emitters are smoke, dust and screen effects.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsCandidates, 0, RtxOptionFlags::NoSave,
                    "How many passed the rule: additive, and a colour that reads as a glow.\n"
                    "This is the number to watch if fires are going unlit. Raise rtx.dusklight.game.effectLightReportCommit to get the per effect log that "
                    "names which one was rejected and why.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsSites, 0, RtxOptionFlags::NoSave,
                    "How many distinct places got a light, after the several emitters that make up one visible fire were merged.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsDrawn, 0, RtxOptionFlags::NoSave,
                    "How many of those reached Remix this frame.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsDerived, 0, RtxOptionFlags::NoSave,
                    "How many took their colour and reach from a light the game itself authored, rather than from the settings.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsOrphans, 0, RtxOptionFlags::NoSave,
                    "How many of the game's own lights had no effect near them, and so were dropped entirely.\n"
                    "This is the number that decides whether dropping them is the right default. Those lights are the ones whose placement the system exists "
                    "to stop trusting - but some of them are real sources with no particle at all, dungeon fill lights and glowing crystals, and those go "
                    "dark. A consistently high count in rooms that look under-lit is the signal that the policy needs an escape hatch.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsCulled, 0, RtxOptionFlags::NoSave,
                    "How many sites were dropped by the distance cull or the per frame budget.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, effLightsExcluded, 0, RtxOptionFlags::NoSave,
                    "How many effects passed the additive-and-glow rule and were then refused because the game names them as a substance that is never a "
                    "light source.\n"
                    "The list behind this is deliberately narrow - drool and body fluid, two words - and it exists because the Deku Baba was seen lighting "
                    "rooms from its jaw joints on 2026-08-07. That is the case that showed additive blending alone does not mean 'emits light': a wet "
                    "surface is authored additively too, so that it reads as glossy.\n"
                    "Watch this rather than trust it. A non zero count in a room that looks under lit is the signal that the list is too wide, and the "
                    "classification report names every effect it refused.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, effLightsVanilla, "", RtxOptionFlags::NoSave,
                    "The game's own lights available to copy parameters from this frame, as point/spot.\n"
                    "The game keeps two registries and many torches use the second one, so both have to be read. The spot half depends on a per frame flag "
                    "still being set when the bridge runs, which in turn depends on where the game's environment process falls in the frame - a question we "
                    "could not answer by reading. If the spot count is always zero while you are stood at a lit torch, the answer is 'too late', and those "
                    "torches are falling back to the configured defaults instead of the game's own colour. That is a quality loss, not a misplacement.");

    // HD texture replacement packs, game side.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, texrepEnabled, false, RtxOptionFlags::NoSave,
                    "True when the game is handing its HD texture replacement pack to Remix. Written by the game's kankyo bridge.\n"
                    "False means the pack is off, empty, or the game build predates this - which are different from the pack being handed over and "
                    "then ignored on this side. Read it together with the fork's own texrep counters before debugging a pack that is not showing up.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, texrepEntries, 0, RtxOptionFlags::NoSave,
                    "How many replacements the game's registry selected, before any were handed over. Written by the game's kankyo bridge.\n"
                    "Zero with texrepEnabled true means the pack directory is empty or nothing in it parsed as a replacement filename.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, texrepCreated, 0, RtxOptionFlags::NoSave,
                    "How many Remix materials the game has created for those replacements so far. Written by the game's kankyo bridge.\n"
                    "Creation is spread over frames, so this climbs after launch and then stops. This counter versus the fork's applied count is what "
                    "separates 'the game never sent it' from 'the fork ignored it'.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, texrepSkipped, 0, RtxOptionFlags::NoSave,
                    "How many replacements the game could not hand over. Written by the game's kankyo bridge.\n"
                    "Almost always PNG files: Remix's asset loader accepts .dds only, while the game's own registry accepts both. The game logs one "
                    "bounded line per skipped entry with the reason.");
    // Diagnostic state. These drive no rendering at all - they exist so the material
    // report's log can say *when* something happened, in the same file and in order,
    // rather than requiring the game's log to be read alongside it and correlated by
    // wall clock. Deliberately not part of kRequiredProtocol: the overlay has no
    // control that depends on them, so an older game build should not be reported as
    // out of date for lacking them. It simply produces no markers, and the context
    // line says so.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, dash, false, RtxOptionFlags::NoSave,
                    "True while the player is dashing on horseback. Written by the game's kankyo bridge.\n"
                    "Exists so a matrep.rmx line that appears mid dash can be attributed to the dash rather than "
                    "to whatever else was on screen. Reported by the material report's dusklight.mark line.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, camInWater, false, RtxOptionFlags::NoSave,
                    "True while the game considers the camera to be under water (dKy_camera_water_in_status_check).\n"
                    "The MA00/MA01/MA04/MA16 fog materials swap their alpha compare and Z mode on this, so the same "
                    "texture legitimately arrives as two different materials either side of it.");
  };

}
