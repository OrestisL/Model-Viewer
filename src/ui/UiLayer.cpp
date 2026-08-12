#include "ui/UiLayer.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <cmath>
#include <vector>
#include <cstdio>

#include <imgui.h>
#include <ImGuizmo.h>

#include <imgui_internal.h>   // DockBuilder

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "App.hpp"
#include "core/Log.hpp"
#include "core/Utf8.hpp"
#include "core/Window.hpp"
#include "scene/ModelLoader.hpp"
#include "scene/UsdBackend.hpp"

namespace fs = std::filesystem;

namespace mv {
namespace {

/// Per-user config location, created if needed. Empty on failure, in which
/// case ImGui simply does not persist anything.
std::string configFilePath(const char* name)
{
    namespace fs = std::filesystem;
    fs::path dir;

#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) dir = fs::path(appdata) / "ModelViewer";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) dir = fs::path(xdg) / "modelviewer";
    else if (const char* home = std::getenv("HOME")) dir = fs::path(home) / ".config" / "modelviewer";
#endif

    if (dir.empty()) return {};

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return {};

    return pathToUtf8(dir / name);
}

} // namespace

namespace {

void helpMarker(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string formatDuration(float seconds)
{
    const int minutes = static_cast<int>(seconds) / 60;
    const float rest  = seconds - static_cast<float>(minutes * 60);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d:%05.2f", minutes, static_cast<double>(rest));
    return buffer;
}

} // namespace

UiLayer::~UiLayer() { destroyContext(); }

void UiLayer::createContext(Window&)
{
    if (m_contextCreated) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef IMGUI_HAS_DOCK
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
    // Layout is saved next to the user's other config, not in the working
    // directory: launched from a file manager the CWD is wherever the file
    // happened to be, so a relative path scatters ini files around the disk.
    m_iniPath = configFilePath("imgui.ini");
    io.IniFilename = m_iniPath.empty() ? nullptr : m_iniPath.c_str();

    // A saved layout takes precedence over the built-in default, so changing
    // the default alone never reaches anyone who has already run the app. The
    // stamp forces the new arrangement through once, after which the user's
    // own adjustments are theirs to keep.
    constexpr int kLayoutVersion = 2;

    const std::string stampPath = configFilePath("layout-version");
    int               stamp     = 0;

    if (!stampPath.empty())
    {
        if (std::ifstream in{stampPath}) in >> stamp;
    }

    m_resetLayout = m_iniPath.empty() ||
                    !std::filesystem::exists(m_iniPath) ||
                    stamp != kLayoutVersion;

    if (m_resetLayout && !stampPath.empty())
    {
        if (std::ofstream out{stampPath}) out << kLayoutVersion;
    }

    // The display scale is not known here, so the font is loaded at a size
    // that stays legible unscaled and sharpens rather than blurs when the UI
    // scale is raised.
    m_fontPath = ui::loadUiFont(17.0f);
    if (m_fontPath.empty())
        log::warn("No system UI font found; falling back to the built-in bitmap font");
    else
        log::info("UI font: ", m_fontPath);

    applyTheme();
    m_contextCreated = true;
}

void UiLayer::destroyContext()
{
    if (!m_contextCreated) return;
    ImGui::DestroyContext();
    m_contextCreated = false;
}

void UiLayer::applyTheme()
{
    ui::applyStyle(m_theme, m_accent, m_uiScale);
}

std::vector<uint8_t> UiLayer::visibilitySnapshot(const Scene& scene) const
{
    std::vector<uint8_t> snapshot(scene.nodes.size());
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        snapshot[i] = scene.nodes[i].visible ? 1u : 0u;
    return snapshot;
}

void UiLayer::recordVisibilityEdit(App& app, const std::vector<uint8_t>& before,
                                   const char* label)
{
    const Scene& scene = app.scene();

    VisibilityEdit edit;
    edit.label = label;

    for (size_t i = 0; i < scene.nodes.size() && i < before.size(); ++i)
    {
        const bool now = scene.nodes[i].visible;
        if ((before[i] != 0) == now) continue;

        VisibilityChange change;
        change.node   = static_cast<int>(i);
        change.before = before[i] != 0;
        change.after  = now;
        edit.changes.push_back(change);
    }

    app.history().push(std::move(edit));   // empty edits are dropped inside
}

void UiLayer::applyUndoRedo(App& app, bool redoInstead)
{
    const std::vector<uint8_t> before = visibilitySnapshot(app.scene());

    const int touched = redoInstead
                            ? app.history().redo(app.scene(), app.animator())
                            : app.history().undo(app.scene(), app.animator());

    // Reveal what just changed rather than leaving the gumball elsewhere.
    if (touched >= 0) m_selectedNode = touched;

    // If visibility moved, any isolate snapshot describes a state that no
    // longer exists -- leaving it would make the next I restore nonsense.
    if (m_isolated && visibilitySnapshot(app.scene()) != before)
        clearVisibilityState();
}

void UiLayer::clearVisibilityState()
{
    m_isolated = false;
    m_visibilityBeforeIsolate.clear();
}

void UiLayer::toggleIsolate(App& app)
{
    Scene& scene = app.scene();

    if (m_isolated)
    {
        const std::vector<uint8_t> before = visibilitySnapshot(scene);

        // Restore rather than show-all: anything hidden before isolating was
        // hidden deliberately and should stay that way.
        for (size_t i = 0; i < scene.nodes.size() && i < m_visibilityBeforeIsolate.size(); ++i)
            scene.nodes[i].visible = m_visibilityBeforeIsolate[i] != 0;

        m_isolated = false;
        m_visibilityBeforeIsolate.clear();
        recordVisibilityEdit(app, before, "Leave isolate");
        log::info("Isolate off");
        return;
    }

    if (m_selectedNode < 0 || static_cast<size_t>(m_selectedNode) >= scene.nodes.size())
    {
        log::warn("Select something to isolate");
        return;
    }

    m_visibilityBeforeIsolate.resize(scene.nodes.size());
    for (size_t i = 0; i < scene.nodes.size(); ++i)
        m_visibilityBeforeIsolate[i] = scene.nodes[i].visible ? 1u : 0u;

    const std::vector<uint8_t> before = visibilitySnapshot(scene);
    scene.isolate(m_selectedNode);
    m_isolated = true;
    recordVisibilityEdit(app, before, "Isolate");

    const Node& node = scene.nodes[static_cast<size_t>(m_selectedNode)];
    log::info("Isolated ", node.name.empty() ? "<unnamed>" : node.name,
              " (I to leave)");
}

void UiLayer::handleVisibilityShortcuts(App& app)
{
    if (!app.hasModel()) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl) return;

    // A plain toggle. Trying to be clever about re-isolating onto a different
    // selection makes the key unpredictable: sometimes it leaves, sometimes it
    // does not. Leave, select, isolate again is two keystrokes and always does
    // what it looks like.
    if (ImGui::IsKeyPressed(ImGuiKey_I, false))
    {
        toggleIsolate(app);
        return;
    }

    // Shift+H reveals everything rather than un-hiding the selection: once a
    // node is hidden it cannot be picked, so a selection-based un-hide would
    // strand anything hidden while nothing was selected.
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_H, false))
    {
        const std::vector<uint8_t> before = visibilitySnapshot(app.scene());
        app.scene().showAll();
        clearVisibilityState();
        recordVisibilityEdit(app, before, "Show all");
        log::info("All meshes shown");
        return;
    }

    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_H, false))
    {
        Scene& scene = app.scene();
        if (m_selectedNode < 0 ||
            static_cast<size_t>(m_selectedNode) >= scene.nodes.size())
        {
            log::warn("Nothing selected to hide");
            return;
        }

        const std::vector<uint8_t> before = visibilitySnapshot(scene);

        Node& node   = scene.nodes[static_cast<size_t>(m_selectedNode)];
        node.visible = false;

        recordVisibilityEdit(app, before, "Hide mesh");

        log::info("Hid ", node.name.empty() ? "<unnamed>" : node.name,
                  " (Shift+H shows everything again)");

        // Keep the selection: the manipulator would otherwise sit on something
        // invisible, and the node tree still shows what is selected.
        m_selectedNode = kInvalidIndex;
    }
}

void UiLayer::handleViewportPicking(App& app)
{
    if (!app.hasModel()) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || ImGuizmo::IsOver() || ImGuizmo::IsUsing()) return;
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

    // Select on release, not press, so an orbit drag that happens to start on
    // the model does not also reselect it.
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;

    const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    if (std::abs(drag.x) + std::abs(drag.y) > 4.0f) return;

    // Cursor -> NDC. The projection already carries Vulkan's Y flip, so screen
    // Y and clip Y both run downwards and no extra negation is needed.
    const float ndcX = (2.0f * io.MousePos.x / io.DisplaySize.x) - 1.0f;
    const float ndcY = (2.0f * io.MousePos.y / io.DisplaySize.y) - 1.0f;

    const Camera&   camera = app.camera();
    const glm::mat4 invVP  = glm::inverse(
        camera.projection(io.DisplaySize.x / io.DisplaySize.y) * camera.view());

    // Depth 0 is the near plane and 1 the far plane under
    // GLM_FORCE_DEPTH_ZERO_TO_ONE.
    glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPoint  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::abs(nearPoint.w) < 1e-9f || std::abs(farPoint.w) < 1e-9f) return;

    const glm::vec3 origin    = glm::vec3(nearPoint) / nearPoint.w;
    const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);

    const int hit = app.scene().pickNode(origin, direction);

    // A click on empty space clears the selection, which is how you dismiss
    // the manipulator.
    m_selectedNode = hit;
}

void UiLayer::drawGizmo(App& app)
{
    if (!m_gizmoEnabled || !app.hasModel()) return;

    Scene& scene = app.scene();
    if (m_selectedNode < 0 || static_cast<size_t>(m_selectedNode) >= scene.nodes.size())
        return;

    // The animator rewrites every node's transform each frame, so an edit made
    // while a clip is running would be discarded before it was ever drawn.
    if (app.animator().hasAnimations() && app.animator().isPlaying()) return;

    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

    const Camera& camera = app.camera();
    glm::mat4 view = camera.view();
    glm::mat4 proj = camera.projection(io.DisplaySize.x / io.DisplaySize.y);

    // Camera::projection() negates [1][1] for Vulkan's flipped clip space.
    // ImGuizmo does its own screen-space maths assuming the OpenGL
    // convention, so hand it a matrix with that flip undone.
    proj[1][1] *= -1.0f;

    const std::vector<glm::mat4> globals = scene.computeGlobalTransforms();
    glm::mat4 world = globals[static_cast<size_t>(m_selectedNode)];

    ImGuizmo::OPERATION operation = ImGuizmo::UNIVERSAL;
    switch (m_gizmoOp)
    {
        case GizmoOp::Translate: operation = ImGuizmo::TRANSLATE; break;
        case GizmoOp::Rotate:    operation = ImGuizmo::ROTATE;    break;
        case GizmoOp::Scale:     operation = ImGuizmo::SCALE;     break;
        case GizmoOp::Universal:
        default: break;
    }

    // Latch which part of the gumball the cursor is over while it is idle:
    // once a drag begins ImGuizmo reports every operation as hovered, so this
    // is the only moment the distinction can be read.
    if (m_gizmoOp == GizmoOp::Universal && !ImGuizmo::IsUsing())
    {
        if (ImGuizmo::IsOver(ImGuizmo::ROTATE))       m_gizmoHovered = GizmoOp::Rotate;
        else if (ImGuizmo::IsOver(ImGuizmo::SCALEU))  m_gizmoHovered = GizmoOp::Scale;
        else                                          m_gizmoHovered = GizmoOp::Translate;
    }

    const GizmoOp snapFor = (m_gizmoOp == GizmoOp::Universal) ? m_gizmoHovered : m_gizmoOp;
    float         snapValue = m_snapTranslate;
    if (snapFor == GizmoOp::Rotate)     snapValue = m_snapRotate;
    else if (snapFor == GizmoOp::Scale) snapValue = m_snapScale;

    // ImGuizmo forces LOCAL itself whenever the operation contains SCALE, so
    // only the single-operation scale mode needs pinning here. UNIVERSAL uses
    // SCALEU instead and works correctly in world space.
    const ImGuizmo::MODE mode =
        (m_gizmoOp == GizmoOp::Scale || m_gizmoMode == GizmoMode::Local)
            ? ImGuizmo::LOCAL
            : ImGuizmo::WORLD;

    const float  snap[3] = {snapValue, snapValue, snapValue};
    const float* snapPtr = m_gizmoSnap ? snap : nullptr;

    // Snapshot the pre-drag state while the widget is idle. Once a drag is
    // running this is the only record of where the node started.
    const bool wasUsing = ImGuizmo::IsUsing();
    if (!wasUsing)
    {
        m_preDrag     = captureTransform(scene, m_selectedNode);
        m_preDragNode = m_selectedNode;
    }

    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                             operation, mode, glm::value_ptr(world),
                             nullptr, snapPtr))
    {
        Node& node = scene.nodes[static_cast<size_t>(m_selectedNode)];

        // Manipulate() hands back a world matrix; the node stores a local one.
        glm::mat4 parent(1.0f);
        if (node.parent != kInvalidIndex)
            parent = globals[static_cast<size_t>(node.parent)];

        const glm::mat4 local = glm::inverse(parent) * world;

        glm::vec3 translation{0.0f};
        glm::vec3 scale{1.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 skew{0.0f};
        glm::vec4 perspective{0.0f};

        if (glm::decompose(local, scale, rotation, translation, skew, perspective))
        {
            node.translation = translation;
            node.rotation    = rotation;
            node.scale       = scale;

            // The animator reapplies the rest pose every frame; without this
            // the edit is overwritten before it is ever drawn.
            app.animator().syncRestPose(m_selectedNode);
        }
    }

    // Commit one history entry per gesture, on release.
    if (wasUsing && !ImGuizmo::IsUsing() && m_preDragNode >= 0)
    {
        static const char* kLabels[] = {"Transform", "Move", "Rotate", "Scale"};
        const GizmoOp      kind      = (m_gizmoOp == GizmoOp::Universal)
                                           ? m_gizmoHovered
                                           : m_gizmoOp;

        TransformEdit edit;
        edit.node   = m_preDragNode;
        edit.before = m_preDrag;
        edit.after  = captureTransform(scene, m_preDragNode);
        edit.label  = kLabels[static_cast<int>(kind)];

        app.history().push(std::move(edit));   // no-ops are dropped inside
    }
}

void UiLayer::handleUndoShortcuts(App& app)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;           // typing in a field
    if (!io.KeyCtrl) return;

    const bool undo = ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.KeyShift;
    const bool redo = (ImGui::IsKeyPressed(ImGuiKey_Y, false)) ||
                      (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyShift);

    if (undo)      applyUndoRedo(app, false);
    else if (redo) applyUndoRedo(app, true);
}

void UiLayer::drawGizmoSettings(App& app)
{
    ImGui::Checkbox("Show manipulator", &m_gizmoEnabled);
    helpMarker("Select a node in the Model panel to attach the manipulator to it. "
               "Disabled while an animation is playing, since the animator owns "
               "node transforms in that case.");

    ImGui::BeginDisabled(!m_gizmoEnabled);

    int op = static_cast<int>(m_gizmoOp);
    ImGui::RadioButton("Gumball", &op, 0);
    helpMarker("Move, rotate and scale on one widget: straight arrows translate, "
               "arcs rotate, the outer handles scale.");
    ImGui::RadioButton("Move", &op, 1);   ImGui::SameLine();
    ImGui::RadioButton("Rotate", &op, 2); ImGui::SameLine();
    ImGui::RadioButton("Scale", &op, 3);
    m_gizmoOp = static_cast<GizmoOp>(op);

    int mode = static_cast<int>(m_gizmoMode);
    ImGui::BeginDisabled(m_gizmoOp == GizmoOp::Scale);
    ImGui::Combo("Frame", &mode, "World\0Local\0");
    ImGui::EndDisabled();
    m_gizmoMode = static_cast<GizmoMode>(mode);
    if (m_gizmoOp == GizmoOp::Scale)
        helpMarker("Scaling is always done in the object's own frame.");

    ImGui::Checkbox("Snap", &m_gizmoSnap);
    ImGui::BeginDisabled(!m_gizmoSnap);
    if (m_gizmoOp == GizmoOp::Universal)
    {
        // All three are shown because any of them can be the next drag.
        ImGui::DragFloat("Move step",   &m_snapTranslate, 0.01f, 0.001f, 10.0f, "%.3f");
        ImGui::DragFloat("Rotate step", &m_snapRotate,    0.5f,  1.0f,   90.0f, "%.0f deg");
        ImGui::DragFloat("Scale step",  &m_snapScale,     0.01f, 0.01f,  1.0f,  "%.2f");
    }
    else
    {
        switch (m_gizmoOp)
        {
            case GizmoOp::Rotate:
                ImGui::DragFloat("Step##rot", &m_snapRotate, 0.5f, 1.0f, 90.0f, "%.0f deg");
                break;
            case GizmoOp::Scale:
                ImGui::DragFloat("Step##scale", &m_snapScale, 0.01f, 0.01f, 1.0f, "%.2f");
                break;
            case GizmoOp::Translate:
            default:
                ImGui::DragFloat("Step##move", &m_snapTranslate, 0.01f, 0.001f, 10.0f, "%.3f");
                break;
        }
    }
    ImGui::EndDisabled();

    if (m_selectedNode < 0)
        ImGui::TextDisabled("No node selected.");
    else if (static_cast<size_t>(m_selectedNode) < app.scene().nodes.size())
        ImGui::TextDisabled("Editing: %s",
                            app.scene().nodes[static_cast<size_t>(m_selectedNode)].name.c_str());

    if (ImGui::Button("Reset transform") && m_selectedNode >= 0 &&
        static_cast<size_t>(m_selectedNode) < app.scene().nodes.size())
    {
        const NodeTransform before = captureTransform(app.scene(), m_selectedNode);

        Node& node = app.scene().nodes[static_cast<size_t>(m_selectedNode)];
        node.translation = glm::vec3(0.0f);
        node.rotation    = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        node.scale       = glm::vec3(1.0f);

        // Same trap as the manipulator: the animator would restore the old
        // rest pose on the next update without this.
        app.animator().syncRestPose(m_selectedNode);

        TransformEdit edit;
        edit.node   = m_selectedNode;
        edit.before = before;
        edit.after  = captureTransform(app.scene(), m_selectedNode);
        edit.label  = "Reset transform";
        app.history().push(std::move(edit));
    }

    ImGui::EndDisabled();
}

bool UiLayer::wantsMouse() const
{
    if (!m_contextCreated) return false;

    // Claim the mouse while the manipulator is hovered or being dragged,
    // otherwise the camera would orbit underneath it.
    return ImGui::GetIO().WantCaptureMouse ||
           ImGuizmo::IsOver() || ImGuizmo::IsUsing();
}

bool UiLayer::wantsKeyboard() const
{
    return m_contextCreated && ImGui::GetIO().WantCaptureKeyboard;
}

// ---------------------------------------------------------------------------

void UiLayer::sampleTiming(App& app)
{
    const float ms = app.deltaTime() * 1000.0f;
    m_frameTimes[m_frameCursor] = ms;
    m_frameCursor = (m_frameCursor + 1) % kFrameHistory;

    m_statsTimer += app.deltaTime();
    if (m_statsTimer < 0.25f) return;
    m_statsTimer = 0.0f;

    // Recompute the headline figures over the whole window, so a single hitch
    // shows up in max and p99 rather than vanishing into the average.
    std::vector<float> sorted;
    sorted.reserve(kFrameHistory);
    for (float v : m_frameTimes)
        if (v > 0.0f) sorted.push_back(v);
    if (sorted.empty()) return;

    std::sort(sorted.begin(), sorted.end());

    float sum = 0.0f;
    for (float v : sorted) sum += v;

    m_shownMs  = sum / static_cast<float>(sorted.size());
    m_shownFps = (m_shownMs > 0.0f) ? 1000.0f / m_shownMs : 0.0f;
    m_shownMin = sorted.front();
    m_shownMax = sorted.back();
    m_shownP99 = sorted[static_cast<size_t>(sorted.size() * 0.99f)];
}

void UiLayer::drawDockspace()
{
    // PassthruCentralNode leaves the middle of the dockspace empty and
    // transparent: the 3D view shows through it and, crucially, ImGui does not
    // claim the mouse there, so orbiting and picking still work.
    const ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode;

    m_dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), flags);

    if (m_resetLayout)
    {
        m_resetLayout = false;
        buildDefaultLayout();
    }
}

void UiLayer::buildDefaultLayout()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(m_dockspaceId);
    // DockSpace comes from imgui_internal.h's private enum and
    // PassthruCentralNode from the public one. Combining them directly is a
    // deprecated cross-enum operation in C++20, so widen both to the flags
    // type first -- which is what the parameter takes anyway.
    const ImGuiDockNodeFlags nodeFlags =
        static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) |
        static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::DockBuilderAddNode(m_dockspaceId, nodeFlags);
    ImGui::DockBuilderSetNodeSize(m_dockspaceId, viewport->WorkSize);

    ImGuiID side   = 0;
    ImGuiID centre = 0;
    ImGui::DockBuilderSplitNode(m_dockspaceId,
                                m_panelOnLeft ? ImGuiDir_Left : ImGuiDir_Right,
                                m_panelWidth, &side, &centre);

    // Everything goes into the one side node as tabs. The central node is left
    // deliberately empty -- docking anything there would cover the model.
    ImGui::DockBuilderDockWindow("Viewer", side);
    ImGui::DockBuilderDockWindow("Model", side);
    ImGui::DockBuilderDockWindow("Animation", side);
    ImGui::DockBuilderDockWindow("Log", side);

    ImGui::DockBuilderFinish(m_dockspaceId);
}

void UiLayer::build(App& app)
{
    sampleTiming(app);

    // Must follow ImGui::NewFrame(), which the renderer has already called.
    ImGuizmo::BeginFrame();

    drawMenuBar(app);

    // After the menu bar so the viewport work area already excludes it.
    drawDockspace();

    // T hides the whole side panel. Skipping submission is what collapses it:
    // the dock node empties and the central area expands to fill the window.
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_T, false))
        m_showPanel = !m_showPanel;

    if (m_showPanel)
    {
        if (m_showViewer)                       drawViewerPanel(app);
        if (m_showModel)                        drawModelPanel(app);
        if (m_showAnimation && app.animator().hasAnimations()) drawAnimationPanel(app);
        if (m_showLog)                          drawLogPanel();
    }
    if (m_showStats)                            drawStatusOverlay(app);

    handleUndoShortcuts(app);
    handleVisibilityShortcuts(app);

    // Picking first: a click that lands on the manipulator must not also
    // change the selection out from under it.
    handleViewportPicking(app);

    // After the panels so the manipulator sits on top of the 3D view but
    // below any window the user might drag over it.
    drawGizmo(app);

    drawDialogs(app);
}

void UiLayer::drawMenuBar(App& app)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Open...", "Ctrl+O"))
        {
            m_pending = PendingAction::Open;

            std::vector<std::string> extensions = ModelLoader::importExtensions();
            fs::path start;
            if (!app.modelPath().empty()) start = pathFromUtf8(app.modelPath()).parent_path();

            m_browser.open(FileBrowser::Mode::Open, "Open a 3D model",
                           std::move(extensions), start);
        }

        if (ImGui::MenuItem("Export as...", "Ctrl+E", false, app.hasModel()))
            m_showExport = true;

        ImGui::Separator();

        if (ImGui::MenuItem("Close model", nullptr, false, app.hasModel()))
            app.closeModel();

        if (ImGui::MenuItem("Quit", "Alt+F4"))
            app.quit();

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        History& history = app.history();

        const char* undoName = history.undoLabel();
        const char* redoName = history.redoLabel();

        char undoText[96];
        char redoText[96];
        std::snprintf(undoText, sizeof(undoText), "Undo %s", undoName ? undoName : "");
        std::snprintf(redoText, sizeof(redoText), "Redo %s", redoName ? redoName : "");

        if (ImGui::MenuItem(undoText, "Ctrl+Z", false, history.canUndo()))
            applyUndoRedo(app, false);
        if (ImGui::MenuItem(redoText, "Ctrl+Shift+Z", false, history.canRedo()))
            applyUndoRedo(app, true);

        ImGui::Separator();
        ImGui::BeginDisabled(history.depth() == 0);
        if (ImGui::MenuItem("Clear history")) history.clear();
        ImGui::EndDisabled();

        ImGui::TextDisabled("%zu / %zu steps", history.cursor(), history.depth());
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        gfx::RenderSettings& settings = app.settings();

        ImGui::MenuItem("Side panel", "T", &m_showPanel);

        ImGui::BeginDisabled(!m_showPanel);
        ImGui::Indent();
        ImGui::MenuItem("Viewer",    nullptr, &m_showViewer);
        ImGui::MenuItem("Model",     nullptr, &m_showModel);
        ImGui::MenuItem("Animation", nullptr, &m_showAnimation);
        ImGui::MenuItem("Log",       nullptr, &m_showLog);
        ImGui::Unindent();
        ImGui::EndDisabled();

        ImGui::MenuItem("Statistics", nullptr, &m_showStats);

        ImGui::Separator();

        if (ImGui::MenuItem("Dock panel left", nullptr, m_panelOnLeft))
        {
            m_panelOnLeft = true;
            m_resetLayout = true;
        }
        if (ImGui::MenuItem("Dock panel right", nullptr, !m_panelOnLeft))
        {
            m_panelOnLeft = false;
            m_resetLayout = true;
        }
        if (ImGui::MenuItem("Reset layout"))
            m_resetLayout = true;

        ImGui::Separator();
        ImGui::MenuItem("Grid",      "G", &settings.showGrid);
        ImGui::MenuItem("Wireframe", "W", &settings.wireframe);

        if (ImGui::MenuItem("Frame model", "F", false, app.hasModel()))
            app.focusCamera();

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About")) m_showAbout = true;
        ImGui::EndMenu();
    }

    // Right-aligned frame timing.
    char timing[64];
    std::snprintf(timing, sizeof(timing), "%.0f FPS  |  %.2f ms",
                  static_cast<double>(m_shownFps),
                  static_cast<double>(m_shownMs));

    const float width = ImGui::CalcTextSize(timing).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - width - 16.0f);
    ImGui::TextDisabled("%s", timing);

    ImGui::EndMainMenuBar();
}

void UiLayer::drawViewerPanel(App& app)
{
    ImGui::SetNextWindowPos(ImVec2(12, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 640), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Viewer", &m_showViewer))
    {
        ImGui::End();
        return;
    }

    gfx::RenderSettings& settings = app.settings();

    if (ImGui::CollapsingHeader("Material colour", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Override untextured materials", &settings.overrideUntextured);
        helpMarker("When a material has no base colour texture, use the colour below instead. "
                   "Models that ship with textures are left alone unless you tick this.");

        ImGui::ColorPicker3("##defaultColor", &settings.defaultColor.x,
                            ImGuiColorEditFlags_PickerHueWheel |
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoSmallPreview);

        ImGui::ColorEdit3("Colour", &settings.defaultColor.x,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoInputs);

        ImGui::SliderFloat("Metallic",  &settings.defaultMetallic, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &settings.defaultRoughness, 0.02f, 1.0f);

        if (ImGui::SmallButton("Neutral grey")) settings.defaultColor = {0.78f, 0.78f, 0.80f};
        ImGui::SameLine();
        if (ImGui::SmallButton("Clay"))         settings.defaultColor = {0.82f, 0.52f, 0.40f};
        ImGui::SameLine();
        if (ImGui::SmallButton("White"))        settings.defaultColor = {0.95f, 0.95f, 0.95f};
    }

    if (ImGui::CollapsingHeader("Appearance"))
    {
        int theme = static_cast<int>(m_theme);
        const char* names[static_cast<int>(ui::ThemeStyle::Count)];
        for (int i = 0; i < static_cast<int>(ui::ThemeStyle::Count); ++i)
            names[i] = ui::themeStyleName(static_cast<ui::ThemeStyle>(i));

        if (ImGui::Combo("Theme", &theme, names, static_cast<int>(ui::ThemeStyle::Count)))
        {
            m_theme = static_cast<ui::ThemeStyle>(theme);
            applyTheme();
        }

        if (ImGui::ColorEdit3("Accent", &m_accent.x, ImGuiColorEditFlags_NoInputs))
            applyTheme();
        helpMarker("Drives every interactive colour at once -- checkmarks, "
                   "sliders, selected tabs, resize grips.");

        // Applied on release: rebuilding the style every frame mid-drag makes
        // the control jump around under the cursor as the metrics change.
        ImGui::SliderFloat("UI scale", &m_uiScale, 0.75f, 2.0f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) applyTheme();

        static const struct { const char* name; ImVec4 colour; } kAccents[] = {
            {"Blue",   ImVec4(0.29f, 0.56f, 0.95f, 1.0f)},
            {"Teal",   ImVec4(0.16f, 0.71f, 0.68f, 1.0f)},
            {"Violet", ImVec4(0.58f, 0.45f, 0.93f, 1.0f)},
            {"Amber",  ImVec4(0.95f, 0.66f, 0.20f, 1.0f)},
            {"Rose",   ImVec4(0.93f, 0.40f, 0.52f, 1.0f)},
        };
        for (int i = 0; i < IM_ARRAYSIZE(kAccents); ++i)
        {
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(kAccents[i].name))
            {
                m_accent = kAccents[i].colour;
                applyTheme();
            }
        }

        if (!m_fontPath.empty())
            ImGui::TextDisabled("Font: %s",
                                m_fontPath.substr(m_fontPath.find_last_of("/\\") + 1).c_str());
    }

    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
    {
        struct Binding { const char* input; const char* action; };
        static constexpr Binding kBindings[] = {
            {"Left drag",          "Orbit"},
            {"Middle drag",        "Pan"},
            {"Shift + left drag",  "Pan"},
            {"Right drag",         "Orbit"},
            {"Mouse wheel",        "Zoom"},
            {"= / -",              "Zoom"},
            {"F",                  "Frame model"},
            {"Space",              "Play / pause animation"},
            {"W",                  "Wireframe"},
            {"G",                  "Toggle grid"},
            {"T",                  "Show / hide side panel"},
            {"H",                  "Hide selected mesh"},
            {"Shift + H",          "Show all meshes"},
            {"I",                  "Isolate selection / leave isolate"},
            {"Left click model",   "Select / show gumball"},
            {"Drag arrow / arc",   "Move / rotate / scale"},
            {"Ctrl+Z",             "Undo transform"},
            {"Ctrl+Shift+Z",       "Redo transform"},
            {"Left click empty",   "Clear selection"},
        };

        if (ImGui::BeginTable("##controls", 2, ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("##input",  ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch, 0.5f);

            for (const Binding& binding : kBindings)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(binding.input);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", binding.action);
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();

        float smoothing = app.camera().smoothing();
        if (ImGui::SliderFloat("Camera smoothing", &smoothing, 0.0f, 0.15f, "%.3f s"))
            app.camera().setSmoothing(smoothing);
        helpMarker("Eases the camera towards where the mouse asks it to be. "
                   "Mouse deltas arrive at the mouse's polling rate rather than "
                   "the render rate, so without this the camera lurches at high "
                   "framerates. Set to 0 to disable.");

        ImGui::Spacing();
        ImGui::TextDisabled("Drag a file onto the window to load it.");
    }

    if (ImGui::CollapsingHeader("Manipulator", ImGuiTreeNodeFlags_DefaultOpen))
    {
        drawGizmoSettings(app);
    }

    if (ImGui::CollapsingHeader("Environment"))
    {
        ImGui::ColorEdit3("Background", &settings.clearColor.x);
        ImGui::ColorEdit3("Ambient",    &settings.ambientColor.x);
        ImGui::SliderFloat("Ambient intensity", &settings.ambientIntensity, 0.0f, 2.0f);
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 4.0f);

        ImGui::Checkbox("Sky", &settings.showSky);
        ImGui::BeginDisabled(!settings.showSky);

        ImGui::ColorEdit3("Zenith",  &settings.skyZenith.x);
        ImGui::ColorEdit3("Horizon", &settings.skyHorizon.x);
        ImGui::ColorEdit3("Ground",  &settings.skyGround.x);
        ImGui::SliderFloat("Sky intensity", &settings.skyIntensity, 0.0f, 3.0f, "%.2f");
        ImGui::SliderFloat("Horizon width", &settings.skyTightness, 0.05f, 3.0f, "%.2f");
        ImGui::Checkbox("Sun disc", &settings.skySun);
        helpMarker("Drawn along the first directional light, so the sky and the "
                   "lighting agree about where the sun is.");
        ImGui::Checkbox("Sky drives ambient", &settings.skyDrivesAmbient);
        helpMarker("Lights the model from the sky gradient itself, sampled along "
                   "the surface normal and the reflection vector. Turn it off to "
                   "go back to a single flat ambient colour.");
        ImGui::SliderFloat("Ambient strength", &settings.ambientIntensity, 0.0f, 2.0f, "%.2f");

        struct SkyPreset
        {
            const char* name;
            glm::vec3   zenith, horizon, ground;
            float       intensity;
        };
        static constexpr SkyPreset kPresets[] = {
            {"Day",      {0.20f, 0.36f, 0.62f}, {0.72f, 0.80f, 0.90f}, {0.26f, 0.24f, 0.22f}, 1.00f},
            {"Overcast", {0.55f, 0.57f, 0.60f}, {0.78f, 0.79f, 0.80f}, {0.30f, 0.30f, 0.30f}, 0.90f},
            {"Dusk",     {0.12f, 0.14f, 0.30f}, {0.90f, 0.48f, 0.28f}, {0.14f, 0.12f, 0.14f}, 0.85f},
            {"Studio",   {0.30f, 0.30f, 0.32f}, {0.62f, 0.62f, 0.64f}, {0.18f, 0.18f, 0.19f}, 1.00f},
            {"Night",    {0.02f, 0.03f, 0.07f}, {0.08f, 0.10f, 0.18f}, {0.02f, 0.02f, 0.03f}, 0.70f},
        };

        ImGui::TextDisabled("Presets");
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i)
        {
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(kPresets[i].name))
            {
                settings.skyZenith    = kPresets[i].zenith;
                settings.skyHorizon   = kPresets[i].horizon;
                settings.skyGround    = kPresets[i].ground;
                settings.skyIntensity = kPresets[i].intensity;
            }
        }

        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Checkbox("Shadows", &settings.shadows);
        helpMarker("Cast by the first directional light. If nothing casts, check "
                   "that a directional light exists in Lighting.");
        ImGui::BeginDisabled(!settings.shadows);
        ImGui::SliderFloat("Depth bias",  &settings.shadowDepthBias, 0.0f, 0.02f, "%.4f");
        helpMarker("Too low gives acne (dark speckling on lit surfaces); too high "
                   "detaches shadows from whatever casts them.");
        ImGui::SliderFloat("Normal bias", &settings.shadowNormalBias, 0.0f, 8.0f, "%.2f");

        const int caster = app.renderer().shadowCaster();
        if (caster < 0)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "No light is casting");
        else
            ImGui::TextDisabled("Casting: light %d  |  %ux%u map", caster,
                                app.renderer().shadowResolution(),
                                app.renderer().shadowResolution());
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Checkbox("Show origin axes", &settings.showAxes);
        helpMarker("Red = +X, green = +Y, blue = +Z.");
        ImGui::BeginDisabled(!settings.showAxes);
        ImGui::SliderFloat("Axis length", &settings.axesScale, 0.05f, 2.0f, "%.2f");
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Checkbox("Show grid", &settings.showGrid);
        ImGui::BeginDisabled(!settings.showGrid);
        ImGui::DragFloat("Cell size", &settings.gridCell, 0.01f, 0.01f, 100.0f, "%.2f");
        ImGui::ColorEdit3("Grid colour", &settings.gridColor.x);
        ImGui::SliderInt("Grid extent (cells)", &settings.gridHalfExtent, 8, 400);
        helpMarker("Half-width of the grid mesh. It is finite on purpose: an "
                   "infinite plane always has a band near the horizon where "
                   "cells fall below pixel size and alias.");
        ImGui::Checkbox("Coloured axis lines", &settings.gridAxisLines);
        helpMarker("Draws red/blue X and Z lines across the whole ground plane. "
                   "Redundant with the origin arrows, which is why it is off by "
                   "default.");
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Lighting"))
    {
        const bool hasLights = !app.scene().lights.empty();

        // The mode selector is always available: unlit is most useful precisely
        // on files that carry no lights of their own.
        int mode = static_cast<int>(settings.lightingMode);
        const char* modeNames[static_cast<int>(gfx::LightingMode::Count)];
        for (int i = 0; i < static_cast<int>(gfx::LightingMode::Count); ++i)
            modeNames[i] = gfx::lightingModeName(static_cast<gfx::LightingMode>(i));

        if (ImGui::Combo("Mode", &mode, modeNames,
                         static_cast<int>(gfx::LightingMode::Count)))
            settings.lightingMode = static_cast<gfx::LightingMode>(mode);

        helpMarker("Unlit turns off every light -- the built-in rig, any lights "
                   "imported from the file, ambient and shadows -- and draws the "
                   "base colour and emissive exactly as authored. Use it for "
                   "models whose lighting is already baked into their textures.");

        const bool unlit = settings.lightingMode == gfx::LightingMode::Unlit;

        ImGui::BeginDisabled(!hasLights || unlit);
        ImGui::Checkbox("Use lights from file", &settings.useSceneLights);
        ImGui::EndDisabled();

        if (!hasLights)
            ImGui::TextDisabled("No lights in this file; using a default three-point rig.");
        else
            ImGui::TextDisabled("%zu light(s) imported.", app.scene().lights.size());

        ImGui::BeginDisabled(unlit);
        ImGui::SliderFloat("Intensity scale", &settings.lightIntensityScale, 0.0f, 5.0f);
        ImGui::EndDisabled();

        if (unlit)
            ImGui::TextDisabled("Unlit: lights, ambient and shadows are all bypassed.");

        for (const Light& light : app.scene().lights)
        {
            const char* type = light.type == LightType::Directional ? "directional"
                             : light.type == LightType::Spot        ? "spot"
                                                                    : "point";
            ImGui::BulletText("%s (%s)", light.name.empty() ? "<unnamed>" : light.name.c_str(), type);
        }
    }

    if (ImGui::CollapsingHeader("Camera"))
    {
        Camera& camera = app.camera();

        float fov = glm::degrees(camera.yfov());
        if (ImGui::SliderFloat("Field of view", &fov, 10.0f, 110.0f, "%.0f deg"))
            camera.setYfov(glm::radians(fov));

        float clip[2]{camera.znear(), camera.zfar()};
        if (ImGui::DragFloat2("Clip planes", clip, 0.01f, 0.0001f, 100000.0f, "%.4f"))
            camera.setClipPlanes(std::max(clip[0], 1e-4f), std::max(clip[1], clip[0] * 1.01f));

        if (ImGui::Button("Frame model", ImVec2(-1, 0))) app.focusCamera();

        const std::vector<CameraDef>& cameras = app.scene().cameras;
        if (!cameras.empty())
        {
            ImGui::SeparatorText("Cameras in file");
            for (int i = 0; i < static_cast<int>(cameras.size()); ++i)
            {
                const std::string label =
                    (cameras[static_cast<size_t>(i)].name.empty()
                         ? "camera " + std::to_string(i)
                         : cameras[static_cast<size_t>(i)].name);

                if (ImGui::Button(label.c_str(), ImVec2(-1, 0)))
                    app.adoptSceneCamera(i);
            }
        }

        ImGui::TextDisabled("LMB orbit - MMB/Shift+LMB pan - wheel zoom");
    }

    if (ImGui::CollapsingHeader("Render"))
    {
        ImGui::Checkbox("Wireframe", &settings.wireframe);
        ImGui::Checkbox("Backface culling", &settings.backfaceCulling);
        helpMarker("Off by default, so everything renders double sided. Hair "
                   "cards, foliage and clothing are frequently single-sided "
                   "geometry that disappears from one side when culled.");

        int debugView = static_cast<int>(settings.debugView);
        const char* names[static_cast<int>(gfx::DebugView::Count)];
        for (int i = 0; i < static_cast<int>(gfx::DebugView::Count); ++i)
            names[i] = gfx::debugViewName(static_cast<gfx::DebugView>(i));

        if (ImGui::Combo("Debug view", &debugView, names,
                         static_cast<int>(gfx::DebugView::Count)))
            settings.debugView = static_cast<gfx::DebugView>(debugView);

        if (ImGui::Checkbox("V-sync", &settings.vsync))
            log::info("V-sync ", settings.vsync ? "enabled" : "disabled");

        static const int  kCaps[]  = {0, 30, 60, 72, 120, 144, 165};
        static const char* kNames[] = {"Uncapped", "30", "60", "72", "120", "144", "165"};
        int capIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(kCaps); ++i)
            if (kCaps[i] == settings.frameRateCap) capIndex = i;

        if (ImGui::Combo("Frame rate cap", &capIndex, kNames, IM_ARRAYSIZE(kNames)))
            settings.frameRateCap = kCaps[capIndex];
        helpMarker("Holds the loop to a fixed rate. Useful as a test: if flicker "
                   "changes or stops when the rate is pinned, it is coming from "
                   "display timing rather than from the renderer.");
    }

    if (ImGui::CollapsingHeader("Import options"))
    {
        ImportOptions& options = app.importOptions();

        int flip = static_cast<int>(options.flipV);
        if (ImGui::Combo("Flip V", &flip, "Auto\0Always\0Never\0"))
            options.flipV = static_cast<ImportOptions::FlipV>(flip);
        helpMarker("Assimp reports texture coordinates with a bottom-left origin for "
                   "every format, while our textures are uploaded top-down, so Auto "
                   "flips V in all cases. Override and reload only if a particular "
                   "file looks upside down.");

        ImGui::Checkbox("Generate missing normals", &options.generateNormals);
        ImGui::Checkbox("Optimise meshes", &options.optimizeMeshes);
        ImGui::Checkbox("Repair invalid data", &options.repairInvalidData);
        helpMarker("Fixes zeroed normals and degenerate UVs, but deletes UV "
                   "channels it judges invalid and shifts the rest down, which "
                   "can misalign textures. Try it only if a model looks wrong "
                   "without it. Reload after changing.");
        ImGui::DragFloat("Import scale", &options.importScale, 0.001f, 0.0001f, 1000.0f, "%.4f");

        if (ImGui::Button("Reload with these options", ImVec2(-1, 0)) && app.hasModel())
            app.loadModel(app.modelPath());
    }

    ImGui::End();
}

void UiLayer::drawNodeTree(App& app, int nodeIndex)
{
    Scene& scene = app.scene();
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(scene.nodes.size())) return;

    Node& node = scene.nodes[static_cast<size_t>(nodeIndex)];

    // Per-row visibility toggle, so a hidden node can be brought back without
    // resorting to Shift+H.
    ImGui::PushID(nodeIndex);
    if (ImGui::SmallButton(node.visible ? "o" : "-"))
    {
        const std::vector<uint8_t> before = visibilitySnapshot(scene);
        node.visible = !node.visible;
        recordVisibilityEdit(app, before, node.visible ? "Show mesh" : "Hide mesh");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(node.visible ? "Visible - click to hide"
                                       : "Hidden - click to show");
    ImGui::PopID();
    ImGui::SameLine();

    // Captured once: the toggle above can flip node.visible mid-row, and the
    // push and pop must agree or the style stack unbalances.
    const bool dimmed = !node.visible;
    if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.58f, 1.0f));

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (m_selectedNode == nodeIndex) flags |= ImGuiTreeNodeFlags_Selected;

    const std::string label = node.name.empty() ? ("node " + std::to_string(nodeIndex)) : node.name;
    const bool opened = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(nodeIndex)),
                                          flags, "%s%s%s", label.c_str(),
                                          node.meshes.empty() ? "" : "  [mesh]",
                                          node.visible ? "" : "  (hidden)");

    if (dimmed) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked()) m_selectedNode = nodeIndex;

    if (opened)
    {
        for (int child : node.children)
            drawNodeTree(app, child);
        ImGui::TreePop();
    }
}

void UiLayer::drawModelPanel(App& app)
{
    ImGui::SetNextWindowPos(ImVec2(360, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 460), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Model", &m_showModel))
    {
        ImGui::End();
        return;
    }

    if (!app.hasModel())
    {
        ImGui::TextWrapped("No model loaded.\n\nDrag a file onto the window or use File > Open.");
        ImGui::Spacing();
        ImGui::TextDisabled("Readable formats include glTF/GLB, FBX, OBJ, STL, DAE, PLY, 3DS and more.");
        if (!UsdBackend::compiledIn())
            ImGui::TextDisabled("USD is not compiled in (configure with -DMV_ENABLE_USD=ON).");
        ImGui::End();
        return;
    }

    const Scene& scene = app.scene();

    ImGui::TextWrapped("%s", pathToUtf8(pathFromUtf8(scene.sourcePath).filename()).c_str());
    ImGui::TextDisabled("%s", scene.sourcePath.c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("##counts", 2, ImGuiTableFlags_SizingStretchProp))
    {
        auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", label);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(value.c_str());
        };

        row("Meshes",     std::to_string(scene.meshes.size()));
        row("Triangles",  std::to_string(scene.indices.size() / 3));
        row("Vertices",   std::to_string(scene.vertices.size()));
        row("Materials",  std::to_string(scene.materials.size()));
        row("Textures",   std::to_string(scene.textures.size()));
        row("Nodes",      std::to_string(scene.nodes.size()));
        row("Skins",      std::to_string(scene.skins.size()));
        row("Animations", std::to_string(scene.animations.size()));
        row("Lights",     std::to_string(scene.lights.size()));
        row("Cameras",    std::to_string(scene.cameras.size()));

        ImGui::EndTable();
    }

    if (scene.bounds.valid())
    {
        const glm::vec3 extent = scene.bounds.extent();
        ImGui::Text("Size: %.3f x %.3f x %.3f", static_cast<double>(extent.x),
                    static_cast<double>(extent.y), static_cast<double>(extent.z));
    }

    if (ImGui::CollapsingHeader("Hierarchy"))
    {
        ImGui::BeginDisabled(m_selectedNode < 0 && !m_isolated);
        if (ImGui::Button(m_isolated ? "Leave isolate" : "Isolate selection"))
            toggleIsolate(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Show all"))
        {
            // Through app rather than the local: this panel's reference is
            // const because everything else here only reads, and that is worth
            // keeping.
            const std::vector<uint8_t> before = visibilitySnapshot(app.scene());
            app.scene().showAll();
            clearVisibilityState();
            recordVisibilityEdit(app, before, "Show all");
        }

        // Isolate hides most of the tree, which looks a lot like a broken
        // model unless the state is stated plainly.
        if (m_isolated)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                               "Isolated - press I or the button to restore");

        if (ImGui::BeginChild("##tree", ImVec2(0, 200), ImGuiChildFlags_Borders))
            for (int root : scene.roots)
                drawNodeTree(app, root);
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Materials"))
    {
        for (size_t i = 0; i < scene.materials.size(); ++i)
        {
            const Material& material = scene.materials[i];

            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(material.name.empty() ? "<unnamed>" : material.name.c_str()))
            {
                ImVec4 color(material.baseColorFactor.r, material.baseColorFactor.g,
                             material.baseColorFactor.b, material.baseColorFactor.a);
                ImGui::ColorButton("##base", color, ImGuiColorEditFlags_NoTooltip, ImVec2(24, 24));
                ImGui::SameLine();
                ImGui::Text("metallic %.2f  roughness %.2f",
                            static_cast<double>(material.metallicFactor),
                            static_cast<double>(material.roughnessFactor));

                auto textureRow = [&](const char* label, int index) {
                    if (index == kInvalidIndex) return;
                    const TextureData& tex = scene.textures[static_cast<size_t>(index)];
                    ImGui::BulletText("%s: %s", label, tex.name.c_str());
                };

                textureRow("base colour",        material.baseColorTexture);
                textureRow("normal",             material.normalTexture);
                textureRow("metallic/roughness", material.metalRoughTexture);
                textureRow("emissive",           material.emissiveTexture);

                // Editable rather than reported: alpha mode is the single most
                // commonly wrong thing in exported assets, and a viewer that
                // cannot correct it is stuck showing hair as opaque cards.
                Material& editable = app.scene().materials[i];

                // Alpha mode can only do something if there is alpha to read.
                // Saying so here saves working out why Mask changes nothing.
                const bool hasAlpha =
                    editable.baseColorTexture != kInvalidIndex &&
                    scene.textures[static_cast<size_t>(editable.baseColorTexture)].hasAlpha;

                if (!hasAlpha)
                    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                                       "Base colour texture has no alpha channel");

                int alpha = static_cast<int>(editable.alphaMode);
                if (ImGui::Combo("Alpha", &alpha, "Opaque\0Mask\0Blend\0"))
                    editable.alphaMode = static_cast<AlphaMode>(alpha);

                ImGui::BeginDisabled(editable.alphaMode != AlphaMode::Mask);
                ImGui::SliderFloat("Cutoff", &editable.alphaCutoff, 0.0f, 1.0f, "%.2f");
                ImGui::EndDisabled();

                ImGui::Checkbox("Double sided", &editable.doubleSided);
                helpMarker("Hair cards and foliage are usually double sided; with "
                           "backface culling on, single-sided cards vanish from "
                           "one direction.");

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    ImGui::End();
}

void UiLayer::drawAnimationPanel(App& app)
{
    Animator& animator = app.animator();

    ImGui::SetNextWindowPos(ImVec2(12, 700), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(690, 150), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Animation", &m_showAnimation))
    {
        ImGui::End();
        return;
    }

    const Scene& scene = app.scene();

    int current = animator.activeAnimation();
    if (ImGui::BeginCombo("Clip",
                          (current >= 0 && current < static_cast<int>(scene.animations.size()))
                              ? scene.animations[static_cast<size_t>(current)].name.c_str()
                              : "<none>"))
    {
        for (int i = 0; i < static_cast<int>(scene.animations.size()); ++i)
        {
            const bool selected = (i == current);
            if (ImGui::Selectable(scene.animations[static_cast<size_t>(i)].name.c_str(), selected))
                animator.setActiveAnimation(i);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button(animator.isPlaying() ? "Pause" : "Play", ImVec2(80, 0)))
        animator.togglePlay();

    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(80, 0))) animator.stop();

    ImGui::SameLine();
    if (ImGui::Button("Restart", ImVec2(80, 0))) { animator.restart(); animator.setPlaying(true); }

    ImGui::SameLine();
    bool loop = animator.isLooping();
    if (ImGui::Checkbox("Loop", &loop)) animator.setLooping(loop);

    ImGui::SameLine();
    float speed = animator.speed();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Speed", &speed, -3.0f, 3.0f, "%.2fx")) animator.setSpeed(speed);

    const float duration = animator.duration();
    float time = animator.time();

    ImGui::SetNextItemWidth(-120.0f);
    if (ImGui::SliderFloat("##timeline", &time, 0.0f, std::max(duration, 0.001f), ""))
    {
        animator.setPlaying(false);
        animator.setTime(time);
    }

    ImGui::SameLine();
    ImGui::Text("%s / %s", formatDuration(animator.time()).c_str(),
                formatDuration(duration).c_str());

    ImGui::End();
}

void UiLayer::drawLogPanel()
{
    ImGui::SetNextWindowSize(ImVec2(720, 260), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Log", &m_showLog))
    {
        ImGui::End();
        return;
    }

    if (ImGui::SmallButton("Clear")) log::Sink::get().clear();

    ImGui::Separator();

    if (ImGui::BeginChild("##entries"))
    {
        for (const log::Entry& entry : log::Sink::get().snapshot())
        {
            ImVec4 color(0.85f, 0.86f, 0.90f, 1.0f);
            if (entry.level == log::Level::Warn)  color = ImVec4(1.00f, 0.80f, 0.35f, 1.0f);
            if (entry.level == log::Level::Error) color = ImVec4(1.00f, 0.45f, 0.40f, 1.0f);
            if (entry.level == log::Level::Trace) color = ImVec4(0.60f, 0.62f, 0.68f, 1.0f);

            ImGui::TextColored(color, "%s", entry.message.c_str());
        }

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}

void UiLayer::drawStatusOverlay(App& app)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##stats", &m_showStats, flags))
    {
        const gfx::FrameStats stats = app.renderer().stats();
        ImGui::Text("%s", app.context().deviceName().c_str());
        ImGui::Text("%u draw calls, %u triangles", stats.drawCalls, stats.triangles);

        ImGui::Separator();

        // A flat line here means presentation is steady. A regular sawtooth or
        // a bimodal split means frames are being paced against something they
        // do not divide into evenly, which reads as judder however clean the
        // rendering itself is.
        float plotMax = m_shownMax > 0.0f ? m_shownMax * 1.25f : 20.0f;
        ImGui::PlotLines("##frametimes", m_frameTimes, kFrameHistory, m_frameCursor,
                         "frame time (ms)", 0.0f, plotMax, ImVec2(240.0f, 48.0f));

        ImGui::Text("avg %.2f  min %.2f  p99 %.2f  max %.2f ms",
                    static_cast<double>(m_shownMs), static_cast<double>(m_shownMin),
                    static_cast<double>(m_shownP99), static_cast<double>(m_shownMax));

        const char* mode = "unknown";
        switch (app.context().swapchain().presentMode)
        {
            case VK_PRESENT_MODE_FIFO_KHR:         mode = "FIFO (v-synced)";   break;
            case VK_PRESENT_MODE_FIFO_RELAXED_KHR: mode = "FIFO relaxed";      break;
            case VK_PRESENT_MODE_MAILBOX_KHR:      mode = "Mailbox";           break;
            case VK_PRESENT_MODE_IMMEDIATE_KHR:    mode = "Immediate (tears)"; break;
            default: break;
        }
        ImGui::Text("glTF: Draco %s  |  meshopt %s",
#if MV_WITH_DRACO
                    "yes",
#else
                    "no",
#endif
                    "no");

        const int hz = app.window().refreshRate();
        ImGui::Text("Present: %s  |  MSAA %ux", mode,
                    static_cast<unsigned>(app.context().sampleCount()));

        if (hz > 0)
        {
            const float ratio = (m_shownFps > 1.0f) ? m_shownFps / static_cast<float>(hz) : 0.0f;
            ImGui::Text("Display: %d Hz  |  frames per refresh %.2f", hz,
                        static_cast<double>(ratio));

            // A non-integer ratio means frames are not landing one per refresh,
            // which is what judder is.
            const float nearest = std::round(ratio);
            if (ratio > 0.05f && std::abs(ratio - nearest) > 0.06f)
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                                   "Frame rate does not divide into the refresh rate");
        }

        ImGui::Separator();
        ImGui::TextWrapped("%s", app.status().c_str());
    }
    ImGui::End();
}

void UiLayer::drawDialogs(App& app)
{
    // -- export ------------------------------------------------------------
    if (m_showExport)
    {
        ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Export model", &m_showExport, ImGuiWindowFlags_NoCollapse))
        {
            const std::vector<ExportFormat>& formats = ModelLoader::exportFormats();

            if (formats.empty())
            {
                ImGui::TextWrapped("This build of Assimp has no exporters enabled.");
            }
            else
            {
                m_exportFormat = std::clamp(m_exportFormat, 0, static_cast<int>(formats.size()) - 1);

                const ExportFormat& selected = formats[static_cast<size_t>(m_exportFormat)];

                if (ImGui::BeginCombo("Format", (selected.description + "  (." +
                                                 selected.extension + ")").c_str()))
                {
                    for (int i = 0; i < static_cast<int>(formats.size()); ++i)
                    {
                        const ExportFormat& format = formats[static_cast<size_t>(i)];
                        const std::string label = format.description + "  (." + format.extension + ")";

                        if (ImGui::Selectable(label.c_str(), i == m_exportFormat))
                            m_exportFormat = i;
                    }
                    ImGui::EndCombo();
                }

                ImGui::TextDisabled("Assimp format id: %s", selected.id.c_str());
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "The originally imported scene is written out, so geometry, materials, "
                    "skeletons and animations survive as far as the target format supports them.");
                ImGui::Spacing();

                ImGui::BeginDisabled(!app.hasModel());
                if (ImGui::Button("Choose destination...", ImVec2(-1, 0)))
                {
                    m_pending = PendingAction::Export;

                    const fs::path source = pathFromUtf8(app.modelPath());
                    const std::string suggested =
                        pathToUtf8(source.stem()) + "." + selected.extension;

                    m_browser.open(FileBrowser::Mode::Save, "Export as " + selected.description,
                                   {selected.extension}, source.parent_path(), suggested);
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    // -- about -------------------------------------------------------------
    if (m_showAbout)
    {
        ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("About", &m_showAbout, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextUnformatted("ModelViewer");
            ImGui::TextDisabled("A cross-platform Vulkan model viewer.");
            ImGui::Separator();
            ImGui::BulletText("Rendering: Vulkan 1.3 (dynamic rendering)");
            ImGui::BulletText("Import/export: Assimp");
            ImGui::BulletText("UI: Dear ImGui + GLFW");
            ImGui::BulletText("USD backend: %s", UsdBackend::compiledIn() ? "enabled" : "not compiled in");
            ImGui::Separator();
            ImGui::TextWrapped("Shortcuts: F frame model, G grid, W wireframe, Space play/pause.");
        }
        ImGui::End();
    }

    // -- file browser ------------------------------------------------------
    if (m_browser.draw())
    {
        const fs::path path = m_browser.result();

        switch (m_pending)
        {
            case PendingAction::Open:
                app.loadModel(path);
                break;

            case PendingAction::Export:
            {
                const std::vector<ExportFormat>& formats = ModelLoader::exportFormats();
                if (!formats.empty())
                {
                    const int index = std::clamp(m_exportFormat, 0,
                                                 static_cast<int>(formats.size()) - 1);
                    if (app.exportModel(path, formats[static_cast<size_t>(index)].id))
                        m_showExport = false;
                }
                break;
            }

            case PendingAction::None:
                break;
        }

        m_pending = PendingAction::None;
    }
}

} // namespace mv
