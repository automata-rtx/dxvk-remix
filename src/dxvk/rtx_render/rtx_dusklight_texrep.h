/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
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

// Dusklight HD texture packs, Remix half.
//
// The game already supports Dolphin-format replacement packs, but on the D3D9
// backend their bytes must not travel through D3D9: D3D9 cannot carry BC7/BC5,
// and - more importantly - the D3D9 texture is what Remix hashes to build the
// texture categorization grid. Change those bytes and every tag, every
// rtx.conf category entry and every USD binding keyed on that hash moves with
// the pack.
//
// So the pack does not go through D3D9 at all. Aurora keeps uploading the
// original, unchanged, which makes tagging bit-identical to a run with no pack
// installed. Instead:
//
//   1. The game calls remixapi_CreateMaterial once per replacement, with the
//      .dds path and a handle of kHandleBase | index. That material is never
//      bound as a material - it is used purely as a file loader, because it is
//      the one tested path in this runtime from a path to a TextureRef.
//   2. Aurora ships the same index per draw in D3DMATERIAL9::Ambient.g.
//   3. Here, the loaded texture is swapped in at the two places a draw can
//      consume a texture - the ray-traced material and the rasterized bind.
//
// The two sites exist because Remix splits the frame before either runs:
// makeDrawCallType sends UI draws down the rasterized path, where no material
// of any kind is consulted and the D3D9 view is sampled directly. Substituting
// only in the material would leave the HUD - one of the two things that still
// has to rasterize correctly - at original resolution.
//
// Aurora's half, the index contract, and the pack rules that follow from it:
//   aurora-ao/docs/dx9/texture-replacements.md

#include "rtx_option.h"
#include "rtx_materials.h"
#include "rtx_texture.h"

#include <cstdint>

// D3DMATERIAL9 is a typedef of an anonymous-tag struct, so it cannot be forward declared.
// rtx_materials.h already carries it (LegacyMaterialData::d3dMaterial) through the same
// include chain, which is why it is not included directly here.

namespace dxvk {
  struct AssetReplacer;
  class DxvkContext;

  struct DusklightTexRep {
    RTX_OPTION("rtx.dusklight.texrep", bool, enable, true,
               "Use the HD texture replacements the game registered through the Remix API.\n"
               "The game pushes one material per replacement and tags each draw with its index; this swaps the loaded\n"
               "texture in for the game's own. Turning it off leaves the original D3D9 textures visible, which is\n"
               "exactly what a run with no pack installed looks like.");
    RTX_OPTION("rtx.dusklight.texrep", bool, applyToRaster, true,
               "Also substitute on rasterized (UI/HUD) draws, which Remix does not path-trace.\n"
               "This is the only way HD art reaches the HUD. Turn it off to isolate a HUD-only regression: the HUD\n"
               "reverts to the game's textures while the world keeps the pack.");
    RTX_OPTION("rtx.dusklight.texrep", bool, forceFullMips, true,
               "Hold substituted textures at full resolution.\n"
               "Rasterized draws generate no sampler feedback, so without this a HUD texture can sit at the low-mip\n"
               "tail the streamer loaded and look softer than the game's own.");
    RTX_OPTION("rtx.dusklight.texrep", bool, captureReplaced, true,
               "Write the substituted texture, rather than the game's own, into scene captures.\n"
               "The capture's material names and every hash keyed on them are unaffected either way - they come from the\n"
               "game's texture, which is still what Remix hashes. This only decides which pixels land in the .dds, and\n"
               "the substituted ones are what a normal, roughness or displacement map has to be derived from if it is\n"
               "going to line up with the albedo the game actually shows. Turn it off to capture the original art.");
    RTX_OPTION_FLAG("rtx.dusklight.texrep", bool, report, false, RtxOptionFlags::NoSave,
                    "Log a bounded texrep.* summary of what was substituted. Clears itself after one report.");
  };

  namespace dusklightTexRep {
    // Handle namespace shared with the game. The low 48 bits are aurora's 1-based replacement
    // index; index 0 means "no replacement" and never produces a handle (remixapi_CreateMaterial
    // rejects a handle of 0 anyway).
    constexpr uint64_t kHandleBase = 0xD05C000000000000ull;
    constexpr uint64_t kIndexMask = 0x0000FFFFFFFFFFFFull;

    // Reads the replacement index out of aurora's D3D9 material side channel (Ambient.g) and
    // returns the handle the game would have registered it under, or 0 for "no replacement".
    uint64_t handleFromLegacyMaterial(const D3DMATERIAL9& material);
    uint64_t handleFromLegacyMaterial(const LegacyMaterialData& material);

    // Same, for the rasterized path, which substitutes per texture bind rather than per draw.
    // Returns 0 unless `stateSampler` is the stage the index actually describes (Ambient.b):
    // only dirty textures rebind, so a multi-texture draw can rebind a stage this material's
    // index says nothing about, and substituting there would be the wrong texture.
    //
    // Consequence, and it is deliberate: on the rasterized path only the albedo stage is
    // substituted. A multi-texture UI draw keeps the game's texture on its other stages even
    // if the pack replaces them. That is a missed improvement, not a wrong pixel, and the
    // alternative needs a per-stage index the side channel has no room for. The ray-traced
    // path is unaffected - Remix takes its albedo from that same stage anyway.
    uint64_t handleForRasterStage(const D3DMATERIAL9& material, uint32_t stateSampler);

    // The substituted albedo for this handle, or nullptr when there is none, the option is off,
    // or the texture is not resident yet. Deliberately falls back to the game's texture while a
    // load is in flight rather than binding an empty slot, which reads as solid black.
    const TextureRef* resolveAlbedo(AssetReplacer* replacer, uint64_t handle, bool forRaster);

    // What a capture should write for a draw's albedo. Separate from resolveAlbedo above for two
    // reasons, and both were nearly missed:
    //
    //   - it does not touch the per-frame counters. A capture is not a frame of rendering, and
    //     folding it in would have texrep.rmx report substitutions that never reached a pixel.
    //   - it insists the *top* mip is resident. The streamer sizes a texture from what the renderer
    //     asked for, so a texture that looks right on screen can still be sitting several mips
    //     down; substituting that into a capture writes a quietly half-resolution "HD" texture,
    //     which is the kind of thing nobody notices until the normal maps are already baked off it.
    //
    // The outcome is returned rather than just the texture so the capture can report what it did.
    enum class CaptureAlbedo {
      NoReplacement,  // this draw carries no replacement index; the game's texture is correct
      Substituted,    // *textureOut is the replacement, at full resolution
      NotResident,    // tagged, but the top mip has not streamed in - fell back to the game's
      Missing,        // tagged, but the game never registered a material for that index
    };
    CaptureAlbedo resolveAlbedoForCapture(AssetReplacer* replacer,
                                          const LegacyMaterialData& legacy,
                                          const TextureRef** textureOut);

    // Ray-traced path: overwrite just the albedo on an already-converted legacy material.
    // Called after MaterialData::as<OpaqueMaterialData>() so the sampler override and the
    // ignore-alpha flag that conversion sets are preserved - a merge would drop both.
    void applyAlbedo(AssetReplacer* replacer, const LegacyMaterialData& legacy, MaterialData& renderMaterialData);

    // True while this draw's replacement could still change what its material resolves to.
    // The preserve path skips material re-evaluation for stable instances, and its identity
    // hash knows nothing about replacement residency, so without this a room drawn while its
    // textures were still streaming keeps the game's textures for its whole lifetime.
    // Goes false permanently once the handle settles - resident, or known to have no material -
    // so a pack entry the game never created does not disable preservation forever.
    bool awaitingReplacement(const LegacyMaterialData& legacy);

    // Counters for the overlay and the report. Cheap enough to keep always on.
    struct Stats {
      uint32_t handlesSeen = 0;
      uint32_t applied = 0;
      uint32_t appliedRaster = 0;
      uint32_t pending = 0;
      uint32_t missing = 0;
    };
    const Stats& stats();
    void onFrameEnd();
    void reportIfRequested();
  }
}
