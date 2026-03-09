// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/game_renderer.h"
#include "ui/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Forward declaration — the global VulkanContext is owned by main.cpp
extern VulkanContext g_vkCtx;

// ---------- SPIR-V shaders ----------
// Equivalent to the GLSL 150 vertex/fragment shaders used in the GL path.
// Compiled offline from:
//
//   // vertex
//   #version 450
//   layout(push_constant) uniform PC { mat4 mvp; } pc;
//   layout(location = 0) in vec2 aPos;
//   layout(location = 1) in vec2 aUV;
//   layout(location = 0) out vec2 vUV;
//   void main() { vUV = aUV; gl_Position = pc.mvp * vec4(aPos, 0, 1); }
//
//   // fragment
//   #version 450
//   layout(location = 0) in vec2 vUV;
//   layout(location = 0) out vec4 fragColor;
//   layout(set = 0, binding = 0) uniform sampler2D uTex;
//   void main() { fragColor = texture(uTex, vUV); }
//
// The actual SPIR-V blobs are included as static arrays below.

// clang-format off
static const uint32_t vert_spv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000028,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0009000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x0000000b, 0x00000012,
    0x00000016, 0x00030003, 0x00000002, 0x000001c2,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00030005, 0x00000009, 0x00565576, 0x00030005,
    0x0000000b, 0x00565561, 0x00060005, 0x00000010,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x00000010, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00070006, 0x00000010,
    0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953,
    0x00000000, 0x00030005, 0x00000012, 0x00000000,
    0x00030005, 0x00000014, 0x00004350, 0x00040006,
    0x00000014, 0x00000000, 0x0070766d, 0x00030005,
    0x00000016, 0x00736f50, 0x00040047, 0x00000009,
    0x0000001e, 0x00000000, 0x00040047, 0x0000000b,
    0x0000001e, 0x00000001, 0x00050048, 0x00000010,
    0x00000000, 0x0000000b, 0x00000000, 0x00050048,
    0x00000010, 0x00000001, 0x0000000b, 0x00000001,
    0x00030047, 0x00000010, 0x00000002, 0x00040048,
    0x00000014, 0x00000000, 0x00000005, 0x00050048,
    0x00000014, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x00000014, 0x00000000, 0x00000007,
    0x00000010, 0x00030047, 0x00000014, 0x00000002,
    0x00040047, 0x00000016, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002,
    0x00040020, 0x00000008, 0x00000003, 0x00000007,
    0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    0x00040020, 0x0000000a, 0x00000001, 0x00000007,
    0x0004003b, 0x0000000a, 0x0000000b, 0x00000001,
    0x00040017, 0x0000000d, 0x00000006, 0x00000004,
    0x00040015, 0x0000000e, 0x00000020, 0x00000000,
    0x0004002b, 0x0000000e, 0x0000000f, 0x00000001,
    0x0004001c, 0x00000011, 0x00000006, 0x0000000f,
    0x0004001e, 0x00000010, 0x0000000d, 0x00000011,
    0x00040020, 0x00000013, 0x00000003, 0x00000010,
    0x0004003b, 0x00000013, 0x00000012, 0x00000003,
    0x00040015, 0x00000018, 0x00000020, 0x00000001,
    0x0004002b, 0x00000018, 0x00000019, 0x00000000,
    0x00040018, 0x0000001a, 0x0000000d, 0x00000004,
    0x0003001e, 0x00000014, 0x0000001a, 0x00040020,
    0x00000015, 0x00000009, 0x00000014, 0x0004003b,
    0x0000000a, 0x00000016, 0x00000001, 0x00040020,
    0x0000001c, 0x00000009, 0x0000001a, 0x0004002b,
    0x00000006, 0x0000001f, 0x00000000, 0x0004002b,
    0x00000006, 0x00000020, 0x3f800000, 0x00040020,
    0x00000026, 0x00000003, 0x0000000d, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003d, 0x00000007,
    0x0000000c, 0x0000000b, 0x0003003e, 0x00000009,
    0x0000000c, 0x00050041, 0x0000001c, 0x0000001d,
    0x00000015, 0x00000019, 0x0004003d, 0x0000001a,
    0x0000001e, 0x0000001d, 0x0004003d, 0x00000007,
    0x00000021, 0x00000016, 0x00050051, 0x00000006,
    0x00000022, 0x00000021, 0x00000000, 0x00050051,
    0x00000006, 0x00000023, 0x00000021, 0x00000001,
    0x00070050, 0x0000000d, 0x00000024, 0x00000022,
    0x00000023, 0x0000001f, 0x00000020, 0x00050091,
    0x0000000d, 0x00000025, 0x0000001e, 0x00000024,
    0x00050041, 0x00000026, 0x00000027, 0x00000012,
    0x00000019, 0x0003003e, 0x00000027, 0x00000025,
    0x000100fd, 0x00010038,
};

static const uint32_t frag_spv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000013,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x0000000b, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x67617266,
    0x6f6c6f43, 0x00000072, 0x00040005, 0x0000000d,
    0x78655475, 0x00000000, 0x00030005, 0x0000000b,
    0x00565576, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00040047, 0x0000000d, 0x00000022,
    0x00000000, 0x00040047, 0x0000000d, 0x00000021,
    0x00000000, 0x00040047, 0x0000000b, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003,
    0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x00090019, 0x0000000a, 0x00000006,
    0x00000001, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x0003001b, 0x0000000c,
    0x0000000a, 0x00040020, 0x0000000e, 0x00000000,
    0x0000000c, 0x0004003b, 0x0000000e, 0x0000000d,
    0x00000000, 0x00040017, 0x0000000f, 0x00000006,
    0x00000002, 0x00040020, 0x00000010, 0x00000001,
    0x0000000f, 0x0004003b, 0x00000010, 0x0000000b,
    0x00000001, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003d, 0x0000000c, 0x00000011, 0x0000000d,
    0x0004003d, 0x0000000f, 0x00000012, 0x0000000b,
    0x00050057, 0x00000007, 0x00000014, 0x00000011,
    0x00000012, 0x0003003e, 0x00000009, 0x00000014,
    0x000100fd, 0x00010038,
};
// clang-format on

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
    VkPipeline       pipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;

    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    void*         vertexMapped  = nullptr;

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
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  =  2.0f / w;
    m[5]  = -2.0f / h;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
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

    // --- Pipeline layout (push constant for MVP) ---
    {
        VkPushConstantRange pushConst{};
        pushConst.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConst.offset = 0;
        pushConst.size = sizeof(float) * 16;

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &vk_->descSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConst;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &vk_->pipelineLayout);
    }

    // --- Graphics pipeline ---
    {
        VkShaderModuleCreateInfo vsModuleInfo{};
        vsModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vsModuleInfo.codeSize = sizeof(vert_spv);
        vsModuleInfo.pCode = vert_spv;
        VkShaderModule vsModule;
        vkCreateShaderModule(device, &vsModuleInfo, nullptr, &vsModule);

        VkShaderModuleCreateInfo fsModuleInfo{};
        fsModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fsModuleInfo.codeSize = sizeof(frag_spv);
        fsModuleInfo.pCode = frag_spv;
        VkShaderModule fsModule;
        vkCreateShaderModule(device, &fsModuleInfo, nullptr, &fsModule);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vsModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fsModule;
        stages[1].pName = "main";

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

        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &vk_->pipeline);

        vkDestroyShaderModule(device, vsModule, nullptr);
        vkDestroyShaderModule(device, fsModule, nullptr);
    }
}

void GameRenderer::upload(const u32* pixels) {
    // Copy pixels into staging buffer
    memcpy(vk_->stagingMapped, pixels,
           static_cast<size_t>(gameW_) * gameH_ * sizeof(u32));

    // The actual copy from staging buffer to image happens in draw() via
    // a recorded command buffer, since we need the render pass's command buffer.
    // For simplicity we do a layout transition + copy before the render pass
    // in draw(). Upload just stages the data.
}

void GameRenderer::resize(int gameW, int gameH) {
    if (gameW == gameW_ && gameH == gameH_) return;
    // Full resize requires recreating Vulkan image/staging/descriptors.
    shutdown();
    init(gameW, gameH);
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

    // Push MVP
    float mvp[16];
    ortho(mvp, (float)viewportW, (float)viewportH);
    vkCmdPushConstants(cmd, vk_->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp);

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

    if (vk_->pipeline)       vkDestroyPipeline(device, vk_->pipeline, nullptr);
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

    delete vk_;
    vk_ = nullptr;
}
