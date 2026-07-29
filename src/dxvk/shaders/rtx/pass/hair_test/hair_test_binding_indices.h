/*
* Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
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

#include "rtx/pass/common_binding_indices.h"
#include "rtx/utility/shader_types.h"

// Geometry representation the hair test pass is tracing this frame.
// Matches the RTXCR SDK's tessellation modes for strand curves.
#define HAIR_TEST_GEOMETRY_MODE_LSS  0
#define HAIR_TEST_GEOMETRY_MODE_DOTS 1

// Hair BCSDF models from the RTXCR material library.
#define HAIR_TEST_BSDF_MODEL_CHIANG    0
#define HAIR_TEST_BSDF_MODEL_FAR_FIELD 1

// Debug visualization modes for the hair test tab.
#define HAIR_TEST_DEBUG_MODE_LIT      0
#define HAIR_TEST_DEBUG_MODE_NORMALS  1
#define HAIR_TEST_DEBUG_MODE_TANGENTS 2
#define HAIR_TEST_DEBUG_MODE_STRAND_U 3

struct HairTestConstants {
  vec3 spherePosition;
  float sphereRadius;

  // World-space movement of the test sphere since the previous frame, used to
  // produce correct motion vectors while the sphere is being dragged around.
  vec3 sphereMotion;
  float pad0;

  vec3 lightDirection;             // Normalized, direction the light travels.
  float lightIntensity;

  vec3 lightColor;
  float ambientIntensity;

  vec3 baseColor;
  float longitudinalRoughness;     // beta_m

  vec3 diffuseReflectionTint;      // Far-field BCSDF diffuse lobe tint.
  float azimuthalRoughness;        // beta_n

  uvec2 outputResolution;
  vec2 outputResolutionInv;

  float ior;
  float cuticleAngleDegrees;
  float melanin;
  float melaninRedness;

  uint absorptionModel;            // RTXCR_HairAbsorptionModel
  uint bsdfModel;                  // HAIR_TEST_BSDF_MODEL_*
  uint geometryMode;               // HAIR_TEST_GEOMETRY_MODE_* actually traced this frame
  uint debugMode;                  // HAIR_TEST_DEBUG_MODE_*

  float farFieldRoughness;
  float diffuseReflectionWeight;
  float hairShadowIntensity;
  float aoDistance;                // World-space length of the ambient occlusion probe ray.

  uint enableHairShadows;
  uint enableSceneShadows;
  uint enableAmbientOcclusion;
  uint aaSamples;                  // Rays traced per pixel (1, 2 or 4) for edge anti-aliasing.
};

// Inputs

#define HAIR_TEST_BINDING_CONSTANTS                    40
#define HAIR_TEST_BINDING_TLAS                         41
#define HAIR_TEST_BINDING_SEGMENT_POSITIONS_INPUT      42
#define HAIR_TEST_BINDING_SEGMENT_RADII_INPUT          43

// Inputs/Outputs

#define HAIR_TEST_BINDING_DEPTH_INPUT_OUTPUT           44
#define HAIR_TEST_BINDING_COMPOSITE_INPUT_OUTPUT       45

// Outputs

#define HAIR_TEST_BINDING_MOTION_VECTOR_OUTPUT         46
