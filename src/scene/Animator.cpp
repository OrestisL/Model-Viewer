#include "scene/Animator.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

namespace mv {
namespace {

/// Index of the last key at or before `time`.
template <typename T>
size_t findKey(const std::vector<Key<T>>& keys, float time)
{
    if (keys.size() < 2) return 0;

    const auto it = std::upper_bound(keys.begin(), keys.end(), time,
                                     [](float t, const Key<T>& key) { return t < key.time; });
    if (it == keys.begin()) return 0;
    return static_cast<size_t>(std::distance(keys.begin(), it)) - 1;
}

float blendFactor(float t0, float t1, float time)
{
    const float span = t1 - t0;
    if (span <= 1e-6f) return 0.0f;
    return glm::clamp((time - t0) / span, 0.0f, 1.0f);
}

glm::vec3 sampleVec3(const std::vector<Key<glm::vec3>>& keys, float time, const glm::vec3& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1) return keys.front().value;

    const size_t i = findKey(keys, time);
    if (i + 1 >= keys.size()) return keys.back().value;

    const float f = blendFactor(keys[i].time, keys[i + 1].time, time);
    return glm::mix(keys[i].value, keys[i + 1].value, f);
}

glm::quat sampleQuat(const std::vector<Key<glm::quat>>& keys, float time, const glm::quat& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1) return glm::normalize(keys.front().value);

    const size_t i = findKey(keys, time);
    if (i + 1 >= keys.size()) return glm::normalize(keys.back().value);

    const float f = blendFactor(keys[i].time, keys[i + 1].time, time);
    return glm::normalize(glm::slerp(keys[i].value, keys[i + 1].value, f));
}

} // namespace

// ---------------------------------------------------------------------------

void Animator::setScene(Scene* scene)
{
    m_scene = scene;
    m_time  = 0.0f;
    m_globals.clear();
    m_boneMatrices.clear();
    m_restPose.clear();

    if (!m_scene) { m_active = kInvalidIndex; return; }

    captureRestPose();

    m_boneMatrices.assign(std::max<size_t>(m_scene->totalBoneSlots, 1), glm::mat4(1.0f));
    m_active = m_scene->animations.empty() ? kInvalidIndex : 0;

    refresh();
}

void Animator::captureRestPose()
{
    m_restPose.clear();
    m_restPose.reserve(m_scene->nodes.size());
    for (const Node& node : m_scene->nodes)
        m_restPose.push_back({node.translation, node.rotation, node.scale});
}

void Animator::syncRestPose(int nodeIndex)
{
    if (!m_scene) return;
    if (nodeIndex < 0) return;

    const size_t index = static_cast<size_t>(nodeIndex);
    if (index >= m_restPose.size() || index >= m_scene->nodes.size()) return;

    const Node& node = m_scene->nodes[index];
    m_restPose[index] = Pose{node.translation, node.rotation, node.scale};
}

void Animator::applyRestPose()
{
    if (!m_scene) return;
    for (size_t i = 0; i < m_scene->nodes.size() && i < m_restPose.size(); ++i)
    {
        Node& node       = m_scene->nodes[i];
        node.translation = m_restPose[i].translation;
        node.rotation    = m_restPose[i].rotation;
        node.scale       = m_restPose[i].scale;
    }
}

void Animator::setActiveAnimation(int index)
{
    if (!m_scene) return;
    if (index < kInvalidIndex || index >= static_cast<int>(m_scene->animations.size()))
        return;

    m_active = index;
    m_time   = 0.0f;
    refresh();
}

void Animator::setTime(float seconds)
{
    m_time = glm::max(seconds, 0.0f);
    const float d = duration();
    if (d > 0.0f) m_time = m_loop ? std::fmod(m_time, d) : glm::min(m_time, d);
    refresh();
}

float Animator::duration() const
{
    if (!m_scene || m_active == kInvalidIndex) return 0.0f;
    return m_scene->animations[static_cast<size_t>(m_active)].duration;
}

void Animator::stop()
{
    m_playing = false;
    m_time    = 0.0f;
    applyRestPose();
    evaluateHierarchy();
}

void Animator::update(float deltaSeconds)
{
    if (!m_scene) return;

    if (m_playing && m_active != kInvalidIndex)
    {
        const float d = duration();
        if (d > 0.0f)
        {
            m_time += deltaSeconds * m_speed;
            if (m_loop)
            {
                m_time = std::fmod(m_time, d);
                if (m_time < 0.0f) m_time += d;
            }
            else if (m_time >= d)
            {
                m_time   = d;
                m_playing = false;
            }
            else if (m_time < 0.0f)
            {
                m_time = 0.0f;
            }
        }
    }

    refresh();
}

void Animator::refresh()
{
    if (!m_scene) return;

    if (m_active != kInvalidIndex)
        samplePose();
    else
        applyRestPose();

    evaluateHierarchy();
}

void Animator::samplePose()
{
    applyRestPose();

    const Animation& animation = m_scene->animations[static_cast<size_t>(m_active)];

    for (const AnimationChannel& channel : animation.channels)
    {
        if (channel.node < 0 || channel.node >= static_cast<int>(m_scene->nodes.size()))
            continue;

        Node&       node = m_scene->nodes[static_cast<size_t>(channel.node)];
        const Pose& rest = m_restPose[static_cast<size_t>(channel.node)];

        node.translation = sampleVec3(channel.positions, m_time, rest.translation);
        node.rotation    = sampleQuat(channel.rotations, m_time, rest.rotation);
        node.scale       = sampleVec3(channel.scales,    m_time, rest.scale);
    }
}

void Animator::evaluateHierarchy()
{
    m_globals = m_scene->computeGlobalTransforms();

    if (m_boneMatrices.size() < std::max<size_t>(m_scene->totalBoneSlots, 1))
        m_boneMatrices.assign(std::max<size_t>(m_scene->totalBoneSlots, 1), glm::mat4(1.0f));

    for (const Skin& skin : m_scene->skins)
    {
        for (size_t j = 0; j < skin.joints.size(); ++j)
        {
            const size_t slot = skin.gpuOffset + j;
            if (slot >= m_boneMatrices.size()) break;

            const int joint = skin.joints[j];
            // Mesh vertices are authored in scene-root space, so no extra
            // inverse-root term is needed here: the model matrix for skinned
            // meshes is identity and the joint chain carries the world space.
            m_boneMatrices[slot] =
                (joint == kInvalidIndex)
                    ? glm::mat4(1.0f)
                    : m_globals[static_cast<size_t>(joint)] * skin.inverseBind[j];
        }
    }
}

} // namespace mv
