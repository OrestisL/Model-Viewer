#pragma once

#include <string>

#include "scene/History.hpp"
#include "ui/FileBrowser.hpp"

namespace mv {

class App;
class Window;

/// Owns the Dear ImGui context and draws every panel.
/// The Vulkan/GLFW backends are initialised by the renderer.
class UiLayer
{
public:
    ~UiLayer();

    /// Creates the ImGui context and applies the theme. Must run before the
    /// renderer initialises the ImGui backends.
    void createContext(Window& window);
    void destroyContext();

    /// Builds the frame's UI. Call between Renderer::beginFrame/endFrame.
    void build(App& app);

    bool wantsMouse() const;
    bool wantsKeyboard() const;

private:
    void applyTheme();

    void drawMenuBar(App& app);
    void drawGizmo(App& app);
    void drawDockspace();
    void buildDefaultLayout();
    void sampleTiming(App& app);
    void handleUndoShortcuts(App& app);
    void handleViewportPicking(App& app);
    void drawGizmoSettings(App& app);
    void drawViewerPanel(App& app);
    void drawModelPanel(App& app);
    void drawAnimationPanel(App& app);
    void drawLogPanel();
    void drawStatusOverlay(App& app);
    void drawDialogs(App& app);
    void drawNodeTree(App& app, int nodeIndex);

    enum class PendingAction { None, Open, Export };

    /// Manipulator mode. Mapped onto ImGuizmo's enums in the .cpp so this
    /// header stays free of the ImGuizmo include.
    /// Universal is the Rhino-style gumball: translate arrows, rotation arcs
    /// and scale handles on one widget. The rest restrict it to a single
    /// operation, which is easier to hit precisely on dense geometry.
    enum class GizmoOp   { Universal, Translate, Rotate, Scale };
    enum class GizmoMode { World, Local };

    FileBrowser   m_browser;
    PendingAction m_pending = PendingAction::None;

    bool m_showViewer    = true;
    bool m_showModel     = true;
    bool m_showAnimation = true;
    bool m_showLog       = false;
    bool m_showStats     = true;
    bool m_showAbout     = false;
    bool m_showExport    = false;

    int  m_exportFormat = 0;
    int  m_selectedNode = -1;

    bool      m_gizmoEnabled = true;
    GizmoOp   m_gizmoOp      = GizmoOp::Universal;
    /// Which sub-gizmo the cursor was last over. ImGuizmo takes a single snap
    /// value per call and reports every operation as hovered once a drag is
    /// under way, so the choice has to be latched before the drag starts.
    GizmoOp   m_gizmoHovered = GizmoOp::Translate;

    /// Undo is recorded per gesture, not per frame: ImGuizmo reports a change
    /// on every frame of a drag, so the node's transform is snapshotted while
    /// the widget is idle and committed once the drag ends.
    NodeTransform m_preDrag;
    int           m_preDragNode = -1;
    GizmoMode m_gizmoMode    = GizmoMode::World;
    // Frame timing. Sampled every frame, but only *displayed* four times a
    // second -- a number that updates 144 times a second is unreadable, and a
    // constantly changing readout is itself a source of apparent flicker.
    static constexpr int kFrameHistory = 240;
    float m_frameTimes[kFrameHistory] = {};
    int   m_frameCursor = 0;
    float m_statsTimer  = 0.0f;
    float m_shownFps    = 0.0f;
    float m_shownMs     = 0.0f;
    float m_shownMin    = 0.0f;
    float m_shownMax    = 0.0f;
    float m_shownP99    = 0.0f;

    /// Side panel docking. The panels live in one dock node pinned to an edge;
    /// the central node is left empty and transparent so the 3D view shows
    /// through it and still receives mouse input.
    bool         m_showPanel   = true;
    bool         m_panelOnLeft = true;
    bool         m_resetLayout = false;
    float        m_panelWidth  = 0.26f;
    unsigned int m_dockspaceId = 0;

    /// io.IniFilename holds this pointer, so the string has to outlive it.
    std::string m_iniPath;

    bool      m_gizmoSnap    = false;
    float     m_snapTranslate = 0.1f;
    float     m_snapRotate    = 15.0f;
    float     m_snapScale     = 0.1f;

    bool m_contextCreated = false;
};

} // namespace mv
