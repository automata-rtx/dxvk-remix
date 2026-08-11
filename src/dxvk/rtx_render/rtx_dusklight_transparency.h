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

// Dusklight transparency classification, Remix half.
//
// Remix sorts an alpha-blended draw into one of two very different renderers, and it decides
// which by asking whether the draw's *texture* was tagged a particle
// (`rtx_instance_manager.cpp`, `setAlphaState`: `out.isParticle =
// drawCall.testCategoryFlags(InstanceCategories::Particle)`). That is the one question this
// project has repeatedly established cannot be answered per texture: this game reuses textures
// across contexts constantly, so a tag is wrong somewhere almost by construction, and several
// of these draws land after Remix's RTX injection boundary where the categorization UI cannot
// reach them at all. So the game answers it per draw instead, through GXSetDrawClass ->
// D3DMATERIAL9::Ambient.a. CLAUDE.md rule 1.
//
// There are THREE paths, not two. The first draft of this file described two and that was the
// reason haze had no good answer:
//
//   1. STOCHASTIC (the default for any blended draw). Primary TLAS. The resolve loop keeps one
//      randomly chosen layer per pixel per frame and the composite reconstructs its lighting by
//      hunting for a neighbouring *opaque* pixel and borrowing its denoised radiance
//      (`composite_alpha_blend.comp.slang`). For a stack of smoke quads that is one random layer
//      a frame tinted by whatever solid thing is near it on screen - the noise and the wrong
//      colour, one mechanism.
//
//   2. UNORDERED TLAS (isParticle). Every layer accumulated in one order-independent traversal -
//      no stochastic pick, no borrowed lighting - lit from the volumetric radiance cache. Cheap
//      for dense sprites. But `evaluateOpaqueApproximations` returns true, so the hit is never
//      resolved as a surface: no NEE, no RTXDI, no direct light at all. And the froxel lookup
//      *saturates* past the grid, capped at 120 m
//      (`rtx.dusklight.atmosphere.froxelMaxDistanceMaxMeters`), so anything beyond it is lit as
//      though it stood at the grid's edge.
//
//   3. ORDERED (isOrderedTransparency). Decline the stochastic path and the surface falls
//      through to a real resolve: a genuine translucent surface, correctly ordered, path-traced,
//      keeping its own albedo. Globally this is `rtx.enableStochasticAlphaBlend = False`, which
//      is unaffordable because it applies to every blended draw in the scene. Per draw it is
//      exactly right for a handful of large authored layers.
//
// Which class takes which, and why:
//
//   PARTICLE -> 2. Near by nature (an enemy dies next to you), so inside the froxel grid where
//               that path's lighting is real, and dense enough that resolving every layer as a
//               surface would be correct and unaffordable. Tested in game 2026-08-11.
//
//   HAZE     -> 3. A layered wall standing in for distance. Path 2 is wrong for it twice over:
//               it sits far beyond the 120 m cap, and it is *authored scenery* that is meant to
//               read as itself rather than as generic haze - the owner's word for the Death
//               Mountain wall is that it "looks distinctly different from the rest of the distant
//               vista", which is the game's intent, not a defect. So neither suppressing it nor
//               flattening it into the fog is the answer; resolving it properly is.
//               `hazeOrdered` (default on) and `hazeAsParticle` (default off) A/B all three.
//
// **Known open question**: a haze surface on path 3 is a real surface, so `applyFog`'s far ramp
// now fogs it by its own view distance - on top of the fade the game already painted into it.
// That is the same double-count as moya particles versus volumetric density
// (`DusklightAtmosphere.md` §8.1) and it has NOT been measured. If the wall comes back looking
// flatter than intended, that is the first suspect.
//
// Design and the measurements behind it: aurora-ao/docs/dx9/remix-material-interface.md §11.

// D3DMATERIAL9 is a typedef of an anonymous-tag struct, so it cannot be forward declared.
// rtx_materials.h carries it through the same include chain the other side-channel readers use.
#include "rtx_option.h"
#include "rtx_materials.h"

#include <cstdint>

namespace dxvk {

  // Mirrors GX_AURORA_DRAW_CLASS_* in aurora's include/dolphin/gx/GXAurora.h. The two are a
  // single wire contract carried in D3DMATERIAL9::Ambient.a; changing either alone breaks it.
  enum class DusklightDrawClass : uint32_t {
    None = 0,
    Particle = 1,
    Haze = 2,
  };

  class DusklightTransparency {
  public:
    // Decodes what the game said this draw represents. Returns None for any value that is not
    // one aurora writes - a channel carrying something else must not be rounded into a class.
    static DusklightDrawClass drawClass(const D3DMATERIAL9& material);

    // Which of the game's draw lists issued this draw, from the high byte of the same channel.
    // Diagnostic only - nothing decides anything on it. It is the working replacement for the
    // `grp=` label, which never worked: debug groups are pushed where a draw is *scheduled*
    // rather than *issued*, and are compiled out in release besides.
    static uint32_t drawPhase(const D3DMATERIAL9& material);
    static const char* drawPhaseName(uint32_t phase);

    // Whether this draw should be resolved as a particle (unordered TLAS) rather than through
    // the stochastic alpha blend. False for every draw when the game is too old to send a
    // class, which is what makes this safe to default on: an unclassified draw reads 0 and
    // behaves exactly as it does today.
    static bool treatAsParticle(const D3DMATERIAL9& material);

    // Whether this draw should decline the stochastic alpha blend and resolve as a real
    // translucent surface. This is the accommodation for authored distant scenery - a fog wall
    // that is meant to look like *itself*, not like the generic haze everything else fades into.
    static bool treatAsOrderedTransparency(const D3DMATERIAL9& material);

    // Counts what was classified this frame, for the periodic report. Called from the one
    // decision site so the log describes the decision actually taken, not a re-derivation.
    static void recordClassified(DusklightDrawClass drawClass, bool promoted);

    // Records one alpha-blended draw the game did NOT classify, so the ones that matter can be
    // told apart from the ones that do not.
    //
    // This exists because a material report alone could not answer "which of these is the fog
    // wall in front of Death Mountain". The 2026-08-11 session logged 30 `blend=alpha
    // class=none` draws, and texture size, format and tfactor do not distinguish a distant
    // haze curtain from a small decal - the two things that would are how *big* it is and how
    // *far away*. So they are measured here.
    //
    // `spanDegrees` is the angular size of the draw's world-space bounding box from the camera,
    // the scale-free version of "how much of the view does this cover".
    //
    // Span alone does NOT identify the wall, and it is worth saying why rather than letting the
    // next reader rediscover it. Checked against realistic numbers: a fog wall 4000 units across
    // at 6000 units reads 37 degrees - but a mid-distance water surface reads 56, and a
    // full-screen blit reads 180. What separates them is span *together with* distance, so both
    // are reported and the report is explicit about reading them as a pair.
    //
    // `cameraInside` is true when the camera is within the draw's bounding box, which is where
    // an angular measure stops meaning anything. It is also exactly what a screen-space blit
    // looks like, so those rows sort last instead of monopolising the top of the report. That is
    // a property of the geometry rather than a distance threshold picked to get a nice answer.
    //
    // `texHash` is `tex0hash` from `matrep.rmx`, so a row here joins to the fork's material
    // report, which joins to the game's `matrep.sum` through the texture pointer.
    static void recordUnclassifiedAlpha(uint64_t texHash, float worldSize, float distance,
                                        float spanDegrees, bool cameraInside, uint32_t phase);

    // Emits `dusklight.xparency` once every reportPeriodFrames frames when reportClasses is on,
    // followed by the unclassified survey when surveyUnclassified is on. Bounded by
    // construction: one summary line plus at most kSurveyRows rows, whatever the scene does.
    static void reportFrame();

    static const char* drawClassName(DusklightDrawClass drawClass);

  private:
    // Emits the survey table. Split out only so reportFrame stays readable.
    static void reportUnclassifiedSurvey();

  public:

    RTX_OPTION("rtx.dusklight.transparency", bool, enable, true,
               "Read the game's per-draw transparency class from D3DMATERIAL9::Ambient.a and let it decide how a blended draw is resolved. "
               "Safe to leave on with an older game build: a draw that carries no class reads 0 and is resolved exactly as it is today.");

    RTX_OPTION("rtx.dusklight.transparency", bool, particleAsParticle, true,
               "Resolve draws the game marked as particles (smoke, dust, explosion puffs) through the unordered TLAS instead of the stochastic alpha blend. "
               "This is the fix for noisy enemy death smoke: the stochastic path keeps one randomly chosen layer per pixel per frame and lights it from a neighbouring opaque pixel, "
               "while the unordered path accumulates every layer and lights them from the volumetric radiance cache.");

    RTX_OPTION("rtx.dusklight.transparency", bool, hazeOrdered, true,
               "Resolve draws the game marked as haze as genuine translucent surfaces rather than through the stochastic alpha blend. "
               "This is the third transparency path and the one authored distant scenery wants: ordered rather than one random layer per pixel per frame, "
               "path-traced rather than lit by borrowing a neighbouring opaque pixel's radiance, and keeping its own colour rather than being tinted by whatever solid thing is near it on screen. "
               "It costs a resolve iteration per layer, which is why it is per-draw and not the global rtx.enableStochasticAlphaBlend switch. "
               "Turn it off to compare against the stochastic path.");

    RTX_OPTION("rtx.dusklight.transparency", bool, hazeAsParticle, false,
               "Also resolve draws the game marked as haze (distant layered fog walls) through the unordered TLAS. "
               "Off by default because haze sits far beyond the froxel grid, where the volumetric lookup saturates and the unordered path would light it as though it stood at the grid's edge - "
               "and where it would lose the game's fog ramp, which only the composite path applies. Turn it on to A/B that trade.");

    RTX_OPTION("rtx.dusklight.transparency", bool, reportClasses, false,
               "Log a per-frame count of transparent draws by class, so a play session says which draws the game is actually classifying without anyone having to describe a picture. "
               "One bounded line every rtx.dusklight.transparency.reportPeriodFrames frames.");

    RTX_OPTION("rtx.dusklight.transparency", int, reportPeriodFrames, 600,
               "How many frames between dusklight.xparency report lines. Matches the dx9.draws period so the two can be read side by side.");

    RTX_OPTION("rtx.dusklight.transparency", bool, surveyUnclassified, false,
               "Survey the alpha-blended draws the game did NOT classify, and report them largest-on-screen first. "
               "This is how a distant fog wall identifies itself: it is the transparency covering tens of degrees of view from hundreds of metres away, "
               "which no amount of texture size or format in the material report can distinguish. Each row carries the tex0hash that joins it to matrep.rmx and from there to the game's matrep.sum. "
               "Requires rtx.dusklight.transparency.reportClasses to be on, since it shares its reporting period.");

    // Deliberately small. The point is to name the handful of transparencies big enough to
    // matter, not to enumerate every blended draw in the scene - a report nobody reads to the
    // end is the same as no report.
    static constexpr size_t kSurveyRows = 12;
  };

}  // namespace dxvk
