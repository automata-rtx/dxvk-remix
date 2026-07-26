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
  // docs/kankyo-remix.md). The game pushes these through the Remix API every frame it is
  // running under Remix with the bridge enabled; they describe what the game's environment
  // system computed, not how strongly Remix should respond to it. Response knobs live with
  // the passes that consume them (e.g. rtx.bloom.dusklightThresholdScale). All options here
  // are NoSave so a settings save never bakes one moment's weather into a config file.
  struct DusklightEnv {
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, enable, false, RtxOptionFlags::NoSave,
                    "True while the game's kankyo bridge is pushing environment state into Remix.\n"
                    "Set by the game itself; do not set by hand. Consumers only honour the other rtx.dusklight.env options while this is true.");
    RTX_OPTION_FLAG("rtx.dusklight.env", bool, bloomEnable, true, RtxOptionFlags::NoSave,
                    "Whether the game's current environment palette wants bloom at all. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomThreshold, 0.5f, RtxOptionFlags::NoSave,
                    "The game's bloom threshold (its 0..255 'point' value normalized to 0..1). Written by the game's kankyo bridge.\n"
                    "Consumed by the Dusklight bloom pass scaled by rtx.bloom.dusklightThresholdScale.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomBlurSize, 64.0f, RtxOptionFlags::NoSave,
                    "The game's bloom blur size in its native 0..255 range. Written by the game's kankyo bridge.");
    RTX_OPTION_FLAG("rtx.dusklight.env", float, bloomBlurRatio, 128.0f, RtxOptionFlags::NoSave,
                    "The game's bloom brightness in its native 0..255 range. Written by the game's kankyo bridge.");
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
  };

}
