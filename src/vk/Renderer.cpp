#include "vk/Renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <cstring>
#include <stdexcept>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <glm/gtc/matrix_transform.hpp>

#include "core/Camera.hpp"
#include "core/Log.hpp"
#include "core/Window.hpp"
#include "scene/Animator.hpp"
#include "scene/Scene.hpp"

namespace mv::gfx {

struct GridVertex
{
    glm::vec3 position;
    glm::vec4 color;
};

namespace {

// Material flag bits; keep in sync with shaders/common.glsl.
constexpr int kFlagBaseColorTex = 1;
constexpr int kFlagNormalTex    = 2;
constexpr int kFlagMrTex        = 4;
constexpr int kFlagEmissiveTex  = 8;
constexpr int kFlagUnlit        = 16;
constexpr int kFlagMasked       = 32;

struct GpuLight
{
    glm::vec4 positionType{0.0f};
    glm::vec4 directionRange{0.0f, -1.0f, 0.0f, 0.0f};
    glm::vec4 colorIntensity{1.0f};
    glm::vec4 cone{1.0f, 0.9f, 0.0f, 0.0f};
};

struct GpuGlobals
{
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 viewProj{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 ambient{1.0f};
    glm::vec4 params{0.0f};
    // Must match GlobalsBlock in shaders/common.glsl, field for field.
    glm::vec4 skyZenith{0.0f};
    glm::vec4 skyHorizon{0.0f};
    glm::vec4 skyGround{0.0f};
    glm::mat4 lightViewProj{1.0f};
    glm::vec4 shadowParams{0.0f};
    GpuLight  lights[kMaxLights]{};
};

VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage, VkShaderModule module)
{
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage  = stage;
    info.module = module;
    info.pName  = "main";
    return info;
}

} // namespace

const char* lightingModeName(LightingMode mode)
{
    switch (mode)
    {
        case LightingMode::Scene: return "Scene lights";
        case LightingMode::Unlit: return "Unlit (baked textures)";
        default:                  return "unknown";
    }
}

const char* debugViewName(DebugView view)
{
    switch (view)
    {
        case DebugView::Shaded:    return "Shaded";
        case DebugView::BaseColor: return "Base colour";
        case DebugView::Normals:   return "Normals";
        case DebugView::TexCoords: return "Texture coordinates";
        case DebugView::Metallic:  return "Metallic";
        case DebugView::Roughness: return "Roughness";
        default:                   return "?";
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

Renderer::~Renderer() { shutdown(); }

void Renderer::init(Context& context, Window& window)
{
    m_context = &context;
    m_window  = &window;

    createShadowResources(2048);
    createDescriptorLayouts();
    createDescriptorPool();
    createFrameResources();

    // Shared trilinear sampler; anisotropy when the device offers it.
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod       = VK_LOD_CLAMP_NONE;
    samplerInfo.anisotropyEnable = context.anisotropySupported() ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy    = std::min(16.0f, context.maxAnisotropy());
    check(vkCreateSampler(context.device(), &samplerInfo, nullptr, &m_sampler), "vkCreateSampler");

    m_whiteTexture  = createSolidTexture(context, 255, 255, 255, 255, true);
    m_normalTexture = createSolidTexture(context, 128, 128, 255, 255, false);
    m_blackTexture  = createSolidTexture(context, 0, 0, 0, 255, false);

    createPipelines();

    m_gpuScene.fallbackSet = allocateMaterialSet(
        {m_whiteTexture.view, m_normalTexture.view, m_whiteTexture.view, m_blackTexture.view});

    // -- Dear ImGui --------------------------------------------------------
    {
        const std::array<VkDescriptorPoolSize, 1> sizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128}}};

        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets       = 128;
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes    = sizes.data();
        check(vkCreateDescriptorPool(context.device(), &poolInfo, nullptr, &m_imguiPool),
              "vkCreateDescriptorPool(imgui)");
    }

    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);

    VkFormat colorFormat = context.swapchain().format;

    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = context.depthFormat();

    ImGui_ImplVulkan_InitInfo imguiInfo{};
    imguiInfo.Instance            = context.instance();
    imguiInfo.PhysicalDevice      = context.physicalDevice();
    imguiInfo.Device              = context.device();
    imguiInfo.QueueFamily         = context.graphicsQueueFamily();
    imguiInfo.Queue               = context.graphicsQueue();
    imguiInfo.DescriptorPool      = m_imguiPool;
    imguiInfo.MinImageCount       = std::max(2u, context.swapchain().imageCount());
    imguiInfo.ImageCount          = std::max(2u, context.swapchain().imageCount());
    imguiInfo.MSAASamples         = context.sampleCount();
    imguiInfo.UseDynamicRendering = true;
    imguiInfo.PipelineRenderingCreateInfo = renderingInfo;
    imguiInfo.CheckVkResultFn     = [](VkResult result) {
        if (result != VK_SUCCESS) log::error("[imgui] Vulkan error: ", resultString(result));
    };

    if (!ImGui_ImplVulkan_Init(&imguiInfo))
        throw std::runtime_error("Failed to initialise the Dear ImGui Vulkan backend");

    m_imguiInitialised = true;
    log::info("Renderer ready");
}

void Renderer::shutdown()
{
    if (!m_context) return;

    m_context->waitIdle();

    if (m_imguiInitialised)
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        m_imguiInitialised = false;
    }

    clearScene();

    destroyShadowResources();
    destroyImage(*m_context, m_whiteTexture);
    destroyImage(*m_context, m_normalTexture);
    destroyImage(*m_context, m_blackTexture);

    if (m_sampler) vkDestroySampler(m_context->device(), m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;

    destroyPipelines();

    for (FrameData& frame : m_frames)
    {
        destroyBuffer(*m_context, frame.globals);
        destroyBuffer(*m_context, frame.bones);
        if (frame.inFlight)       vkDestroyFence(m_context->device(), frame.inFlight, nullptr);
        if (frame.imageAvailable) vkDestroySemaphore(m_context->device(), frame.imageAvailable, nullptr);
        if (frame.commandPool)    vkDestroyCommandPool(m_context->device(), frame.commandPool, nullptr);
        frame = {};
    }

    for (VkSemaphore semaphore : m_renderFinished)
        vkDestroySemaphore(m_context->device(), semaphore, nullptr);
    m_renderFinished.clear();

    if (m_descriptorPool) vkDestroyDescriptorPool(m_context->device(), m_descriptorPool, nullptr);
    if (m_imguiPool)      vkDestroyDescriptorPool(m_context->device(), m_imguiPool, nullptr);
    if (m_globalLayout)   vkDestroyDescriptorSetLayout(m_context->device(), m_globalLayout, nullptr);
    if (m_materialLayout) vkDestroyDescriptorSetLayout(m_context->device(), m_materialLayout, nullptr);

    m_descriptorPool = VK_NULL_HANDLE;
    m_imguiPool      = VK_NULL_HANDLE;
    m_globalLayout   = VK_NULL_HANDLE;
    m_materialLayout = VK_NULL_HANDLE;

    m_context = nullptr;
    m_window  = nullptr;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void Renderer::createShadowResources(uint32_t resolution)
{
    destroyShadowResources();
    m_shadowExtent = resolution;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_D32_SFLOAT;
        info.extent        = {resolution, resolution, 1};
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;   // never multisampled
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc{};
        alloc.usage         = VMA_MEMORY_USAGE_AUTO;
        alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        check(vmaCreateImage(m_context->allocator(), &info, &alloc,
                             &m_shadowMaps[i].handle, &m_shadowMaps[i].allocation, nullptr),
              "vmaCreateImage(shadow)");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image                       = m_shadowMaps[i].handle;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        check(vkCreateImageView(m_context->device(), &viewInfo, nullptr, &m_shadowMaps[i].view),
              "vkCreateImageView(shadow)");

        m_shadowMaps[i].extent = {resolution, resolution};
        m_shadowMaps[i].format = VK_FORMAT_D32_SFLOAT;
    }

    // A depth-compare sampler: the hardware performs the comparison and then
    // bilinearly filters the 0/1 results, so a single fetch is already 2x2 PCF.
    // Clamping to a white border means anything outside the map reads as lit,
    // which is what you want beyond the shadow frustum.
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter    = VK_FILTER_LINEAR;
    sampler.minFilter    = VK_FILTER_LINEAR;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
    sampler.maxLod        = 0.0f;

    check(vkCreateSampler(m_context->device(), &sampler, nullptr, &m_shadowSampler),
          "vkCreateSampler(shadow)");
}

void Renderer::destroyShadowResources()
{
    if (!m_context) return;

    for (Image& image : m_shadowMaps)
    {
        if (image.view) vkDestroyImageView(m_context->device(), image.view, nullptr);
        if (image.handle)
            vmaDestroyImage(m_context->allocator(), image.handle, image.allocation);
        image = Image{};
    }

    if (m_shadowSampler)
    {
        vkDestroySampler(m_context->device(), m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
    m_shadowExtent = 0;
}

void Renderer::createDescriptorLayouts()
{
    const std::array<VkDescriptorSetLayoutBinding, 3> globalBindings{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};

    VkDescriptorSetLayoutCreateInfo globalInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    globalInfo.bindingCount = static_cast<uint32_t>(globalBindings.size());
    globalInfo.pBindings    = globalBindings.data();
    check(vkCreateDescriptorSetLayout(m_context->device(), &globalInfo, nullptr, &m_globalLayout),
          "vkCreateDescriptorSetLayout(global)");

    std::array<VkDescriptorSetLayoutBinding, 4> materialBindings{};
    for (uint32_t i = 0; i < materialBindings.size(); ++i)
        materialBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo materialInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    materialInfo.bindingCount = static_cast<uint32_t>(materialBindings.size());
    materialInfo.pBindings    = materialBindings.data();
    check(vkCreateDescriptorSetLayout(m_context->device(), &materialInfo, nullptr, &m_materialLayout),
          "vkCreateDescriptorSetLayout(material)");
}

void Renderer::createDescriptorPool()
{
    constexpr uint32_t kMaxMaterials = 1024;

    const std::array<VkDescriptorPoolSize, 3> sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterials * 4 + kFramesInFlight}}};

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = kMaxMaterials + kFramesInFlight + 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes    = sizes.data();

    check(vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &m_descriptorPool),
          "vkCreateDescriptorPool");
}

void Renderer::createFrameResources()
{
    // Indexed rather than range-based: each frame needs its own shadow map,
    // for the same reason the depth buffer is per-frame.
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        FrameData& frame = m_frames[i];

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_context->graphicsQueueFamily();
        check(vkCreateCommandPool(m_context->device(), &poolInfo, nullptr, &frame.commandPool),
              "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool        = frame.commandPool;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(m_context->device(), &allocInfo, &frame.commandBuffer),
              "vkAllocateCommandBuffers");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(m_context->device(), &fenceInfo, nullptr, &frame.inFlight),
              "vkCreateFence");

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(m_context->device(), &semaphoreInfo, nullptr, &frame.imageAvailable),
              "vkCreateSemaphore");

        frame.globals = createHostBuffer(*m_context, sizeof(GpuGlobals),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        frame.bones   = createHostBuffer(*m_context, sizeof(glm::mat4) * 256,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool     = m_descriptorPool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts        = &m_globalLayout;
        check(vkAllocateDescriptorSets(m_context->device(), &setInfo, &frame.globalSet),
              "vkAllocateDescriptorSets(global)");

        VkDescriptorBufferInfo globalBuffer{frame.globals.handle, 0, sizeof(GpuGlobals)};
        VkDescriptorBufferInfo boneBuffer{frame.bones.handle, 0, VK_WHOLE_SIZE};

        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler     = m_shadowSampler;
        shadowInfo.imageView   = m_shadowMaps[i].view;
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0]                 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet          = frame.globalSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &globalBuffer;

        writes[1]                 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet          = frame.globalSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo     = &boneBuffer;

        writes[2]                 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet          = frame.globalSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &shadowInfo;

        vkUpdateDescriptorSets(m_context->device(),
                               static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    m_renderFinished.resize(m_context->swapchain().imageCount());
    for (VkSemaphore& semaphore : m_renderFinished)
    {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(m_context->device(), &info, nullptr, &semaphore),
              "vkCreateSemaphore(present)");
    }
}

void Renderer::createPipelines()
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size       = sizeof(PushConstants);

    static_assert(sizeof(PushConstants) <= 128,
                  "Push constants must fit the guaranteed minimum of 128 bytes");

    const std::array<VkDescriptorSetLayout, 2> layouts{m_globalLayout, m_materialLayout};

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount         = static_cast<uint32_t>(layouts.size());
    layoutInfo.pSetLayouts            = layouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    check(vkCreatePipelineLayout(m_context->device(), &layoutInfo, nullptr, &m_pipelineLayout),
          "vkCreatePipelineLayout");

    VkShaderModule meshVert = loadShaderModule(*m_context, "mesh.vert");
    VkShaderModule meshFrag = loadShaderModule(*m_context, "mesh.frag");
    VkShaderModule gridVert = loadShaderModule(*m_context, "grid.vert");
    VkShaderModule gridFrag = loadShaderModule(*m_context, "grid.frag");
    VkShaderModule shadowVert = loadShaderModule(*m_context, "shadow.vert");
    VkShaderModule skyVert  = loadShaderModule(*m_context, "sky.vert");
    VkShaderModule skyFrag  = loadShaderModule(*m_context, "sky.frag");
    VkShaderModule axesVert = loadShaderModule(*m_context, "axes.vert");
    VkShaderModule axesFrag = loadShaderModule(*m_context, "axes.frag");

    // -- shared state ------------------------------------------------------
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const std::array<VkVertexInputAttributeDescription, 6> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Vertex, uv)},
        {4, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(Vertex, joints)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, weights)}}};

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions    = attributes.data();

    VkVertexInputBindingDescription gridBinding{};
    gridBinding.binding   = 0;
    gridBinding.stride    = sizeof(GridVertex);
    gridBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    const std::array<VkVertexInputAttributeDescription, 2> gridAttributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(GridVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GridVertex, color)}}};

    VkPipelineVertexInputStateCreateInfo gridVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    gridVertexInput.vertexBindingDescriptionCount   = 1;
    gridVertexInput.pVertexBindingDescriptions      = &gridBinding;
    gridVertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(gridAttributes.size());
    gridVertexInput.pVertexAttributeDescriptions    = gridAttributes.data();

    VkPipelineVertexInputStateCreateInfo emptyVertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = m_context->sampleCount();

    const std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    VkFormat colorFormat = m_context->swapchain().format;
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = m_context->depthFormat();

    auto makePipeline = [&](VkShaderModule vert, VkShaderModule frag,
                            const VkPipelineVertexInputStateCreateInfo& vi,
                            VkCullModeFlags cullMode, VkPolygonMode polygonMode,
                            bool depthWrite, bool blend,
                            float depthBias = 0.0f,
                            VkPrimitiveTopology topology =
                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                            bool depthTest = true) -> VkPipeline
    {
        const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
            shaderStage(VK_SHADER_STAGE_VERTEX_BIT, vert),
            shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, frag)};

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = polygonMode;
        raster.cullMode    = cullMode;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        // A negative bias pulls fragments towards the viewer, which is how the
        // axis arrows win against the grid plane they are coplanar with.
        if (depthBias != 0.0f)
        {
            raster.depthBiasEnable         = VK_TRUE;
            raster.depthBiasConstantFactor = depthBias;
            raster.depthBiasSlopeFactor    = depthBias;
        }

        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable  = depthTest ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencil.maxDepthBounds   = 1.0f;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable         = blend ? VK_TRUE : VK_FALSE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &blendAttachment;

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext               = &renderingInfo;
        info.stageCount          = static_cast<uint32_t>(stages.size());
        info.pStages             = stages.data();
        info.pVertexInputState   = &vi;
        VkPipelineInputAssemblyStateCreateInfo assembly = inputAssembly;
        assembly.topology = topology;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depthStencil;
        info.pColorBlendState    = &colorBlend;
        info.pDynamicState       = &dynamicState;
        info.layout              = m_pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &info,
                                        nullptr, &pipeline),
              "vkCreateGraphicsPipelines");
        return pipeline;
    };

    m_meshPipelines[0][0] = makePipeline(meshVert, meshFrag, vertexInput,
                                         VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, true, false);
    m_meshPipelines[1][0] = makePipeline(meshVert, meshFrag, vertexInput,
                                         VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, true, false);
    m_meshPipelines[0][1] = makePipeline(meshVert, meshFrag, vertexInput,
                                         VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true);
    m_meshPipelines[1][1] = makePipeline(meshVert, meshFrag, vertexInput,
                                         VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, true);

    m_wireframePipeline = makePipeline(meshVert, meshFrag, vertexInput,
                                       VK_CULL_MODE_NONE, VK_POLYGON_MODE_LINE, true, false);

    {
        // Depth-only, so no colour attachment and no fragment shader. A
        // slope-scaled depth bias here is the standard defence against shadow
        // acne on surfaces nearly parallel to the light; the shader adds a
        // normal offset on top for curved geometry.
        VkPipelineRenderingCreateInfo shadowRendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        shadowRendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

        VkPipelineShaderStageCreateInfo stage =
            shaderStage(VK_SHADER_STAGE_VERTEX_BIT, shadowVert);

        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode             = VK_POLYGON_MODE_FILL;
        raster.cullMode                = VK_CULL_MODE_NONE;
        raster.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth               = 1.0f;
        raster.depthBiasEnable         = VK_TRUE;
        raster.depthBiasConstantFactor = 1.5f;
        raster.depthBiasSlopeFactor    = 2.5f;

        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable  = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencil.maxDepthBounds   = 1.0f;

        VkPipelineColorBlendStateCreateInfo blendState{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext               = &shadowRendering;
        info.stageCount          = 1;
        info.pStages             = &stage;
        info.pVertexInputState   = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depthStencil;
        info.pColorBlendState    = &blendState;
        info.pDynamicState       = &dynamicState;
        info.layout              = m_pipelineLayout;

        check(vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &info,
                                        nullptr, &m_shadowPipeline),
              "vkCreateGraphicsPipelines(shadow)");
    }

    // The sky is drawn first with neither depth test nor depth write, so it
    // fills the background and everything else simply covers it. No need to
    // push it to the far plane or fight precision there.
    m_skyPipeline = makePipeline(skyVert, skyFrag, emptyVertexInput,
                                 VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false,
                                 0.0f, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false);

    // Line list, blended, depth tested but not depth writing: the grid is an
    // overlay on the ground plane, not an occluder.
    m_gridPipeline = makePipeline(gridVert, gridFrag, gridVertexInput,
                                  VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, true,
                                  0.0f, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);

    // Opaque and depth-writing: the arrows should be occluded by geometry
    // that sits in front of them, exactly like any other solid object.
    m_axesPipeline = makePipeline(axesVert, axesFrag, emptyVertexInput,
                                  VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, true, false);

    vkDestroyShaderModule(m_context->device(), meshVert, nullptr);
    vkDestroyShaderModule(m_context->device(), meshFrag, nullptr);
    vkDestroyShaderModule(m_context->device(), gridVert, nullptr);
    vkDestroyShaderModule(m_context->device(), gridFrag, nullptr);
    vkDestroyShaderModule(m_context->device(), shadowVert, nullptr);
    vkDestroyShaderModule(m_context->device(), skyVert, nullptr);
    vkDestroyShaderModule(m_context->device(), skyFrag, nullptr);
    vkDestroyShaderModule(m_context->device(), axesVert, nullptr);
    vkDestroyShaderModule(m_context->device(), axesFrag, nullptr);
}

void Renderer::destroyPipelines()
{
    if (!m_context || !m_context->device()) return;

    for (auto& row : m_meshPipelines)
        for (VkPipeline& pipeline : row)
            if (pipeline) { vkDestroyPipeline(m_context->device(), pipeline, nullptr); pipeline = VK_NULL_HANDLE; }

    if (m_wireframePipeline) vkDestroyPipeline(m_context->device(), m_wireframePipeline, nullptr);
    if (m_gridMesh) destroyBuffer(*m_context, m_gridMesh);
    m_gridVertexCount = 0;
    m_gridBuiltHalf   = -1;
    m_gridBuiltCell   = -1.0f;

    if (m_gridPipeline)      vkDestroyPipeline(m_context->device(), m_gridPipeline, nullptr);
    if (m_axesPipeline)      vkDestroyPipeline(m_context->device(), m_axesPipeline, nullptr);
    if (m_skyPipeline)       vkDestroyPipeline(m_context->device(), m_skyPipeline, nullptr);
    if (m_shadowPipeline)    vkDestroyPipeline(m_context->device(), m_shadowPipeline, nullptr);
    if (m_pipelineLayout)    vkDestroyPipelineLayout(m_context->device(), m_pipelineLayout, nullptr);

    m_wireframePipeline = VK_NULL_HANDLE;
    m_gridPipeline      = VK_NULL_HANDLE;
    m_axesPipeline      = VK_NULL_HANDLE;
    m_skyPipeline       = VK_NULL_HANDLE;
    m_shadowPipeline    = VK_NULL_HANDLE;
    m_pipelineLayout    = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Scene upload
// ---------------------------------------------------------------------------

VkDescriptorSet Renderer::allocateMaterialSet(const std::vector<VkImageView>& views)
{
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_materialLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(m_context->device(), &allocInfo, &set),
          "vkAllocateDescriptorSets(material)");

    std::array<VkDescriptorImageInfo, 4> images{};
    std::array<VkWriteDescriptorSet, 4>  writes{};

    for (size_t i = 0; i < images.size(); ++i)
    {
        images[i].sampler     = m_sampler;
        images[i].imageView   = (i < views.size() && views[i]) ? views[i] : m_whiteTexture.view;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i]                 = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet          = set;
        writes[i].dstBinding      = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = &images[i];
    }

    vkUpdateDescriptorSets(m_context->device(),
                           static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return set;
}

void Renderer::clearScene()
{
    if (!m_context) return;

    m_context->waitIdle();

    destroyBuffer(*m_context, m_gpuScene.vertexBuffer);
    destroyBuffer(*m_context, m_gpuScene.indexBuffer);

    for (Image& texture : m_gpuScene.textures)
        destroyImage(*m_context, texture);
    m_gpuScene.textures.clear();

    if (!m_gpuScene.materialSets.empty())
    {
        vkFreeDescriptorSets(m_context->device(), m_descriptorPool,
                             static_cast<uint32_t>(m_gpuScene.materialSets.size()),
                             m_gpuScene.materialSets.data());
        m_gpuScene.materialSets.clear();
    }

    m_gpuScene.indexCount    = 0;
    m_gpuScene.triangleCount = 0;

    m_opaqueDraws.clear();
    m_blendedDraws.clear();
}

void Renderer::uploadScene(const Scene& scene)
{
    clearScene();

    if (scene.vertices.empty() || scene.indices.empty())
    {
        log::warn("Scene has no geometry to upload");
        return;
    }

    m_gpuScene.vertexBuffer = createDeviceBuffer(
        *m_context, scene.vertices.data(),
        sizeof(Vertex) * scene.vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_gpuScene.indexBuffer = createDeviceBuffer(
        *m_context, scene.indices.data(),
        sizeof(uint32_t) * scene.indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    m_gpuScene.indexCount    = static_cast<uint32_t>(scene.indices.size());
    m_gpuScene.triangleCount = m_gpuScene.indexCount / 3;

    // Textures.
    m_gpuScene.textures.reserve(scene.textures.size());
    for (const TextureData& texture : scene.textures)
    {
        try
        {
            m_gpuScene.textures.push_back(createTexture2D(*m_context, texture));
        }
        catch (const std::exception& e)
        {
            log::warn("Texture upload failed (", texture.name, "): ", e.what());
            m_gpuScene.textures.push_back({});
        }
    }

    auto viewFor = [&](int index, VkImageView fallback) -> VkImageView {
        if (index < 0 || index >= static_cast<int>(m_gpuScene.textures.size())) return fallback;
        const Image& image = m_gpuScene.textures[static_cast<size_t>(index)];
        return image.view ? image.view : fallback;
    };

    m_gpuScene.materialSets.reserve(scene.materials.size());
    for (const Material& material : scene.materials)
    {
        m_gpuScene.materialSets.push_back(allocateMaterialSet({
            viewFor(material.baseColorTexture,  m_whiteTexture.view),
            viewFor(material.normalTexture,     m_normalTexture.view),
            viewFor(material.metalRoughTexture, m_whiteTexture.view),
            viewFor(material.emissiveTexture,   m_whiteTexture.view)}));
    }

    // Resize the per-frame bone buffers if the new skeleton needs more slots.
    const VkDeviceSize required = sizeof(glm::mat4) * std::max<uint32_t>(scene.totalBoneSlots, 1);
    for (FrameData& frame : m_frames)
    {
        if (frame.bones.size >= required) continue;

        destroyBuffer(*m_context, frame.bones);
        frame.bones = createHostBuffer(*m_context, required, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        VkDescriptorBufferInfo bufferInfo{frame.bones.handle, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet          = frame.globalSet;
        write.dstBinding      = 1;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &bufferInfo;
        vkUpdateDescriptorSets(m_context->device(), 1, &write, 0, nullptr);
    }

    log::info("Uploaded ", scene.vertices.size(), " vertices / ",
              m_gpuScene.triangleCount, " triangles to the GPU");
}

// ---------------------------------------------------------------------------
// Per-frame data
// ---------------------------------------------------------------------------

void Renderer::updateGlobals(const Scene* scene, const Camera& camera, const RenderSettings& settings)
{
    GpuGlobals globals{};
    globals.view      = camera.view();
    globals.proj      = camera.projection(aspectRatio());
    globals.viewProj  = globals.proj * globals.view;
    globals.cameraPos = glm::vec4(camera.position(), 1.0f);
    globals.ambient = glm::vec4(settings.ambientColor, settings.ambientIntensity);

    // The shaders evaluate the sky gradient themselves -- for the background,
    // and per-normal for ambient -- so they need the parameters rather than a
    // pre-averaged colour. skyGround.a doubles as the "use this for ambient"
    // switch, which keeps the branch uniform across a draw.
    const bool skyAmbient = settings.showSky && settings.skyDrivesAmbient;
    globals.skyZenith  = glm::vec4(settings.skyZenith,  settings.skyIntensity);
    globals.skyHorizon = glm::vec4(settings.skyHorizon, settings.skyTightness);
    globals.skyGround  = glm::vec4(settings.skyGround,  skyAmbient ? 1.0f : 0.0f);

    uint32_t lightCount = 0;

    auto addLight = [&](const glm::vec3& position, const glm::vec3& direction, LightType type,
                        const glm::vec3& color, float intensity, float range,
                        float innerCone, float outerCone)
    {
        if (lightCount >= kMaxLights) return;

        GpuLight& light = globals.lights[lightCount++];
        light.positionType   = glm::vec4(position, static_cast<float>(type));
        light.directionRange = glm::vec4(glm::normalize(direction), range);
        light.colorIntensity = glm::vec4(color, intensity * settings.lightIntensityScale);
        light.cone           = glm::vec4(std::cos(innerCone), std::cos(outerCone), 0.0f, 0.0f);
    };

    // Nothing is lit in unlit mode, so neither the rig nor the imported lights
    // are worth assembling -- and with no caster the shadow pass is skipped too.
    const bool unlitMode  = settings.lightingMode == LightingMode::Unlit;
    const bool useImported = !unlitMode && settings.useSceneLights &&
                             scene && !scene->lights.empty();

    if (useImported)
    {
        const std::vector<glm::mat4> globalTransforms = scene->computeGlobalTransforms();

        for (const Light& light : scene->lights)
        {
            glm::vec3 position  = light.position;
            glm::vec3 direction = light.direction;

            if (light.node != kInvalidIndex &&
                light.node < static_cast<int>(globalTransforms.size()))
            {
                const glm::mat4& world = globalTransforms[static_cast<size_t>(light.node)];
                position  = glm::vec3(world * glm::vec4(light.position, 1.0f));
                direction = glm::normalize(glm::mat3(world) * light.direction);
            }

            if (glm::length(direction) < 1e-5f) direction = glm::vec3(0.0f, -1.0f, 0.0f);

            addLight(position, direction, light.type, light.color, light.intensity,
                     light.range, light.innerCone, light.outerCone);
        }
    }
    else
    {
        // Default three-point rig, anchored to the model so it scales with it.
        const float radius = (scene && scene->bounds.valid()) ? std::max(scene->bounds.radius(), 0.1f) : 1.0f;
        const glm::vec3 center = (scene && scene->bounds.valid()) ? scene->bounds.center() : glm::vec3(0.0f);
        const float intensity = radius * radius * 4.0f;

        addLight(center + glm::vec3( 1.6f,  2.0f,  1.4f) * radius, glm::vec3(-1.0f, -1.0f, -1.0f),
                 LightType::Point, glm::vec3(1.0f, 0.97f, 0.92f), intensity, 0.0f, 0.0f, 0.0f);
        addLight(center + glm::vec3(-2.0f,  0.8f,  1.0f) * radius, glm::vec3( 1.0f, -0.4f, -1.0f),
                 LightType::Point, glm::vec3(0.75f, 0.82f, 1.0f), intensity * 0.45f, 0.0f, 0.0f, 0.0f);
        addLight(center + glm::vec3( 0.2f,  0.6f, -2.2f) * radius, glm::vec3( 0.0f, -0.3f,  1.0f),
                 LightType::Point, glm::vec3(1.0f, 0.9f, 0.8f), intensity * 0.35f, 0.0f, 0.0f, 0.0f);
    }

    // Fit an orthographic frustum to the scene for the first directional light.
    //
    // Sized from the bounding sphere rather than the box so the extent does not
    // change as the camera orbits -- a frustum that resizes makes the shadow
    // edges crawl.
    m_shadowCaster  = 0;
    m_lightViewProj = glm::mat4(1.0f);

    if (settings.shadows && m_shadowExtent > 0 && lightCount > 0)
    {
        glm::vec3 centre{0.0f};
        float     radius = 1.0f;
        if (scene && scene->bounds.valid())
        {
            centre = scene->bounds.center();
            radius = glm::max(scene->bounds.radius(), 1e-3f);
        }

        // Prefer a real directional light. Falling back to the brightest of
        // whatever else is present matters because nothing guarantees a
        // directional light exists -- the built-in rig is three point lights,
        // and plenty of imported scenes have none either. A point light far
        // from a small model is close enough to directional for one shadow
        // map, and a shadow from roughly the right angle beats no shadow.
        int   caster    = -1;
        float bestScore = -1.0f;

        for (uint32_t i = 0; i < lightCount; ++i)
        {
            const bool directional =
                static_cast<int>(globals.lights[i].positionType.w) ==
                static_cast<int>(LightType::Directional);

            const float score = globals.lights[i].colorIntensity.a +
                                (directional ? 1000.0f : 0.0f);

            if (score > bestScore) { bestScore = score; caster = static_cast<int>(i); }
        }

        if (caster >= 0)
        {
            const GpuLight& light = globals.lights[static_cast<size_t>(caster)];
            const bool      directional =
                static_cast<int>(light.positionType.w) ==
                static_cast<int>(LightType::Directional);

            glm::vec3 direction = directional
                                      ? glm::vec3(light.directionRange)
                                      : (centre - glm::vec3(light.positionType));

            if (glm::length(direction) < 1e-5f) direction = glm::vec3(0.0f, -1.0f, 0.0f);
            direction = glm::normalize(direction);

            // Stand well clear of the scene so nothing clips the near plane.
            const glm::vec3 eye = centre - direction * (radius * 2.5f);

            // Any up vector will do except one parallel to the light.
            const glm::vec3 up = (std::abs(direction.y) > 0.99f)
                                     ? glm::vec3(0.0f, 0.0f, 1.0f)
                                     : glm::vec3(0.0f, 1.0f, 0.0f);

            const glm::mat4 lightView = glm::lookAt(eye, centre, up);
            const glm::mat4 lightProj = glm::ortho(-radius * 1.2f, radius * 1.2f,
                                                   -radius * 1.2f, radius * 1.2f,
                                                   0.0f, radius * 5.0f);

            m_lightViewProj = lightProj * lightView;
            m_shadowCaster  = static_cast<uint32_t>(caster) + 1;
        }
    }

    globals.lightViewProj = m_lightViewProj;
    globals.shadowParams  = glm::vec4(static_cast<float>(m_shadowCaster),
                                      settings.shadowDepthBias,
                                      settings.shadowNormalBias,
                                      m_shadowExtent > 0
                                          ? 1.0f / static_cast<float>(m_shadowExtent)
                                          : 0.0f);

    globals.params = glm::vec4(unlitMode ? 0.0f : static_cast<float>(lightCount),
                               static_cast<float>(static_cast<int>(settings.debugView)),
                               settings.exposure,
                               unlitMode ? 1.0f : 0.0f);

    std::memcpy(m_frames[m_frameIndex].globals.mapped(), &globals, sizeof(globals));
}

void Renderer::updateBones(const Animator* animator)
{
    FrameData& frame = m_frames[m_frameIndex];
    if (!frame.bones.mapped()) return;

    const size_t capacity = static_cast<size_t>(frame.bones.size / sizeof(glm::mat4));
    if (capacity == 0) return;

    if (!animator || animator->boneMatrices().empty())
    {
        const glm::mat4 identity(1.0f);
        std::memcpy(frame.bones.mapped(), &identity, sizeof(identity));
        return;
    }

    const std::vector<glm::mat4>& matrices = animator->boneMatrices();
    const size_t count = std::min(capacity, matrices.size());
    std::memcpy(frame.bones.mapped(), matrices.data(), count * sizeof(glm::mat4));
}

void Renderer::buildDrawList(const Scene*    scene,
                             const Animator* animator,
                             const Camera&   camera,
                             const RenderSettings& settings)
{
    m_opaqueDraws.clear();
    m_blendedDraws.clear();

    if (!scene || !m_gpuScene.valid()) return;

    // Prefer the animator's posed transforms; fall back to the static
    // hierarchy when there is no animator or it is out of sync.
    std::vector<glm::mat4>        fallbackGlobals;
    const std::vector<glm::mat4>* transforms = nullptr;

    if (animator && animator->globalTransforms().size() == scene->nodes.size())
    {
        transforms = &animator->globalTransforms();
    }
    else
    {
        fallbackGlobals = scene->computeGlobalTransforms();
        transforms      = &fallbackGlobals;
    }

    const glm::vec3 eye = camera.position();

    // Hidden nodes are dropped here rather than at draw time, so they also
    // stop casting shadows -- the shadow pass reuses this list.
    const std::vector<uint8_t> visible = scene->computeVisibility();

    for (size_t nodeIndex = 0; nodeIndex < scene->nodes.size(); ++nodeIndex)
    {
        const Node& node = scene->nodes[nodeIndex];

        if (nodeIndex < visible.size() && !visible[nodeIndex]) continue;

        for (uint32_t meshIndex : node.meshes)
        {
            if (meshIndex >= scene->meshes.size()) continue;

            const Mesh& mesh = scene->meshes[meshIndex];
            if (mesh.indexCount == 0) continue;

            const bool skinned = mesh.skin != kInvalidIndex;

            DrawItem item;
            item.firstIndex   = mesh.firstIndex;
            item.indexCount   = mesh.indexCount;
            item.vertexOffset = mesh.vertexOffset;
            item.push.model   = skinned ? glm::mat4(1.0f) : (*transforms)[nodeIndex];

            int   skinOffset = -1;
            if (skinned && mesh.skin < static_cast<int>(scene->skins.size()))
                skinOffset = static_cast<int>(scene->skins[static_cast<size_t>(mesh.skin)].gpuOffset);

            const Material* material = nullptr;
            if (mesh.material >= 0 && mesh.material < static_cast<int>(scene->materials.size()))
                material = &scene->materials[static_cast<size_t>(mesh.material)];

            int flags = 0;
            glm::vec4 baseColor{1.0f};
            float metallic  = settings.defaultMetallic;
            float roughness = settings.defaultRoughness;
            glm::vec3 emissive{0.0f};
            float cutoff = 0.5f;
            bool  blended = false;

            if (material)
            {
                baseColor = material->baseColorFactor;
                metallic  = material->metallicFactor;
                roughness = material->roughnessFactor;
                emissive  = material->emissiveFactor;
                cutoff    = material->alphaCutoff;

                const bool hasBaseColorTexture = material->baseColorTexture != kInvalidIndex &&
                                                 material->baseColorTexture < static_cast<int>(m_gpuScene.textures.size()) &&
                                                 m_gpuScene.textures[static_cast<size_t>(material->baseColorTexture)].view != VK_NULL_HANDLE;

                if (hasBaseColorTexture) flags |= kFlagBaseColorTex;
                if (material->normalTexture     != kInvalidIndex) flags |= kFlagNormalTex;
                if (material->metalRoughTexture != kInvalidIndex) flags |= kFlagMrTex;
                if (material->emissiveTexture   != kInvalidIndex) flags |= kFlagEmissiveTex;
                if (material->unlit)                              flags |= kFlagUnlit;
                if (material->alphaMode == AlphaMode::Mask)       flags |= kFlagMasked;

                blended = material->alphaMode == AlphaMode::Blend;

                // The colour wheel drives everything that has no base colour map.
                if (settings.overrideUntextured && !hasBaseColorTexture)
                {
                    baseColor = glm::vec4(settings.defaultColor, baseColor.a);
                    metallic  = settings.defaultMetallic;
                    roughness = settings.defaultRoughness;
                }

                item.doubleSided = material->doubleSided;
                item.materialSet = (mesh.material < static_cast<int>(m_gpuScene.materialSets.size()))
                                       ? m_gpuScene.materialSets[static_cast<size_t>(mesh.material)]
                                       : m_gpuScene.fallbackSet;
            }
            else
            {
                baseColor    = glm::vec4(settings.defaultColor, 1.0f);
                item.materialSet = m_gpuScene.fallbackSet;
            }

            if (!settings.backfaceCulling) item.doubleSided = true;

            item.push.baseColor = baseColor;
            item.push.mrfs      = {metallic, roughness, static_cast<float>(flags),
                                   static_cast<float>(skinOffset)};
            item.push.emissive  = glm::vec4(emissive, cutoff);

            const glm::vec3 worldCenter =
                glm::vec3(item.push.model * glm::vec4(mesh.bounds.center(), 1.0f));
            item.viewDepth = glm::length(worldCenter - eye);

            if (blended) m_blendedDraws.push_back(item);
            else         m_opaqueDraws.push_back(item);
        }
    }

    std::sort(m_blendedDraws.begin(), m_blendedDraws.end(),
              [](const DrawItem& a, const DrawItem& b) { return a.viewDepth > b.viewDepth; });
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

float Renderer::aspectRatio() const
{
    const VkExtent2D extent = m_context->swapchain().extent;
    if (extent.height == 0) return 1.0f;
    return static_cast<float>(extent.width) / static_cast<float>(extent.height);
}

VkExtent2D Renderer::viewportExtent() const
{
    return m_context->swapchain().extent;
}

void Renderer::recreateSwapchainIfNeeded()
{
    if (!m_swapchainDirty) return;

    const glm::uvec2 size = m_window->framebufferSize();
    if (size.x == 0 || size.y == 0) return;

    m_context->recreateSwapchain({size.x, size.y});

    // The number of swapchain images can change; rebuild the present semaphores.
    for (VkSemaphore semaphore : m_renderFinished)
        vkDestroySemaphore(m_context->device(), semaphore, nullptr);

    m_renderFinished.assign(m_context->swapchain().imageCount(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : m_renderFinished)
    {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(m_context->device(), &info, nullptr, &semaphore),
              "vkCreateSemaphore(present)");
    }

    m_swapchainDirty = false;
}

bool Renderer::beginFrame()
{
    if (m_window->isMinimised()) return false;

    recreateSwapchainIfNeeded();

    FrameData& frame = m_frames[m_frameIndex];

    check(vkWaitForFences(m_context->device(), 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
          "vkWaitForFences");

    const VkResult acquire = vkAcquireNextImageKHR(
        m_context->device(), m_context->swapchain().handle, UINT64_MAX,
        frame.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_swapchainDirty = true;
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
    {
        check(acquire, "vkAcquireNextImageKHR");
        return false;
    }

    check(vkResetFences(m_context->device(), 1, &frame.inFlight), "vkResetFences");
    check(vkResetCommandPool(m_context->device(), frame.commandPool, 0), "vkResetCommandPool");

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    m_frameStarted = true;
    return true;
}

void Renderer::endFrame(const Scene*          scene,
                        const Animator*       animator,
                        const Camera&         camera,
                        const RenderSettings& settings)
{
    if (!m_frameStarted)
    {
        ImGui::EndFrame();
        return;
    }
    m_frameStarted = false;

    ImGui::Render();

    updateGlobals(scene, camera, settings);
    updateBones(animator);
    buildDrawList(scene, animator, camera, settings);

    FrameData& frame = m_frames[m_frameIndex];
    VkCommandBuffer cmd = frame.commandBuffer;

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer");

    const SwapchainInfo& swapchain = m_context->swapchain();
    VkImage     targetImage = swapchain.images[m_imageIndex];
    VkImageView targetView  = swapchain.views[m_imageIndex];

    // Shadow map first: it is a separate render pass and its result is sampled
    // by the main pass, so it has to be fully written before that begins.
    recordShadowPass(cmd, settings);

    transitionImageLayout(cmd, targetImage, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transitionImageLayout(cmd, m_context->depthImage(m_frameIndex), VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    if (m_context->multisampled())
        transitionImageLayout(cmd, m_context->colorImage(m_frameIndex),
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (m_context->multisampled())
    {
        // Draw into the multisampled buffer; the resolve writes the averaged
        // result straight into the swapchain image, so no extra blit is
        // needed and the multisampled contents never have to be stored.
        colorAttachment.imageView          = m_context->colorView(m_frameIndex);
        colorAttachment.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
        colorAttachment.resolveImageView   = targetView;
        colorAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.storeOp            = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    else
    {
        colorAttachment.imageView = targetView;
        colorAttachment.storeOp   = VK_ATTACHMENT_STORE_OP_STORE;
    }
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.clearValue.color = {{settings.clearColor.r, settings.clearColor.g,
                                         settings.clearColor.b, 1.0f}};

    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView   = m_context->depthView(m_frameIndex);
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea           = {{0, 0}, swapchain.extent};
    renderingInfo.layerCount           = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments    = &colorAttachment;
    renderingInfo.pDepthAttachment     = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(swapchain.extent.width);
    viewport.height   = static_cast<float>(swapchain.extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, swapchain.extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &frame.globalSet, 0, nullptr);

    m_stats.drawCalls = 0;
    m_stats.triangles = 0;

    const float sceneRadius = (scene && scene->bounds.valid())
                                  ? scene->bounds.radius()
                                  : 1.0f;

    if (settings.showSky) recordSky(cmd, settings);

    recordScene(cmd, settings);

    if (settings.showGrid)
    {
        rebuildGridMesh(settings, sceneRadius);
        recordGrid(cmd, settings);
    }

    if (settings.showAxes) recordAxes(cmd, settings, sceneRadius);

    if (ImDrawData* drawData = ImGui::GetDrawData())
        ImGui_ImplVulkan_RenderDrawData(drawData, cmd);

    vkCmdEndRendering(cmd);

    transitionImageLayout(cmd, targetImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    // -- submit ------------------------------------------------------------
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = m_renderFinished[m_imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkCommandBufferSubmitInfo bufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    bufferInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount   = 1;
    submit.pWaitSemaphoreInfos      = &waitInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos    = &signalInfo;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &bufferInfo;

    check(vkQueueSubmit2(m_context->graphicsQueue(), 1, &submit, frame.inFlight),
          "vkQueueSubmit2");

    // -- present -----------------------------------------------------------
    VkSwapchainKHR swapchainHandle = swapchain.handle;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &m_renderFinished[m_imageIndex];
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchainHandle;
    present.pImageIndices      = &m_imageIndex;

    const VkResult result = vkQueuePresentKHR(m_context->graphicsQueue(), &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_swapchainDirty = true;
    else if (result != VK_SUCCESS)
        check(result, "vkQueuePresentKHR");

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

VkPipeline Renderer::pipelineFor(const RenderSettings& settings, bool doubleSided, bool blended) const
{
    if (settings.wireframe) return m_wireframePipeline;
    return m_meshPipelines[doubleSided ? 1 : 0][blended ? 1 : 0];
}

void Renderer::recordScene(VkCommandBuffer cmd, const RenderSettings& settings)
{
    if (!m_gpuScene.valid()) return;
    if (m_opaqueDraws.empty() && m_blendedDraws.empty()) return;

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_gpuScene.vertexBuffer.handle, &offset);
    vkCmdBindIndexBuffer(cmd, m_gpuScene.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkDescriptorSet boundSet = VK_NULL_HANDLE;

    auto record = [&](const std::vector<DrawItem>& items, bool blended)
    {
        for (const DrawItem& item : items)
        {
            const VkPipeline pipeline = pipelineFor(settings, item.doubleSided, blended);
            if (pipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
            }

            VkDescriptorSet set = item.materialSet ? item.materialSet : m_gpuScene.fallbackSet;
            if (set != boundSet)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                        1, 1, &set, 0, nullptr);
                boundSet = set;
            }

            vkCmdPushConstants(cmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstants), &item.push);

            vkCmdDrawIndexed(cmd, item.indexCount, 1, item.firstIndex, item.vertexOffset, 0);

            ++m_stats.drawCalls;
            m_stats.triangles += item.indexCount / 3;
        }
    };

    record(m_opaqueDraws, false);
    record(m_blendedDraws, true);
}

void Renderer::recordShadowPass(VkCommandBuffer cmd, const RenderSettings& settings)
{
    if (m_shadowExtent == 0) return;

    // Runs even with nothing to draw. The descriptor set points at this image
    // every frame, so it has to be left in a layout the fragment shader can
    // legally sample; a cleared map simply reads as "everything lit".
    const bool draw = m_shadowCaster != 0 && m_gpuScene.valid() && !m_opaqueDraws.empty();

    Image& map = m_shadowMaps[m_frameIndex];

    transitionImageLayout(cmd, map.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView              = map.view;
    depthAttachment.imageLayout            = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp                 = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp                = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea           = {{0, 0}, {m_shadowExtent, m_shadowExtent}};
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 0;          // depth only
    rendering.pDepthAttachment     = &depthAttachment;

    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f,
                              static_cast<float>(m_shadowExtent),
                              static_cast<float>(m_shadowExtent), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_shadowExtent, m_shadowExtent}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (draw)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_gpuScene.vertexBuffer.handle, &offset);
        vkCmdBindIndexBuffer(cmd, m_gpuScene.indexBuffer.handle, 0, VK_INDEX_TYPE_UINT32);

        // Opaque geometry only: a blended surface casting a solid shadow looks
        // worse than it casting none.
        for (const DrawItem& item : m_opaqueDraws)
        {
            vkCmdPushConstants(cmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstants), &item.push);

            vkCmdDrawIndexed(cmd, item.indexCount, 1, item.firstIndex, item.vertexOffset, 0);
            ++m_stats.drawCalls;
        }
    }

    vkCmdEndRendering(cmd);

    // Hand it to the fragment shader for the main pass.
    transitionImageLayout(cmd, map.handle, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::recordSky(VkCommandBuffer cmd, const RenderSettings& settings)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);

    // Colours come from the globals block; only the sun toggle is per-draw.
    PushConstants push{};
    push.mrfs = glm::vec4(0.0f, 0.0f, 0.0f, settings.skySun ? 1.0f : 0.0f);

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &push);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    ++m_stats.drawCalls;
}

void Renderer::recordAxes(VkCommandBuffer cmd, const RenderSettings& settings,
                          float sceneRadius)
{
    // 3 axes x 16 segments x (6 shaft + 3 cone + 3 cap) vertices.
    // Must match the constants at the top of shaders/axes.vert.
    constexpr uint32_t kAxisVertexCount = 3u * 16u * 12u;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_axesPipeline);

    PushConstants push{};
    push.mrfs = {glm::max(sceneRadius, 1e-3f) * settings.axesScale,   // length
                 0.022f,                                              // shaft radius
                 0.0f, -1.0f};

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &push);

    vkCmdDraw(cmd, kAxisVertexCount, 1, 0, 0);
    ++m_stats.drawCalls;
}

void Renderer::rebuildGridMesh(const RenderSettings& settings, float sceneRadius)
{
    const float cell = glm::max(settings.gridCell, 1e-3f);

    // Size the grid to the model, but keep the vertex count bounded.
    const int wanted = static_cast<int>(std::ceil(sceneRadius * 4.0f / cell));
    const int half   = glm::clamp(glm::max(wanted, settings.gridHalfExtent), 8, 400);

    if (m_gridMesh && half == m_gridBuiltHalf &&
        std::abs(cell - m_gridBuiltCell) < 1e-6f &&
        settings.gridAxisLines == m_gridBuiltAxes)
        return;

    const glm::vec3 base  = settings.gridColor;
    const glm::vec4 minor{base, 0.30f};
    const glm::vec4 major{base, 0.55f};
    const glm::vec4 xAxis{0.90f, 0.25f, 0.27f, 0.85f};
    const glm::vec4 zAxis{0.28f, 0.48f, 0.95f, 0.85f};

    const float extent = static_cast<float>(half) * cell;

    std::vector<GridVertex> vertices;
    vertices.reserve(static_cast<size_t>(half * 2 + 1) * 4);

    for (int i = -half; i <= half; ++i)
    {
        const float p = static_cast<float>(i) * cell;

        // Every tenth line reads as a major division.
        glm::vec4 colorAlongZ = (i % 10 == 0) ? major : minor;
        glm::vec4 colorAlongX = colorAlongZ;

        if (i == 0 && settings.gridAxisLines)
        {
            colorAlongZ = zAxis;   // the line at x = 0 runs along Z
            colorAlongX = xAxis;   // the line at z = 0 runs along X
        }

        vertices.push_back({{p, 0.0f, -extent}, colorAlongZ});
        vertices.push_back({{p, 0.0f,  extent}, colorAlongZ});
        vertices.push_back({{-extent, 0.0f, p}, colorAlongX});
        vertices.push_back({{ extent, 0.0f, p}, colorAlongX});
    }

    if (m_gridMesh) destroyBuffer(*m_context, m_gridMesh);

    m_gridMesh = createDeviceBuffer(*m_context, vertices.data(),
                                    vertices.size() * sizeof(GridVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_gridVertexCount = static_cast<uint32_t>(vertices.size());
    m_gridBuiltCell   = cell;
    m_gridBuiltHalf   = half;
    m_gridBuiltAxes   = settings.gridAxisLines;
}

void Renderer::recordGrid(VkCommandBuffer cmd, const RenderSettings& settings)
{
    if (!m_gridMesh || m_gridVertexCount == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_gridMesh.handle, &offset);

    PushConstants push{};
    push.baseColor = glm::vec4(settings.gridColor, 1.0f);

    // Fade out before the mesh edge so the boundary is never visible.
    const float extent = static_cast<float>(m_gridBuiltHalf) * m_gridBuiltCell;
    push.mrfs = {m_gridBuiltCell, extent, 0.0f, -1.0f};

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &push);

    vkCmdDraw(cmd, m_gridVertexCount, 1, 0, 0);
    ++m_stats.drawCalls;
}

} // namespace mv::gfx
