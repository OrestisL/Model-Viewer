// Standalone splat loader validation tool.
//
// Builds without Vulkan or the rest of the viewer: it compiles SplatLoader.cpp
// against the real project headers and prints a summary of any splat file, so
// the loader can be validated against real data in CI or on the command line.
//
//   splat_inspect <file.ply | file.spz> [numSamples]
//
// Prints: importer, gaussian count, SH degree, AABB, and a few decoded splats
// with activations applied (exp scale, sigmoid alpha, normalised quaternion) as
// a sanity check that the raw stored values are reasonable.

#include "scene/SplatLoader.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <file.ply|file.spz> [numSamples]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const int samples = (argc >= 3) ? std::atoi(argv[2]) : 3;

    if (!mv::SplatLoader::canLoad(path))
    {
        std::fprintf(stderr, "canLoad() returned false for '%s' "
                             "(not a splat file, or unreadable header)\n", path.c_str());
        return 1;
    }

    mv::SplatCloud cloud;
    std::string    err;
    if (!mv::SplatLoader::load(path, cloud, err))
    {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }

    const auto& b = cloud.bounds;
    std::printf("file        : %s\n", path.c_str());
    std::printf("importer    : %s\n", cloud.importerName.c_str());
    std::printf("gaussians   : %zu\n", cloud.count());
    std::printf("SH degree   : %d  (shDim=%d, floats/splat=%d)\n",
                cloud.shDegree, cloud.shDim(), cloud.shDim() * 3);
    std::printf("AABB min    : (%.4f, %.4f, %.4f)\n", b.min.x, b.min.y, b.min.z);
    std::printf("AABB max    : (%.4f, %.4f, %.4f)\n", b.max.x, b.max.y, b.max.z);
    std::printf("AABB centre : (%.4f, %.4f, %.4f)\n", b.center().x, b.center().y, b.center().z);

    // Consistency checks on array lengths.
    const std::size_t n = cloud.count();
    bool sizesOk =
        cloud.scales.size() == n && cloud.rotations.size() == n &&
        cloud.alphas.size() == n && cloud.colorsDC.size() == n &&
        cloud.sh.size() == n * static_cast<std::size_t>(cloud.shDim()) * 3;
    std::printf("array sizes : %s\n", sizesOk ? "consistent" : "MISMATCH");

    std::printf("\nfirst %d gaussians (activations applied for readability):\n", samples);
    for (int i = 0; i < samples && static_cast<std::size_t>(i) < n; ++i)
    {
        const auto& p  = cloud.positions[i];
        const auto& ls = cloud.scales[i];
        const auto& q  = cloud.rotations[i];
        const float qn = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        const float op = 1.0f / (1.0f + std::exp(-cloud.alphas[i]));
        const auto& dc = cloud.colorsDC[i];
        std::printf("  #%d pos(%.3f,%.3f,%.3f) scale_exp(%.3f,%.3f,%.3f) "
                    "quat_xyzw(%.3f,%.3f,%.3f,%.3f)|q|=%.3f opacity=%.3f dc(%.3f,%.3f,%.3f)\n",
                    i, p.x, p.y, p.z,
                    std::exp(ls.x), std::exp(ls.y), std::exp(ls.z),
                    q.x, q.y, q.z, q.w, qn, op, dc.x, dc.y, dc.z);
    }

    if (!sizesOk) return 1;
    std::printf("\nOK\n");
    return 0;
}
