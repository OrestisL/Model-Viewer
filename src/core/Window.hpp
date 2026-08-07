#pragma once

#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

struct GLFWwindow;

namespace mv {

/// Thin GLFW wrapper. Owns the window and the Vulkan surface creation.
class Window
{
public:
    Window(uint32_t width, uint32_t height, std::string title);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    GLFWwindow* handle() const { return m_window; }

    bool shouldClose() const;
    void requestClose();
    void pollEvents();
    void waitEvents();

    VkSurfaceKHR createSurface(VkInstance instance) const;

    glm::uvec2 framebufferSize() const;
    bool       isMinimised() const;

    /// Refresh rate of the monitor the window currently sits on, in Hz, or 0
    /// if it cannot be determined. On a mixed-refresh desktop this is what the
    /// compositor ought to be pacing us to; when it does not match the
    /// measured frame rate, that mismatch is the whole story.
    int        refreshRate() const;

    /// Sets the taskbar/titlebar icon from a PNG. No-op on Wayland, which has
    /// no such protocol request -- see the .desktop file instead.
    void       setIconFromFile(const std::string& path);

    /// True once per resize; clears the flag.
    bool consumeResizeFlag();

    void setTitle(const std::string& title);

    /// Files dropped onto the window (drag & drop import).
    std::function<void(const std::vector<std::string>&)> onFilesDropped;

    /// Mouse wheel, as (xoffset, yoffset). Registered in the constructor,
    /// which runs before ImGui installs its own GLFW callbacks -- ImGui
    /// therefore records this one as its "previous" callback and chains to
    /// it after taking its own look at the event.
    std::function<void(double, double)> onScroll;

    /// Required instance extensions for the current platform. Only valid
    /// while at least one Window is alive (GLFW owns the returned strings).
    std::vector<const char*> requiredInstanceExtensions() const;

private:
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void dropCallback(GLFWwindow* window, int count, const char** paths);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);

    GLFWwindow* m_window  = nullptr;
    bool        m_resized = false;
    std::string m_title;
};

} // namespace mv
