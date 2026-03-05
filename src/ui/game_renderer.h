// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"

enum class ScalingMode { IntegerOnly, Fit, Stretch };
enum class AspectRatio { Auto, FourThree, SixteenNine, Stretch };

// Game framebuffer renderer — uploads the Genesis VDP framebuffer to a
// texture and draws it as a scaled, centered quad.
// Implementation selected at compile time: OpenGL 3.2 or Vulkan.

struct GameRenderer {
    // Call once after the graphics context is created.
    // For Vulkan, call init() after VulkanContext is ready.
    void init(int gameW, int gameH);

    // Upload new framebuffer pixels (ARGB8888, gameW*gameH u32s).
    void upload(const u32* pixels);

    // Resize texture if game dimensions changed (e.g., interlace mode).
    void resize(int gameW, int gameH);

    // Draw the game quad filling the viewport with the given scaling/aspect.
    // viewportW/H are in actual pixels.
    void draw(int viewportW, int viewportH,
              ScalingMode scaling = ScalingMode::IntegerOnly,
              AspectRatio aspect = AspectRatio::Auto);

    // Toggle between nearest-neighbor and bilinear filtering.
    void setFilterMode(bool bilinear);

    // Clean up resources.
    void shutdown();

    // Current scale factor (set after draw()).
    int scale() const { return scale_; }

    // Expose texture ID (GL texture handle; VK descriptor set cast).
    unsigned int textureID() const { return texture_; }

private:
    int gameW_ = 0, gameH_ = 0;
    int scale_ = 1;

#ifdef USE_VULKAN
    unsigned int texture_ = 0; // placeholder — VkDescriptorSet stored as intptr_t
    // Vulkan-specific resources are managed by VulkanContext + game_renderer_vk.cpp
    // They're stored in an opaque pointer to avoid leaking Vulkan headers here.
    struct VkResources;
    VkResources* vk_ = nullptr;
#else
    unsigned int texture_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    unsigned int shader_ = 0;
    int locMVP_ = -1;
#endif
};
