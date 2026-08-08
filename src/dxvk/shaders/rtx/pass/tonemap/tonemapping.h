/*
* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
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
#ifndef TONEMAPPING_H
#define TONEMAPPING_H

#include "rtx/utility/shader_types.h"

#define AUTO_EXPOSURE_HISTOGRAM_INPUT_OUTPUT              0
#define AUTO_EXPOSURE_EXPOSURE_INPUT_OUTPUT               1
#define AUTO_EXPOSURE_COLOR_INPUT                         2
#define AUTO_EXPOSURE_DEBUG_VIEW_OUTPUT                   3
#define AUTO_EXPOSURE_DEBUG_STATS_OUTPUT                  4

#define TONEMAPPING_HISTOGRAM_COLOR_INPUT                 0
#define TONEMAPPING_HISTOGRAM_HISTOGRAM_INPUT_OUTPUT      1
#define TONEMAPPING_HISTOGRAM_EXPOSURE_INPUT              2

#define TONEMAPPING_TONE_CURVE_HISTOGRAM_INPUT_OUTPUT     0
#define TONEMAPPING_TONE_CURVE_TONE_CURVE_INPUT_OUTPUT    1

#define TONEMAPPING_APPLY_TONEMAPPING_COLOR_INPUT          0
#define TONEMAPPING_APPLY_TONEMAPPING_TONE_CURVE_INPUT     1
#define TONEMAPPING_APPLY_TONEMAPPING_EXPOSURE_INPUT       2
#define TONEMAPPING_APPLY_TONEMAPPING_COLOR_OUTPUT         3

#define TONEMAPPING_TONE_CURVE_SAMPLE_COUNT               256

#define EXPOSURE_HISTOGRAM_SIZE                           256

// Fixed log2-luminance (EV100) domain of the auto exposure histogram.
//
// This used to be driven by rtx.autoExposure.evMinValue/evMaxValue, whose defaults gave a 7 EV
// window - narrower than most outdoor scenes, so anything past either end piled up in the end
// bins and dragged the average with it. The domain is now fixed and wide enough to cover
// starlight through direct sun, and limiting is done by the soft limiter instead.
//
// Bin 0 is reserved for pixels below the floor (including true black) and is excluded from the
// metered average entirely. Bins 1..EXPOSURE_HISTOGRAM_SIZE-1 span the range above.
#define EXPOSURE_HISTOGRAM_MIN_EV100                      (-12.0f)
#define EXPOSURE_HISTOGRAM_MAX_EV100                      (14.0f)

// Per-pixel metering weights are accumulated as fixed point so that a weighted count can go
// through InterlockedAdd on a uint histogram. At 8K a single bin tops out around 2.1e9, which
// still fits a uint32.
#define EXPOSURE_HISTOGRAM_WEIGHT_SCALE                   64

// Constants

static const uint32_t ditherModeNone = 0;
static const uint32_t ditherModeSpatialOnly = 1;
static const uint32_t ditherModeSpatialTemporal = 2;

// Final tone mapping operator selection. Mirrored by dxvk::TonemapOperator in rtx_agx.h, which
// static_asserts against these.
static const uint32_t tonemapOperatorNone = 0;
static const uint32_t tonemapOperatorACES = 1;
static const uint32_t tonemapOperatorAgX = 2;
static const uint32_t tonemapOperatorGT7 = 3;

// AgX defaults, from Blender/Filament via three.js:
//   LOG2_MIN = -10, LOG2_MAX = +6.5, MIDDLE_GRAY = 0.18
//   minEv = log2(2^LOG2_MIN * 0.18), maxEv = log2(2^LOG2_MAX * 0.18)
#define AGX_DEFAULT_MIN_EV                                (-12.47393f)
#define AGX_DEFAULT_MAX_EV                                (4.026069f)
// log2(0.18). The pivot the contrast control scales the EV range around.
#define AGX_MIDDLE_GRAY_LOG2                              (-2.473931f)

// Constant buffers

struct ToneMappingAutoExposureArgs {
  uint numPixels;
  float deltaTimeSeconds;
  float evMinValue;               // Histogram floor, EV100. See EXPOSURE_HISTOGRAM_MIN_EV100.
  float evRange;                  // Histogram span, EV100.

  uint debugMode;
  uint enableCenterMetering;
  float centerMeteringSize;
  uint resetState;                // Snap instead of easing this frame (first frame, resolution change, camera cut).

  float lowPercentile;            // Fraction of the CDF trimmed off the dark end.
  float highPercentile;           // Upper edge of the CDF window. Always > lowPercentile.
  float keyValue;                 // Target mid grey.
  float adaptationStrength;       // 0 = auto exposure does nothing, 1 = every scene normalises alike.

  float tauBrighten;              // Seconds. Applies when the image has to brighten (slow direction).
  float tauDarken;                // Seconds. Applies when the image has to darken (fast direction).
  float softLimitCenterEV;
  float softLimitRangeEV;

  float deadbandEV;               // Below this, hold rather than move. Kills histogram-noise hunting.
  float cutSnapThresholdEV;       // Above this single-frame jump, snap. 0 disables.
  uint writeDebugStats;
  uint pad0;
};

// Read back to the CPU for the auto exposure debug readout. Written by thread 0 of the
// reduction pass only when requested, so the copy stays off the hot path when the UI is closed.
struct AutoExposureDebugStats {
  float currentEV;                // Post-adaptation metered scene EV100 currently in force.
  float targetEV;                 // Where adaptation is heading, after strength blend and soft limit.
  float sceneEV;                  // Raw trimmed metering result, before the strength blend.
  float trimmedFraction;          // Share of the histogram that landed inside the percentile window.

  float loPercentileEV;           // EV100 of the first bin inside the window.
  float hiPercentileEV;           // EV100 of the last bin inside the window.
  float exposure;                 // The linear multiplier actually written to the exposure texture.
  uint valid;
};

// AgX look transform and dynamic range, shared by the global and local tone mapping paths.
struct AgxArgs {
  vec3 lookSlope;
  float lookOffset;

  float lookPower;
  float lookSaturation;
  float minEv;                    // Log2 encode floor. Narrowing the range raises contrast.
  float maxEv;
};

// GT7 operator parameters. Everything here is derived on the CPU by a direct transcription of the
// reference's initializeAsSDR()/initializeCurve() - see rtx_gt7.cpp - so the shader carries no
// setup maths of its own.
//
// Only the values that actually vary are passed. blendRatio (0.6), fadeStart (0.98) and fadeEnd
// (1.16) are fixed literals in the reference's initializeParameters() and live as compile time
// constants in gt7.slangh; promoting them to options would need this block moved out of push
// constants, which are capped at 128 bytes and are already exactly full.
struct Gt7Args {
  float peakIntensity;    // framebufferLuminanceTarget_, in GT frame buffer units
  float kA;               // shoulder constants, precomputed exactly as initializeCurve() does
  float kB;
  float kC;

  float targetUcs;        // framebufferLuminanceTargetUcs_
  float inputScale;       // scene referred (mid grey at keyValue) -> GT frame buffer units
  float outputScale;      // sdrCorrectionFactor_; 1.0 in HDR mode
  float saturationBoost;  // 1.0 = untouched reference behaviour. See gt7SaturationBoost.
};

struct ToneMappingHistogramArgs {
  float toneCurveMinStops;
  float toneCurveMaxStops;
  uint enableAutoExposure;
  float exposureFactor;
};

struct ToneMappingCurveArgs {
  // Range [0, inf). Without further adjustments, the tone curve will try to fit the entire luminance of the scene into the range
  // [-dynamicRange, 0] in linear photographic stops. Higher values adjust for ambient monitor lighting; perfect conditions -> 17.587 stops.
  float dynamicRange;
  float shadowMinSlope;       // Range [0, inf). Forces the tone curve below a linear value of 0.18 to have at least this slope, making the tone darker.
  float shadowContrast;       // Range [0, inf). Additional gamma power to apply to the tone of the tone curve below shadowContrastEnd
  float shadowContrastEnd;    // Range (-inf, 0]. High endpoint for the shadow contrast effect in linear stops; values above this are unaffected

  float maxExposureIncrease;  // Range [0, inf). Forces the tone curve to not increase luminance values at any point more than this value
  float curveShift;           // Range [0, inf). Amount by which to shift the tone curve up or down. Nonzero values will cause additional clipping!
  uint needsReset;            // Invalidates tone curve history
  float toneCurveMinStops;
  
  float toneCurveMaxStops;
  uint pad0;
  uint pad1;
  uint pad2;
};

struct ToneMappingApplyToneMappingArgs {
  uint toneMappingEnabled;
  uint debugMode; // If true shows from left to right: Reinhard (0-0.25), Heji Burgess-Dawson (0.25-0.5), and dynamic tone mappers (0.5-1) along with a tone curve on the same screen.
  uint enableAutoExposure;
  uint colorGradingEnabled;

  float shadowContrast;       // See ToneMappingCurveArgs
  float shadowContrastEnd;    // See ToneMappingCurveArgs
  float exposureFactor;
  float contrast;

  // Color grading
  vec3 colorBalance;
  float saturation;

  float toneCurveMinStops;
  float toneCurveMaxStops;
  uint tonemapOperator;   // tonemapOperatorNone / ACES / AgX / GT7
  uint useLegacyACES;

  AgxArgs agx;
  Gt7Args gt7;
};


#endif  // TONEMAPPING_H