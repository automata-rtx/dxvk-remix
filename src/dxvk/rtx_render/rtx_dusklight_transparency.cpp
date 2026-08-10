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
  }

  DusklightDrawClass DusklightTransparency::drawClass(const D3DMATERIAL9& material) {
    if (!enable()) {
      return DusklightDrawClass::None;
    }

    // Aurora writes a small non-negative integer. Anything else - a NaN, a negative, a value
    // past the enum - means the channel is carrying something other than this contract, and the
    // correct answer is "unclassified" rather than a truncation into a plausible class.
    const float raw = material.Ambient.a;

    if (!(raw >= 1.0f) || raw > 2.0f) {
      return DusklightDrawClass::None;
    }

    const float rounded = std::floor(raw + 0.5f);

    if (std::fabs(raw - rounded) > 0.001f) {
      return DusklightDrawClass::None;
    }

    return static_cast<DusklightDrawClass>(static_cast<uint32_t>(rounded));
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

    s_seenParticle.store(0, std::memory_order_relaxed);
    s_seenHaze.store(0, std::memory_order_relaxed);
    s_frames.store(0, std::memory_order_relaxed);
    s_lastTotal.store(0, std::memory_order_relaxed);
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
