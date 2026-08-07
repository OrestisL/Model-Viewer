#include "core/Camera.hpp"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

namespace mv {
namespace {
constexpr float kPitchLimit = 1.55334f;   // ~89 degrees
}

glm::vec3 Camera::position() const
{
    const float cp = std::cos(m_pitch);
    const glm::vec3 offset{
        cp * std::sin(m_yaw),
        std::sin(m_pitch),
        cp * std::cos(m_yaw)};
    return m_target + offset * m_distance;
}

glm::vec3 Camera::forward() const
{
    return glm::normalize(m_target - position());
}

glm::mat4 Camera::view() const
{
    return glm::lookAt(position(), m_target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::projection(float aspect) const
{
    aspect = glm::max(aspect, 1e-3f);
    glm::mat4 proj = glm::perspective(m_yfov, aspect, m_znear, m_zfar);
    proj[1][1] *= -1.0f;   // Vulkan's clip space has +Y pointing down
    return proj;
}

void Camera::applyScroll(float delta)
{
    m_pendingScroll += delta;
}

void Camera::focus(const AABB& bounds)
{
    if (!bounds.valid())
    {
        m_target = glm::vec3(0.0f);
        m_distance = 5.0f;
        syncGoal();
        return;
    }

    m_target = bounds.center();

    const float radius = glm::max(bounds.radius(), 1e-3f);
    m_distance = radius / std::tan(m_yfov * 0.5f) * 1.35f;

    // Keep the clip planes proportional so large and tiny assets both work.
    m_znear = glm::max(radius * 0.001f, 1e-4f);
    m_zfar  = glm::max(radius * 100.0f, 100.0f);

    syncGoal();
}

void Camera::syncGoal()
{
    m_goalTarget   = m_target;
    m_goalDistance = m_distance;
    m_goalYaw      = m_yaw;
    m_goalPitch    = m_pitch;
}

void Camera::adopt(const CameraDef& definition)
{
    m_target   = definition.lookAt;
    m_yfov     = definition.yfov;
    m_znear    = definition.znear;
    m_zfar     = definition.zfar;

    const glm::vec3 offset = definition.position - definition.lookAt;
    m_distance = glm::max(glm::length(offset), 1e-3f);

    const glm::vec3 dir = offset / m_distance;
    m_pitch = glm::clamp(std::asin(glm::clamp(dir.y, -1.0f, 1.0f)), -kPitchLimit, kPitchLimit);
    m_yaw   = std::atan2(dir.x, dir.z);

    syncGoal();
}

void Camera::update(GLFWwindow* window, float deltaSeconds, bool inputCaptured)
{
    // Scrolling is delivered through applyScroll() from the GLFW callback so
    // that ImGui gets first refusal on the event.
    if (std::abs(m_pendingScroll) > 1e-5f)
    {
        m_goalDistance *= std::pow(1.0f - m_zoomSpeed, m_pendingScroll);
        m_goalDistance  = glm::clamp(m_goalDistance, m_znear * 2.0f, m_zfar * 0.5f);
        m_pendingScroll = 0.0f;
    }

    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(window, &cx, &cy);
    const glm::dvec2 cursor{cx, cy};

    if (inputCaptured)
    {
        m_dragging   = false;
        m_lastCursor = cursor;
        return;
    }

    const bool left   = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)   == GLFW_PRESS;
    const bool middle = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    const bool right  = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT)  == GLFW_PRESS;
    const bool shift  = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                        glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    const bool anyButton = left || middle || right;
    if (!m_dragging && anyButton)
        m_lastCursor = cursor;
    m_dragging = anyButton;

    const glm::vec2 delta{cursor - m_lastCursor};
    m_lastCursor = cursor;

    if (m_dragging)
    {
        const bool panning = middle || (left && shift);

        if (panning)
        {
            const glm::vec3 fwd   = forward();
            const glm::vec3 rightV = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
            const glm::vec3 upV   = glm::normalize(glm::cross(rightV, fwd));

            // Scale panning with distance so it feels the same at any zoom.
            const float scale = m_distance * std::tan(m_yfov * 0.5f) * 0.0025f * m_panSpeed;
            m_goalTarget += (-rightV * delta.x + upV * delta.y) * scale;
        }
        else if (left || right)
        {
            m_goalYaw   -= delta.x * m_orbitSpeed;
            m_goalPitch  = glm::clamp(m_goalPitch + delta.y * m_orbitSpeed,
                                      -kPitchLimit, kPitchLimit);
        }
    }

    // Keyboard dolly, useful without a wheel.
    const float keyboardZoom = deltaSeconds * 2.0f;
    if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS) m_goalDistance *= (1.0f - keyboardZoom);
    if (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS) m_goalDistance *= (1.0f + keyboardZoom);
    m_goalDistance = glm::clamp(m_goalDistance, m_znear * 2.0f, m_zfar * 0.5f);

    // Ease the live values towards the goal.
    //
    // The exponential form makes this framerate independent: the fraction
    // covered per second is the same at 30 fps and at 144 fps, so the motion
    // does not get faster or slower with the render rate. A per-frame lerp
    // would.
    if (m_smoothing > 1e-4f && deltaSeconds > 0.0f)
    {
        const float lambda = 1.0f - std::exp(-deltaSeconds / m_smoothing);
        m_yaw      = glm::mix(m_yaw, m_goalYaw, lambda);
        m_pitch    = glm::mix(m_pitch, m_goalPitch, lambda);
        m_distance = glm::mix(m_distance, m_goalDistance, lambda);
        m_target   = glm::mix(m_target, m_goalTarget, lambda);
    }
    else
    {
        m_yaw      = m_goalYaw;
        m_pitch    = m_goalPitch;
        m_distance = m_goalDistance;
        m_target   = m_goalTarget;
    }
}

} // namespace mv
