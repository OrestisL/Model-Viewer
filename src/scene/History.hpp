#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mv {

struct Scene;
class Animator;

/// A node's local transform, captured for the undo stack.
struct NodeTransform
{
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

/// One undoable edit: a single node's transform before and after one gesture.
///
/// Deliberately narrow. Only manipulator edits are undoable, so a fixed record
/// is enough and there is no need for a polymorphic command hierarchy -- that
/// would be scaffolding for features this viewer does not have.
struct TransformEdit
{
    int           node = -1;
    NodeTransform before;
    NodeTransform after;
    std::string   label = "Transform";
};

/// One node's visibility flipping.
struct VisibilityChange
{
    int  node   = -1;
    bool before = true;
    bool after  = true;
};

/// A visibility edit, which may cover many nodes at once.
///
/// Hiding one mesh touches one node; isolating or showing everything touches
/// most of them. Only nodes that actually changed are recorded, so the common
/// single-mesh case stays a single entry rather than a full scene snapshot.
struct VisibilityEdit
{
    std::vector<VisibilityChange> changes;
    std::string                   label = "Visibility";
};

/// Linear undo history with a cursor.
///
/// Entries below the cursor have been applied; entries at or above it have
/// been undone and can be redone. Pushing a new edit discards anything above
/// the cursor, which is the behaviour every editor has trained people to
/// expect.
class History
{
public:
    void clear();

    /// Records an edit that has *already* been applied to the scene.
    /// A no-op edit (before == after) is dropped rather than cluttering the
    /// stack, which happens whenever a drag is started and released without
    /// actually moving anything.
    void push(TransformEdit edit);

    /// Records a visibility edit that has already been applied. Edits with no
    /// actual changes are dropped.
    void push(VisibilityEdit edit);

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_entries.size(); }

    /// Label of the edit the next undo/redo would affect, or nullptr.
    const char* undoLabel() const;
    const char* redoLabel() const;

    /// Apply the previous / next edit. Returns the node index that changed,
    /// or -1 if there was nothing to do. The animator is told about the change
    /// because it re-applies the rest pose on every update and would otherwise
    /// undo the undo.
    int undo(Scene& scene, Animator& animator);
    int redo(Scene& scene, Animator& animator);

    size_t depth() const { return m_entries.size(); }
    size_t cursor() const { return m_cursor; }

private:
    /// Old entries are dropped once this is exceeded. Transform records are
    /// tiny, so this is generous.
    static constexpr size_t kCapacity = 256;

    /// Two kinds is not enough to justify a polymorphic command hierarchy;
    /// a tag plus the two payloads keeps the whole thing readable in one file.
    struct Entry
    {
        enum class Kind { Transform, Visibility };

        Kind           kind = Kind::Transform;
        std::string    label;
        TransformEdit  transform;
        VisibilityEdit visibility;
    };

    void applyTo(Scene& scene, Animator& animator, int node,
                 const NodeTransform& transform) const;
    void applyVisibility(Scene& scene, const VisibilityEdit& edit, bool forward) const;
    void record(Entry entry);

    std::vector<Entry> m_entries;
    size_t             m_cursor = 0;
};

/// Reads a node's current transform, for capturing before/after states.
NodeTransform captureTransform(const Scene& scene, int node);

bool operator==(const NodeTransform& a, const NodeTransform& b);
inline bool operator!=(const NodeTransform& a, const NodeTransform& b) { return !(a == b); }

} // namespace mv
