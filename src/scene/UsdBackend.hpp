#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scene/Scene.hpp"

namespace mv {

/// USD (.usd/.usda/.usdc/.usdz) support.
///
/// Reading is done by Assimp itself when the project is configured with
/// -DMV_ENABLE_USD=ON, which turns on ASSIMP_BUILD_USD_IMPORTER and pulls a
/// pinned tinyusdz into the Assimp build. What is left here is the extension
/// list -- so USD files are recognised in dialogs regardless -- and a clear
/// message for builds without USD support.
class UsdBackend
{
public:
    static bool compiledIn();

    static const std::vector<std::string>& extensions();

    static bool handles(const std::filesystem::path& path);

    static bool load(const std::filesystem::path& path,
                     Scene&                       outScene,
                     std::string&                 outError);
};

} // namespace mv
