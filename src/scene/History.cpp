#include "scene/History.hpp"

#include <algorithm>
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

void History::record(Entry entry)
{
    // Anything that was undone is now unreachable.
    if (m_cursor < m_entries.size())
        m_entries.erase(m_entries.begin() + static_cast<ptrdiff_t>(m_cursor), m_entries.end());

    m_entries.push_back(std::move(entry));

    if (m_entries.size() > kCapacity)
        m_entries.erase(m_entries.begin(),
                        m_entries.begin() +
                            static_cast<ptrdiff_t>(m_entries.size() - kCapacity));

    m_cursor = m_entries.size();
}

void History::push(TransformEdit edit)
{
    if (edit.node < 0) return;
    if (edit.before == edit.after) return;   // nothing actually moved

    Entry entry;
    entry.kind      = Entry::Kind::Transform;
    entry.label     = edit.label;
    entry.transform = std::move(edit);
    record(std::move(entry));
}

void History::push(VisibilityEdit edit)
{
    // Drop no-ops, and any per-node record that did not actually flip.
    edit.changes.erase(
        std::remove_if(edit.changes.begin(), edit.changes.end(),
                       [](const VisibilityChange& c) {
                           return c.node < 0 || c.before == c.after;
                       }),
        edit.changes.end());

    if (edit.changes.empty()) return;

    Entry entry;
    entry.kind       = Entry::Kind::Visibility;
    entry.label      = edit.label;
    entry.visibility = std::move(edit);
    record(std::move(entry));
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

void History::applyVisibility(Scene& scene, const VisibilityEdit& edit, bool forward) const
{
    for (const VisibilityChange& change : edit.changes)
    {
        if (change.node < 0 || static_cast<size_t>(change.node) >= scene.nodes.size())
            continue;

        scene.nodes[static_cast<size_t>(change.node)].visible =
            forward ? change.after : change.before;
    }
}

int History::undo(Scene& scene, Animator& animator)
{
    if (!canUndo()) return -1;

    --m_cursor;
    const Entry& entry = m_entries[m_cursor];

    if (entry.kind == Entry::Kind::Transform)
    {
        applyTo(scene, animator, entry.transform.node, entry.transform.before);
        return entry.transform.node;
    }

    applyVisibility(scene, entry.visibility, false);

    // Only worth moving the selection when exactly one node was involved;
    // for a bulk change there is no single sensible thing to select.
    return entry.visibility.changes.size() == 1 ? entry.visibility.changes.front().node : -1;
}

int History::redo(Scene& scene, Animator& animator)
{
    if (!canRedo()) return -1;

    const Entry& entry = m_entries[m_cursor];
    ++m_cursor;

    if (entry.kind == Entry::Kind::Transform)
    {
        applyTo(scene, animator, entry.transform.node, entry.transform.after);
        return entry.transform.node;
    }

    applyVisibility(scene, entry.visibility, true);
    return entry.visibility.changes.size() == 1 ? entry.visibility.changes.front().node : -1;
}

} // namespace mv
