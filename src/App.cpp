#include "App.hpp"

#include <chrono>
#include <thread>
#include <exception>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "core/Log.hpp"
#include "core/Utf8.hpp"
#include "scene/SplatLoader.hpp"

namespace fs = std::filesystem;

namespace mv {
namespace {

constexpr uint32_t kDefaultWidth  = 1600;
constexpr uint32_t kDefaultHeight = 900;

} // namespace

App::App()
    : m_window(kDefaultWidth, kDefaultHeight, "ModelViewer")
{
}

App::~App() { shutdown(); }

// ---------------------------------------------------------------------------

void App::init()
{
#if MV_VALIDATION
    constexpr bool validation = true;
#else
    constexpr bool validation = false;
#endif

    m_context.init(m_window, validation);

    // The ImGui context must exist before the renderer initialises its backends.
    m_ui.createContext(m_window);
    m_renderer.init(m_context, m_window);

    installCallbacks();

    m_camera.focus(AABB{glm::vec3(-1.0f), glm::vec3(1.0f)});
    m_animator.setScene(&m_scene);
}

void App::installCallbacks()
{
    m_window.onFilesDropped = [this](const std::vector<std::string>& files) {
        // Defer to the main loop: the callback fires inside glfwPollEvents.
        m_pendingFiles = files;
    };

    m_window.onScroll = [this](double /*xoffset*/, double yoffset) {
        // ImGui has already seen this event and chained to us; ignore the
        // wheel when the cursor is over a panel so its lists still scroll.
        if (m_ui.wantsMouse()) return;
        m_camera.applyScroll(static_cast<float>(yoffset));
    };

    // ImGui installs its own GLFW callbacks and chains to whatever was set
    // before it, so this one keeps working.
    glfwSetWindowUserPointer(m_window.handle(), &m_window);
}

void App::shutdown()
{
    if (m_context.device()) m_context.waitIdle();

    m_renderer.shutdown();
    m_ui.destroyContext();
    m_context.shutdown();
}

// ---------------------------------------------------------------------------

int App::run(const std::vector<std::string>& arguments)
{
    try
    {
        init();
    }
    catch (const std::exception& e)
    {
        log::error("Startup failed: ", e.what());
        return 1;
    }

    for (const std::string& argument : arguments)
    {
        if (argument.empty() || argument.front() == '-') continue;
        loadModel(argument);
        break;
    }

    m_running = true;

    auto previous = std::chrono::steady_clock::now();
    float fpsAccumulator = 0.0f;
    int   fpsFrames      = 0;

    while (m_running && !m_window.shouldClose())
    {
        m_window.pollEvents();

        if (!m_pendingFiles.empty())
        {
            const std::string file = m_pendingFiles.front();
            m_pendingFiles.clear();
            loadModel(pathFromUtf8(file));
        }

        const auto now = std::chrono::steady_clock::now();
        m_deltaTime = std::chrono::duration<float>(now - previous).count();
        previous    = now;
        m_deltaTime = glm::clamp(m_deltaTime, 0.0f, 0.1f);   // survive breakpoints

        fpsAccumulator += m_deltaTime;
        ++fpsFrames;
        if (fpsAccumulator >= 0.25f)
        {
            m_fps          = static_cast<float>(fpsFrames) / fpsAccumulator;
            fpsAccumulator = 0.0f;
            fpsFrames      = 0;
        }

        if (m_window.isMinimised())
        {
            m_window.waitEvents();
            continue;
        }

        if (m_window.consumeResizeFlag()) m_renderer.onWindowResized();

        // -- input ---------------------------------------------------------
        const bool uiWantsMouse    = m_ui.wantsMouse();
        const bool uiWantsKeyboard = m_ui.wantsKeyboard();

        m_camera.update(m_window.handle(), m_deltaTime, uiWantsMouse);

        if (!uiWantsKeyboard)
        {
            if (glfwGetKey(m_window.handle(), GLFW_KEY_F) == GLFW_PRESS) focusCamera();

            static bool gLatch = false, wLatch = false, spaceLatch = false;

            const bool g = glfwGetKey(m_window.handle(), GLFW_KEY_G) == GLFW_PRESS;
            if (g && !gLatch) m_settings.showGrid = !m_settings.showGrid;
            gLatch = g;

            const bool w = glfwGetKey(m_window.handle(), GLFW_KEY_W) == GLFW_PRESS;
            if (w && !wLatch) m_settings.wireframe = !m_settings.wireframe;
            wLatch = w;

            const bool space = glfwGetKey(m_window.handle(), GLFW_KEY_SPACE) == GLFW_PRESS;
            if (space && !spaceLatch && m_animator.hasAnimations()) m_animator.togglePlay();
            spaceLatch = space;
        }

        if (m_settings.vsync != m_context.vsync())
        {
            m_context.setVsync(m_settings.vsync);
            m_renderer.onWindowResized();
        }

        // -- simulate ------------------------------------------------------
        m_animator.update(m_deltaTime);

        // -- render --------------------------------------------------------
        if (!m_renderer.beginFrame()) continue;

        m_ui.build(*this);

        m_renderer.endFrame(hasModel() ? &m_scene : nullptr,
                            hasModel() ? &m_animator : nullptr,
                            m_camera, m_settings);

        // Optional frame rate cap.
        //
        // Sleeping gets close but overshoots by a millisecond or so, which is
        // enough to miss a 144 Hz deadline, so the last stretch is spun out.
        if (m_settings.frameRateCap > 0)
        {
            const auto target = std::chrono::duration<double>(
                1.0 / static_cast<double>(m_settings.frameRateCap));
            const auto deadline =
                now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(target);

            const auto spinFrom = deadline - std::chrono::microseconds(1200);
            if (std::chrono::steady_clock::now() < spinFrom)
                std::this_thread::sleep_until(spinFrom);
            while (std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
        }
    }

    m_context.waitIdle();
    return 0;
}

void App::quit() { m_running = false; }

// ---------------------------------------------------------------------------

bool App::loadModel(const fs::path& path)
{
    setStatus("Loading " + pathToUtf8(path.filename()) + "...");
    log::info("Loading ", pathToUtf8(path));

    // Gaussian-splat assets (.spz, or a .ply carrying splat properties) take a
    // separate path: they are point clouds of anisotropic Gaussians, not meshes.
    if (SplatLoader::canLoad(path))
        return loadSplat(path);

    Scene       loaded;
    std::string error;

    if (!ModelLoader::load(path, m_importOptions, loaded, error))
    {
        log::error("Import failed: ", error);
        setStatus("Import failed: " + error);
        return false;
    }

    m_context.waitIdle();

    // A mesh and a splat cloud are mutually exclusive as "the model"; drop any
    // splats the previous load left on the GPU.
    if (!m_splatCloud.empty()) { m_splatCloud.clear(); m_renderer.clearSplats(); }

    m_scene = std::move(loaded);
    m_animator.setScene(&m_scene);
    m_renderer.uploadScene(m_scene);

    // Node indices from the previous model mean nothing now.
    m_history.clear();
    m_ui.clearVisibilityState();

    // Textured models keep their own colours; untextured ones follow the
    // colour wheel so there is always something sensible on screen.
    m_settings.overrideUntextured = m_scene.textures.empty();

    if (!m_scene.cameras.empty())
        adoptSceneCamera(0);
    else
        focusCamera();

    updateWindowTitle();

    setStatus(pathToUtf8(path.filename()) + " loaded: " +
              std::to_string(m_scene.meshes.size()) + " meshes, " +
              std::to_string(m_scene.indices.size() / 3) + " triangles" +
              (m_scene.animations.empty()
                   ? ""
                   : ", " + std::to_string(m_scene.animations.size()) + " animation(s)"));

    return true;
}

bool App::loadSplat(const fs::path& path)
{
    SplatCloud  loaded;
    std::string error;

    if (!SplatLoader::load(path, loaded, error))
    {
        log::error("Splat import failed: ", error);
        setStatus("Import failed: " + error);
        return false;
    }

    m_context.waitIdle();

    // Splats replace whatever was loaded before, mesh or splat.
    if (!m_scene.empty())
    {
        m_renderer.clearScene();
        m_scene.clear();
        m_animator.setScene(&m_scene);
    }

    m_splatCloud = std::move(loaded);
    m_renderer.uploadSplats(m_splatCloud);

    m_history.clear();
    m_ui.clearVisibilityState();

    focusCamera();
    updateWindowTitle();

    setStatus(pathToUtf8(path.filename()) + " loaded: " +
              std::to_string(m_splatCloud.count()) + " gaussians, SH degree " +
              std::to_string(m_splatCloud.shDegree));

    return true;
}

bool App::exportModel(const fs::path& path, const std::string& formatId)
{
    if (!hasModel())
    {
        setStatus("Nothing to export.");
        return false;
    }

    std::string error;
    if (!ModelLoader::exportScene(m_scene, formatId, path, error))
    {
        log::error("Export failed: ", error);
        setStatus("Export failed: " + error);
        return false;
    }

    setStatus("Exported to " + pathToUtf8(path));
    return true;
}

void App::closeModel()
{
    m_context.waitIdle();

    m_renderer.clearScene();
    m_scene.clear();
    m_animator.setScene(&m_scene);

    if (!m_splatCloud.empty()) { m_splatCloud.clear(); m_renderer.clearSplats(); }

    m_history.clear();
    m_ui.clearVisibilityState();

    updateWindowTitle();
    setStatus("Model closed.");
}

void App::focusCamera()
{
    if (m_scene.bounds.valid())            m_camera.focus(m_scene.bounds);
    else if (m_splatCloud.bounds.valid())  m_camera.focus(m_splatCloud.bounds);
    else                                   m_camera.focus(AABB{glm::vec3(-1.0f), glm::vec3(1.0f)});
}

void App::adoptSceneCamera(int index)
{
    if (index < 0 || index >= static_cast<int>(m_scene.cameras.size())) return;

    CameraDef definition = m_scene.cameras[static_cast<size_t>(index)];

    // Place the camera in world space if it is parented to a node.
    if (definition.node != kInvalidIndex)
    {
        const std::vector<glm::mat4> globals = m_scene.computeGlobalTransforms();
        if (definition.node < static_cast<int>(globals.size()))
        {
            const glm::mat4& world = globals[static_cast<size_t>(definition.node)];
            definition.position = glm::vec3(world * glm::vec4(definition.position, 1.0f));
            definition.lookAt   = glm::vec3(world * glm::vec4(definition.lookAt, 1.0f));
            definition.up       = glm::normalize(glm::mat3(world) * definition.up);
        }
    }

    m_camera.adopt(definition);
    setStatus("Using camera '" + definition.name + "' from the file.");
}

void App::updateWindowTitle()
{
    const std::string& source = modelPath();
    if (source.empty())
        m_window.setTitle("ModelViewer");
    else
        m_window.setTitle("ModelViewer - " +
                          pathToUtf8(pathFromUtf8(source).filename()));
}

} // namespace mv
