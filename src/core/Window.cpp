#include "core/Window.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

#include <stb_image.h>

#include <stdexcept>

#include <GLFW/glfw3.h>

#include "core/Log.hpp"
#include "core/Utf8.hpp"

namespace mv {
namespace {

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

/// Directory the executable lives in. Resources are staged alongside it so a
/// packaged build works from any working directory.
std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0)
        return std::filesystem::path(std::wstring(buffer, length)).parent_path();
#elif defined(__linux__)
    char buffer[4096]{};
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length > 0)
        return std::filesystem::path(std::string(buffer, static_cast<size_t>(length)))
            .parent_path();
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

/// Looks for a resource next to the executable, then one level up (which is
/// where it lands in a plain build tree), then in the source tree.
std::string resourcePath(const char* name)
{
    const std::filesystem::path exeDir = executableDirectory();
    const std::filesystem::path candidates[] = {
        exeDir / "resources" / name,
        exeDir / ".." / "resources" / name,
        exeDir / ".." / "share" / "modelviewer" / name,
        std::filesystem::path(MV_RESOURCE_DIR) / name};

    std::error_code ec;
    for (const auto& candidate : candidates)
        if (std::filesystem::exists(candidate, ec)) return pathToUtf8(candidate);

    return {};
}

} // namespace

namespace {

int  g_glfwRefCount = 0;

void glfwErrorCallback(int code, const char* description)
{
    // Some GLFW errors are raised once per frame by design (Wayland refuses
    // window-position queries, for instance). Logging every occurrence buries
    // everything else and makes the log panel churn, so identical consecutive
    // errors are reported once.
    static int         lastCode = 0;
    static std::string lastText;
    static int         repeats = 0;

    const std::string text = description ? description : "(no description)";
    if (code == lastCode && text == lastText)
    {
        if (++repeats == 1)
            log::error("GLFW error ", code, ": ", text, " (further repeats suppressed)");
        return;
    }

    lastCode = code;
    lastText = text;
    repeats  = 0;
    log::error("GLFW error ", code, ": ", text);
}

void initGlfw()
{
    if (g_glfwRefCount++ > 0) return;

    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
        throw std::runtime_error("Failed to initialise GLFW");

    if (!glfwVulkanSupported())
    {
        glfwTerminate();
        throw std::runtime_error(
            "No Vulkan loader found. Install the Vulkan runtime / GPU drivers with Vulkan support.");
    }
}

void shutdownGlfw()
{
    if (--g_glfwRefCount == 0)
        glfwTerminate();
}

} // namespace

// ---------------------------------------------------------------------------

Window::Window(uint32_t width, uint32_t height, std::string title)
    : m_title(std::move(title))
{
    initGlfw();

    // The compositor matches this against the .desktop file's StartupWMClass
    // to find the application icon. On Wayland this is the ONLY way to get an
    // icon: the protocol has no "set window icon" request at all, which is why
    // glfwSetWindowIcon below is X11/Windows-only.
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "modelviewer");
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "modelviewer");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "modelviewer");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
#ifdef GLFW_SCALE_TO_MONITOR
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

    m_window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                                m_title.c_str(), nullptr, nullptr);
    if (!m_window)
    {
        shutdownGlfw();
        throw std::runtime_error("Failed to create a window");
    }

    glfwSetWindowUserPointer(m_window, this);
    setIconFromFile(resourcePath("icon.png"));

    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallback);
    glfwSetDropCallback(m_window, dropCallback);
    glfwSetScrollCallback(m_window, scrollCallback);
}

Window::~Window()
{
    if (m_window) glfwDestroyWindow(m_window);
    shutdownGlfw();
}

void Window::framebufferSizeCallback(GLFWwindow* window, int, int)
{
    if (auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window)))
        self->m_resized = true;
}

void Window::dropCallback(GLFWwindow* window, int count, const char** paths)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self || !self->onFilesDropped) return;

    std::vector<std::string> files;
    files.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        files.emplace_back(paths[i]);

    self->onFilesDropped(files);
}

void Window::scrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self || !self->onScroll) return;

    self->onScroll(xoffset, yoffset);
}

bool Window::shouldClose() const { return glfwWindowShouldClose(m_window) != 0; }
void Window::requestClose()      { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }
void Window::pollEvents()        { glfwPollEvents(); }
void Window::waitEvents()        { glfwWaitEvents(); }

VkSurfaceKHR Window::createSurface(VkInstance instance) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = glfwCreateWindowSurface(instance, m_window, nullptr, &surface);
    if (result != VK_SUCCESS)
        throw std::runtime_error("Failed to create a Vulkan surface for the window");
    return surface;
}

glm::uvec2 Window::framebufferSize() const
{
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

void Window::setIconFromFile(const std::string& path)
{
    if (!m_window || path.empty()) return;

    // Wayland has no window-icon request; the icon comes from the .desktop
    // file matched by app id. Asking anyway only produces an error.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) return;

    // Read through an fs::path stream (wide handle on Windows) and decode from
    // memory, so an install path with non-ASCII characters still loads. path is
    // UTF-8; pathFromUtf8 turns it back into the native wide path.
    std::ifstream stream(pathFromUtf8(path), std::ios::binary);
    if (!stream)
    {
        log::warn("Could not open window icon: ", path);
        return;
    }
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                  &width, &height, &channels, 4);
    if (!pixels)
    {
        log::warn("Could not load window icon: ", path);
        return;
    }

    GLFWimage image{};
    image.width  = width;
    image.height = height;
    image.pixels = pixels;
    glfwSetWindowIcon(m_window, 1, &image);

    stbi_image_free(pixels);
}

int Window::refreshRate() const
{
    if (!m_window) return 0;

    // A fullscreen window knows its own monitor.
    if (GLFWmonitor* fullscreen = glfwGetWindowMonitor(m_window))
    {
        const GLFWvidmode* mode = glfwGetVideoMode(fullscreen);
        return mode ? mode->refreshRate : 0;
    }

    // Wayland deliberately does not tell a client where its window is, so the
    // overlap test below cannot run there -- calling glfwGetWindowPos only
    // raises an error. Fall back to the primary monitor, which is right for
    // the common single-display case and a reasonable guess otherwise.
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
    {
        const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
        return mode ? mode->refreshRate : 0;
    }

    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(m_window, &wx, &wy);
    glfwGetWindowSize(m_window, &ww, &wh);

    int           count    = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (!monitors || count == 0) return 0;

    int bestArea = 0;
    int bestHz   = 0;

    for (int i = 0; i < count; ++i)
    {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (!mode) continue;

        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);

        const int overlapW = imin(wx + ww, mx + mode->width)  - imax(wx, mx);
        const int overlapH = imin(wy + wh, my + mode->height) - imax(wy, my);
        if (overlapW <= 0 || overlapH <= 0) continue;

        const int area = overlapW * overlapH;
        if (area > bestArea)
        {
            bestArea = area;
            bestHz   = mode->refreshRate;
        }
    }

    return bestHz;
}

bool Window::isMinimised() const
{
    const glm::uvec2 size = framebufferSize();
    return size.x == 0 || size.y == 0;
}

bool Window::consumeResizeFlag()
{
    const bool value = m_resized;
    m_resized = false;
    return value;
}

void Window::setTitle(const std::string& title)
{
    m_title = title;
    glfwSetWindowTitle(m_window, m_title.c_str());
}

std::vector<const char*> Window::requiredInstanceExtensions() const
{
    uint32_t     count      = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    if (!extensions)
        throw std::runtime_error("GLFW could not report the required Vulkan instance extensions");

    return {extensions, extensions + count};
}

} // namespace mv
