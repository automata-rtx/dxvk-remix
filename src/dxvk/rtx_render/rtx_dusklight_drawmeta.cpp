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
#include "rtx_dusklight_drawmeta.h"

#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <algorithm>
#include <cstring>

namespace dxvk {

  namespace {
    // Not thread local. aurora drains the FIFO and issues its draws from one thread, and the
    // metadata is only meaningful in that stream's order - a per-thread copy would silently give
    // a second thread an empty block rather than failing where the mistake is.
    DusklightDrawMeta g_pending = { sizeof(DusklightDrawMeta), 0 };
    bool g_everSet = false;
  }

  namespace DusklightDrawMetaState {

    const DusklightDrawMeta& peek() {
      return g_pending;
    }

    bool everSet() {
      return g_everSet;
    }

    void set(const void* meta, uint32_t callerStructSize) {
      // A caller that cannot state its own size is a caller whose layout is unknown, and copying an
      // unknown layout is how a wire like this takes a process down rather than degrading.
      if (meta == nullptr || callerStructSize < sizeof(uint32_t)) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          Logger::warn("[Dusklight] draw metadata: rejected a block with no usable size. The caller must set "
                       "structSize to sizeof(DusklightDrawMeta) as it sees it. Reported once per run.");
        }
        return;
      }

      // Forward and backward in one expression. A newer aurora sends more than this build knows
      // about and the tail is ignored; an older one sends less and the rest stays zero, which every
      // field is defined to read as "as before".
      const uint32_t copyBytes = std::min(callerStructSize, static_cast<uint32_t>(sizeof(DusklightDrawMeta)));

      g_pending = DusklightDrawMeta { sizeof(DusklightDrawMeta), 0 };
      std::memcpy(&g_pending, meta, copyBytes);
      // Restored after the copy: the caller's value describes the caller's struct, and everything
      // downstream should see this build's.
      g_pending.structSize = sizeof(DusklightDrawMeta);

      if (!g_everSet) {
        g_everSet = true;
        Logger::info(str::format(
          "[Dusklight] draw metadata: first block received, caller struct ", callerStructSize,
          " bytes against this build's ", sizeof(DusklightDrawMeta),
          ". A per-draw channel that is not D3DMATERIAL9 is live; see rtx_dusklight_drawmeta.h."));
      }
    }

  }

}

// Declared with the export attribute in the header, defined without it here - the same shape as
// getRtxOptionValue.
extern "C" void dusklightSetDrawMeta(const void* meta, uint32_t structSize) {
  dxvk::DusklightDrawMetaState::set(meta, structSize);
}
