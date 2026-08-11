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

// Dusklight texture-category report.
//
// Exists because of a specific failure on 2026-08-09. The owner reported that
// an effect was rendering as a UI element. A hash was found sitting in
// rtx.worldSpaceUiTextures, the symptom matched what that category does, and
// the two were connected - wrongly. There was no way to check, because nothing
// anywhere records WHICH texture actually received a category.
//
// A texture category is the one piece of Remix state that is invisible from
// both sides: the game cannot see it (it is keyed on a hash Remix computes
// from the D3D9 texture, which aurora never sees) and the log never mentions
// it. So the only way to answer "what is being treated as UI" was to read
// rtx.conf and guess which entry mattered.
//
// This records, per texture hash, exactly which categories it received and how
// many draws carried it. It answers three questions that previously required
// guessing:
//
//   - which textures are tagged at all, as opposed to which hashes sit in the
//     config (a config entry for a texture the game never draws is invisible);
//   - whether one hash carries contradictory categories;
//   - how much of a frame each tag actually affects, via the draw count.
//
// It does NOT identify what a texture depicts. That still needs Remix's
// texture categorization screen. What it removes is the step where someone
// infers which hash is responsible from the shape of a symptom.

#include "rtx_option.h"
#include "rtx_types.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dxvk {

  struct DusklightCatrep {
    RTX_OPTION_FLAG("rtx.dusklight", int, catrepCommit, 0, RtxOptionFlags::NoSave,
                    "Dump the texture-category report to the log.\n"
                    "Records, for every texture hash the runtime has categorized this run, which categories it received "
                    "and how many draws carried it. Use it to find out what is actually being treated as UI, particle, "
                    "decal or ignored, rather than reading rtx.conf and inferring.\n"
                    "An action, not a value: NoSave, and it fires when the number CHANGES.");

    // Bounded like every other Dusklight log. A frame of this game runs a few
    // hundred distinct textures; 4096 is well clear of that and still a fixed
    // ~200 KB, so a long session cannot grow it without limit.
    static constexpr size_t kMaxTextures = 4096;
  };

  namespace catrep {

    // Spelled out so a reader without the source can follow the report, per the
    // logging rule. Order matches InstanceCategories in rtx_types.h.
    inline const char* categoryName(InstanceCategories c) {
      switch (c) {
      case InstanceCategories::WorldUI:                return "WorldUI";
      case InstanceCategories::WorldMatte:             return "WorldMatte";
      case InstanceCategories::Sky:                    return "Sky";
      case InstanceCategories::Ignore:                 return "Ignore";
      case InstanceCategories::IgnoreLights:           return "IgnoreLights";
      case InstanceCategories::IgnoreAntiCulling:      return "IgnoreAntiCulling";
      case InstanceCategories::IgnoreMotionBlur:       return "IgnoreMotionBlur";
      case InstanceCategories::IgnoreOpacityMicromap:  return "IgnoreOpacityMicromap";
      case InstanceCategories::IgnoreAlphaChannel:     return "IgnoreAlphaChannel";
      case InstanceCategories::Hidden:                 return "Hidden";
      case InstanceCategories::Particle:               return "Particle";
      case InstanceCategories::Beam:                   return "Beam";
      case InstanceCategories::DecalStatic:            return "DecalStatic";
      case InstanceCategories::DecalDynamic:           return "DecalDynamic";
      case InstanceCategories::DecalSingleOffset:      return "DecalSingleOffset";
      case InstanceCategories::DecalNoOffset:          return "DecalNoOffset";
      case InstanceCategories::AlphaBlendToCutout:     return "AlphaBlendToCutout";
      case InstanceCategories::Terrain:                return "Terrain";
      case InstanceCategories::AnimatedWater:          return "AnimatedWater";
      case InstanceCategories::ThirdPersonPlayerModel: return "ThirdPersonPlayerModel";
      case InstanceCategories::ThirdPersonPlayerBody:  return "ThirdPersonPlayerBody";
      case InstanceCategories::IgnoreBakedLighting:    return "IgnoreBakedLighting";
      case InstanceCategories::IgnoreTransparencyLayer:return "IgnoreTransparencyLayer";
      case InstanceCategories::ParticleEmitter:        return "ParticleEmitter";
      case InstanceCategories::SmoothNormals:          return "SmoothNormals";
      default:                                         return "?";
      }
    }

    struct Entry {
      uint32_t mask = 0;        // bit per InstanceCategories, OR'd over every sighting
      uint32_t stableMask = 0;  // categories present on EVERY sighting
      uint64_t draws = 0;
      bool first = true;
    };

    inline std::unordered_map<XXH64_hash_t, Entry>& table() {
      static std::unordered_map<XXH64_hash_t, Entry> s_table;
      return s_table;
    }

    // note() runs on the application thread, from D3D9Rtx::processTextures via
    // PrepareDrawGeometryForRT; emitReport() runs on the DXVK CS thread from
    // SceneManager::onFrameEnd. Different threads, so the map needs a lock -
    // without one, a rehash on the writer while the reader iterates is a crash,
    // not a wrong number.
    //
    // Cost is an uncontended lock plus a hash probe per textured draw. The
    // reader takes it once per report, so contention is effectively zero.
    inline std::mutex& tableMutex() {
      static std::mutex s_mutex;
      return s_mutex;
    }

    inline bool& overflowed() {
      static bool s_overflowed = false;
      return s_overflowed;
    }

    // Called from DrawCallState::setupCategoriesForTexture, after the category
    // flags for this draw have been resolved. Kept to a hash-map probe and two
    // integer ops so it can sit on a per-draw path.
    inline void note(XXH64_hash_t textureHash, uint32_t mask) {
      std::lock_guard<std::mutex> guard(tableMutex());
      auto& t = table();
      auto it = t.find(textureHash);
      if (it == t.end()) {
        if (t.size() >= DusklightCatrep::kMaxTextures) {
          overflowed() = true;
          return;
        }
        Entry e;
        e.mask = mask;
        e.stableMask = mask;
        e.draws = 1;
        e.first = false;
        t.emplace(textureHash, e);
        return;
      }
      // Both accumulated: mask says "ever had this category", stableMask says
      // "always had it". They differ when a texture is categorized
      // inconsistently between draws, which is a real thing worth seeing and
      // is invisible if only one of the two is kept.
      it->second.mask |= mask;
      it->second.stableMask &= mask;
      it->second.draws++;
    }

    inline std::string describeMask(uint32_t mask, uint32_t stableMask) {
      if (mask == 0) {
        return "-";
      }
      std::string out;
      for (uint32_t i = 0; i < static_cast<uint32_t>(InstanceCategories::Count); ++i) {
        if ((mask & (1u << i)) == 0) {
          continue;
        }
        if (!out.empty()) {
          out += ",";
        }
        out += categoryName(static_cast<InstanceCategories>(i));
        // A category the texture only sometimes had. Marked rather than hidden:
        // an intermittent category is exactly the shape of a bug that presents
        // as "it only happens sometimes".
        if ((stableMask & (1u << i)) == 0) {
          out += "?";
        }
      }
      return out;
    }

    inline void emitReport() {
      // Copied under the lock rather than formatted under it: emitReport does
      // hundreds of Logger::info calls, and holding a lock the draw path wants
      // for that long would stall the application thread for the whole dump.
      std::unordered_map<XXH64_hash_t, Entry> t;
      bool capped = false;
      {
        std::lock_guard<std::mutex> guard(tableMutex());
        t = table();
        capped = overflowed();
      }

      Logger::info("dusklight.catrep ---- texture category report ----");
      Logger::info(str::format(
        "dusklight.catrep ", t.size(), " distinct textures categorized so far",
        capped ? str::format(" (CAPPED at ", DusklightCatrep::kMaxTextures,
                             " - later textures were not recorded)")
               : std::string()));
      Logger::info("dusklight.catrep a category marked ? was present on some draws of that texture and not others.");
      Logger::info("dusklight.catrep textures with NO category are the normal case and are counted, not listed.");

      // Only the tagged ones are worth printing: an untagged texture is the
      // default and listing hundreds of them would bury the handful that are
      // not. The count above still reports the whole population so the reader
      // can see what fraction is tagged.
      std::vector<std::pair<XXH64_hash_t, Entry>> tagged;
      tagged.reserve(64);
      uint64_t untagged = 0;
      for (const auto& kv : t) {
        if (kv.second.mask == 0) {
          untagged++;
        } else {
          tagged.push_back(kv);
        }
      }

      // Busiest first - a tag on a texture drawn 40,000 times matters more than
      // one drawn twice, and that ordering is what makes the list scannable.
      std::sort(tagged.begin(), tagged.end(),
                [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });

      Logger::info(str::format("dusklight.catrep ", tagged.size(), " tagged, ", untagged, " untagged"));

      if (tagged.empty()) {
        Logger::info("dusklight.catrep no texture drawn this run carried any category.");
        Logger::info("dusklight.catrep NOTE: a hash listed in rtx.conf for a texture the game has not drawn "
                     "will not appear here - the config is not the same list as this one.");
      }

      for (const auto& kv : tagged) {
        Logger::info(str::format(
          "dusklight.catrep tex=", std::hex, kv.first, std::dec,
          " draws=", kv.second.draws,
          " categories=", describeMask(kv.second.mask, kv.second.stableMask)));
      }

      Logger::info("dusklight.catrep ---- end of report ----");
    }

    // Fires on CHANGE, and latches the first value seen without acting - the
    // standard Dusklight action-counter shape. Without the latch, attaching to
    // a Remix that outlived a game restart dumps a report nobody asked for.
    inline void pollCommit() {
      static int s_last = 0;
      static bool s_latched = false;

      const int now = DusklightCatrep::catrepCommit();
      if (!s_latched) {
        s_last = now;
        s_latched = true;
        return;
      }
      if (now != s_last) {
        s_last = now;
        emitReport();
      }
    }

  }

}
