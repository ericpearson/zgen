// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/game_renderer.h"
#include "ui/shaders.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cmath>

// ---------- Helpers ----------

// Concatenate the GL #version prelude with the shared GLSL body and feed
// both strings to glShaderSource. Source-line numbers in error messages
// match shaders.h (the prelude is one line and is prepended as a separate
// string, so the body still starts at line 1 from glCompileShader's view).
static GLuint compileShader(GLenum type, const char* body) {
    const char* parts[2] = { shaders::kGLPrelude, body };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, parts, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        fprintf(stderr, "Shader compile error: %s\n", buf);
    }
    return s;
}

static GLuint linkProgram(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        fprintf(stderr, "Shader link error: %s\n", buf);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// Build a simple orthographic matrix mapping [0..w, 0..h] to clip space.
static void ortho(float* m, float w, float h) {
    // column-major 4x4
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  =  2.0f / w;
    m[5]  = -2.0f / h;   // flip Y so (0,0) is top-left
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[14] =  0.0f;
    m[15] =  1.0f;
}

// ---------- GameRenderer ----------

void GameRenderer::init(int gameW, int gameH) {
    gameW_ = gameW;
    gameH_ = gameH;

    // Texture
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gameW, gameH, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);

    // Passthrough shader
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, shaders::kVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, shaders::kFragPassthrough);
        shaderNone_ = linkProgram(vs, fs);
    }

    // CRT shader
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, shaders::kVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, shaders::kFragCRT);
        shaderCRT_ = linkProgram(vs, fs);
    }

    shader_ = shaderNone_;
    locMVP_ = glGetUniformLocation(shader_, "uMVP");

    // Quad VAO/VBO (positions + UVs, updated each frame in draw())
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // 6 vertices * 4 floats (x, y, u, v)
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    GLint aPos = glGetAttribLocation(shaderNone_, "aPos");
    GLint aUV  = glGetAttribLocation(shaderNone_, "aUV");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(aUV);
    glVertexAttribPointer(aUV, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
}

void GameRenderer::upload(const u32* pixels) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gameW_, gameH_,
                    GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
}

void GameRenderer::resize(int gameW, int gameH) {
    if (gameW == gameW_ && gameH == gameH_) return;
    gameW_ = gameW;
    gameH_ = gameH;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gameW, gameH, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
}

void GameRenderer::setFilterMode(bool bilinear) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    GLenum filter = bilinear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

void GameRenderer::setShaderMode(ShaderMode mode) {
    shaderMode_ = mode;
    switch (mode) {
        case ShaderMode::None: shader_ = shaderNone_; break;
        case ShaderMode::CRT:  shader_ = shaderCRT_;  break;
    }
    locMVP_ = glGetUniformLocation(shader_, "uMVP");
    locResolution_ = glGetUniformLocation(shader_, "uResolution");
    locInputRes_ = glGetUniformLocation(shader_, "uInputRes");
    locCurvature_ = glGetUniformLocation(shader_, "uCurvature");
    locScanline_ = glGetUniformLocation(shader_, "uScanline");
    locBloom_ = glGetUniformLocation(shader_, "uBloom");
    locVignette_ = glGetUniformLocation(shader_, "uVignette");
    locMaskType_ = glGetUniformLocation(shader_, "uMaskType");
}

void GameRenderer::draw(int viewportW, int viewportH,
                         ScalingMode scaling, AspectRatio aspect) {
    if (viewportW <= 0 || viewportH <= 0) return;

    // Determine target aspect ratio
    float targetAR;
    switch (aspect) {
        case AspectRatio::FourThree:   targetAR = 4.0f / 3.0f; break;
        case AspectRatio::SixteenNine: targetAR = 16.0f / 9.0f; break;
        case AspectRatio::Stretch:     targetAR = (float)viewportW / (float)viewportH; break;
        case AspectRatio::Auto:
        default: {
            // Interlace modes double the framebuffer height, but the display
            // aspect ratio stays the same (scan lines are half-height on CRT).
            int nativeH = (gameH_ > 240) ? gameH_ / 2 : gameH_;
            targetAR = (float)gameW_ / (float)nativeH;
            break;
        }
    }

    float quadW, quadH;

    switch (scaling) {
        case ScalingMode::IntegerOnly: {
            // Largest integer N where N*gameW_ fits, maintaining target AR
            int scaleX = viewportW / gameW_;
            int scaleY = viewportH / gameH_;
            scale_ = (scaleX < scaleY) ? scaleX : scaleY;
            if (scale_ < 1) scale_ = 1;

            // Use integer-scaled game dimensions, then adjust for AR
            quadH = (float)(gameH_ * scale_);
            quadW = quadH * targetAR;

            // Clamp to viewport
            if (quadW > viewportW) {
                quadW = (float)(gameW_ * scale_);
                quadH = quadW / targetAR;
            }
            break;
        }

        case ScalingMode::Fit: {
            float vpAR = (float)viewportW / (float)viewportH;
            if (targetAR > vpAR) {
                quadW = (float)viewportW;
                quadH = quadW / targetAR;
            } else {
                quadH = (float)viewportH;
                quadW = quadH * targetAR;
            }
            scale_ = (int)(quadW / gameW_);
            if (scale_ < 1) scale_ = 1;
            break;
        }

        case ScalingMode::Stretch: {
            quadW = (float)viewportW;
            quadH = (float)viewportH;
            scale_ = (int)(quadW / gameW_);
            if (scale_ < 1) scale_ = 1;
            break;
        }
    }

    float offsetX = ((float)viewportW - quadW) / 2.0f;
    float offsetY = ((float)viewportH - quadH) / 2.0f;

    float x0 = offsetX;
    float y0 = offsetY;
    float x1 = offsetX + quadW;
    float y1 = offsetY + quadH;

    // Update quad vertices (two triangles)
    float verts[6 * 4] = {
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f,
        x0, y1, 0.0f, 1.0f,
        x1, y0, 1.0f, 0.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y1, 0.0f, 1.0f,
    };

    glViewport(0, 0, viewportW, viewportH);

    glUseProgram(shader_);
    float mvp[16];
    ortho(mvp, (float)viewportW, (float)viewportH);
    glUniformMatrix4fv(locMVP_, 1, GL_FALSE, mvp);

    // CRT shader uniforms
    if (shaderMode_ == ShaderMode::CRT) {
        glUniform2f(locResolution_, (float)viewportW, (float)viewportH);
        glUniform2f(locInputRes_, (float)gameW_, (float)gameH_);
        glUniform1f(locCurvature_, curvature_);
        glUniform1f(locScanline_, scanline_);
        glUniform1f(locBloom_, bloom_);
        glUniform1f(locVignette_, vignette_);
        glUniform1i(locMaskType_, maskType_);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void GameRenderer::shutdown() {
    if (texture_)    { glDeleteTextures(1, &texture_); texture_ = 0; }
    if (vbo_)        { glDeleteBuffers(1, &vbo_);      vbo_ = 0; }
    if (vao_)        { glDeleteVertexArrays(1, &vao_);  vao_ = 0; }
    if (shaderNone_) { glDeleteProgram(shaderNone_);   shaderNone_ = 0; }
    if (shaderCRT_)  { glDeleteProgram(shaderCRT_);    shaderCRT_ = 0; }
    shader_ = 0;
}
