// The single translation unit that instantiates the header-only dependencies.
// Keeping them here means the rest of the project can include the headers
// freely without worrying about duplicate symbols, and it isolates the (large)
// compile cost and the third-party warnings to one file.

// ---------------------------------------------------------------------------
// Warning suppression -- these are other people's headers.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#    pragma warning(push, 0)
#elif defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wall"
#    pragma GCC diagnostic ignored "-Wextra"
#    pragma GCC diagnostic ignored "-Wpedantic"
#    pragma GCC diagnostic ignored "-Wunused-variable"
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#    pragma GCC diagnostic ignored "-Wtype-limits"
#endif

// ---------------------------------------------------------------------------
// stb_image -- texture decoding (PNG/JPG/TGA/BMP/PSD/GIF/HDR).
// ---------------------------------------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WARNINGS
#include <stb_image.h>

// ---------------------------------------------------------------------------
// VulkanMemoryAllocator.
//
// Vulkan 1.3 is the target, so tell VMA it may use the newer allocation
// entry points directly rather than probing for the KHR extension versions.
// ---------------------------------------------------------------------------
#define VMA_IMPLEMENTATION
#define VMA_VULKAN_VERSION 1003000
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

// VMA's own assertions route through the project logger in debug builds so a
// bad allocation shows up in the in-app log panel instead of only in stderr.
#if !defined(NDEBUG)
#    include <cassert>
#    define VMA_ASSERT(expr) assert(expr)
#else
#    define VMA_ASSERT(expr) ((void)0)
#endif

#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#    pragma warning(pop)
#elif defined(__clang__)
#    pragma clang diagnostic pop
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif
