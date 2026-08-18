#pragma once

// Renders a SplatCloud as screen-space Gaussian ellipses (EWA splatting).
//
// This is deliberately a self-contained sibling to the mesh path rather than
// another branch inside Renderer: splats share none of the mesh pipeline
// (no index/vertex/material/skinning state), so keeping them separate keeps
// both readable. Renderer owns one SplatRenderer and calls into it.
//
// First-cut scope (milestone 2): SH degree 0 (flat, view-independent colour,
// pre-activated on the CPU), a per-frame CPU depth sort, and back-to-front
// premultiplied-alpha blending. Full SH evaluation and a GPU sort are later
// milestones; the data path is built so those slot in without reshaping it.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "vk/Resources.hpp"
#include "vk/SplatSorter.hpp"

namespace mv {

struct SplatCloud;
class Camera;

namespace gfx {

class Context;

class SplatRenderer
{
public:
    /// Per-splat GPU record. std430, 64 bytes. MUST match GpuSplat in splat.vert.
    struct GpuSplat
    {
        glm::vec4 posOpacity;  // xyz world position, w opacity (0..1)
        glm::vec4 color;       // rgb colour (SH degree 0, activated), w unused
        glm::vec4 cov0;        // 3D covariance: xx, xy, xz, yy
        glm::vec4 cov1;        //                yz, zz, pad, pad
    };
    static_assert(sizeof(GpuSplat) == 64, "GpuSplat must be 64 bytes (std430)");

    /// Push-constant block for the splat pipeline. MUST match SplatPush.
    struct Push
    {
        glm::vec2 viewport{0.0f};
        float     scaleMod = 1.0f;
        uint32_t  shDegree = 0;   // spherical-harmonics degree to evaluate (0..3)
    };

    void init(Context& context,
              VkDescriptorSetLayout globalLayout,   // set 0 (reused from Renderer)
              VkFormat colorFormat, VkFormat depthFormat,
              VkSampleCountFlagBits samples);
    void shutdown();

    /// Replaces the GPU splat set. Pre-activates colour/opacity and precomputes
    /// each splat's 3D covariance from its scale + rotation. Safe to call with
    /// an empty cloud (clears).
    void upload(const SplatCloud& cloud);
    void clear();

    bool hasSplats() const { return m_count > 0; }
    uint32_t count() const { return m_count; }

    /// Switch between the GPU radix sort (default) and the CPU std::sort
    /// fallback. Rewrites the graphics order descriptors under a device wait, so
    /// it is cheap but not free; intended for a debug toggle.
    void setGpuSort(bool enabled);
    bool gpuSort() const { return m_gpuSort; }

    /// Toggle view-dependent SH colour (on) vs flat DC-only colour (off).
    /// Off reproduces the original degree-0 look; useful for comparison.
    void setShEnabled(bool enabled) { m_shEnabled = enabled; }
    bool shEnabled() const { return m_shEnabled; }
    int  shDegree()  const { return static_cast<int>(m_shDegree); }

    /// Produces the back-to-front draw order for this frame. MUST be recorded
    /// BEFORE vkCmdBeginRendering (the GPU path dispatches compute, which cannot
    /// run inside a dynamic-rendering scope). GPU path: runs the radix sort and
    /// inserts a compute->vertex barrier. CPU path: sorts on the CPU and uploads
    /// the per-frame host order buffer.
    void recordSort(VkCommandBuffer cmd, uint32_t frameIndex,
                    VkDescriptorSet globalSet, const glm::mat4& view);

    /// Records the splat draw into an already-begun dynamic-rendering scope.
    /// Assumes recordSort() ran earlier this frame.
    void record(VkCommandBuffer cmd, uint32_t frameIndex,
                VkDescriptorSet globalSet,
                VkExtent2D viewport, float scaleMod);

private:
    void createPipeline(VkDescriptorSetLayout globalLayout,
                        VkFormat colorFormat, VkFormat depthFormat,
                        VkSampleCountFlagBits samples);
    void createDescriptors();
    void ensureOrderCapacity(uint32_t frameIndex, uint32_t count);
    void writeOrderDescriptor(uint32_t frameIndex);   // points binding 1 at CPU or GPU buffer

    Context* m_context = nullptr;
    bool     m_gpuSort = true;
    bool     m_shEnabled = true;

    SplatSorter m_sorter;

    // Static per-model data.
    Buffer   m_splatBuffer;                 // device-local GpuSplat[count]
    Buffer   m_shBuffer;                    // device-local higher-order SH coeffs
    uint32_t m_count = 0;
    uint32_t m_shDegree = 0;
    std::vector<glm::vec3> m_positions;     // CPU copy, kept for the CPU sort

    // Per-frame draw order for the CPU path (host-visible; rewritten each frame).
    static constexpr uint32_t kFrames = kFramesInFlight;
    Buffer          m_orderBuffers[kFrames]{};
    uint32_t        m_orderCapacity[kFrames]{0, 0};
    VkDescriptorSet m_descSets[kFrames]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    // Scratch reused across frames to avoid per-frame allocation.
    std::vector<uint32_t> m_sortIndices;
    std::vector<float>    m_sortDepths;

    VkDescriptorSetLayout m_splatLayout    = VK_NULL_HANDLE;  // set 1 (two SSBOs)
    VkDescriptorPool      m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
};

} // namespace gfx
} // namespace mv
