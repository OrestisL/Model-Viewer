#include "scene/Scene.hpp"

#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace mv {

AABB AABB::transformed(const glm::mat4& m) const
{
    if (!valid()) return {};

    AABB out;
    for (int i = 0; i < 8; ++i)
    {
        const glm::vec3 corner{
            (i & 1) ? max.x : min.x,
            (i & 2) ? max.y : min.y,
            (i & 4) ? max.z : min.z};
        out.expand(glm::vec3(m * glm::vec4(corner, 1.0f)));
    }
    return out;
}

glm::mat4 Node::localMatrix() const
{
    glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
    glm::mat4 r = glm::mat4_cast(rotation);
    glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
    return t * r * s;
}

std::vector<glm::mat4> Scene::computeGlobalTransforms() const
{
    std::vector<glm::mat4> globals(nodes.size(), glm::mat4(1.0f));
    std::vector<int> stack(roots.begin(), roots.end());

    while (!stack.empty())
    {
        const int index = stack.back();
        stack.pop_back();

        const Node& node = nodes[static_cast<size_t>(index)];
        const glm::mat4 parent =
            (node.parent == kInvalidIndex) ? glm::mat4(1.0f)
                                           : globals[static_cast<size_t>(node.parent)];

        globals[static_cast<size_t>(index)] = parent * node.localMatrix();

        for (int child : node.children)
            stack.push_back(child);
    }

    return globals;
}

std::vector<uint8_t> Scene::computeVisibility() const
{
    std::vector<uint8_t> visible(nodes.size(), 1);
    std::vector<int>     stack(roots.begin(), roots.end());

    while (!stack.empty())
    {
        const int index = stack.back();
        stack.pop_back();

        const Node& node = nodes[static_cast<size_t>(index)];

        const bool parentVisible =
            (node.parent == kInvalidIndex) ||
            visible[static_cast<size_t>(node.parent)] != 0;

        visible[static_cast<size_t>(index)] =
            (parentVisible && node.visible) ? 1u : 0u;

        for (int child : node.children)
            stack.push_back(child);
    }

    return visible;
}

void Scene::showAll()
{
    for (Node& node : nodes) node.visible = true;
}

void Scene::isolate(int node)
{
    if (node < 0 || static_cast<size_t>(node) >= nodes.size()) return;

    for (Node& n : nodes) n.visible = false;

    // The node itself and everything under it.
    std::vector<int> stack{node};
    while (!stack.empty())
    {
        const int index = stack.back();
        stack.pop_back();

        nodes[static_cast<size_t>(index)].visible = true;
        for (int child : nodes[static_cast<size_t>(index)].children)
            stack.push_back(child);
    }

    // Then walk back up, or the whole subtree stays hidden behind a hidden
    // parent.
    for (int p = nodes[static_cast<size_t>(node)].parent; p != kInvalidIndex;
         p = nodes[static_cast<size_t>(p)].parent)
    {
        nodes[static_cast<size_t>(p)].visible = true;
    }
}

void Scene::updateBounds()
{
    bounds = AABB{};

    const std::vector<glm::mat4> globals = computeGlobalTransforms();
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        for (uint32_t meshIndex : nodes[i].meshes)
        {
            if (meshIndex >= meshes.size()) continue;
            const Mesh& mesh = meshes[meshIndex];
            // Skinned meshes are posed by the animator; use the untransformed
            // bind-pose bounds so the framing stays stable.
            bounds.expand(mesh.skin == kInvalidIndex
                              ? mesh.bounds.transformed(globals[i])
                              : mesh.bounds);
        }
    }

    if (!bounds.valid())
    {
        bounds.min = glm::vec3(-1.0f);
        bounds.max = glm::vec3(1.0f);
    }
}

namespace {

/// Slab test. Returns false when the ray misses the box entirely.
bool rayHitsAabb(const glm::vec3& origin, const glm::vec3& invDir, const AABB& box)
{
    const glm::vec3 t0 = (box.min - origin) * invDir;
    const glm::vec3 t1 = (box.max - origin) * invDir;
    const glm::vec3 lo = glm::min(t0, t1);
    const glm::vec3 hi = glm::max(t0, t1);

    const float enter = glm::max(glm::max(lo.x, lo.y), lo.z);
    const float exit  = glm::min(glm::min(hi.x, hi.y), hi.z);
    return exit >= glm::max(enter, 0.0f);
}

/// Moller-Trumbore, double sided: a viewer should select a face you can see
/// regardless of which way its winding happens to run.
bool rayHitsTriangle(const glm::vec3& origin, const glm::vec3& direction,
                     const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                     float& t)
{
    constexpr float kEpsilon = 1e-8f;

    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 p  = glm::cross(direction, ac);
    const float     det = glm::dot(ab, p);

    if (std::abs(det) < kEpsilon) return false;

    const float     invDet = 1.0f / det;
    const glm::vec3 s      = origin - a;

    const float u = glm::dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    const glm::vec3 q = glm::cross(s, ab);
    const float     v = glm::dot(direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    t = glm::dot(ac, q) * invDet;
    return t > kEpsilon;
}

} // namespace

int Scene::pickNode(const glm::vec3& origin, const glm::vec3& direction,
                    float* distance) const
{
    if (meshes.empty() || indices.empty()) return kInvalidIndex;

    const std::vector<glm::mat4> globals = computeGlobalTransforms();

    // You should not be able to click something you cannot see.
    const std::vector<uint8_t> visible = computeVisibility();

    int   bestNode = kInvalidIndex;
    float bestT    = std::numeric_limits<float>::max();

    for (size_t n = 0; n < nodes.size(); ++n)
    {
        if (n < visible.size() && !visible[n]) continue;

        for (uint32_t meshIndex : nodes[n].meshes)
        {
            if (meshIndex >= meshes.size()) continue;
            const Mesh& mesh = meshes[meshIndex];

            // Skinned meshes are authored in scene-root space and drawn with an
            // identity model matrix, so they must be tested there too. This
            // tests the bind pose: a posed skin will not match exactly.
            const bool      skinned = mesh.skin != kInvalidIndex;
            const glm::mat4 model   = skinned ? glm::mat4(1.0f) : globals[n];

            // Cheaper to bring the ray into object space than the mesh into
            // world space -- one matrix inverse instead of thousands of
            // vertex transforms.
            const glm::mat4 inverseModel = glm::inverse(model);
            const glm::vec3 localOrigin  = glm::vec3(inverseModel * glm::vec4(origin, 1.0f));
            const glm::vec3 localDir     = glm::vec3(inverseModel * glm::vec4(direction, 0.0f));

            const glm::vec3 invDir{
                1.0f / (std::abs(localDir.x) < 1e-20f ? 1e-20f : localDir.x),
                1.0f / (std::abs(localDir.y) < 1e-20f ? 1e-20f : localDir.y),
                1.0f / (std::abs(localDir.z) < 1e-20f ? 1e-20f : localDir.z)};

            if (mesh.bounds.valid() && !rayHitsAabb(localOrigin, invDir, mesh.bounds))
                continue;

            const uint32_t end = mesh.firstIndex + mesh.indexCount;
            for (uint32_t i = mesh.firstIndex; i + 2 < end; i += 3)
            {
                if (i + 2 >= indices.size()) break;

                const size_t i0 = static_cast<size_t>(mesh.vertexOffset) + indices[i];
                const size_t i1 = static_cast<size_t>(mesh.vertexOffset) + indices[i + 1];
                const size_t i2 = static_cast<size_t>(mesh.vertexOffset) + indices[i + 2];
                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                    continue;

                float t = 0.0f;
                if (rayHitsTriangle(localOrigin, localDir,
                                    vertices[i0].position,
                                    vertices[i1].position,
                                    vertices[i2].position, t) &&
                    t < bestT)
                {
                    bestT    = t;
                    bestNode = static_cast<int>(n);
                }
            }
        }
    }

    if (bestNode != kInvalidIndex && distance) *distance = bestT;
    return bestNode;
}

} // namespace mv
