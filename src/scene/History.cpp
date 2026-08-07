#include "scene/History.hpp"

#include <cmath>

#include <glm/gtc/epsilon.hpp>

#include "scene/Animator.hpp"
#include "scene/Scene.hpp"

namespace mv {
namespace {

constexpr float kEpsilon = 1e-7f;

bool nearlyEqual(const glm::vec3& a, const glm::vec3& b)
{
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

} // namespace

bool operator==(const NodeTransform& a, const NodeTransform& b)
{
    return nearlyEqual(a.translation, b.translation) &&
           nearlyEqual(a.scale, b.scale) &&
           std::abs(glm::dot(a.rotation, b.rotation)) > 1.0f - kEpsilon;
}

NodeTransform captureTransform(const Scene& scene, int node)
{
    NodeTransform transform;
    if (node < 0 || static_cast<size_t>(node) >= scene.nodes.size()) return transform;

    const Node& n     = scene.nodes[static_cast<size_t>(node)];
    transform.translation = n.translation;
    transform.rotation    = n.rotation;
    transform.scale       = n.scale;
    return transform;
}

// ---------------------------------------------------------------------------

void History::clear()
{
    m_entries.clear();
    m_cursor = 0;
}

void History::push(TransformEdit edit)
{
    if (edit.node < 0) return;
    if (edit.before == edit.after) return;   // nothing actually moved

    // Anything that was undone is now unreachable.
    if (m_cursor < m_entries.size())
        m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(m_cursor), m_entries.end());

    m_entries.push_back(std::move(edit));

    if (m_entries.size() > kCapacity)
        m_entries.erase(m_entries.begin(),
                        m_entries.begin() +
                            static_cast<ptrdiff_t>(m_entries.size() - kCapacity));

    m_cursor = m_entries.size();
}

const char* History::undoLabel() const
{
    if (!canUndo()) return nullptr;
    return m_entries[m_cursor - 1].label.c_str();
}

const char* History::redoLabel() const
{
    if (!canRedo()) return nullptr;
    return m_entries[m_cursor].label.c_str();
}

void History::applyTo(Scene& scene, Animator& animator, int node,
                      const NodeTransform& transform) const
{
    if (node < 0 || static_cast<size_t>(node) >= scene.nodes.size()) return;

    Node& n       = scene.nodes[static_cast<size_t>(node)];
    n.translation = transform.translation;
    n.rotation    = transform.rotation;
    n.scale       = transform.scale;

    // Without this the animator's next update restores the old rest pose and
    // the undo appears to do nothing.
    animator.syncRestPose(node);
}

int History::undo(Scene& scene, Animator& animator)
{
    if (!canUndo()) return -1;

    --m_cursor;
    const TransformEdit& edit = m_entries[m_cursor];
    applyTo(scene, animator, edit.node, edit.before);
    return edit.node;
}

int History::redo(Scene& scene, Animator& animator)
{
    if (!canRedo()) return -1;

    const TransformEdit& edit = m_entries[m_cursor];
    applyTo(scene, animator, edit.node, edit.after);
    ++m_cursor;
    return edit.node;
}

} // namespace mv
