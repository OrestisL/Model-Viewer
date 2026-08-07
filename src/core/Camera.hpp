#pragma once

#include <glm/glm.hpp>

#include "scene/Scene.hpp"

struct GLFWwindow;

namespace mv {

/// Turntable camera: LMB orbits, MMB (or shift+LMB) pans, wheel dollies.
/// Can also be driven from an imported CameraDef.
class Camera
{
public:
    void update(GLFWwindow* window, float deltaSeconds, bool inputCaptured);

    /// Frame an axis-aligned box with a small margin.
    void focus(const AABB& bounds);

private:
    /// Snap the damped goal onto the current values (used after a jump).
    void syncGoal();

public:

    /// Adopt an imported camera's placement and projection settings.
    void adopt(const CameraDef& definition);

    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;

    glm::vec3 position() const;
    glm::vec3 target() const { return m_target; }
    glm::vec3 forward() const;

    float yfov() const { return m_yfov; }
    void  setYfov(float radians) { m_yfov = glm::clamp(radians, glm::radians(5.0f), glm::radians(120.0f)); }

    float znear() const { return m_znear; }
    float zfar() const { return m_zfar; }
    void  setClipPlanes(float znear, float zfar) { m_znear = znear; m_zfar = zfar; }

    float distance() const { return m_distance; }
    void  setDistance(float d) { m_distance = glm::max(d, 1e-3f); m_goalDistance = m_distance; }

    /// Time constant for camera damping, in seconds. 0 disables it.
    ///
    /// Mouse deltas arrive at the mouse's polling rate, not the render rate.
    /// At 144 fps against a 125 Hz mouse, roughly one frame in six sees no
    /// movement at all and the next sees double, so the camera lurches. The
    /// goal angles are set from input and the live ones ease towards them,
    /// which decouples camera motion from both input timing and framerate.
    float smoothing() const { return m_smoothing; }
    void  setSmoothing(float seconds) { m_smoothing = glm::clamp(seconds, 0.0f, 0.25f); }

    void setOrbitSpeed(float s) { m_orbitSpeed = s; }
    void setPanSpeed(float s) { m_panSpeed = s; }
    void setZoomSpeed(float s) { m_zoomSpeed = s; }

    /// Called from the GLFW scroll callback.
    void applyScroll(float delta);

private:
    // Live values, used for rendering.
    glm::vec3 m_target{0.0f};
    float     m_distance = 5.0f;
    float     m_yaw      = glm::radians(35.0f);
    float     m_pitch    = glm::radians(20.0f);

    // Where input wants them to be; the live values ease towards these.
    glm::vec3 m_goalTarget{0.0f};
    float     m_goalDistance = 5.0f;
    float     m_goalYaw      = glm::radians(35.0f);
    float     m_goalPitch    = glm::radians(20.0f);

    float m_smoothing = 0.035f;

    float m_yfov  = glm::radians(45.0f);
    float m_znear = 0.05f;
    float m_zfar  = 500.0f;

    float m_orbitSpeed = 0.006f;
    float m_panSpeed   = 1.0f;
    float m_zoomSpeed  = 0.12f;

    glm::dvec2 m_lastCursor{0.0};
    bool       m_dragging = false;
    float      m_pendingScroll = 0.0f;
};

} // namespace mv
