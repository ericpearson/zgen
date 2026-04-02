// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"

struct SDL_Window;

// Encapsulates all Vulkan boilerplate: instance, device, swapchain, render
// pass, framebuffers, command buffers, sync primitives.  Keeps main.cpp
// manageable when building with -DUSE_VULKAN=ON.

struct VulkanContext {
    // Create Vulkan instance, device, surface, swapchain, render pass, etc.
    // Returns false on failure (logs to stderr).
    bool init(SDL_Window* window);

    // Recreate the swapchain (e.g. after window resize).
    void recreateSwapchain(int w, int h);

    // Begin a frame: acquire next swapchain image, reset + begin command buffer.
    // Returns the command buffer to record into, or VK_NULL_HANDLE on error.
    VkCommandBuffer beginFrame();

    // End a frame: end command buffer, submit, present.
    void endFrame(VkCommandBuffer cmd);

    // Destroy everything.
    void shutdown();

    // Accessors used by ImGui and game renderer.
    VkInstance              instance()      const { return instance_; }
    VkPhysicalDevice        physicalDevice()const { return physicalDevice_; }
    VkDevice                device()        const { return device_; }
    VkQueue                 queue()         const { return queue_; }
    uint32_t                queueFamily()   const { return queueFamily_; }
    VkRenderPass            renderPass()    const { return wd_.RenderPass; }
    VkCommandBuffer         commandBuffer() const { return wd_.Frames[wd_.FrameIndex].CommandBuffer; }
    uint32_t                imageCount()    const { return wd_.ImageCount; }
    uint32_t                minImageCount() const { return minImageCount_; }
    VkDescriptorPool        descriptorPool()const { return descriptorPool_; }
    ImGui_ImplVulkanH_Window* windowData()        { return &wd_; }

private:
    VkInstance              instance_       = VK_NULL_HANDLE;
    VkPhysicalDevice        physicalDevice_ = VK_NULL_HANDLE;
    VkDevice                device_         = VK_NULL_HANDLE;
    VkQueue                 queue_          = VK_NULL_HANDLE;
    uint32_t                queueFamily_    = 0;
    VkDescriptorPool        descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineCache         pipelineCache_  = VK_NULL_HANDLE;
    uint32_t                minImageCount_  = 2;

    ImGui_ImplVulkanH_Window wd_{};

    SDL_Window*             window_         = nullptr;
    const VkAllocationCallbacks* allocator_ = nullptr;

    static void checkVkResult(VkResult err);
};
