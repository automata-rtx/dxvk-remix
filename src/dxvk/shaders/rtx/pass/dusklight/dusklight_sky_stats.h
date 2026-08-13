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

#ifndef DUSKLIGHT_SKY_STATS_H
#define DUSKLIGHT_SKY_STATS_H

#include "rtx/utility/shader_types.h"

// Reduces the generated sky dome to one number the host can read: the mean radiance over the whole
// sphere, weighted by solid angle.
//
// It exists because of a gap that is easy to miss. Remix's froxel grid is lit by next event
// estimation over the RTXDI light list, and that list has five types - sphere, rect, disk, cylinder,
// distant. There is no dome light in it. So the volumetric fog is lit by the sun and by analytic
// lights and by nothing else: outdoors, fog standing in a shadow receives nothing at all from the
// sky, indoors it receives nothing from anything but the effect lights. Everything the fog showed in
// shadow used to come from a flat constant.
//
// A sphere average is the right stand in rather than a compromise. The term it feeds is an isotropic
// in-scatter estimate, so what it wants is the froxel's irradiance from the environment, not the
// radiance in some direction - and that is exactly this integral. The far half of the fog samples
// the dome in the view direction instead, which is strictly better where it can be done; the two
// agree on average, which is the kind of agreement that matters here.
//
// Read back through a host visible ring some frames later, never waited on. The sky changes over
// seconds, so a few frames of lag is invisible, and the alternative - stalling the pipeline once a
// frame to read four floats - is not worth considering.

#define DUSKLIGHT_SKY_STATS_INPUT   0
#define DUSKLIGHT_SKY_STATS_OUTPUT  1

// One workgroup, this wide. Each thread walks a strided slice of the 256x128 dome and the group
// reduces in shared memory, so the whole pass is a single dispatch of a single group.
#define DUSKLIGHT_SKY_STATS_THREADS 256

struct DusklightSkyStats {
  // Solid angle weighted mean radiance over the sphere, with the generator's intensity already
  // applied - it is whatever the dome light actually holds, not the palette colour behind it.
  vec3 ambient;
  // 0 until the reduction has run at least once. The host keeps the flat palette colour until this
  // turns 1, so a build that never dispatches this pass behaves exactly as it did before.
  uint valid;
};

#ifdef __cplusplus
// The host memcpys this out of a mapped buffer, so the two sides have to agree byte for byte. std430
// puts the uint straight after the vec3 and rounds the struct to the vec3's own 16 byte alignment,
// which is what the host layout gives as well - but only while the members stay in this order.
static_assert(sizeof(DusklightSkyStats) == 16);
#endif

#endif  // DUSKLIGHT_SKY_STATS_H
