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

    // Local point lights.
    RTX_OPTION("rtx.dusklight.game", bool, localLights, false,
               "Mirrors the game's own point lights - torches, braziers, lanterns, campfires and the dungeon lights - into Remix as sphere lights.\n"
               "The game's D3D9 path does not forward its lights, so without this Remix sees no light from the game at all: outdoors the sun covers "
               "that, but interiors and night fall through to Remix's fallback light.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, localLightIntensity, 1.0f,
                    "Scales the game's local lights. At 1.0 each light is as bright as Remix's own conversion would make a legacy light that reached "
                    "as far as the game's influence radius.\n"
                    "The game's attenuation curve actually carries further than that radius - about 19 here reproduces that reading instead, which is "
                    "the number to try if the torches look weak.",
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
                    "and sunset elevations barely move either, so those transitions look the same.",
                    args.minValue = 1.0f,
                    args.maxValue = 90.0f);
    RTX_OPTION("rtx.dusklight.game", bool, hideSkyBillboards, false,
               "Stops the game drawing its sun, moon and star billboards.\n"
               "Those are placed at a fixed offset from the camera, so they travel with the player. Any of them that Remix captures as ordinary world geometry "
               "becomes an occluder that follows you around - a candidate for shadowed areas appearing to wander as the camera moves, and one that would only "
               "show at night, since stars and the moon are the only sky billboards drawn then.\n"
               "Tagging those textures as Sky is the real fix; this is here to test the theory in one click. It does remove the visible stars and moon.");
    RTX_OPTION("rtx.dusklight.game", bool, hideVrbox, false,
               "Stops the game drawing its own sky dome.\n"
               "The dome is painted by handing the hardware a handful of colours rather than by drawing a texture, so Remix has nothing "
               "distinctive to hash and the dome can never be tagged as sky - which is why rtx.dusklight.atmosphere.skyEnable generates a "
               "sky from those same colours instead. Turn this on together with that, or the generated sky and the game's own dome will "
               "both be visible; leave it off and the atmosphere still lights the scene but you will be looking at the game's dome.\n"
               "Also set rtx.skyAutoDetect to None, otherwise the auto detected dome keeps feeding a second, dimmer sky into the same pixels.");
    RTX_OPTION("rtx.dusklight.game", bool, recordingMode, false,
               "The game's own recording mode: hides its HUD and silences its music.\n"
               "It is a game setting rather than a Remix one, and the game's settings screen is not drawn in the fixed function D3D9 mode, so without this it "
               "can only be changed by editing config.json and restarting - and only in one direction, since a value set there could not be turned back off "
               "while running.");
    RTX_OPTION_ARGS("rtx.dusklight.game", float, localLightRadius, 4.0f,
                    "Emitter radius of the game's local lights in world units, matching Remix's own default for converted point lights.\n"
                    "This changes brightness as well as softness: the radiance is solved so the light still reaches the same distance, so a larger "
                    "emitter needs less of it. Large radii on lights sitting inside wall sconces will clip through the geometry.",
                    args.minValue = 0.5f,
                    args.maxValue = 64.0f);
  };

}
