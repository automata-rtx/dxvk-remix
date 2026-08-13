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
                    "The game's blur width in its native 0..255 range, the first half of how its bloom is authored. Written by the game's kankyo bridge.");
    // Corrected 2026-08-12: this said "the game's bloom brightness", which is what the value does
    // rather than what the game calls it, and it invites reaching for this to brighten a bloom. The
    // original team's own slider (d_kankyo.cpp:7084) labels the field blur DENSITY, paired with
    // blur WIDTH on the line above it (:7083) - dusklight-ao docs/japanese-naming.md section 8
    // carries the whole panel, which is a primary source in its section 6 sense.
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomBlurRatio, 128.0f, RtxOptionFlags::NoSave,
                    "The game's blur density in its native 0..255 range, the other half. Written by the game's kankyo bridge.\n"
                    "Density rather than brightness: it is the weight each blur sample carries, which does read as brightness on screen, but the game "
                    "authors width and density as a pair and reading it as a brightness dial loses that.");
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
    // Corrected 2026-08-12: this said "room and terrain geometry" without qualification, which
    // reads as all of it. The game keeps FOUR background ambient layers and hands a piece of room
    // geometry one of them by the low two bits of its tevstr type (d_kankyo.cpp:4199-4200), from a
    // fixed table in the room actor - d_a_bg.cpp:336, over the six room model files model.bmd ..
    // model5.bmd. This option carries layer 0 only. dusklight-ao
    // docs/kankyo-tuning-surface.md section 2.1a has the routing table and why the other three
    // are not sent.
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, bgAmbient, Vector3(1.0f, 1.0f, 1.0f), RtxOptionFlags::NoSave,
                    "The ambient colour the game's environment system is applying to background layer 0, normalized to 0..1. Written by the game's kankyo bridge.\n"
                    "Layer 0 is the one the original team's panel labels 'chikei', terrain; it lights the room's first and last model files, and it is what the "
                    "game itself reuses whenever it wants 'the' background ambient without a piece of geometry in hand - particles, grass, flowers, mirror "
                    "reflections. The other three layers light the room's other model files and are deliberately not sent: along that path they are ambient "
                    "light, which path tracing replaces. The counterpart to rtx.dusklight.env.actorAmbient for everything that is not an actor.");

    // The three background ALPHAS, carried since protocol 13. They live in the alpha slot of the
    // ambient colours above and have nothing to do with ambient light: setLight_bg overwrites all
    // four of those alphas with 255 (d_kankyo.cpp:2931-2934) before anything is lit. Each is
    // blended per frame from its own palette column and each had a slider in the original team's
    // own panel, so they are authored values rather than struct padding.
    //
    // Pushed and displayed only, by both sides. Nothing consumes them, and the reason to carry
    // them at all is that the fork cannot recover them from the D3D9 feed - they arrive per draw
    // already folded into the TEV chain and no material name reaches Remix, so there is no way to
    // tell which draw carried which one. -1 means the game has not reported it: a build older
    // than protocol 13, or the bridge not running. Distinguishing that from a genuine 0 matters,
    // because 0 is a real authored value.
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bgWaterAlpha, -1.0f, RtxOptionFlags::NoSave,
                    "The game's water-surface alpha, 0..1, or -1 when the game has not reported it. Written by the game's kankyo bridge.\n"
                    "The original team's slider calls it 'suimen alpha' - suimen is water surface. The game feeds it to the murk material's konstant alpha "
                    "and to the water and shimmer materials beside it, and its mud particles read it too. Not consumed.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bgAuxAlpha, -1.0f, RtxOptionFlags::NoSave,
                    "The game's 'auxiliary' background alpha, 0..1, or -1 when the game has not reported it. Written by the game's kankyo bridge.\n"
                    "The original team's slider calls it 'hosa alpha' - hosa is assistance or support - and named it that rather than for a surface, so no "
                    "more specific meaning is claimed here. It travels with the water alpha above, as the second of the pair the murk and water materials "
                    "take. Not consumed.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bgFakeFogAlpha, -1.0f, RtxOptionFlags::NoSave,
                    "The strength of the game's own faked fog, 0..1, or -1 when the game has not reported it. Written by the game's kankyo bridge.\n"
                    "The original team's slider is labelled 'uso Fog' - uso means a lie - and the game applies it as a material constant on three of its "
                    "background material classes rather than through the fog hardware, which is what makes it separate from rtx.dusklight.env.fogActive and "
                    "the distances beside it. Worth watching next to the atmosphere's own extinction in an area that looks over-fogged. Not consumed.");

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
    // Corrected 2026-08-12: this said "at the zenith". Nothing in the game places it there. The
    // original team call it just the sky's colour - the panel heading reads "sora no iro"
    // (d_kankyo.cpp:6280), the palette CSV column is "sorairo" (:6582) and the debug view prints it as
    // "Sky" (d_kankyo_debug.cpp:284). Where on the dome it lands is decided by vrbox_sora.bmd,
    // which is in none of the three checkouts, so the fork treating it as the dome's base colour
    // is a modelling choice of ours and is stated as one. Same class of unsourced positional claim
    // as the kasumi pair corrected above.
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, skyColor, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's sky colour, normalized to 0..1. Written by the game's kankyo bridge.\n"
                    "The game names this one simply the sky's colour and the haze bands separately; the atmosphere uses it as the dome's base colour, which "
                    "is this fork's reading of it rather than something the game states.");
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
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kumoTop, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's lit cloud colour, normalized to 0..1. Written by the game's kankyo bridge. Not consumed yet - clouds are a later phase.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kumoBottom, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's shaded cloud underside colour, normalized to 0..1. Written by the game's kankyo bridge. Not consumed yet.");
    RTX_OPTION_FLAG("rtx.dusklight.env", Vector3, kumoShadow, Vector3(0.0f, 0.0f, 0.0f), RtxOptionFlags::NoSave,
                    "The game's cloud shadow colour, normalized to 0..1. Written by the game's kankyo bridge. Not consumed yet.");

    // Scene classification, used to decide how stylised the atmosphere should be.
    RTX_OPTION_FLAG("rtx.dusklight.env", int, colpat, 0, RtxOptionFlags::NoSave,
                    "Which of the game's colour patterns is currently active. Written by the game's kankyo bridge.\n"
                    "0 is clear weather; others are weather, story and area variants. Pattern 9 is the Palace of Twilight, whose sky has no "
                    "physical description at all - there is no sun and the look is authored - so it is the clearest signal to stop trying to "
                    "model the sky and reproduce the palette instead.");
    // The other two thirds of the same thing. The game never holds a single colour pattern: it
    // holds a crossfade between an outgoing and an incoming one, and every palette value it
    // produces - sky, fog, ambient - is that lerp. colpat above is only the incoming end of it.
    //
    // Outside a transition the game pins colpatBlend at 1.0 and makes colpatPrev equal colpat, so
    // these say nothing new most of the time. During a transition they are the difference between
    // a consumer that steps and a consumer that follows: the game's own kankyo tags drive the
    // blend continuously (the Lost Woods mist tag ramps it over roughly a second, and that ramp is
    // the mist's strength), while the pattern index flips on the first frame.
    //
    // The default of 1.0 is what makes this safe against a game build that does not push it, and
    // against the frames before the first push arrives: at blend 1.0 any lerp between the two
    // patterns collapses to colpat exactly, which is the behaviour that shipped before these
    // existed. See documentation/DusklightAtmosphere.md section 4.
    RTX_OPTION_FLAG("rtx.dusklight.env", int, colpatPrev, 0, RtxOptionFlags::NoSave,
                    "The colour pattern the game is fading OUT of. Written by the game's kankyo bridge.\n"
                    "Equal to the current pattern except while a weather, room or event transition is running. Pair it with colpatBlend: "
                    "at blend 0 the game's colours are entirely this pattern, at 1 entirely the current one.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, colpatBlend, 1.0f, RtxOptionFlags::NoSave,
                    "How far the game is through the fade from the previous colour pattern to the current one, 0..1. Written by the game's kankyo bridge.\n"
                    "Sits at 1.0 whenever no transition is running, so 1.0 is also the correct reading when nothing has been pushed yet. "
                    "A value outside 0..1 means the reading is wrong rather than extreme - the game clamps its own ratio to 0..1 - and consumers clamp defensively.");
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

    // Room lights - the room's own authored lights. found vs drawn is the same separation the
    // mirror above needed; shaped vs unshapeable is the pair that answers the cone question,
    // which nothing in this project has ever been able to answer by reading, because the data
    // lives in the game's stage files rather than in its source.
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, roomLightsRunning, false, RtxOptionFlags::NoSave,
                    "True when the game got past every gate and actually ran its room light submission. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, roomLightsFound, 0, RtxOptionFlags::NoSave,
                    "How many lights the room the player is standing in was authored with, as the game itself counts them - so at most six, and with the two "
                    "slots the sun and moon take outdoors already removed. Written by the game's kankyo bridge.\n"
                    "This is what tells a room that has no authored lights apart from a bridge that is failing to submit them.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, roomLightsDrawn, 0, RtxOptionFlags::NoSave,
                    "How many of the room's authored lights were submitted to Remix this frame. Written by the game's kankyo bridge.\n"
                    "Lower than roomLightsFound is normal and not a fault: a light whose switch is off, or whose colour the palette has taken to black, is "
                    "skipped exactly as the game skips it.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, roomLightsTracked, 0, RtxOptionFlags::NoSave,
                    "How many of the room's authored lights currently hold a live Remix light. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, roomLightsShaped, 0, RtxOptionFlags::NoSave,
                    "How many of the lights submitted this frame carried a cone.\n"
                    "The first number this project has ever had for how much of the game is actually spotlit. Zero everywhere would mean the cone work is "
                    "carrying no weight at all, which is worth knowing before anyone tunes it.\n"
                    "Counted only while roomLightsRunning is true - it counts submissions, not the room. A zero while the system is off says nothing about "
                    "whether the room has cones; the game's log answers that either way, one block per room entered.");
    RTX_OPTION_FLAG("rtx.dusklight.env", int, roomLightsUnshapeable, 0, RtxOptionFlags::NoSave,
                    "How many room lights used one of the game's two ring-shaped cone functions, which are dark on the axis and brightest partway out.\n"
                    "Remix's light shaping only ever gets brighter towards the axis, so a ring cannot be expressed at any setting. Those lights are sent "
                    "with no cone at all rather than dropped, on the grounds that a wrongly-shaped light beats an unlit room - this counts how often that "
                    "compromise is being made. If it is always zero the game does not use them and nothing is owed.\n"
                    "Counted only while roomLightsRunning is true, same as roomLightsShaped.");

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
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, effLightsAuthored, "", RtxOptionFlags::NoSave,
                    "Which of this frame's lights were solved from values the effect's own artists authored, rather than from the settings, as "
                    "'colour N  radius N  lantern N'.\n"
                    "colour counts sites whose hue came from the effect's authored colour ramp. It does NOT count sites that adopted one of the game's "
                    "own lights - those take that light's colour, which wins over both - so in a room full of registered torches this can legitimately "
                    "read 0 while the setting is on. radius counts sites whose sphere grew to the authored extent, which is 0 unless "
                    "rtx.dusklight.game.effectLightAuthoredRadius is on. lantern counts sites solved from the lantern's own settings.\n"
                    "This is the readout that says whether the authored derivations are running at all. A colour count of 0 with no game lights available "
                    "means the resources carried no colour, which is a different failure from the setting being off.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, effLightsClasses, "", RtxOptionFlags::NoSave,
                    "This frame's lights split by what the game's own name says each effect IS, as 'other N  fire N  lantrn N  glow N  spark N  lava N  "
                    "burst N  excl N'.\n"
                    "The classes come from the original team's vocabulary rather than from anything we invented: kantera is Link's lantern, kirakira is "
                    "glitter, yogan is lava, and so on. excl is always 0 here - an effect the name refuses never becomes a site, and "
                    "rtx.dusklight.env.effLightsExcluded is where those are counted instead. burst is 0 unless "
                    "rtx.dusklight.game.effectLightBursts is on.\n"
                    "A lantrn count of 0 while the lamp is lit means the lantern's emitter failed the additive-and-glow rule that frame, not that the "
                    "classification is wrong; the full report names it either way.");
    RTX_OPTION_FLAG("rtx.dusklight.env", std::string, effLightsSparks, "", RtxOptionFlags::NoSave,
                    "Spark effects this frame, as 'seen N  lit N'. This is the Shadow Insect readout - one glance says whether the twilight bug's spark "
                    "is being lit.\n"
                    "seen counts spark emitters the game was actually DRAWING, so a non-zero seen means a bug was sparking in front of the camera. lit "
                    "counts how many of those the additive-and-glow rule then accepted. The two numbers separate two failures that have opposite fixes: "
                    "'seen 4  lit 0' means the bug sparked in view and the rule refused it, which is a property of the effect's authored blend mode and "
                    "colour and cannot be fixed by classification - press Log Full Effect Light Report and read the verdict on ids 0x393-0x396. "
                    "'seen 0  lit 0' means no spark was ever in view at all, which is a question about where you were standing or about the draw-group "
                    "filter, not about the rule.\n"
                    "lit is forced to 0 when rtx.dusklight.game.effectLightSparks is off, by construction.");

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
