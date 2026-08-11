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

#ifndef DUSKLIGHT_SKY_H
#define DUSKLIGHT_SKY_H

#include "rtx/pass/dusklight/dusklight_atmosphere.h"

// The generated sky dome.
//
// This image is a lat-long map consumed by Remix's existing dome light sampling, so its layout is
// fixed by cartesianDirectionToLatLongSphere: theta = acos(dir.z) down the V axis, phi =
// atan2(dir.x, dir.y) across U with 0.5 at phi = 0. The polar axis is Z in light space; the world
// is Y up, and the dome light's worldToLight transform carries that swap, so this shader works
// purely in light space and treats +Z as up.
//
// It is also, when the physical model is running, exactly what Hillaire calls the sky-view lookup
// table - which is why there is no separate one. The same image feeds the visible sky, the light
// the sky casts into the scene, and the colour distant geometry fades towards. Three consumers,
// one evaluation, and no way for them to disagree.
//
// Push constants are DusklightAtmosphereArgs, shared with the two lookup table passes.

#define DUSKLIGHT_SKY_OUTPUT         0
#define DUSKLIGHT_SKY_TRANSMITTANCE  1
#define DUSKLIGHT_SKY_MULTISCATTER   2

#endif  // DUSKLIGHT_SKY_H
