#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "vk/Context.hpp"

namespace mv {

struct TextureData;

namespace gfx {

struct Buffer
{
    VkBuffer          handle     = VK_NULL_HANDLE;
    VmaAllocation     allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    VkDeviceSize      size = 0;

    void*    mapped() const { return info.pMappedData; }
    explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

struct Image
{
    VkImage       handle     = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkExtent2D    extent{0, 0};
    VkFormat      format    = VK_FORMAT_UNDEFINED;
    uint32_t      mipLevels = 1;

    explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

// -- buffers ----------------------------------------------------------------

Buffer createBuffer(Context&                 context,
                    VkDeviceSize             size,
                    VkBufferUsageFlags       usage,
                    VmaMemoryUsage           memoryUsage,
                    VmaAllocationCreateFlags flags = 0);

/// Host-visible, persistently mapped buffer for per-frame data.
Buffer createHostBuffer(Context& context, VkDeviceSize size, VkBufferUsageFlags usage);

/// Device-local buffer initialised from `data` through a staging copy.
Buffer createDeviceBuffer(Context&           context,
                          const void*        data,
                          VkDeviceSize       size,
                          VkBufferUsageFlags usage);

void destroyBuffer(Context& context, Buffer& buffer);

// -- images -----------------------------------------------------------------

/// Uploads an RGBA8 texture and generates a full mip chain.
Image createTexture2D(Context& context, const TextureData& source, bool generateMips = true);

/// Single-pixel helper used for missing texture slots.
Image createSolidTexture(Context& context, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb);

void destroyImage(Context& context, Image& image);

// -- misc -------------------------------------------------------------------

void transitionImageLayout(VkCommandBuffer      cmd,
                           VkImage              image,
                           VkImageLayout        oldLayout,
                           VkImageLayout        newLayout,
                           VkImageAspectFlags   aspect    = VK_IMAGE_ASPECT_COLOR_BIT,
                           uint32_t             baseMip   = 0,
                           uint32_t             mipLevels = 1);

VkShaderModule loadShaderModule(Context& context, const std::string& path);

/// Directory that contains the SPIR-V modules, resolved next to the executable.
std::string shaderDirectory();

} // namespace gfx
} // namespace mv
