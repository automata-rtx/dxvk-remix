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

#include <cstdint>

// Per-draw metadata, handed from aurora straight to this runtime.
//
// WHY THIS EXISTS, AND WHY IT IS NOT ANOTHER D3DMATERIAL9 CHANNEL.
//
// Everything the game wants to say about a draw has so far been squeezed into spare fields of
// D3DMATERIAL9 - the emissive facts into Specular.a/.b and Emissive.rgb, HD texture packs into
// Ambient.g/.b, all three water facts packed into the single Power float. That worked because
// D3DMATERIAL9 survives the D3D9 capture path unchanged, and it has now run out: one field is
// left and an unmerged branch already claims it.
//
// The scarcity was never the real constraint, and treating it as one was the mistake. D3DMATERIAL9
// is fixed by the D3D9 API, but nothing about *this* boundary is: aurora is the D3D9 caller and
// this runtime is the D3D9 implementation, and both are ours. aurora-ao's
// docs/dx9/remix-material-interface.md section 9 has specified the alternative all along - "a small
// versioned export from the fork's d3d9.dll, called per draw" - and calls the packing "a fallback
// working hard, not a stated intent". This is that export.
//
// The precedent for the mechanism is already shipping in the other direction: the game resolves
// getRtxOptionValue with GetProcAddress every launch and warns if it is missing. That is what makes
// this safe to add to one side at a time - an aurora that cannot find the export skips it and every
// draw behaves exactly as it did before, and a runtime that is never called sees no metadata and
// does the same. There is no lockstep rebuild in it.
//
// GROWING IT. The struct is versioned by its own size, not by a version number that someone has to
// remember to bump: the caller writes structSize, the callee copies min(structSize, sizeof(its own))
// and zero-fills the rest. Add fields at the END only, give every field a zero value that means
// "as before", and old and new pair in both directions with no branch anywhere.
struct DusklightDrawMeta {
  // sizeof(DusklightDrawMeta) as the CALLER sees it. The first field so that it can be read before
  // anything else is trusted.
  uint32_t structSize;

  // What this draw is, as a set of bits rather than an enum, because a draw can be several things
  // at once and the next feature should not have to renegotiate the field.
  uint32_t flags;

  // A dropped pickup - a rupee, a heart, an arrow bundle. They are self-lit in GX and so are
  // accepted by the emissive rule (rtx_dusklight_emissive.h), which is correct as far as it goes:
  // the console does draw them full-bright. What the rule cannot see is that full-bright means
  // "do not shade me", not "light the room", and a rupee is the case where those two come apart.
  static constexpr uint32_t kFlagPickup = 1u << 0;
};

namespace dxvk {

  // The runtime's side of the export. One pending block, consumed by the next draw.
  //
  // ORDERING IS THE WHOLE TRICK, and aurora learned it the expensive way on its own water mark:
  // the GX FIFO is drained in end_frame, so a value the *game* writes at the moment it draws is
  // read long after every draw in the frame has gone past. The mark therefore has to be written
  // from the command processor as the FIFO is drained, at the point in the stream the game meant.
  // aurora's lib/dx9/dx9.hpp says so at length above set_dusklight_water, and that comment cost two
  // test sessions. The same rule applies here, one layer further along: aurora calls this export
  // while draining, immediately before the draw the metadata belongs to.
  namespace DusklightDrawMetaState {
    // What the next draw should be built with, or a zeroed block if nothing was set. Reading it
    // does not clear it - aurora clears by sending a zeroed block, exactly as it does for water,
    // because "leave it latched" and "clear it after every draw" are both wrong and both were tried.
    const DusklightDrawMeta& peek();

    // Called by the export. Copies at most sizeof(DusklightDrawMeta) and zero-fills any field the
    // caller's older struct did not carry.
    void set(const void* meta, uint32_t callerStructSize);

    // True once anything has ever been set, so a build that is paired with an aurora too old to
    // call the export can say so once rather than reporting "no pickups" forever.
    bool everSet();
  }

}

// The export. Declared here with the attribute and defined without it, matching getRtxOptionValue
// (rtx_option_manager.h) - the one this fork already ships in the other direction, and the reason
// this whole mechanism is known to degrade cleanly rather than needing both sides rebuilt together.
//
// void* and an explicit size rather than the struct type, because the caller is a different binary
// compiled from its own copy of this header and the size is what reconciles the two.
extern "C" __declspec(dllexport) void dusklightSetDrawMeta(const void* meta, uint32_t structSize);
