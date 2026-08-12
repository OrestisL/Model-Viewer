#include "vk/Resources.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "core/Log.hpp"
#include "core/Utf8.hpp"
#include "scene/Scene.hpp"

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__linux__)
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mv::gfx {
namespace {

fs::path executableDirectory()
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0) return fs::path(std::wstring(buffer, length)).parent_path();
#elif defined(__linux__)
    char buffer[4096]{};
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0) return fs::path(std::string(buffer, static_cast<size_t>(length))).parent_path();
#endif
    std::error_code ec;
    return fs::current_path(ec);
}

uint32_t mipLevelsFor(uint32_t width, uint32_t height)
{
    return 1u + static_cast<uint32_t>(std::floor(std::log2(std::max(width, height))));
}

} // namespace

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

Buffer createBuffer(Context&                 context,
                    VkDeviceSize             size,
                    VkBufferUsageFlags       usage,
                    VmaMemoryUsage           memoryUsage,
                    VmaAllocationCreateFlags flags)
{
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size        = std::max<VkDeviceSize>(size, 1);
    bufferInfo.usage       = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = flags;

    Buffer buffer;
    buffer.size = bufferInfo.size;
    check(vmaCreateBuffer(context.allocator(), &bufferInfo, &allocInfo,
                          &buffer.handle, &buffer.allocation, &buffer.info),
          "vmaCreateBuffer");
    return buffer;
}

Buffer createHostBuffer(Context& context, VkDeviceSize size, VkBufferUsageFlags usage)
{
    return createBuffer(context, size, usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

Buffer createDeviceBuffer(Context& context, const void* data, VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer target = createBuffer(context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    if (!data || size == 0) return target;

    Buffer staging = createBuffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT);

    std::memcpy(staging.mapped(), data, static_cast<size_t>(size));
    check(vmaFlushAllocation(context.allocator(), staging.allocation, 0, size), "vmaFlushAllocation");

    context.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(cmd, staging.handle, target.handle, 1, &region);
    });

    destroyBuffer(context, staging);
    return target;
}

void destroyBuffer(Context& context, Buffer& buffer)
{
    if (!buffer.handle) return;
    vmaDestroyBuffer(context.allocator(), buffer.handle, buffer.allocation);
    buffer = {};
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

void transitionImageLayout(VkCommandBuffer    cmd,
                           VkImage            image,
                           VkImageLayout      oldLayout,
                           VkImageLayout      newLayout,
                           VkImageAspectFlags aspect,
                           uint32_t           baseMip,
                           uint32_t           mipLevels)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.oldLayout           = oldLayout;
    barrier.newLayout           = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = {aspect, baseMip, mipLevels, 0, VK_REMAINING_ARRAY_LAYERS};

    // Conservative but correct: full pipeline stages with the access masks the
    // layout implies. Uploads are not hot-path work.
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;

    vkCmdPipelineBarrier2(cmd, &dependency);
}

namespace {

Image createImageFromPixels(Context&       context,
                            const uint8_t* pixels,
                            uint32_t       width,
                            uint32_t       height,
                            bool           srgb,
                            bool           generateMips,
                            const char*    debugName)
{
    if (width == 0 || height == 0)
        throw std::runtime_error(std::string("Zero-sized texture: ") + (debugName ? debugName : "?"));

    Image image;
    image.extent    = {width, height};
    image.format    = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    image.mipLevels = generateMips ? mipLevelsFor(width, height) : 1;

    // Blitting is required for mip generation; fall back to a single level.
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice(), image.format, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        image.mipLevels = 1;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType     = VK_IMAGE_TYPE_2D;
    imageInfo.format        = image.format;
    imageInfo.extent        = {width, height, 1};
    imageInfo.mipLevels     = image.mipLevels;
    imageInfo.arrayLayers   = 1;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage         = VMA_MEMORY_USAGE_AUTO;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    check(vmaCreateImage(context.allocator(), &imageInfo, &allocInfo,
                         &image.handle, &image.allocation, nullptr),
          "vmaCreateImage");

    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(width) * height * 4;
    Buffer staging = createBuffer(context, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mapped(), pixels, static_cast<size_t>(byteSize));
    check(vmaFlushAllocation(context.allocator(), staging.allocation, 0, byteSize),
          "vmaFlushAllocation");

    context.immediateSubmit([&](VkCommandBuffer cmd) {
        transitionImageLayout(cmd, image.handle, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mipLevels);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.handle, image.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Successive halving blits build the mip chain.
        int32_t mipWidth  = static_cast<int32_t>(width);
        int32_t mipHeight = static_cast<int32_t>(height);

        for (uint32_t level = 1; level < image.mipLevels; ++level)
        {
            transitionImageLayout(cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1);

            const int32_t nextWidth  = std::max(mipWidth / 2, 1);
            const int32_t nextHeight = std::max(mipHeight / 2, 1);

            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            blit.srcOffsets[1]  = {mipWidth, mipHeight, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1]  = {nextWidth, nextHeight, 1};

            vkCmdBlitImage(cmd,
                           image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            transitionImageLayout(cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1);

            mipWidth  = nextWidth;
            mipHeight = nextHeight;
        }

        transitionImageLayout(cmd, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_ASPECT_COLOR_BIT, image.mipLevels - 1, 1);
    });

    destroyBuffer(context, staging);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image                       = image.handle;
    viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                      = image.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = image.mipLevels;
    viewInfo.subresourceRange.layerCount = 1;

    check(vkCreateImageView(context.device(), &viewInfo, nullptr, &image.view),
          "vkCreateImageView(texture)");

    return image;
}

} // namespace

Image createTexture2D(Context& context, const TextureData& source, bool generateMips)
{
    return createImageFromPixels(context, source.pixels.data(), source.width, source.height,
                                 source.srgb, generateMips, source.name.c_str());
}

Image createSolidTexture(Context& context, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb)
{
    const uint8_t pixel[4]{r, g, b, a};
    return createImageFromPixels(context, pixel, 1, 1, srgb, false, "solid");
}

void destroyImage(Context& context, Image& image)
{
    if (image.view)   vkDestroyImageView(context.device(), image.view, nullptr);
    if (image.handle) vmaDestroyImage(context.allocator(), image.handle, image.allocation);
    image = {};
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

std::string shaderDirectory()
{
    static const std::string directory = [] {
        const fs::path exeDir = executableDirectory();

        const fs::path candidates[]{
            exeDir / MV_SHADER_DIR,
            exeDir / ".." / MV_SHADER_DIR,
            fs::path(MV_SHADER_DIR)};

        std::error_code ec;
        for (const fs::path& candidate : candidates)
            if (fs::is_directory(candidate, ec))
                return pathToUtf8(fs::weakly_canonical(candidate, ec));

        return pathToUtf8(exeDir / MV_SHADER_DIR);
    }();

    return directory;
}

VkShaderModule loadShaderModule(Context& context, const std::string& name)
{
    const fs::path path = pathFromUtf8(shaderDirectory()) / (name + ".spv");

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open shader module: " + pathToUtf8(path) +
                                 "\nWere the shaders compiled? Check the build output.");

    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0)
        throw std::runtime_error("Malformed SPIR-V module: " + pathToUtf8(path));

    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = static_cast<size_t>(size);
    createInfo.pCode    = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(context.device(), &createInfo, nullptr, &module),
          "vkCreateShaderModule");

    return module;
}

} // namespace mv::gfx
