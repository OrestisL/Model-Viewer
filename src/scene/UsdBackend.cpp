// The standalone tinyusdz backend has been retired.
//
// Assimp 5.4 gained its own USD reader (ASSIMP_BUILD_USD_IMPORTER), which
// clones a pinned tinyusdz revision and exposes USD through the same importer
// interface as every other format. Maintaining a second, parallel path with
// its own material and scene-graph translation was strictly worse: more code
// to keep in step, and a different set of bugs from the rest of the pipeline.
//
// What remains here is the extension list and a clear message for builds
// configured without USD support.

#include "scene/UsdBackend.hpp"

#include "core/Utf8.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mv {

bool UsdBackend::compiledIn()
{
#if MV_USD_VIA_ASSIMP
    return true;
#else
    return false;
#endif
}

const std::vector<std::string>& UsdBackend::extensions()
{
    static const std::vector<std::string> exts{"usd", "usda", "usdc", "usdz"};
    return exts;
}

bool UsdBackend::handles(const fs::path& path)
{
    std::string ext = pathToUtf8(path.extension());
    if (ext.empty()) return false;
    if (ext.front() == '.') ext.erase(ext.begin());
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

    for (const std::string& candidate : extensions())
        if (ext == candidate) return true;
    return false;
}

bool UsdBackend::load(const fs::path& path, Scene&, std::string& outError)
{
    outError = "This build has no USD support. Reconfigure with "
               "-DMV_ENABLE_USD=ON, or convert " + pathToUtf8(path.filename()) +
               " to glTF first (usdcat or usdzconvert will do it).";
    return false;
}

} // namespace mv
