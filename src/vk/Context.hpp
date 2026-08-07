#pragma once

#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <array>

#include <vk_mem_alloc.h>

namespace mv {

class Window;

namespace gfx {

/// How many frames may be recorded before waiting on the oldest. Declared here
/// because the depth targets are per-frame and Context owns them.
inline constexpr uint32_t kFramesInFlight = 2;

/// Aborts with a descriptive message when `result` is not VK_SUCCESS.
void check(VkResult result, const char* what);

const char* resultString(VkResult result);

struct SwapchainInfo
{
    VkSwapchainKHR           handle = VK_NULL_HANDLE;
    VkFormat                 format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR          colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D               extent{0, 0};
    VkPresentModeKHR         presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     images;
    std::vector<VkImageView> views;

    uint32_t imageCount() const { return static_cast<uint32_t>(images.size()); }
};

/// Owns the instance, device, allocator and swapchain.
class Context
{
public:
    Context() = default;
    ~Context();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    void init(Window& window, bool enableValidation);
    void shutdown();

    /// Tears down and rebuilds the swapchain and its depth buffer.
    void recreateSwapchain(VkExtent2D extent);

    void setVsync(bool enabled);
    bool vsync() const { return m_vsync; }

    /// Runs a one-shot command buffer on the graphics queue and waits.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    void waitIdle() const;

    VkInstance       instance()       const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice         device()         const { return m_device; }
    VmaAllocator     allocator()      const { return m_allocator; }
    VkSurfaceKHR     surface()        const { return m_surface; }

    VkQueue  graphicsQueue()       const { return m_graphicsQueue; }
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }

    const SwapchainInfo& swapchain() const { return m_swapchain; }

    // One depth target per frame in flight. A single shared depth image is a
    // data race: with two frames in flight the GPU can still be reading and
    // writing frame N-1's depth while frame N clears and rewrites it, which
    // shows up as flicker on geometry that only just wins its depth test.
    /// Sample count in use. 1 means MSAA is unavailable or disabled.
    VkSampleCountFlagBits sampleCount() const { return m_samples; }
    bool                  multisampled() const { return m_samples != VK_SAMPLE_COUNT_1_BIT; }

    /// Multisampled colour target, resolved into the swapchain image at the
    /// end of the pass. Null when running without MSAA.
    VkImage     colorImage(uint32_t frame) const { return m_colorImages[frame % kFramesInFlight]; }
    VkImageView colorView(uint32_t frame)  const { return m_colorViews[frame % kFramesInFlight]; }

    VkImage     depthImage(uint32_t frame) const { return m_depthImages[frame % kFramesInFlight]; }
    VkImageView depthView(uint32_t frame)  const { return m_depthViews[frame % kFramesInFlight]; }
    VkFormat    depthFormat() const { return m_depthFormat; }

    const VkPhysicalDeviceProperties& deviceProperties() const { return m_deviceProperties; }
    std::string                       deviceName() const;
    VkSampleCountFlagBits             maxSampleCount() const;

    bool wideLinesSupported()    const { return m_wideLines; }
    bool anisotropySupported()   const { return m_anisotropy; }
    float maxAnisotropy()        const { return m_deviceProperties.limits.maxSamplerAnisotropy; }

private:
    void createSwapchain(VkExtent2D extent);
    void destroySwapchain();
    /// Creates the offscreen render targets: the multisampled colour buffer
    /// (when MSAA is active) and the depth buffer, one of each per frame in
    /// flight.
    void createRenderTargets();
    void destroyRenderTargets();
    void createImmediateContext();

    VkInstance               m_instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice = VK_NULL_HANDLE;
    VkDevice                 m_device         = VK_NULL_HANDLE;
    VmaAllocator             m_allocator      = VK_NULL_HANDLE;

    VkQueue  m_graphicsQueue       = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;

    SwapchainInfo m_swapchain;

    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;

    std::array<VkImage, kFramesInFlight>       m_colorImages{};
    std::array<VmaAllocation, kFramesInFlight> m_colorAllocations{};
    std::array<VkImageView, kFramesInFlight>   m_colorViews{};

    std::array<VkImage, kFramesInFlight>       m_depthImages{};
    std::array<VmaAllocation, kFramesInFlight>  m_depthAllocations{};
    std::array<VkImageView, kFramesInFlight>    m_depthViews{};
    VkFormat      m_depthFormat     = VK_FORMAT_D32_SFLOAT;

    VkCommandPool   m_immediatePool   = VK_NULL_HANDLE;
    VkCommandBuffer m_immediateBuffer = VK_NULL_HANDLE;
    VkFence         m_immediateFence  = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties m_deviceProperties{};

    bool m_vsync      = true;
    bool m_wideLines  = false;
    bool m_anisotropy = false;
    bool m_initialised = false;
};

} // namespace gfx
} // namespace mv
