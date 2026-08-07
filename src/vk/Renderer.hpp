#pragma once

#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "vk/Context.hpp"
#include "vk/Resources.hpp"

namespace mv {

class Camera;
class Animator;
class Window;
struct Scene;

namespace gfx {

inline constexpr uint32_t kMaxLights      = 16;   // must match shaders/common.glsl

enum class DebugView : int
{
    Shaded = 0,
    BaseColor,
    Normals,
    TexCoords,
    Metallic,
    Roughness,
    Count
};

const char* debugViewName(DebugView view);

/// Everything the UI can tweak about a frame.
struct RenderSettings
{
    glm::vec3 clearColor{0.10f, 0.11f, 0.13f};

    // Colour wheel target: applied to materials that have no base colour map.
    glm::vec3 defaultColor{0.78f, 0.78f, 0.80f};
    float     defaultMetallic  = 0.0f;
    float     defaultRoughness = 0.55f;
    bool      overrideUntextured = true;

    /// Frames per second to hold the loop to. 0 leaves it uncapped.
    ///
    /// A steady frame rate matters beyond smoothness: variable-refresh
    /// displays change panel timing to follow the frame rate, and many panels
    /// visibly shift brightness when it moves around. Pinning the rate removes
    /// that whole class of problem, and tells you immediately whether what you
    /// are seeing comes from the renderer or from the display.
    int       frameRateCap = 0;

    bool      showGrid = true;
    // The grid can draw its own coloured X/Z lines across the whole plane.
    // Off by default now that real 3D axis arrows sit at the origin: two
    // overlapping red lines in the same place is just visual noise.
    bool      gridAxisLines = false;
    /// Half-width of the grid in cells. The mesh is finite, which is the point:
    /// an infinite plane always has a region near the horizon where cells fall
    /// below pixel size and alias no matter how it is drawn.
    int       gridHalfExtent = 60;
    float     gridCell = 1.0f;
    glm::vec3 gridColor{0.45f, 0.47f, 0.52f};

    // Origin axis arrows: X red, Y green, Z blue. Length is a fraction of the
    // scene radius so the same setting suits a bolt and a battleship.
    bool      showAxes  = true;
    float     axesScale = 0.35f;

    bool      wireframe = false;
    bool      backfaceCulling = true;
    DebugView debugView = DebugView::Shaded;

    float     exposure = 1.0f;
    glm::vec3 ambientColor{0.55f, 0.60f, 0.70f};
    float     ambientIntensity = 0.35f;

    bool      useSceneLights = true;
    float     lightIntensityScale = 1.0f;

    bool      vsync = true;
};

/// Handles for the currently uploaded model.
struct GpuScene
{
    Buffer vertexBuffer;
    Buffer indexBuffer;
    Buffer boneBuffer;                     // shared, resized on upload

    std::vector<Image>           textures;
    std::vector<VkDescriptorSet> materialSets;
    VkDescriptorSet              fallbackSet = VK_NULL_HANDLE;

    uint32_t indexCount   = 0;
    uint32_t triangleCount = 0;

    bool valid() const { return indexBuffer.handle != VK_NULL_HANDLE; }
};

struct FrameStats
{
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    float    cpuFrameMs = 0.0f;
};

class Renderer
{
public:
    Renderer() = default;
    ~Renderer();

    void init(Context& context, Window& window);
    void shutdown();

    /// Replaces the GPU-side model. Safe to call with an empty scene.
    void uploadScene(const Scene& scene);
    void clearScene();

    /// Acquires a swapchain image and starts the ImGui frame.
    /// Returns false when the frame should be skipped (minimised / resizing).
    bool beginFrame();

    /// Records the scene + ImGui draw data and presents.
    void endFrame(const Scene*    scene,
                  const Animator* animator,
                  const Camera&   camera,
                  const RenderSettings& settings);

    void onWindowResized() { m_swapchainDirty = true; }

    float      aspectRatio() const;
    FrameStats stats() const { return m_stats; }

    /// Viewport size in pixels.
    VkExtent2D viewportExtent() const;

private:
    struct FrameData
    {
        VkCommandPool   commandPool   = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence         inFlight      = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable = VK_NULL_HANDLE;
        Buffer          globals;
        Buffer          bones;
        VkDescriptorSet globalSet = VK_NULL_HANDLE;
    };

    /// Mirrors the push_constant block in shaders/common.glsl (112 bytes).
    struct PushConstants
    {
        glm::mat4 model{1.0f};
        glm::vec4 baseColor{1.0f};
        glm::vec4 mrfs{0.0f, 0.5f, 0.0f, -1.0f};
        glm::vec4 emissive{0.0f, 0.0f, 0.0f, 0.5f};
    };

    struct DrawItem
    {
        PushConstants   push;
        VkDescriptorSet materialSet = VK_NULL_HANDLE;
        uint32_t        firstIndex   = 0;
        uint32_t        indexCount   = 0;
        int32_t         vertexOffset = 0;
        bool            doubleSided  = false;
        float           viewDepth    = 0.0f;   // for back-to-front blending
    };

    void createFrameResources();
    void createDescriptorLayouts();
    void createDescriptorPool();
    void createPipelines();
    void destroyPipelines();
    void recreateSwapchainIfNeeded();

    void updateGlobals(const Scene* scene, const Camera& camera, const RenderSettings& settings);
    void updateBones(const Animator* animator);

    void buildDrawList(const Scene* scene, const Animator* animator,
                       const Camera& camera, const RenderSettings& settings);

    void recordScene(VkCommandBuffer cmd, const RenderSettings& settings);
    void recordGrid(VkCommandBuffer cmd, const RenderSettings& settings);

    /// Rebuilds the grid line mesh when the cell size or extent changes.
    /// Cheap enough to be lazy about: a few thousand vertices.
    void rebuildGridMesh(const RenderSettings& settings, float sceneRadius);
    void recordAxes(VkCommandBuffer cmd, const RenderSettings& settings, float sceneRadius);

    VkDescriptorSet allocateMaterialSet(const std::vector<VkImageView>& views);
    VkPipeline      pipelineFor(const RenderSettings& settings, bool doubleSided, bool blended) const;

    Context* m_context = nullptr;
    Window*  m_window  = nullptr;

    std::array<FrameData, kFramesInFlight> m_frames{};
    std::vector<VkSemaphore>               m_renderFinished;   // one per swapchain image
    uint32_t m_frameIndex = 0;
    uint32_t m_imageIndex = 0;
    bool     m_frameStarted   = false;
    bool     m_swapchainDirty = false;

    VkDescriptorSetLayout m_globalLayout   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_materialLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorPool      m_imguiPool      = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;

    // [doubleSided][blended]
    VkPipeline m_meshPipelines[2][2]{};
    VkPipeline m_wireframePipeline = VK_NULL_HANDLE;
    VkPipeline m_gridPipeline      = VK_NULL_HANDLE;
    VkPipeline m_axesPipeline      = VK_NULL_HANDLE;

    Buffer   m_gridMesh;
    uint32_t m_gridVertexCount = 0;
    float    m_gridBuiltCell   = -1.0f;
    int      m_gridBuiltHalf   = -1;
    bool     m_gridBuiltAxes   = false;

    VkSampler m_sampler        = VK_NULL_HANDLE;
    Image     m_whiteTexture;
    Image     m_normalTexture;
    Image     m_blackTexture;

    GpuScene              m_gpuScene;
    std::vector<DrawItem> m_opaqueDraws;
    std::vector<DrawItem> m_blendedDraws;

    FrameStats m_stats{};
    bool       m_imguiInitialised = false;
};

} // namespace gfx
} // namespace mv
