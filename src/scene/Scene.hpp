#pragma once

// Renderer-agnostic scene description produced by the importers and consumed
// by the Vulkan renderer. Nothing in here knows about Vulkan or Assimp.

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mv {

inline constexpr int kInvalidIndex = -1;
inline constexpr uint32_t kMaxBoneInfluences = 4;

// ---------------------------------------------------------------------------

struct Vertex
{
    glm::vec3  position{0.0f};
    glm::vec3  normal{0.0f, 1.0f, 0.0f};
    glm::vec4  tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2  uv{0.0f};
    glm::uvec4 joints{0u};
    glm::vec4  weights{0.0f};
};

struct AABB
{
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    void expand(const glm::vec3& p) { min = glm::min(min, p); max = glm::max(max, p); }
    void expand(const AABB& o)      { if (o.valid()) { expand(o.min); expand(o.max); } }

    bool      valid()  const { return min.x <= max.x; }
    glm::vec3 center() const { return valid() ? (min + max) * 0.5f : glm::vec3(0.0f); }
    glm::vec3 extent() const { return valid() ? (max - min) : glm::vec3(0.0f); }
    float     radius() const { return valid() ? glm::length(extent()) * 0.5f : 1.0f; }

    AABB transformed(const glm::mat4& m) const;
};

// ---------------------------------------------------------------------------

/// CPU-side decoded texture. Always 8-bit RGBA.
struct TextureData
{
    std::string          name;
    uint32_t             width  = 0;
    uint32_t             height = 0;
    bool                 srgb   = false;
    std::vector<uint8_t> pixels;
};

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

struct Material
{
    std::string name = "material";

    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float     metallicFactor  = 0.0f;
    float     roughnessFactor = 0.8f;
    float     alphaCutoff     = 0.5f;

    int baseColorTexture = kInvalidIndex;
    int normalTexture    = kInvalidIndex;
    int metalRoughTexture = kInvalidIndex;
    int emissiveTexture  = kInvalidIndex;

    AlphaMode alphaMode   = AlphaMode::Opaque;
    bool      doubleSided = false;
    bool      unlit       = false;
};

/// One drawable range inside the scene's shared vertex/index buffers.
struct Mesh
{
    std::string name;
    uint32_t    firstIndex   = 0;
    uint32_t    indexCount   = 0;
    int32_t     vertexOffset = 0;
    uint32_t    vertexCount  = 0;
    int         material     = kInvalidIndex;
    int         skin         = kInvalidIndex;
    AABB        bounds;
};

// ---------------------------------------------------------------------------

struct Node
{
    std::string name;
    int         parent = kInvalidIndex;
    std::vector<int>      children;
    std::vector<uint32_t> meshes;

    // Local transform, kept decomposed so animation can override components.
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 localMatrix() const;
};

struct Skin
{
    std::string            name;
    std::vector<int>       joints;          // node indices
    std::vector<glm::mat4> inverseBind;     // parallel to joints
    int                    skeletonRoot = kInvalidIndex;
    uint32_t               gpuOffset    = 0; // first slot in the bone matrix buffer
};

// ---------------------------------------------------------------------------

template <typename T>
struct Key
{
    float time = 0.0f;
    T     value{};
};

struct AnimationChannel
{
    int                     node = kInvalidIndex;
    std::vector<Key<glm::vec3>> positions;
    std::vector<Key<glm::quat>> rotations;
    std::vector<Key<glm::vec3>> scales;
};

struct Animation
{
    std::string                   name;
    float                         duration       = 0.0f; // seconds
    float                         ticksPerSecond = 25.0f;
    std::vector<AnimationChannel> channels;
};

// ---------------------------------------------------------------------------

enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };

struct Light
{
    std::string name;
    LightType   type = LightType::Point;
    int         node = kInvalidIndex;

    glm::vec3 color{1.0f};
    float     intensity = 1.0f;
    float     range     = 0.0f;               // 0 = unbounded
    float     innerCone = glm::radians(20.0f);
    float     outerCone = glm::radians(35.0f);

    // Fallback transform used when `node` is invalid (viewer-created lights).
    glm::vec3 position{0.0f, 3.0f, 0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
};

struct CameraDef
{
    std::string name;
    int         node = kInvalidIndex;

    float yfov        = glm::radians(45.0f);
    float aspectRatio = 0.0f;                 // 0 = use viewport aspect
    float znear       = 0.05f;
    float zfar        = 1000.0f;

    glm::vec3 position{0.0f, 0.0f, 5.0f};
    glm::vec3 lookAt{0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

// ---------------------------------------------------------------------------

/// Opaque handle to whatever the importer needs to keep alive in order to be
/// able to re-export the asset (for Assimp: the owning Importer + aiScene).
struct SourceAsset
{
    virtual ~SourceAsset() = default;
};

struct Scene
{
    std::string sourcePath;
    std::string importerName;

    std::vector<Vertex>      vertices;
    std::vector<uint32_t>    indices;
    std::vector<Mesh>        meshes;
    std::vector<Material>    materials;
    std::vector<TextureData> textures;
    std::vector<Node>        nodes;
    std::vector<int>         roots;
    std::vector<Skin>        skins;
    std::vector<Animation>   animations;
    std::vector<Light>       lights;
    std::vector<CameraDef>   cameras;

    AABB     bounds;
    uint32_t totalBoneSlots = 0;
    float    unitScale      = 1.0f;   // source units -> metres, informational

    std::shared_ptr<SourceAsset> source;   // kept alive for re-export

    bool empty() const { return meshes.empty(); }
    void clear() { *this = Scene{}; }

    /// World-space transform of every node, in node order.
    std::vector<glm::mat4> computeGlobalTransforms() const;

    /// Recompute `bounds` from the current node hierarchy.
    void updateBounds();

    /// Nearest node hit by a world-space ray, or kInvalidIndex for a miss.
    /// Triangle-accurate: the CPU keeps the geometry after upload, so there is
    /// no need to settle for bounding-box precision or a GPU readback.
    /// `distance` receives the hit distance along `direction` when non-null.
    int pickNode(const glm::vec3& origin, const glm::vec3& direction,
                 float* distance = nullptr) const;
};

} // namespace mv
