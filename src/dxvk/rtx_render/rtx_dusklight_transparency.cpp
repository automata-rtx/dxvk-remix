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

#include "rtx_dusklight_transparency.h"

#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxvk {

  namespace {
    // Counted per frame and reset by the report, so the line describes one frame rather than a
    // running total nobody can difference. Atomics because setAlphaState runs off the geometry
    // processing threads; relaxed because these are counters, not a synchronisation protocol.
    std::atomic<uint32_t> s_seenParticle { 0 };
    std::atomic<uint32_t> s_seenHaze { 0 };
    std::atomic<uint32_t> s_promoted { 0 };
    std::atomic<uint32_t> s_frames { 0 };
    // Frames in which at least one classified draw was seen. A scene with none is the signature
    // of a game build that predates GXSetDrawClass, and it is worth being able to tell that
    // apart from "the fix ran and did nothing".
    std::atomic<uint32_t> s_framesWithAny { 0 };
    // The classified-draw total as of the end of the previous frame. The counters above are
    // period totals, so "did anything happen this frame" is the total having *moved*, not the
    // total being non-zero - which would read as every frame after the first one.
    std::atomic<uint32_t> s_lastTotal { 0 };

    // The unclassified-alpha survey. Keyed by albedo texture hash, holding the *largest* view
    // this material was ever seen at over the period, because a fog wall glimpsed edge-on for
    // one frame and filling the screen the next is the same material and only the second
    // measurement identifies it.
    struct SurveyEntry {
      float maxSpanDegrees = 0.0f;
      float worldSizeAtMax = 0.0f;
      float distanceAtMax = 0.0f;
      uint32_t draws = 0;
      uint32_t phase = 0;
      // Sticky: seen enclosing the camera even once is enough to call it a screen-space or
      // enclosing draw, which is what this is for.
      bool cameraInside = false;
    };

    // A plain mutex rather than atomics: entries are multi-field and only touched by blended
    // draws, which are a small minority of a frame. Correctness over cleverness here.
    std::mutex s_surveyMutex;
    std::unordered_map<uint64_t, SurveyEntry> s_survey;
    // Distinct materials dropped because the table was full, so a truncated report says so
    // rather than quietly looking complete.
    uint32_t s_surveyDropped = 0;
    // Hard cap on distinct keys held, well above kSurveyRows so the sort still has something
    // to choose between, but bounded so a scene full of unique blended materials cannot grow
    // this without limit.
    constexpr size_t kSurveyCapacity = 256;
  }

  namespace {
    // Ambient.a carries `drawClass + drawPhase * 256`, both small integers, so the float is
    // exact. Returns false when the channel is not carrying that contract at all - a NaN, a
    // negative, or a non-integral value means something else is in there and the honest answer
    // is "nothing", not a truncation into a plausible class.
    bool decodePackedChannel(float raw, uint32_t& outPacked) {
      if (!(raw >= 0.0f) || raw > 65535.0f) {
        return false;
      }

      const float rounded = std::floor(raw + 0.5f);

      if (std::fabs(raw - rounded) > 0.001f) {
        return false;
      }

      outPacked = static_cast<uint32_t>(rounded);
      return true;
    }
  }

  DusklightDrawClass DusklightTransparency::drawClass(const D3DMATERIAL9& material) {
    if (!enable()) {
      return DusklightDrawClass::None;
    }

    uint32_t packed = 0;

    if (!decodePackedChannel(material.Ambient.a, packed)) {
      return DusklightDrawClass::None;
    }

    const uint32_t cls = packed & 0xFFu;

    // Reject a value past the enum rather than rounding it into a plausible class.
    if (cls != 1u && cls != 2u) {
      return DusklightDrawClass::None;
    }

    return static_cast<DusklightDrawClass>(cls);
  }

  uint32_t DusklightTransparency::drawPhase(const D3DMATERIAL9& material) {
    uint32_t packed = 0;

    if (!decodePackedChannel(material.Ambient.a, packed)) {
      return 0;
    }

    return (packed >> 8) & 0xFFu;
  }

  const char* DusklightTransparency::drawPhaseName(uint32_t phase) {
    // Mirrors GX_AURORA_DRAW_PHASE_* and draw_phase_name() in aurora's dx9_tev.cpp. Spelled out
    // so a log line is readable without either header.
    switch (phase) {
    case 1:  return "skyOpa";
    case 2:  return "skyXlu";
    case 3:  return "bgOpa";
    case 4:  return "bgXlu";
    case 5:  return "middle";
    case 6:  return "actorOpa";
    case 7:  return "actorXlu";
    case 8:  return "zxlu";
    case 9:  return "filter";
    case 10: return "invisible";
    case 11: return "screen";
    case 12: return "last3D";
    case 13: return "ui2D";
    default: return "none";
    }
  }

  bool DusklightTransparency::treatAsParticle(const D3DMATERIAL9& material) {
    const DusklightDrawClass cls = drawClass(material);

    bool promote = false;

    switch (cls) {
    case DusklightDrawClass::Particle:
      promote = particleAsParticle();
      break;
    case DusklightDrawClass::Haze:
      promote = hazeAsParticle();
      break;
    default:
      return false;
    }

    recordClassified(cls, promote);

    return promote;
  }

  void DusklightTransparency::recordClassified(DusklightDrawClass drawClass, bool promoted) {
    if (!reportClasses()) {
      return;
    }

    switch (drawClass) {
    case DusklightDrawClass::Particle:
      s_seenParticle.fetch_add(1, std::memory_order_relaxed);
      break;
    case DusklightDrawClass::Haze:
      s_seenHaze.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      return;
    }

    if (promoted) {
      s_promoted.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void DusklightTransparency::recordUnclassifiedAlpha(uint64_t texHash, float worldSize,
                                                      float distance, float spanDegrees,
                                                      bool cameraInside, uint32_t phase) {
    if (!surveyUnclassified() || !reportClasses()) {
      return;
    }

    // Reject anything that is not a real measurement rather than letting a NaN win the sort and
    // sit at the top of the report claiming to be the answer.
    if (!(spanDegrees >= 0.0f) || !(worldSize >= 0.0f) || !(distance >= 0.0f)) {
      return;
    }

    std::lock_guard lock { s_surveyMutex };

    auto it = s_survey.find(texHash);

    if (it == s_survey.end()) {
      if (s_survey.size() >= kSurveyCapacity) {
        ++s_surveyDropped;
        return;
      }
      it = s_survey.emplace(texHash, SurveyEntry {}).first;
    }

    ++it->second.draws;
    it->second.cameraInside = it->second.cameraInside || cameraInside;

    if (spanDegrees > it->second.maxSpanDegrees) {
      it->second.maxSpanDegrees = spanDegrees;
      it->second.worldSizeAtMax = worldSize;
      it->second.distanceAtMax = distance;
      it->second.phase = phase;
    }
  }

  void DusklightTransparency::reportFrame() {
    if (!reportClasses()) {
      return;
    }

    const uint32_t period = static_cast<uint32_t>(std::max(reportPeriodFrames(), 1));

    const uint32_t particle = s_seenParticle.load(std::memory_order_relaxed);
    const uint32_t haze = s_seenHaze.load(std::memory_order_relaxed);

    const uint32_t total = particle + haze;

    if (total != s_lastTotal.exchange(total, std::memory_order_relaxed)) {
      s_framesWithAny.fetch_add(1, std::memory_order_relaxed);
    }

    const uint32_t frames = s_frames.fetch_add(1, std::memory_order_relaxed) + 1;

    if (frames < period) {
      return;
    }

    const uint32_t promoted = s_promoted.exchange(0, std::memory_order_relaxed);
    const uint32_t withAny = s_framesWithAny.exchange(0, std::memory_order_relaxed);

    // One line, fixed fields, whatever the scene does. `classifiedFrames=0` over a period in
    // which transparencies were plainly on screen means the game never called GXSetDrawClass -
    // check the game build before looking at anything here.
    Logger::info(str::format(
      "dusklight.xparency frames=", period,
      " classifiedFrames=", withAny,
      " particleDraws=", particle,
      " hazeDraws=", haze,
      " promotedToUnordered=", promoted,
      " particleAsParticle=", particleAsParticle() ? 1 : 0,
      " hazeAsParticle=", hazeAsParticle() ? 1 : 0));

    reportUnclassifiedSurvey();

    s_seenParticle.store(0, std::memory_order_relaxed);
    s_seenHaze.store(0, std::memory_order_relaxed);
    s_frames.store(0, std::memory_order_relaxed);
    s_lastTotal.store(0, std::memory_order_relaxed);
  }

  void DusklightTransparency::reportUnclassifiedSurvey() {
    if (!surveyUnclassified()) {
      return;
    }

    std::vector<std::pair<uint64_t, SurveyEntry>> rows;
    uint32_t dropped = 0;

    {
      std::lock_guard lock { s_surveyMutex };
      rows.assign(s_survey.begin(), s_survey.end());
      dropped = s_surveyDropped;
      s_survey.clear();
      s_surveyDropped = 0;
    }

    if (rows.empty()) {
      // Said explicitly rather than left silent: "no unclassified transparencies this period"
      // and "the survey is not running" look identical otherwise, and they mean opposite things.
      Logger::info("dusklight.xparency.survey rows=0 - no unclassified alpha-blended draws this period");
      return;
    }

    // Widest first, but anything enclosing the camera sorts last regardless. Those are the
    // screen-space blits and any medium the camera stands inside; their angular size is 180 by
    // construction and would otherwise fill the top of the report with the one category that
    // cannot be a distant wall.
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      if (a.second.cameraInside != b.second.cameraInside) {
        return !a.second.cameraInside;
      }
      return a.second.maxSpanDegrees > b.second.maxSpanDegrees;
    });

    const size_t shown = std::min(rows.size(), kSurveyRows);

    Logger::info(str::format(
      "dusklight.xparency.survey rows=", shown, " of=", rows.size(),
      dropped != 0 ? str::format(" droppedAtCapacity=", dropped) : std::string(),
      " - unclassified alpha-blended draws, widest first."
      " Read spanDeg WITH distance: a distant wall is wide AND far (tens of degrees at hundreds+ units)."
      " camInside=1 means the camera is inside the draw - a screen blit or an enclosing volume, sorted last."
      " phase names the game draw list that issued it."));

    for (size_t i = 0; i < shown; ++i) {
      const SurveyEntry& e = rows[i].second;
      Logger::info(str::format(
        "dusklight.xparency.row ", i,
        " tex0hash=", std::hex, rows[i].first, std::dec,
        " spanDeg=", e.maxSpanDegrees,
        " worldSize=", e.worldSizeAtMax,
        " distance=", e.distanceAtMax,
        " phase=", drawPhaseName(e.phase),
        " camInside=", e.cameraInside ? 1 : 0,
        " draws=", e.draws));
    }

    if (rows.size() > shown) {
      Logger::info(str::format(
        "dusklight.xparency.trunc cap=", kSurveyRows, " omitted=", rows.size() - shown,
        " - smaller than the rows above, raise rtx.dusklight.transparency reporting if needed"));
    }
  }

  const char* DusklightTransparency::drawClassName(DusklightDrawClass drawClass) {
    switch (drawClass) {
    case DusklightDrawClass::Particle:
      return "particle";
    case DusklightDrawClass::Haze:
      return "haze";
    default:
      return "none";
    }
  }

}  // namespace dxvk
