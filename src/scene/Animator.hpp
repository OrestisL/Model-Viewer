#pragma once

#include <vector>

#include "scene/Scene.hpp"

namespace mv {

/// Samples the active animation, poses the node hierarchy and produces the
/// skinning matrices the vertex shader expects.
class Animator
{
public:
    void  setScene(Scene* scene);
    void  update(float deltaSeconds);

    /// Re-evaluate transforms without advancing time (after a manual scrub).
    void  refresh();

    // -- playback control ---------------------------------------------------
    bool  hasAnimations() const { return m_scene && !m_scene->animations.empty(); }
    int   animationCount() const { return m_scene ? static_cast<int>(m_scene->animations.size()) : 0; }
    int   activeAnimation() const { return m_active; }
    void  setActiveAnimation(int index);

    bool  isPlaying() const { return m_playing; }
    void  setPlaying(bool playing) { m_playing = playing; }
    void  togglePlay() { m_playing = !m_playing; }

    bool  isLooping() const { return m_loop; }
    void  setLooping(bool loop) { m_loop = loop; }

    float speed() const { return m_speed; }
    void  setSpeed(float speed) { m_speed = speed; }

    float time() const { return m_time; }
    void  setTime(float seconds);

    /// Records one node's current transform as its new rest pose. The animator
    /// re-applies the rest pose on every update, so an edit made from outside
    /// (the manipulator) must be written here too or it is undone next frame.
    void  syncRestPose(int nodeIndex);
    float duration() const;

    void  restart() { setTime(0.0f); }
    void  stop();

    const std::vector<glm::mat4>& boneMatrices()     const { return m_boneMatrices; }
    const std::vector<glm::mat4>& globalTransforms() const { return m_globals; }

private:
    void captureRestPose();
    void applyRestPose();
    void samplePose();
    void evaluateHierarchy();

    Scene* m_scene = nullptr;

    struct Pose { glm::vec3 translation; glm::quat rotation; glm::vec3 scale; };
    std::vector<Pose>      m_restPose;
    std::vector<glm::mat4> m_globals;
    std::vector<glm::mat4> m_boneMatrices;

    int   m_active  = kInvalidIndex;
    float m_time    = 0.0f;
    float m_speed   = 1.0f;
    bool  m_playing = true;
    bool  m_loop    = true;
};

} // namespace mv
