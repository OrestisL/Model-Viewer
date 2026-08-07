// ModelViewer -- cross-platform Vulkan 3D model viewer.
//
// Usage:
//   ModelViewer [options] [model-file]
//
// Options:
//   -h, --help      print this message and exit
//   -v, --version   print the version and exit
//
// Any non-option argument is treated as a model to open at startup. Files can
// also be dropped onto the window at any time.

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "App.hpp"
#include "core/Log.hpp"

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace {

constexpr const char* kVersion = "ModelViewer 0.1.0";

void printUsage()
{
    std::printf(
        "%s\n"
        "\n"
        "Usage: ModelViewer [options] [model-file]\n"
        "\n"
        "Options:\n"
        "  -h, --help      show this message and exit\n"
        "  -v, --version   show the version and exit\n"
        "\n"
        "Supported input formats: fbx, obj, stl, ply, dae, gltf, glb, 3ds, blend\n"
        "and everything else Assimp reads. USD (usd/usda/usdc/usdz) requires a\n"
        "build configured with -DMV_ENABLE_USD=ON.\n",
        kVersion);
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // Make sure UTF-8 paths and log output survive the console.
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];

        if (argument == "-h" || argument == "--help")
        {
            printUsage();
            return 0;
        }
        if (argument == "-v" || argument == "--version")
        {
            std::printf("%s\n", kVersion);
            return 0;
        }

        arguments.push_back(argument);
    }

    try
    {
        mv::App app;
        return app.run(arguments);
    }
    catch (const std::exception& e)
    {
        mv::log::error("Fatal: ", e.what());
#if defined(_WIN32)
        ::MessageBoxA(nullptr, e.what(), "ModelViewer -- fatal error",
                      MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
    catch (...)
    {
        mv::log::error("Fatal: unknown exception");
        return 1;
    }
}
