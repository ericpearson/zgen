// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/vulkan_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ---------- Error helper ----------

void VulkanContext::checkVkResult(VkResult err) {
    if (err == VK_SUCCESS) return;
    fprintf(stderr, "[Vulkan] VkResult = %d\n", err);
    if (err < 0) std::abort();
}

// ---------- Init ----------

bool VulkanContext::init(SDL_Window* window) {
    window_ = window;

    // --- Instance ---
    Uint32 extCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extCount);
    if (!sdlExtensions) {
        fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + extCount);

    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
#ifdef __APPLE__
    instInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif
    instInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instInfo.ppEnabledExtensionNames = extensions.data();

    VkResult err = vkCreateInstance(&instInfo, allocator_, &instance_);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", err);
        return false;
    }

    // --- Physical device ---
    physicalDevice_ = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
    if (physicalDevice_ == VK_NULL_HANDLE) {
        fprintf(stderr, "Failed to find a suitable GPU\n");
        return false;
    }

    // --- Queue family ---
    queueFamily_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physicalDevice_);

    // --- Logical device ---
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions;
    deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#ifdef __APPLE__
    deviceExtensions.push_back("VK_KHR_portability_subset");
#endif

    VkDeviceCreateInfo devInfo{};
    devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;
    devInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    devInfo.ppEnabledExtensionNames = deviceExtensions.data();

    err = vkCreateDevice(physicalDevice_, &devInfo, allocator_, &device_);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %d\n", err);
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 100;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = poolSizes;
    err = vkCreateDescriptorPool(device_, &poolInfo, allocator_, &descriptorPool_);
    checkVkResult(err);

    // --- Surface ---
    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, instance_, allocator_, &surface)) {
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }

    // --- Window data (swapchain, render pass, framebuffers) ---
    wd_.Surface = surface;

    // Select surface format
    const VkFormat requestFormats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    wd_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        physicalDevice_, surface, requestFormats, 2, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

    // Select present mode
    VkPresentModeKHR presentModes[] = { VK_PRESENT_MODE_FIFO_KHR };
    wd_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        physicalDevice_, surface, presentModes, 1);

    minImageCount_ = ImGui_ImplVulkanH_GetMinImageCountFromPresentMode(wd_.PresentMode);

    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        instance_, physicalDevice_, device_, &wd_,
        queueFamily_, allocator_, w, h, minImageCount_,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    return true;
}

// ---------- Swapchain recreation ----------

void VulkanContext::recreateSwapchain(int w, int h) {
    vkDeviceWaitIdle(device_);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        instance_, physicalDevice_, device_, &wd_,
        queueFamily_, allocator_, w, h, minImageCount_,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    wd_.FrameIndex = 0;
}

// ---------- Frame ----------

VkCommandBuffer VulkanContext::beginFrame() {
    ImGui_ImplVulkanH_Frame* fd = &wd_.Frames[wd_.FrameIndex];
    ImGui_ImplVulkanH_FrameSemaphores* fsd = &wd_.FrameSemaphores[wd_.SemaphoreIndex];

    // Wait for the fence of this frame slot
    VkResult err = vkWaitForFences(device_, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    checkVkResult(err);
    err = vkResetFences(device_, 1, &fd->Fence);
    checkVkResult(err);

    // Acquire next image
    err = vkAcquireNextImageKHR(device_, wd_.Swapchain, UINT64_MAX,
                                 fsd->ImageAcquiredSemaphore, VK_NULL_HANDLE,
                                 &wd_.FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        int w, h;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        recreateSwapchain(w, h);
        return VK_NULL_HANDLE;
    }
    checkVkResult(err);

    fd = &wd_.Frames[wd_.FrameIndex];

    // Reset and begin command buffer
    err = vkResetCommandPool(device_, fd->CommandPool, 0);
    checkVkResult(err);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);
    checkVkResult(err);

    // Begin render pass
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = wd_.RenderPass;
    rpInfo.framebuffer = fd->Framebuffer;
    rpInfo.renderArea.extent.width = static_cast<uint32_t>(wd_.Width);
    rpInfo.renderArea.extent.height = static_cast<uint32_t>(wd_.Height);
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &wd_.ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    return fd->CommandBuffer;
}

void VulkanContext::endFrame(VkCommandBuffer cmd) {
    ImGui_ImplVulkanH_Frame* fd = &wd_.Frames[wd_.FrameIndex];
    ImGui_ImplVulkanH_FrameSemaphores* fsd = &wd_.FrameSemaphores[wd_.SemaphoreIndex];

    vkCmdEndRenderPass(cmd);

    VkResult err = vkEndCommandBuffer(cmd);
    checkVkResult(err);

    // Submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &fsd->ImageAcquiredSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &fd->CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &fsd->RenderCompleteSemaphore;

    err = vkQueueSubmit(queue_, 1, &submitInfo, fd->Fence);
    checkVkResult(err);

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &fsd->RenderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &wd_.Swapchain;
    presentInfo.pImageIndices = &wd_.FrameIndex;
    err = vkQueuePresentKHR(queue_, &presentInfo);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        int w, h;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        recreateSwapchain(w, h);
    } else {
        checkVkResult(err);
    }

    // Advance semaphore index
    wd_.SemaphoreIndex = (wd_.SemaphoreIndex + 1) % wd_.SemaphoreCount;
}

// ---------- Shutdown ----------

void VulkanContext::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device_);

    ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &wd_, allocator_);

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, allocator_);
        descriptorPool_ = VK_NULL_HANDLE;
    }

    vkDestroyDevice(device_, allocator_);
    device_ = VK_NULL_HANDLE;

    if (wd_.Surface != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance_, wd_.Surface, allocator_);
    }

    vkDestroyInstance(instance_, allocator_);
    instance_ = VK_NULL_HANDLE;
}
