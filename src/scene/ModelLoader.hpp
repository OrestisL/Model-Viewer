#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scene/Scene.hpp"

namespace mv {

struct ExportFormat
{
    std::string id;           ///< Assimp format id, e.g. "gltf2", "fbx", "objnomtl"
    std::string description;  ///< Human-readable name
    std::string extension;    ///< Without the dot
};

struct ImportOptions
{
    /// Flip the V texture coordinate. glTF/GLB already use a top-left origin,
    /// most other formats do not, so this is chosen per format by default.
    enum class FlipV { Auto, Always, Never };
    FlipV flipV = FlipV::Auto;

    bool  optimizeMeshes  = false;  ///< aiProcess_OptimizeMeshes (breaks 1:1 node mapping less than you'd think)
    bool  generateNormals = true;
    float importScale     = 1.0f;
};

/// Assimp-backed import and export. USD is routed through UsdBackend.
class ModelLoader
{
public:
    /// Returns true on success. `outScene` is left untouched on failure.
    static bool load(const std::filesystem::path& path,
                     const ImportOptions&         options,
                     Scene&                       outScene,
                     std::string&                 outError);

    /// Formats Assimp can write, queried from the library at runtime.
    static const std::vector<ExportFormat>& exportFormats();

    /// Re-exports the originally imported asset. Requires `scene.source` to be
    /// the Assimp-backed asset produced by load().
    static bool exportScene(const Scene&                 scene,
                            const std::string&           formatId,
                            const std::filesystem::path& path,
                            std::string&                 outError);

    /// Extensions Assimp can read, lower-case and without dots.
    static const std::vector<std::string>& importExtensions();

    static bool canLoad(const std::filesystem::path& path);
};

} // namespace mv
