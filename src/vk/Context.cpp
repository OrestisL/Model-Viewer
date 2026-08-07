#include "vk/Context.hpp"

#include <stdexcept>

#include <VkBootstrap.h>

#include "core/Log.hpp"
#include "core/Window.hpp"

namespace mv::gfx {

const char* resultString(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        default:                                return "VK_ERROR_<unknown>";
    }
}

void check(VkResult result, const char* what)
{
    if (result == VK_SUCCESS) return;
    throw std::runtime_error(std::string(what) + " failed: " + resultString(result));
}

// ---------------------------------------------------------------------------

Context::~Context() { shutdown(); }

void Context::init(Window& window, bool enableValidation)
{
    // -- instance ----------------------------------------------------------
    vkb::InstanceBuilder builder;
    builder.set_app_name("ModelViewer")
           .set_engine_name("ModelViewer")
           .require_api_version(1, 3, 0);

    for (const char* extension : window.requiredInstanceExtensions())
        builder.enable_extension(extension);

    if (enableValidation)
    {
        builder.request_validation_layers(true)
               .set_debug_callback(
                   [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                      VkDebugUtilsMessageTypeFlagsEXT,
                      const VkDebugUtilsMessengerCallbackDataEXT* data,
                      void*) -> VkBool32
                   {
                       if (!data || !data->pMessage) return VK_FALSE;
                       if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                           log::error("[vulkan] ", data->pMessage);
                       else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                           log::warn("[vulkan] ", data->pMessage);
                       return VK_FALSE;
                   });
    }

    auto instanceResult = builder.build();
    if (!instanceResult)
        throw std::runtime_error("Vulkan instance creation failed: " +
                                 instanceResult.error().message());

    vkb::Instance vkbInstance = instanceResult.value();
    m_instance       = vkbInstance.instance;
    m_debugMessenger = vkbInstance.debug_messenger;

    // -- surface -----------------------------------------------------------
    m_surface = window.createSurface(m_instance);

    // -- physical device ---------------------------------------------------
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // glslang targeting Vulkan 1.3 emits SPIR-V 1.6, where OpKill is
    // deprecated: `discard` becomes OpDemoteToHelperInvocation, which needs
    // this feature even though it is core in 1.3.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType                                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.scalarBlockLayout                         = VK_TRUE;

    VkPhysicalDeviceFeatures features10{};
    features10.samplerAnisotropy = VK_TRUE;
    features10.fillModeNonSolid  = VK_TRUE;   // wireframe view
    features10.independentBlend  = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(vkbInstance);
    auto deviceResult = selector.set_surface(m_surface)
                            .set_minimum_version(1, 3)
                            .set_required_features_13(features13)
                            .set_required_features_12(features12)
                            .set_required_features(features10)
                            .select();

    if (!deviceResult)
        throw std::runtime_error(
            "No suitable Vulkan 1.3 device found (" + deviceResult.error().message() +
            "). This viewer needs dynamic rendering and synchronization2; update your GPU driver.");

    vkb::PhysicalDevice vkbPhysical = deviceResult.value();

    // -- logical device ----------------------------------------------------
    vkb::DeviceBuilder deviceBuilder(vkbPhysical);
    auto builtDevice = deviceBuilder.build();
    if (!builtDevice)
        throw std::runtime_error("Vulkan device creation failed: " + builtDevice.error().message());

    vkb::Device vkbDevice = builtDevice.value();
    m_physicalDevice = vkbPhysical.physical_device;
    m_device         = vkbDevice.device;

    auto queue = vkbDevice.get_queue(vkb::QueueType::graphics);
    if (!queue)
        throw std::runtime_error("No graphics queue: " + queue.error().message());
    m_graphicsQueue       = queue.value();
    m_graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    vkGetPhysicalDeviceProperties(m_physicalDevice, &m_deviceProperties);

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
    m_wideLines  = supported.wideLines == VK_TRUE;
    m_anisotropy = supported.samplerAnisotropy == VK_TRUE;

    log::info("GPU: ", deviceName(),
              " (Vulkan ", VK_API_VERSION_MAJOR(m_deviceProperties.apiVersion), ".",
              VK_API_VERSION_MINOR(m_deviceProperties.apiVersion), ".",
              VK_API_VERSION_PATCH(m_deviceProperties.apiVersion), ")");

    // -- allocator ---------------------------------------------------------
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice   = m_physicalDevice;
    allocatorInfo.device           = m_device;
    allocatorInfo.instance         = m_instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    check(vmaCreateAllocator(&allocatorInfo, &m_allocator), "vmaCreateAllocator");

    createImmediateContext();

    const glm::uvec2 size = window.framebufferSize();
    createSwapchain({size.x, size.y});
    createRenderTargets();

    m_initialised = true;
}

void Context::createImmediateContext()
{
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    check(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_immediatePool),
          "vkCreateCommandPool(immediate)");

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool        = m_immediatePool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(m_device, &allocInfo, &m_immediateBuffer),
          "vkAllocateCommandBuffers(immediate)");

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(m_device, &fenceInfo, nullptr, &m_immediateFence),
          "vkCreateFence(immediate)");
}

void Context::immediateSubmit(const std::function<void(VkCommandBuffer)>& record)
{
    check(vkResetFences(m_device, 1, &m_immediateFence), "vkResetFences");
    check(vkResetCommandBuffer(m_immediateBuffer, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(m_immediateBuffer, &beginInfo), "vkBeginCommandBuffer");

    record(m_immediateBuffer);

    check(vkEndCommandBuffer(m_immediateBuffer), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo bufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    bufferInfo.commandBuffer = m_immediateBuffer;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &bufferInfo;

    check(vkQueueSubmit2(m_graphicsQueue, 1, &submit, m_immediateFence), "vkQueueSubmit2");
    check(vkWaitForFences(m_device, 1, &m_immediateFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

void Context::createSwapchain(VkExtent2D extent)
{
    vkb::SwapchainBuilder builder(m_physicalDevice, m_device, m_surface,
                                  m_graphicsQueueFamily, m_graphicsQueueFamily);

    const VkSurfaceFormatKHR preferred{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    const VkSurfaceFormatKHR fallback{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

    auto result = builder.set_desired_format(preferred)
                      .add_fallback_format(fallback)
                      .set_desired_present_mode(m_vsync ? VK_PRESENT_MODE_FIFO_KHR
                                                        : VK_PRESENT_MODE_MAILBOX_KHR)
                      .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                      .set_desired_extent(extent.width, extent.height)
                      .set_desired_min_image_count(vkb::SwapchainBuilder::DOUBLE_BUFFERING)
                      .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                      .set_old_swapchain(m_swapchain.handle)
                      .build();

    if (!result)
        throw std::runtime_error("Swapchain creation failed: " + result.error().message());

    // The old swapchain (if any) is retired by the builder; destroy our handles.
    destroySwapchain();

    vkb::Swapchain vkbSwapchain = result.value();
    m_swapchain.handle      = vkbSwapchain.swapchain;
    m_swapchain.format      = vkbSwapchain.image_format;
    m_swapchain.colorSpace  = vkbSwapchain.color_space;
    m_swapchain.extent      = vkbSwapchain.extent;
    m_swapchain.presentMode = vkbSwapchain.present_mode;
    m_swapchain.images      = vkbSwapchain.get_images().value();
    m_swapchain.views       = vkbSwapchain.get_image_views().value();

    log::trace("Swapchain: ", m_swapchain.extent.width, "x", m_swapchain.extent.height,
               ", ", m_swapchain.imageCount(), " images");
}

void Context::destroySwapchain()
{
    for (VkImageView view : m_swapchain.views)
        if (view) vkDestroyImageView(m_device, view, nullptr);

    if (m_swapchain.handle)
        vkDestroySwapchainKHR(m_device, m_swapchain.handle, nullptr);

    m_swapchain.views.clear();
    m_swapchain.images.clear();
    m_swapchain.handle = VK_NULL_HANDLE;
}

void Context::createRenderTargets()
{
    // Prefer a plain 32-bit depth buffer; fall back to a combined format.
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT,
                                   VK_FORMAT_D32_SFLOAT_S8_UINT,
                                   VK_FORMAT_D24_UNORM_S8_UINT};
    m_depthFormat = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : candidates)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, candidate, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            m_depthFormat = candidate;
            break;
        }
    }
    if (m_depthFormat == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("No usable depth format");

    // 4x is the sweet spot: it removes almost all of the crawling on thin
    // geometry (the axis arrows especially) for a fraction of the cost of 8x.
    // Anything the device cannot manage falls back automatically.
    const VkSampleCountFlagBits wanted = VK_SAMPLE_COUNT_4_BIT;
    const VkSampleCountFlagBits best   = maxSampleCount();
    m_samples = (static_cast<uint32_t>(best) < static_cast<uint32_t>(wanted)) ? best : wanted;

    if (m_samples != VK_SAMPLE_COUNT_1_BIT)
    {
        VkImageCreateInfo colorInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        colorInfo.imageType     = VK_IMAGE_TYPE_2D;
        colorInfo.format        = m_swapchain.format;
        colorInfo.extent        = {m_swapchain.extent.width, m_swapchain.extent.height, 1};
        colorInfo.mipLevels     = 1;
        colorInfo.arrayLayers   = 1;
        colorInfo.samples       = m_samples;
        colorInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        colorInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        colorInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo colorAlloc{};
        colorAlloc.usage         = VMA_MEMORY_USAGE_AUTO;
        colorAlloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            check(vmaCreateImage(m_allocator, &colorInfo, &colorAlloc, &m_colorImages[i],
                                 &m_colorAllocations[i], nullptr),
                  "vmaCreateImage(msaa colour)");

            VkImageViewCreateInfo colorViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            colorViewInfo.image                       = m_colorImages[i];
            colorViewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            colorViewInfo.format                      = m_swapchain.format;
            colorViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            colorViewInfo.subresourceRange.levelCount = 1;
            colorViewInfo.subresourceRange.layerCount = 1;

            check(vkCreateImageView(m_device, &colorViewInfo, nullptr, &m_colorViews[i]),
                  "vkCreateImageView(msaa colour)");
        }

        log::info("MSAA: ", static_cast<uint32_t>(m_samples), "x");
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = m_depthFormat;
    imageInfo.extent        = {m_swapchain.extent.width, m_swapchain.extent.height, 1};
    imageInfo.mipLevels     = 1;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = m_samples;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage         = VMA_MEMORY_USAGE_AUTO;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        check(vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &m_depthImages[i],
                             &m_depthAllocations[i], nullptr),
              "vmaCreateImage(depth)");

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image                       = m_depthImages[i];
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = m_depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        check(vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthViews[i]),
              "vkCreateImageView(depth)");
    }
}

void Context::destroyRenderTargets()
{
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        if (m_colorViews[i])
        {
            vkDestroyImageView(m_device, m_colorViews[i], nullptr);
            m_colorViews[i] = VK_NULL_HANDLE;
        }
        if (m_colorImages[i])
        {
            vmaDestroyImage(m_allocator, m_colorImages[i], m_colorAllocations[i]);
            m_colorImages[i]      = VK_NULL_HANDLE;
            m_colorAllocations[i] = VK_NULL_HANDLE;
        }
        if (m_depthViews[i])
        {
            vkDestroyImageView(m_device, m_depthViews[i], nullptr);
            m_depthViews[i] = VK_NULL_HANDLE;
        }
        if (m_depthImages[i])
        {
            vmaDestroyImage(m_allocator, m_depthImages[i], m_depthAllocations[i]);
            m_depthImages[i]      = VK_NULL_HANDLE;
            m_depthAllocations[i] = VK_NULL_HANDLE;
        }
    }
}

void Context::recreateSwapchain(VkExtent2D extent)
{
    if (extent.width == 0 || extent.height == 0) return;

    vkDeviceWaitIdle(m_device);
    destroyRenderTargets();
    createSwapchain(extent);
    createRenderTargets();
}

void Context::setVsync(bool enabled)
{
    if (m_vsync == enabled) return;
    m_vsync = enabled;
    if (m_initialised) recreateSwapchain(m_swapchain.extent);
}

void Context::waitIdle() const
{
    if (m_device) vkDeviceWaitIdle(m_device);
}

std::string Context::deviceName() const
{
    return m_physicalDevice ? m_deviceProperties.deviceName : "<no device>";
}

VkSampleCountFlagBits Context::maxSampleCount() const
{
    const VkSampleCountFlags counts = m_deviceProperties.limits.framebufferColorSampleCounts &
                                      m_deviceProperties.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_8_BIT) return VK_SAMPLE_COUNT_8_BIT;
    if (counts & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

void Context::shutdown()
{
    if (!m_device) return;

    vkDeviceWaitIdle(m_device);

    destroyRenderTargets();
    destroySwapchain();

    if (m_immediateFence) vkDestroyFence(m_device, m_immediateFence, nullptr);
    if (m_immediatePool)  vkDestroyCommandPool(m_device, m_immediatePool, nullptr);
    m_immediateFence  = VK_NULL_HANDLE;
    m_immediatePool   = VK_NULL_HANDLE;
    m_immediateBuffer = VK_NULL_HANDLE;

    if (m_allocator) { vmaDestroyAllocator(m_allocator); m_allocator = VK_NULL_HANDLE; }

    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;

    if (m_surface) { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
    if (m_debugMessenger)
    {
        vkb::destroy_debug_utils_messenger(m_instance, m_debugMessenger);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance) { vkDestroyInstance(m_instance, nullptr); m_instance = VK_NULL_HANDLE; }

    m_initialised = false;
}

} // namespace mv::gfx
