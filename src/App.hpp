#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/Camera.hpp"
#include "core/Window.hpp"
#include "scene/Animator.hpp"
#include "scene/History.hpp"
#include "scene/ModelLoader.hpp"
#include "scene/Scene.hpp"
#include "scene/SplatCloud.hpp"
#include "ui/UiLayer.hpp"
#include "vk/Context.hpp"
#include "vk/Renderer.hpp"

namespace mv {

/// Ties the window, Vulkan context, scene and UI together.
class App
{
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    int run(const std::vector<std::string>& arguments);

    // -- actions the UI can trigger ---------------------------------------
    bool loadModel(const std::filesystem::path& path);
    bool loadSplat(const std::filesystem::path& path);
    bool exportModel(const std::filesystem::path& path, const std::string& formatId);
    void closeModel();
    void focusCamera();
    void adoptSceneCamera(int index);
    void quit();

    // -- state the UI reads -----------------------------------------------
    Scene&               scene()          { return m_scene; }
    const Scene&         scene() const    { return m_scene; }
    Animator&            animator()       { return m_animator; }
    const Window&        window() const   { return m_window; }
    History&             history()        { return m_history; }
    Camera&              camera()         { return m_camera; }
    gfx::RenderSettings& settings()       { return m_settings; }
    ImportOptions&       importOptions()  { return m_importOptions; }
    const gfx::Context&  context() const  { return m_context; }
    const gfx::Renderer& renderer() const { return m_renderer; }
    gfx::Renderer&       renderer()       { return m_renderer; }

    bool               hasModel() const { return !m_scene.empty() || !m_splatCloud.empty(); }
    const SplatCloud&  splatCloud() const { return m_splatCloud; }
    bool               hasSplats() const { return !m_splatCloud.empty(); }
    const std::string& status() const   { return m_status; }
    const std::string& modelPath() const
    {
        return m_scene.sourcePath.empty() ? m_splatCloud.sourcePath : m_scene.sourcePath;
    }
    float              deltaTime() const { return m_deltaTime; }
    float              fps() const { return m_fps; }

    void setStatus(std::string message) { m_status = std::move(message); }

private:
    void init();
    void shutdown();
    void installCallbacks();
    void updateWindowTitle();

    Window        m_window;
    gfx::Context  m_context;
    gfx::Renderer m_renderer;
    UiLayer       m_ui;

    Scene         m_scene;
    SplatCloud    m_splatCloud;
    Animator      m_animator;
    History       m_history;
    Camera        m_camera;
    ImportOptions m_importOptions;

    gfx::RenderSettings m_settings;

    std::string m_status = "Drop a model onto the window, or use File > Open.";
    float       m_deltaTime = 0.0f;
    float       m_fps       = 0.0f;
    bool        m_running   = false;

    std::vector<std::string> m_pendingFiles;   // filled by the drop callback
};

} // namespace mv
