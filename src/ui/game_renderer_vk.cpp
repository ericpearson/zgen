// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/game_renderer.h"
#include "ui/shaders.h"
#include "ui/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Forward declaration — the global VulkanContext is owned by main.cpp
extern VulkanContext g_vkCtx;

// ---------- Runtime GLSL→SPIR-V compilation ----------

static std::vector<uint32_t> compileGlslToSpv(shaderc_shader_kind kind,
                                              const char* prelude,
                                              const char* body,
                                              const char* tag) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    std::string source(prelude);
    source.append(body);

    auto result = compiler.CompileGlslToSpv(source, kind, tag, options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        fprintf(stderr, "[shaderc] %s compile failed: %s\n",
                tag, result.GetErrorMessage().c_str());
        std::abort();
    }
    return { result.cbegin(), result.cend() };
}

// ---------- Vulkan resource struct (opaque in header) ----------

struct GameRenderer::VkResources {
    VkImage       image        = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView   imageView    = VK_NULL_HANDLE;
    VkSampler     sampler      = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkBuffer      stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void*         stagingMapped  = nullptr;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    // Two pipelines share the same layout (push-constant range covers both
    // mvp at offset 0 and the CRT params block starting at offset 64).
    // setShaderMode() swaps `pipeline` between these.
    VkPipeline       pipelinePassthrough = VK_NULL_HANDLE;
    VkPipeline       pipelineCRT         = VK_NULL_HANDLE;
    VkPipeline       pipeline            = VK_NULL_HANDLE; // currently active
    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;

    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    void*         vertexMapped  = nullptr;

    // Transient command pool used to copy the staging buffer into the
    // game-texture image once per upload. Submitted synchronously
    // (waitIdle on the queue) — simple and correct; one transfer/frame.
    VkCommandPool transferPool = VK_NULL_HANDLE;
    VkCommandBuffer transferCmd = VK_NULL_HANDLE;
    bool imageInitialized = false; // false until first upload completes

    bool bilinear = false;
};

// ---------- Helpers ----------

static uint32_t findMemoryType(VkPhysicalDevice physDev, uint32_t typeFilter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    fprintf(stderr, "Failed to find suitable memory type\n");
    return 0;
}

static void ortho(float* m, float w, float h) {
    // Vulkan NDC: Y points down (clip-space +1 is bottom of framebuffer),
    // unlike GL where Y points up. Map screen (0,0)->clip(-1,-1) and
    // (w,h)->clip(+1,+1) so the same vertex array as the GL backend
    // produces an upright image instead of a vertically flipped one.
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  =  2.0f / w;
    m[5]  =  2.0f / h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] = -1.0f;
    m[14] =  0.0f;
    m[15] =  1.0f;
}

// ---------- GameRenderer implementation (Vulkan) ----------

void GameRenderer::init(int gameW, int gameH) {
    gameW_ = gameW;
    gameH_ = gameH;
    vk_ = new VkResources();

    VkDevice device = g_vkCtx.device();
    VkPhysicalDevice physDev = g_vkCtx.physicalDevice();

    // --- Transient command pool (used by upload() for staging->image copy) ---
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = g_vkCtx.queueFamily();
        vkCreateCommandPool(device, &poolInfo, nullptr, &vk_->transferPool);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = vk_->transferPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &allocInfo, &vk_->transferCmd);
    }

    // --- Staging buffer (CPU-visible, for pixel uploads) ---
    VkDeviceSize pixelSize = static_cast<VkDeviceSize>(gameW) * gameH * 4;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = pixelSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(device, &bufInfo, nullptr, &vk_->stagingBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, vk_->stagingBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDev, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &vk_->stagingMemory);
        vkBindBufferMemory(device, vk_->stagingBuffer, vk_->stagingMemory, 0);
        vkMapMemory(device, vk_->stagingMemory, 0, pixelSize, 0, &vk_->stagingMapped);
    }

    // --- Game texture (device-local image) ---
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imgInfo.extent = { static_cast<uint32_t>(gameW), static_cast<uint32_t>(gameH), 1 };
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imgInfo, nullptr, &vk_->image);

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, vk_->image, &memReq);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDev, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &vk_->imageMemory);
        vkBindImageMemory(device, vk_->image, vk_->imageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vk_->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(device, &viewInfo, nullptr, &vk_->imageView);
    }

    // --- Sampler ---
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(device, &samplerInfo, nullptr, &vk_->sampler);
    }

    // Register with ImGui's Vulkan backend for ImTextureID usage
    vk_->descriptorSet = ImGui_ImplVulkan_AddTexture(
        vk_->sampler, vk_->imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture_ = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(vk_->descriptorSet));

    // --- Vertex buffer ---
    {
        VkDeviceSize vtxSize = 6 * 4 * sizeof(float);
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = vtxSize;
        bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vkCreateBuffer(device, &bufInfo, nullptr, &vk_->vertexBuffer);

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, vk_->vertexBuffer, &memReq);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDev, memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &vk_->vertexMemory);
        vkBindBufferMemory(device, vk_->vertexBuffer, vk_->vertexMemory, 0);
        vkMapMemory(device, vk_->vertexMemory, 0, vtxSize, 0, &vk_->vertexMapped);
    }

    // --- Descriptor set layout ---
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &vk_->descSetLayout);
    }

    // --- Pipeline layout ---
    // Shared by both passthrough and CRT pipelines. Push-constant range
    // covers the vertex MVP at offset 0..63 and the CRT params at 64..99.
    // Passthrough only writes the first 64 bytes; CRT writes the full range.
    {
        VkPushConstantRange pushConst{};
        pushConst.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConst.offset = 0;
        pushConst.size = 100;  // mat4 (64) + vec2*2 + float*4 + int = 100

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vk_->descSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConst;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &vk_->pipelineLayout);
    }

    // --- Graphics pipelines (passthrough + CRT) ---
    // Compile GLSL → SPIR-V at startup via shaderc using the shared sources
    // in shaders.h. Build two pipelines that share everything except the
    // fragment shader module.
    {
        auto vsSpv = compileGlslToSpv(shaderc_glsl_vertex_shader,
            shaders::kVulkanVertexPrelude, shaders::kVertSrc, "vert");
        auto fsPassthroughSpv = compileGlslToSpv(shaderc_glsl_fragment_shader,
            shaders::kVulkanFragmentPrelude, shaders::kFragPassthrough, "frag.passthrough");
        auto fsCRTSpv = compileGlslToSpv(shaderc_glsl_fragment_shader,
            shaders::kVulkanFragmentPrelude, shaders::kFragCRT, "frag.crt");

        auto makeModule = [&](const std::vector<uint32_t>& spv) {
            VkShaderModuleCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = spv.size() * sizeof(uint32_t);
            info.pCode = spv.data();
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(device, &info, nullptr, &m);
            return m;
        };
        VkShaderModule vsModule = makeModule(vsSpv);
        VkShaderModule fsPassModule = makeModule(fsPassthroughSpv);
        VkShaderModule fsCRTModule  = makeModule(fsCRTSpv);

        VkVertexInputBindingDescription bindDesc{};
        bindDesc.binding = 0;
        bindDesc.stride = 4 * sizeof(float);
        bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrDesc[2]{};
        attrDesc[0].location = 0;
        attrDesc[0].binding = 0;
        attrDesc[0].format = VK_FORMAT_R32G32_SFLOAT;
        attrDesc[0].offset = 0;
        attrDesc[1].location = 1;
        attrDesc[1].binding = 0;
        attrDesc[1].format = VK_FORMAT_R32G32_SFLOAT;
        attrDesc[1].offset = 2 * sizeof(float);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &bindDesc;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attrDesc;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynState{};
        dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynState.dynamicStateCount = 2;
        dynState.pDynamicStates = dynStates;

        auto buildPipeline = [&](VkShaderModule fsModule, VkPipeline* outPipeline) {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vsModule;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fsModule;
            stages[1].pName = "main";

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = stages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynState;
            pipelineInfo.layout = vk_->pipelineLayout;
            pipelineInfo.renderPass = g_vkCtx.renderPass();
            pipelineInfo.subpass = 0;
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, outPipeline);
        };

        buildPipeline(fsPassModule, &vk_->pipelinePassthrough);
        buildPipeline(fsCRTModule,  &vk_->pipelineCRT);
        vk_->pipeline = vk_->pipelinePassthrough;

        vkDestroyShaderModule(device, vsModule, nullptr);
        vkDestroyShaderModule(device, fsPassModule, nullptr);
        vkDestroyShaderModule(device, fsCRTModule, nullptr);
    }
}

void GameRenderer::upload(const u32* pixels) {
    memcpy(vk_->stagingMapped, pixels,
           static_cast<size_t>(gameW_) * gameH_ * sizeof(u32));

    VkDevice device = g_vkCtx.device();
    VkCommandBuffer cmd = vk_->transferCmd;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Barrier: (UNDEFINED on first call | SHADER_READ_ONLY) -> TRANSFER_DST
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = vk_->imageInitialized
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcAccessMask = vk_->imageInitialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = vk_->image;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        vk_->imageInitialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { static_cast<uint32_t>(gameW_), static_cast<uint32_t>(gameH_), 1 };
    vkCmdCopyBufferToImage(cmd, vk_->stagingBuffer, vk_->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Barrier: TRANSFER_DST -> SHADER_READ_ONLY
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = vk_->image;
    toRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toRead);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(g_vkCtx.queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vkCtx.queue());

    vk_->imageInitialized = true;
    (void)device;
}

void GameRenderer::resize(int gameW, int gameH) {
    if (gameW == gameW_ && gameH == gameH_) return;
    // Full resize requires recreating Vulkan image/staging/descriptors.
    shutdown();
    init(gameW, gameH);
}

void GameRenderer::setShaderMode(ShaderMode mode) {
    shaderMode_ = mode;
    if (!vk_) return;
    vk_->pipeline = (mode == ShaderMode::CRT)
        ? vk_->pipelineCRT
        : vk_->pipelinePassthrough;
}

void GameRenderer::setFilterMode(bool bilinear) {
    if (!vk_ || vk_->bilinear == bilinear) return;
    vk_->bilinear = bilinear;

    VkDevice device = g_vkCtx.device();
    vkDeviceWaitIdle(device);

    // Recreate sampler
    if (vk_->sampler != VK_NULL_HANDLE)
        vkDestroySampler(device, vk_->sampler, nullptr);

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.minFilter = bilinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &samplerInfo, nullptr, &vk_->sampler);

    // Re-register texture with new sampler
    if (vk_->descriptorSet != VK_NULL_HANDLE)
        ImGui_ImplVulkan_RemoveTexture(vk_->descriptorSet);
    vk_->descriptorSet = ImGui_ImplVulkan_AddTexture(
        vk_->sampler, vk_->imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void GameRenderer::draw(int viewportW, int viewportH,
                         ScalingMode scaling, AspectRatio aspect) {
    if (viewportW <= 0 || viewportH <= 0) return;

    // --- Compute quad geometry (same logic as GL path) ---
    float targetAR;
    switch (aspect) {
        case AspectRatio::FourThree:   targetAR = 4.0f / 3.0f; break;
        case AspectRatio::SixteenNine: targetAR = 16.0f / 9.0f; break;
        case AspectRatio::Stretch:     targetAR = (float)viewportW / (float)viewportH; break;
        case AspectRatio::Auto:
        default: {
            int nativeH = (gameH_ > 240) ? gameH_ / 2 : gameH_;
            targetAR = (float)gameW_ / (float)nativeH;
            break;
        }
    }

    float quadW, quadH;
    switch (scaling) {
        case ScalingMode::IntegerOnly: {
            int scaleX = viewportW / gameW_;
            int scaleY = viewportH / gameH_;
            scale_ = (scaleX < scaleY) ? scaleX : scaleY;
            if (scale_ < 1) scale_ = 1;
            quadH = (float)(gameH_ * scale_);
            quadW = quadH * targetAR;
            if (quadW > viewportW) {
                quadW = (float)(gameW_ * scale_);
                quadH = quadW / targetAR;
            }
            break;
        }
        case ScalingMode::Fit: {
            float vpAR = (float)viewportW / (float)viewportH;
            if (targetAR > vpAR) { quadW = (float)viewportW; quadH = quadW / targetAR; }
            else { quadH = (float)viewportH; quadW = quadH * targetAR; }
            scale_ = (int)(quadW / gameW_);
            if (scale_ < 1) scale_ = 1;
            break;
        }
        case ScalingMode::Stretch:
            quadW = (float)viewportW;
            quadH = (float)viewportH;
            scale_ = (int)(quadW / gameW_);
            if (scale_ < 1) scale_ = 1;
            break;
    }

    float offsetX = ((float)viewportW - quadW) / 2.0f;
    float offsetY = ((float)viewportH - quadH) / 2.0f;
    float x0 = offsetX, y0 = offsetY;
    float x1 = offsetX + quadW, y1 = offsetY + quadH;

    float verts[6 * 4] = {
        x0, y0, 0.0f, 0.0f,  x1, y0, 1.0f, 0.0f,  x0, y1, 0.0f, 1.0f,
        x1, y0, 1.0f, 0.0f,  x1, y1, 1.0f, 1.0f,  x0, y1, 0.0f, 1.0f,
    };
    memcpy(vk_->vertexMapped, verts, sizeof(verts));

    // --- Record draw commands into the current render pass command buffer ---
    VkCommandBuffer cmd = g_vkCtx.commandBuffer();

    // We need to copy staging buffer to image before drawing.
    // End render pass temporarily, do the copy, then restart render pass.
    // NOTE: For better performance this could be done outside the render pass
    // in a separate pre-pass. For simplicity we do it inline here.
    // Actually, the copy must happen outside a render pass, so we do it
    // before the render pass is begun (in VulkanContext::beginFrame).
    // For now the game texture starts as UNDEFINED and we transition it.
    // This is a known simplification; a production engine would pipeline this.

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_->pipeline);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0; viewport.y = 0;
    viewport.width = (float)viewportW;
    viewport.height = (float)viewportH;
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent.width = static_cast<uint32_t>(viewportW);
    scissor.extent.height = static_cast<uint32_t>(viewportH);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Push the full constants block: vertex MVP at offset 0, CRT params at
    // offset 64. Layout must match the push_constant block declared in
    // shaders.h (kFragCRT). Passthrough fragment ignores everything past 64.
    struct PushBlock {
        float mvp[16];
        float resolution[2];
        float inputRes[2];
        float curvature;
        float scanline;
        float bloom;
        float vignette;
        int   maskType;
    } pc;
    static_assert(sizeof(pc) == 100, "push constant block must be 100 bytes");
    ortho(pc.mvp, (float)viewportW, (float)viewportH);
    pc.resolution[0] = (float)viewportW;
    pc.resolution[1] = (float)viewportH;
    pc.inputRes[0]   = (float)gameW_;
    pc.inputRes[1]   = (float)gameH_;
    pc.curvature = curvature_;
    pc.scanline  = scanline_;
    pc.bloom     = bloom_;
    pc.vignette  = vignette_;
    pc.maskType  = maskType_;
    vkCmdPushConstants(cmd, vk_->pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(pc), &pc);

    // Bind descriptor set (texture)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        vk_->pipelineLayout, 0, 1, &vk_->descriptorSet, 0, nullptr);

    // Bind vertex buffer
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vk_->vertexBuffer, &offset);

    // Draw
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void GameRenderer::shutdown() {
    if (!vk_) return;
    VkDevice device = g_vkCtx.device();
    vkDeviceWaitIdle(device);

    if (vk_->pipelinePassthrough) vkDestroyPipeline(device, vk_->pipelinePassthrough, nullptr);
    if (vk_->pipelineCRT)         vkDestroyPipeline(device, vk_->pipelineCRT, nullptr);
    if (vk_->pipelineLayout) vkDestroyPipelineLayout(device, vk_->pipelineLayout, nullptr);
    if (vk_->descSetLayout)  vkDestroyDescriptorSetLayout(device, vk_->descSetLayout, nullptr);

    if (vk_->descriptorSet)  ImGui_ImplVulkan_RemoveTexture(vk_->descriptorSet);
    if (vk_->sampler)        vkDestroySampler(device, vk_->sampler, nullptr);
    if (vk_->imageView)      vkDestroyImageView(device, vk_->imageView, nullptr);
    if (vk_->image)          vkDestroyImage(device, vk_->image, nullptr);
    if (vk_->imageMemory)    vkFreeMemory(device, vk_->imageMemory, nullptr);

    if (vk_->stagingBuffer)  vkDestroyBuffer(device, vk_->stagingBuffer, nullptr);
    if (vk_->stagingMemory)  vkFreeMemory(device, vk_->stagingMemory, nullptr);

    if (vk_->vertexBuffer)   vkDestroyBuffer(device, vk_->vertexBuffer, nullptr);
    if (vk_->vertexMemory)   vkFreeMemory(device, vk_->vertexMemory, nullptr);

    if (vk_->transferPool)   vkDestroyCommandPool(device, vk_->transferPool, nullptr);

    delete vk_;
    vk_ = nullptr;
}
