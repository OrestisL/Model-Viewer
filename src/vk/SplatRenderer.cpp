#include "vk/SplatRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/Camera.hpp"
#include "core/Log.hpp"
#include "scene/SplatCloud.hpp"
#include "vk/Context.hpp"

namespace mv {
namespace gfx {
namespace {

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

VkPipelineShaderStageCreateInfo stageInfo(VkShaderStageFlagBits stage, VkShaderModule module)
{
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage  = stage;
    info.module = module;
    info.pName  = "main";
    return info;
}

} // namespace

void SplatRenderer::init(Context& context, VkDescriptorSetLayout globalLayout,
                         VkFormat colorFormat, VkFormat depthFormat,
                         VkSampleCountFlagBits samples)
{
    m_context = &context;
    createDescriptors();
    createPipeline(globalLayout, colorFormat, depthFormat, samples);
    m_sorter.init(context, globalLayout);
}

void SplatRenderer::createDescriptors()
{
    // set 1: three readonly storage buffers (splats, order, SH coefficients).
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}}};

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    check(vkCreateDescriptorSetLayout(m_context->device(), &layoutInfo, nullptr, &m_splatLayout),
          "vkCreateDescriptorSetLayout(splat)");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kFrames};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets       = kFrames;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    check(vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &m_descriptorPool),
          "vkCreateDescriptorPool(splat)");

    for (uint32_t i = 0; i < kFrames; ++i)
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool     = m_descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &m_splatLayout;
        check(vkAllocateDescriptorSets(m_context->device(), &alloc, &m_descSets[i]),
              "vkAllocateDescriptorSets(splat)");
    }
}

void SplatRenderer::createPipeline(VkDescriptorSetLayout globalLayout,
                                   VkFormat colorFormat, VkFormat depthFormat,
                                   VkSampleCountFlagBits samples)
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.size       = sizeof(Push);

    const std::array<VkDescriptorSetLayout, 2> layouts{globalLayout, m_splatLayout};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount         = static_cast<uint32_t>(layouts.size());
    layoutInfo.pSetLayouts            = layouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    check(vkCreatePipelineLayout(m_context->device(), &layoutInfo, nullptr, &m_pipelineLayout),
          "vkCreatePipelineLayout(splat)");

    VkShaderModule vert = loadShaderModule(*m_context, "splat.vert");
    VkShaderModule frag = loadShaderModule(*m_context, "splat.frag");

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        stageInfo(VK_SHADER_STAGE_VERTEX_BIT, vert),
        stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT, frag)};

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};   // none: corners from gl_VertexIndex

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;   // screen-aligned quads: don't cull
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;

    // Depth-test against the mesh/grid depth so splats are occluded correctly,
    // but no depth write: splats are order-dependent transparency.
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.maxDepthBounds   = 1.0f;

    // Back-to-front over-blend with PREMULTIPLIED colour (frag outputs colour*alpha).
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blend;

    const std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = depthFormat;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext               = &renderingInfo;
    info.stageCount          = static_cast<uint32_t>(stages.size());
    info.pStages             = stages.data();
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState      = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depthStencil;
    info.pColorBlendState    = &colorBlend;
    info.pDynamicState       = &dynamicState;
    info.layout              = m_pipelineLayout;

    check(vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &info,
                                    nullptr, &m_pipeline),
          "vkCreateGraphicsPipelines(splat)");

    vkDestroyShaderModule(m_context->device(), vert, nullptr);
    vkDestroyShaderModule(m_context->device(), frag, nullptr);
}

void SplatRenderer::upload(const SplatCloud& cloud)
{
    clear();
    if (cloud.empty()) return;

    const std::size_t n = cloud.count();
    std::vector<GpuSplat> gpu(n);
    m_positions.resize(n);

    for (std::size_t i = 0; i < n; ++i)
    {
        const glm::vec3 pos = cloud.positions[i];
        m_positions[i] = pos;

        // Activations (SH degree 0 colour is view-independent).
        const glm::vec3 s = glm::exp(cloud.scales[i]);          // log -> linear
        glm::quat q = cloud.rotations[i];
        const float qn = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (qn > 1e-8f) q = glm::quat(q.w / qn, q.x / qn, q.y / qn, q.z / qn);

        // 3D covariance Sigma = R S^2 R^T, built as (R S)(R S)^T.
        glm::mat3 R = glm::mat3_cast(q);
        glm::mat3 M = R * glm::mat3(s.x, 0, 0, 0, s.y, 0, 0, 0, s.z);
        glm::mat3 Sigma = M * glm::transpose(M);

        GpuSplat g{};
        g.posOpacity = glm::vec4(pos, sigmoid(cloud.alphas[i]));
        // color.rgb carries the raw DC (f_dc) SH coefficient; the shader
        // evaluates the full view-dependent colour (DC + higher orders).
        g.color      = glm::vec4(cloud.colorsDC[i], 0.0f);
        g.cov0       = glm::vec4(Sigma[0][0], Sigma[0][1], Sigma[0][2], Sigma[1][1]);
        g.cov1       = glm::vec4(Sigma[1][2], Sigma[2][2], 0.0f, 0.0f);
        gpu[i] = g;
    }

    m_splatBuffer = createDeviceBuffer(*m_context, gpu.data(),
                                       sizeof(GpuSplat) * gpu.size(),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    m_count    = static_cast<uint32_t>(n);
    m_shDegree = cloud.shDegree;

    // Higher-order SH coefficients, quantised to snorm8 over [-1,1] and packed
    // 4 per uint (matching unpackSnorm4x8 in splat.vert). 4x smaller than
    // float32; the coefficients sit within [-0.75,0.875] so nothing clips, and
    // the ~1/254 quantisation error is visually negligible. At degree 0 there
    // are none; upload a single zero uint so the descriptor stays valid.
    {
        const std::size_t coeffCount = cloud.sh.size();          // count * shDim * 3
        const std::size_t wordCount  = std::max<std::size_t>((coeffCount + 3) / 4, 1);
        std::vector<uint32_t> packed(wordCount, 0u);
        for (std::size_t g = 0; g < coeffCount; ++g)
        {
            float v = std::min(std::max(cloud.sh[g], -1.0f), 1.0f);
            int   q = static_cast<int>(std::lround(v * 127.0f));  // [-127,127]
            uint8_t b = static_cast<uint8_t>(static_cast<int8_t>(q));
            packed[g >> 2] |= static_cast<uint32_t>(b) << ((g & 3) * 8);
        }
        m_shBuffer = createDeviceBuffer(*m_context, packed.data(),
                                        sizeof(uint32_t) * packed.size(),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    // Size the GPU sorter's buffers for this cloud (device-local, per frame).
    m_sorter.prepare(m_splatBuffer.handle, m_count);

    // Per-frame CPU order buffers (fallback path) + graphics descriptor writes.
    for (uint32_t f = 0; f < kFrames; ++f)
    {
        ensureOrderCapacity(f, m_count);
        writeOrderDescriptor(f);
    }

    m_sortIndices.resize(n);
    m_sortDepths.resize(n);

    log::info("Uploaded ", m_count, " gaussians to the GPU (SH degree ", cloud.shDegree, ")");
}

void SplatRenderer::writeOrderDescriptor(uint32_t f)
{
    // binding 0 = splat SSBO; binding 1 = the draw-order index buffer, which is
    // the GPU sorter's per-frame sorted output when GPU sort is on, or the CPU
    // host order buffer otherwise.
    const VkBuffer orderBuffer = m_gpuSort ? m_sorter.sortedIndexBuffer(f)
                                           : m_orderBuffers[f].handle;

    const std::array<VkDescriptorBufferInfo, 3> infos{{
        {m_splatBuffer.handle, 0, VK_WHOLE_SIZE},
        {orderBuffer,          0, VK_WHOLE_SIZE},
        {m_shBuffer.handle,    0, VK_WHOLE_SIZE}}};

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t b = 0; b < 3; ++b)
    {
        writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet          = m_descSets[f];
        writes[b].dstBinding      = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo     = &infos[b];
    }
    vkUpdateDescriptorSets(m_context->device(), 3, writes.data(), 0, nullptr);
}

void SplatRenderer::setGpuSort(bool enabled)
{
    if (enabled == m_gpuSort) return;
    m_context->waitIdle();
    m_gpuSort = enabled;
    if (m_count > 0)
        for (uint32_t f = 0; f < kFrames; ++f) writeOrderDescriptor(f);
}

void SplatRenderer::ensureOrderCapacity(uint32_t frameIndex, uint32_t count)
{
    if (m_orderCapacity[frameIndex] >= count && m_orderBuffers[frameIndex]) return;

    if (m_orderBuffers[frameIndex]) destroyBuffer(*m_context, m_orderBuffers[frameIndex]);
    m_orderBuffers[frameIndex] = createHostBuffer(
        *m_context, sizeof(uint32_t) * std::max<uint32_t>(count, 1),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    m_orderCapacity[frameIndex] = count;
}

void SplatRenderer::recordSort(VkCommandBuffer cmd, uint32_t frameIndex,
                               VkDescriptorSet globalSet, const glm::mat4& view)
{
    if (m_count == 0) return;
    frameIndex %= kFrames;

    if (m_gpuSort)
    {
        // GPU radix sort into the sorter's per-frame index0. Runs as compute,
        // so this must be recorded before vkCmdBeginRendering.
        m_sorter.record(cmd, frameIndex, globalSet, m_count);

        // Make the sorted indices visible to the vertex stage of the draw.
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        return;
    }

    // CPU fallback: depth-sort indices back-to-front (ascending view-space z)
    // and upload into this frame's host order buffer.
    const glm::vec3 row2{view[0][2], view[1][2], view[2][2]};
    const float     row2w = view[3][2];
    for (uint32_t i = 0; i < m_count; ++i)
    {
        m_sortIndices[i] = i;
        m_sortDepths[i]  = glm::dot(row2, m_positions[i]) + row2w;  // == (view * p).z
    }
    std::sort(m_sortIndices.begin(), m_sortIndices.begin() + m_count,
              [&](uint32_t a, uint32_t b) { return m_sortDepths[a] < m_sortDepths[b]; });

    std::memcpy(m_orderBuffers[frameIndex].mapped(), m_sortIndices.data(),
                sizeof(uint32_t) * m_count);
}

void SplatRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex,
                           VkDescriptorSet globalSet,
                           VkExtent2D viewport, float scaleMod)
{
    if (m_count == 0) return;
    frameIndex %= kFrames;

    Push push{};
    push.viewport = {static_cast<float>(viewport.width), static_cast<float>(viewport.height)};
    push.scaleMod = scaleMod;
    push.shDegree = m_shEnabled ? m_shDegree : 0u;   // 0 == flat (DC only)

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const std::array<VkDescriptorSet, 2> sets{globalSet, m_descSets[frameIndex]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), &push);

    vkCmdDraw(cmd, 6, m_count, 0, 0);   // 6 verts (a quad) per splat instance
}

void SplatRenderer::clear()
{
    if (!m_context) return;
    if (m_splatBuffer) destroyBuffer(*m_context, m_splatBuffer);
    if (m_shBuffer)    destroyBuffer(*m_context, m_shBuffer);
    m_count    = 0;
    m_shDegree = 0;
    m_positions.clear();
    // Order buffers and descriptor sets are kept and reused across models.
}

void SplatRenderer::shutdown()
{
    if (!m_context) return;
    clear();
    m_sorter.shutdown();
    for (uint32_t f = 0; f < kFrames; ++f)
        if (m_orderBuffers[f]) destroyBuffer(*m_context, m_orderBuffers[f]);

    if (m_pipeline)       vkDestroyPipeline(m_context->device(), m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_context->device(), m_pipelineLayout, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(m_context->device(), m_descriptorPool, nullptr);
    if (m_splatLayout)    vkDestroyDescriptorSetLayout(m_context->device(), m_splatLayout, nullptr);

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_splatLayout = VK_NULL_HANDLE;
    m_context = nullptr;
}

} // namespace gfx
} // namespace mv
