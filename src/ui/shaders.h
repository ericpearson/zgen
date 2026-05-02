// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Shared GLSL shader sources used by both backends.
//
// One source per shader stage. Differences between the OpenGL 3.2 (GLSL 150)
// path and the Vulkan 1.0 (GLSL 450) path are guarded with `#ifdef VULKAN`.
//
// Each backend prepends its own prelude (version line + optional defines)
// before compiling, then:
//   - OpenGL: glCompileShader on the concatenated source
//   - Vulkan: shaderc compiles the concatenated source to SPIR-V at startup,
//     then vkCreateShaderModule consumes the SPIR-V words.

namespace shaders {

// ---------- Backend prelude (prepended before the shared body) ----------

inline const char* kGLPrelude =
    "#version 150\n";

// shaderc/glslang automatically defines `VULKAN` (to a positive integer)
// when targeting Vulkan via the GL_KHR_vulkan_glsl extension, so we don't
// add it ourselves — the body's `#ifdef VULKAN` switches naturally.
inline const char* kVulkanVertexPrelude =
    "#version 450\n";

inline const char* kVulkanFragmentPrelude =
    "#version 450\n";

// ---------- Push-constant block (Vulkan, shared by all stages) ----------
// Every Vulkan shader stage declares the same push_constant block layout.
// Vulkan requires consistent declarations across stages whose pipeline
// layout covers the same push-constant range; declaring a smaller subset
// in one stage (e.g. just mat4 mvp in the vertex shader) is technically
// undefined and breaks pipeline creation on some drivers.
//
// Layout (matches the C++ PushBlock in game_renderer_vk.cpp):
//   offset  0..63  : mat4  mvp        (vertex)
//   offset 64..71  : vec2  resolution (fragment, CRT only)
//   offset 72..79  : vec2  inputRes   (fragment, CRT only)
//   offset 80..83  : float curvature  (fragment, CRT only)
//   offset 84..87  : float scanline   (fragment, CRT only)
//   offset 88..91  : float bloom      (fragment, CRT only)
//   offset 92..95  : float vignette   (fragment, CRT only)
//   offset 96..99  : int   maskType   (fragment, CRT only)

#define VULKAN_PUSH_BLOCK \
    "layout(push_constant) uniform PC {\n" \
    "    layout(offset = 0)  mat4  mvp;\n" \
    "    layout(offset = 64) vec2  resolution;\n" \
    "    layout(offset = 72) vec2  inputRes;\n" \
    "    layout(offset = 80) float curvature;\n" \
    "    layout(offset = 84) float scanline;\n" \
    "    layout(offset = 88) float bloom;\n" \
    "    layout(offset = 92) float vignette;\n" \
    "    layout(offset = 96) int   maskType;\n" \
    "} pc;\n"

// ---------- Vertex (passthrough + CRT both use this) ----------

inline const char* kVertSrc = R"GLSL(
#ifdef VULKAN
)GLSL" VULKAN_PUSH_BLOCK R"GLSL(
#define MVP (pc.mvp)
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
#else
uniform mat4 uMVP;
#define MVP uMVP
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
#endif

void main() {
    vUV = aUV;
    gl_Position = MVP * vec4(aPos, 0.0, 1.0);
}
)GLSL";

// ---------- Passthrough fragment ----------
// Vulkan: declares the full push_constant block too (must match vertex
// shader since the pipeline layout's range covers VERTEX|FRAGMENT) but
// only references vUV/uTex.

inline const char* kFragPassthrough = R"GLSL(
#ifdef VULKAN
)GLSL" VULKAN_PUSH_BLOCK R"GLSL(
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
#else
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
#endif

void main() {
    fragColor = texture(uTex, vUV);
}
)GLSL";

// ---------- CRT fragment ----------
// Vulkan reads CRT params from the same push-constant block as the vertex
// shader (offset 64, after the mat4 mvp). OpenGL reads them via individual
// uniforms (uResolution, uInputRes, uCurvature, uScanline, uBloom,
// uVignette, uMaskType).

inline const char* kFragCRT = R"GLSL(
#ifdef VULKAN
)GLSL" VULKAN_PUSH_BLOCK R"GLSL(
#define U_RESOLUTION (pc.resolution)
#define U_INPUTRES   (pc.inputRes)
#define U_CURVATURE  (pc.curvature)
#define U_SCANLINE   (pc.scanline)
#define U_BLOOM      (pc.bloom)
#define U_VIGNETTE   (pc.vignette)
#define U_MASKTYPE   (pc.maskType)
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
#else
uniform vec2  uResolution;
uniform vec2  uInputRes;
uniform float uCurvature;
uniform float uScanline;
uniform float uBloom;
uniform float uVignette;
uniform int   uMaskType;
#define U_RESOLUTION uResolution
#define U_INPUTRES   uInputRes
#define U_CURVATURE  uCurvature
#define U_SCANLINE   uScanline
#define U_BLOOM      uBloom
#define U_VIGNETTE   uVignette
#define U_MASKTYPE   uMaskType
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
#endif

vec2 distort(vec2 coord) {
    vec2 cc = coord - 0.5;
    float d = dot(cc, cc) * U_CURVATURE;
    return coord + cc * d;
}

float scanlineWeight(float y, float brightness) {
    float s = sin(y * U_INPUTRES.y * 3.14159265) * 0.5 + 0.5;
    float dark = 1.0 - U_SCANLINE;
    float intensity = mix(dark, 1.0, s);
    return mix(intensity, 1.0, brightness * 0.4);
}

vec3 phosphorMask(vec2 fragCoord) {
    if (U_MASKTYPE == 2) return vec3(1.0);
    float strength = 0.3;
    if (U_MASKTYPE == 0) {
        // Aperture grille: vertical RGB stripes
        int col = int(mod(fragCoord.x, 3.0));
        vec3 mask = vec3(1.0 - strength);
        if (col == 0) mask.r = 1.0;
        else if (col == 1) mask.g = 1.0;
        else mask.b = 1.0;
        return mask;
    } else {
        // Slot mask: 2D dot pattern
        int col = int(mod(fragCoord.x, 3.0));
        int row = int(mod(fragCoord.y, 2.0));
        vec3 mask = vec3(1.0 - strength);
        int idx = (col + row) % 3;
        if (idx == 0) mask.r = 1.0;
        else if (idx == 1) mask.g = 1.0;
        else mask.b = 1.0;
        return mask;
    }
}

void main() {
    vec2 uv = distort(vUV);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Bloom: blend with blurred neighbors
    vec2 texel = 1.0 / U_INPUTRES;
    vec3 col = texture(uTex, uv).rgb;
    if (U_BLOOM > 0.0) {
        vec3 blur = col;
        blur += texture(uTex, uv + vec2(-texel.x, 0.0)).rgb * 0.25;
        blur += texture(uTex, uv + vec2( texel.x, 0.0)).rgb * 0.25;
        blur += texture(uTex, uv + vec2(0.0, -texel.y)).rgb * 0.25;
        blur += texture(uTex, uv + vec2(0.0,  texel.y)).rgb * 0.25;
        col = mix(col, blur / 2.0, U_BLOOM);
    }

    // Scanlines
    float brightness = dot(col, vec3(0.299, 0.587, 0.114));
    col *= scanlineWeight(uv.y, brightness);

    // Phosphor mask
    vec2 fragCoord = uv * U_RESOLUTION;
    col *= phosphorMask(fragCoord);

    // Vignette
    if (U_VIGNETTE > 0.0) {
        vec2 vig = uv * (1.0 - uv);
        float v = pow(vig.x * vig.y * 15.0, 0.25);
        col *= mix(1.0, v, U_VIGNETTE);
    }

    // Brightness compensation for scanlines + mask
    col *= 1.3;

    fragColor = vec4(col, 1.0);
}
)GLSL";

} // namespace shaders
