#pragma once

// GPU radix sort for Gaussian-splat draw order.
//
// Replaces the per-frame CPU std::sort (which dominated frame time at ~1M
// splats). Sorts splat indices back-to-front by view-space depth entirely on
// the GPU: a key-generation pass, then four 8-bit LSD radix passes
// (histogram -> scan -> scatter). The sorted index buffer feeds the splat
// graphics pass as its per-frame draw order.
//
// The algorithm and key encoding are validated on the CPU against the real
// sample clouds (tools/radix_ref.cpp: 0 mismatches vs std::stable_sort); the
// compute shaders implement that exact logic. All buffers bind through one
// unified set-0 descriptor set; the src/dst index buffer per radix pass is
// selected by a push-constant flag.
//
// Buffers and descriptor sets are DOUBLE-BUFFERED per frame-in-flight: frame N
// sorts into its own index0[N], so it never races frame N-1's graphics read of
// index0[N-1]. Pipelines and layouts are stateless and shared.

#include <cstdint>

#include <vulkan/vulkan.h>

#include "vk/Context.hpp"   // kFramesInFlight
#include "vk/Resources.hpp"

namespace mv {
namespace gfx {

class SplatSorter
{
public:
    /// `globalLayout` is Renderer's set-0 globals layout (the key pass reads the
    /// view matrix from it at set 1).
    void init(Context& context, VkDescriptorSetLayout globalLayout);
    void shutdown();

    /// (Re)size internal buffers for `count` splats and (re)write the descriptor
    /// sets against `splatBuffer`, for every frame slot. Call from
    /// SplatRenderer::upload whenever the splat buffer changes.
    void prepare(VkBuffer splatBuffer, uint32_t count);

    bool ready() const { return m_capacity > 0; }

    /// Records key-gen + 4 radix passes for frame `frameIndex` into `cmd`,
    /// inserting its own inter-pass compute barriers. Must be recorded OUTSIDE a
    /// dynamic-rendering scope (compute). `globalSet` supplies the view matrix.
    void record(VkCommandBuffer cmd, uint32_t frameIndex,
                VkDescriptorSet globalSet, uint32_t count);

    /// Sorted index buffer for `frameIndex` after record() (result is in
    /// index0 because there are an even number of radix passes).
    VkBuffer sortedIndexBuffer(uint32_t frameIndex) const
    {
        return m_frames[frameIndex % kFramesInFlight].index0.handle;
    }

private:
    static constexpr uint32_t kFrames = kFramesInFlight;

    struct FrameData
    {
        Buffer          keys;
        Buffer          index0, index1;   // ping-pong; result ends in index0
        Buffer          tileHist;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    void createPipelines(VkDescriptorSetLayout globalLayout);
    void allocateBuffers(uint32_t count);
    void writeDescriptorSets(VkBuffer splatBuffer);

    struct Push { uint32_t numElements, numTiles, shift, srcIsA; };

    Context* m_context   = nullptr;
    uint32_t m_capacity  = 0;
    VkBuffer m_lastSplat = VK_NULL_HANDLE;

    FrameData m_frames[kFrames];

    VkDescriptorSetLayout m_setLayout    = VK_NULL_HANDLE;  // unified set 0 (5 SSBOs)
    VkDescriptorSetLayout m_globalLayout = VK_NULL_HANDLE;  // set 1 (owned by Renderer)
    VkDescriptorPool      m_pool         = VK_NULL_HANDLE;

    VkPipelineLayout m_keyPipeLayout  = VK_NULL_HANDLE;  // {set0, set1} + push
    VkPipelineLayout m_sortPipeLayout = VK_NULL_HANDLE;  // {set0}       + push
    VkPipeline m_keyPipe  = VK_NULL_HANDLE;
    VkPipeline m_histPipe = VK_NULL_HANDLE;
    VkPipeline m_scanPipe = VK_NULL_HANDLE;
    VkPipeline m_scatPipe = VK_NULL_HANDLE;
};

} // namespace gfx
} // namespace mv
