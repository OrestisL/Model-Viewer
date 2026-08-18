#include "vk/SplatSorter.hpp"

#include <array>
#include <cstdint>

#include "core/Log.hpp"
#include "vk/Context.hpp"

namespace mv {
namespace gfx {
namespace {

constexpr uint32_t RADIX          = 256;
constexpr uint32_t WG_SIZE        = 256;
constexpr uint32_t ELEMS_PER_TILE = 512;   // must match splat_sort.glsl

uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

VkPipeline makeComputePipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule module)
{
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName  = "main";

    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage  = stage;
    info.layout = layout;

    VkPipeline pipe = VK_NULL_HANDLE;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipe),
          "vkCreateComputePipelines(splat sort)");
    return pipe;
}

void computeBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

} // namespace

void SplatSorter::init(Context& context, VkDescriptorSetLayout globalLayout)
{
    m_context      = &context;
    m_globalLayout = globalLayout;

    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
    {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    check(vkCreateDescriptorSetLayout(m_context->device(), &layoutInfo, nullptr, &m_setLayout),
          "vkCreateDescriptorSetLayout(sort)");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * kFrames};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets       = kFrames;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    check(vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &m_pool),
          "vkCreateDescriptorPool(sort)");

    for (uint32_t f = 0; f < kFrames; ++f)
    {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool     = m_pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &m_setLayout;
        check(vkAllocateDescriptorSets(m_context->device(), &alloc, &m_frames[f].set),
              "vkAllocateDescriptorSets(sort)");
    }

    createPipelines(globalLayout);
}

void SplatSorter::createPipelines(VkDescriptorSetLayout globalLayout)
{
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size       = sizeof(Push);

    const std::array<VkDescriptorSetLayout, 2> keySets{m_setLayout, globalLayout};
    VkPipelineLayoutCreateInfo keyLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    keyLayout.setLayoutCount         = static_cast<uint32_t>(keySets.size());
    keyLayout.pSetLayouts            = keySets.data();
    keyLayout.pushConstantRangeCount = 1;
    keyLayout.pPushConstantRanges    = &push;
    check(vkCreatePipelineLayout(m_context->device(), &keyLayout, nullptr, &m_keyPipeLayout),
          "vkCreatePipelineLayout(sort key)");

    VkPipelineLayoutCreateInfo sortLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    sortLayout.setLayoutCount         = 1;
    sortLayout.pSetLayouts            = &m_setLayout;
    sortLayout.pushConstantRangeCount = 1;
    sortLayout.pPushConstantRanges    = &push;
    check(vkCreatePipelineLayout(m_context->device(), &sortLayout, nullptr, &m_sortPipeLayout),
          "vkCreatePipelineLayout(sort)");

    VkDevice dev = m_context->device();
    VkShaderModule keyMod  = loadShaderModule(*m_context, "splat_sort_key.comp");
    VkShaderModule histMod = loadShaderModule(*m_context, "splat_sort_histogram.comp");
    VkShaderModule scanMod = loadShaderModule(*m_context, "splat_sort_scan.comp");
    VkShaderModule scatMod = loadShaderModule(*m_context, "splat_sort_scatter.comp");

    m_keyPipe  = makeComputePipeline(dev, m_keyPipeLayout,  keyMod);
    m_histPipe = makeComputePipeline(dev, m_sortPipeLayout, histMod);
    m_scanPipe = makeComputePipeline(dev, m_sortPipeLayout, scanMod);
    m_scatPipe = makeComputePipeline(dev, m_sortPipeLayout, scatMod);

    vkDestroyShaderModule(dev, keyMod,  nullptr);
    vkDestroyShaderModule(dev, histMod, nullptr);
    vkDestroyShaderModule(dev, scanMod, nullptr);
    vkDestroyShaderModule(dev, scatMod, nullptr);
}

void SplatSorter::allocateBuffers(uint32_t count)
{
    const uint32_t maxTiles = divUp(count, ELEMS_PER_TILE);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    for (uint32_t f = 0; f < kFrames; ++f)
    {
        FrameData& fr = m_frames[f];
        destroyBuffer(*m_context, fr.keys);
        destroyBuffer(*m_context, fr.index0);
        destroyBuffer(*m_context, fr.index1);
        destroyBuffer(*m_context, fr.tileHist);

        fr.keys     = createBuffer(*m_context, sizeof(uint32_t) * count, usage,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        fr.index0   = createBuffer(*m_context, sizeof(uint32_t) * count, usage,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        fr.index1   = createBuffer(*m_context, sizeof(uint32_t) * count, usage,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
        fr.tileHist = createBuffer(*m_context, sizeof(uint32_t) * RADIX * maxTiles, usage,
                                   VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    }
    m_capacity = count;
}

void SplatSorter::writeDescriptorSets(VkBuffer splatBuffer)
{
    for (uint32_t f = 0; f < kFrames; ++f)
    {
        FrameData& fr = m_frames[f];
        const std::array<VkDescriptorBufferInfo, 5> infos{{
            {splatBuffer,        0, VK_WHOLE_SIZE},
            {fr.keys.handle,     0, VK_WHOLE_SIZE},
            {fr.index0.handle,   0, VK_WHOLE_SIZE},
            {fr.index1.handle,   0, VK_WHOLE_SIZE},
            {fr.tileHist.handle, 0, VK_WHOLE_SIZE}}};

        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = fr.set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo     = &infos[i];
        }
        vkUpdateDescriptorSets(m_context->device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    m_lastSplat = splatBuffer;
}

void SplatSorter::prepare(VkBuffer splatBuffer, uint32_t count)
{
    if (count == 0) return;
    if (count != m_capacity) allocateBuffers(count);
    writeDescriptorSets(splatBuffer);   // splat buffer changes per load
}

void SplatSorter::record(VkCommandBuffer cmd, uint32_t frameIndex,
                         VkDescriptorSet globalSet, uint32_t count)
{
    if (count == 0 || m_capacity == 0) return;
    const uint32_t f        = frameIndex % kFrames;
    const uint32_t numTiles = divUp(count, ELEMS_PER_TILE);
    VkDescriptorSet set     = m_frames[f].set;

    // Key pass: set0 + set1(globals).
    {
        const std::array<VkDescriptorSet, 2> sets{set, globalSet};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_keyPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_keyPipeLayout,
                                0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        Push p{count, numTiles, 0, 1};
        vkCmdPushConstants(cmd, m_keyPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, divUp(count, WG_SIZE), 1, 1);
    }
    computeBarrier(cmd);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortPipeLayout,
                            0, 1, &set, 0, nullptr);

    for (uint32_t pass = 0; pass < 4; ++pass)
    {
        Push p{count, numTiles, pass * 8u, (pass % 2u == 0u) ? 1u : 0u};

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_histPipe);
        vkCmdPushConstants(cmd, m_sortPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, numTiles, 1, 1);
        computeBarrier(cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_scanPipe);
        vkCmdPushConstants(cmd, m_sortPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, 1, 1, 1);
        computeBarrier(cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_scatPipe);
        vkCmdPushConstants(cmd, m_sortPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, numTiles, 1, 1);
        computeBarrier(cmd);
    }
    // Result is in index0[f] (even number of passes).
}

void SplatSorter::shutdown()
{
    if (!m_context) return;
    VkDevice dev = m_context->device();

    for (uint32_t f = 0; f < kFrames; ++f)
    {
        destroyBuffer(*m_context, m_frames[f].keys);
        destroyBuffer(*m_context, m_frames[f].index0);
        destroyBuffer(*m_context, m_frames[f].index1);
        destroyBuffer(*m_context, m_frames[f].tileHist);
    }

    if (m_keyPipe)  vkDestroyPipeline(dev, m_keyPipe,  nullptr);
    if (m_histPipe) vkDestroyPipeline(dev, m_histPipe, nullptr);
    if (m_scanPipe) vkDestroyPipeline(dev, m_scanPipe, nullptr);
    if (m_scatPipe) vkDestroyPipeline(dev, m_scatPipe, nullptr);
    if (m_keyPipeLayout)  vkDestroyPipelineLayout(dev, m_keyPipeLayout,  nullptr);
    if (m_sortPipeLayout) vkDestroyPipelineLayout(dev, m_sortPipeLayout, nullptr);
    if (m_pool)      vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);

    m_keyPipe = m_histPipe = m_scanPipe = m_scatPipe = VK_NULL_HANDLE;
    m_keyPipeLayout = m_sortPipeLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_capacity = 0;
    m_lastSplat = VK_NULL_HANDLE;
    m_context = nullptr;
}

} // namespace gfx
} // namespace mv
