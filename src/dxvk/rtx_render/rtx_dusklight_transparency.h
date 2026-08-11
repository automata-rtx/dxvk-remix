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
// What the two renderers actually are, because the choice between them is the whole point:
//
//   Untagged (today's default). Primary TLAS, non-opaque. The resolve loop takes a *stochastic*
//   decision per pixel per frame - one layer survives, the rest are skipped - and the composite
//   then reconstructs that layer's lighting by hunting for a neighbouring opaque pixel and
//   borrowing its denoised radiance (`composite_alpha_blend.comp.slang`). For a dense stack of
//   smoke quads that is one random layer a frame lit by whatever solid thing happens to be near
//   it on screen, which is both the noise and the "transparency looks wrong".
//
//   Tagged a particle. The separate unordered TLAS, where every layer is accumulated in one
//   order-independent traversal - no stochastic pick, no borrowed lighting - and lit from the
//   volumetric radiance cache, which is genuinely where the light at that point in the air is.
//
// The unordered path is plainly better for smoke, and the reason it is not simply better for
// everything is distance. The froxel grid this game runs is capped at
// `rtx.dusklight.atmosphere.froxelMaxDistanceMaxMeters` (120 m), and the froxel lookup
// *saturates* past its last slice rather than failing, so a surface beyond the grid is lit as
// though it stood at the grid's edge. Everything past that boundary is instead carried by the
// game's own fog ramp in the composite (`DusklightAtmosphere.md` §5.2), which reaches as far as
// you like - and which the alpha-blend layer now gets too.
//
// So the split is by distance, and the handover is the same 120 m:
//
//   PARTICLE - near by nature (an enemy dies next to you). Inside the grid, so the unordered
//              path's lighting is real. Promoted.
//   HAZE     - a layered wall standing in for distance, hundreds of metres out. Outside the
//              grid, so promoting it would trade a stochastic pick for a saturated froxel
//              lookup and lose the far fog ramp that the composite path applies. Left on the
//              composite path by default; `hazeAsParticle` exists to A/B that in one session.
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
