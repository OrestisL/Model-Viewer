#pragma once

// Renderer-agnostic Gaussian-splat cloud, the splat counterpart to Scene.
//
// A splat asset is not a mesh: it is a flat list of anisotropic 3D Gaussians,
// each with a position, an anisotropic scale, an orientation, an opacity and a
// view-dependent colour expressed as spherical-harmonic (SH) coefficients.
// None of Scene's mesh machinery (indices, materials, node hierarchy, skins)
// applies, so splats live in their own type rather than being forced through
// Scene. Nothing here knows about Vulkan or any file format.
//
// Values are stored UN-ACTIVATED, matching the on-disk convention of both
// supported formats and the reference splat libraries:
//   - scales    are log-space   -> apply exp()     at render time
//   - rotations are un-normalised-> normalise       at render time
//   - alphas    are logits      -> apply sigmoid()  at render time
// Keeping the raw values means a single activation path in the renderer serves
// every source format, and re-export stays lossless.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Scene.hpp"   // AABB

namespace mv {

/// Number of SH coefficients per colour channel for a given SH degree.
/// (degree 0 carries only the DC term, which is stored separately as colorDC.)
inline constexpr int splatShDim(int degree)
{
    switch (degree)
    {
        case 0: return 0;
        case 1: return 3;
        case 2: return 8;
        case 3: return 15;
        case 4: return 24;
        default: return 0;
    }
}

/// Inverse of splatShDim: infer the SH degree from a per-channel coeff count.
inline int splatDegreeForDim(int dim)
{
    switch (dim)
    {
        case 0:  return 0;
        case 3:  return 1;
        case 8:  return 2;
        case 15: return 3;
        case 24: return 4;
        default: return 0;
    }
}

/// One anisotropic 3D Gaussian, in the un-activated convention described above.
struct Splat
{
    glm::vec3 position{0.0f};
    glm::vec3 scale{0.0f};              ///< log-space
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< xyzw, un-normalised
    float     alpha = 0.0f;             ///< logit (pre-sigmoid)
    glm::vec3 colorDC{0.0f};            ///< SH degree-0 term (base colour)
};

/// A cloud of Gaussians. Bulk attributes are kept in parallel arrays for cheap
/// GPU upload; `sh` is separate because its per-splat length depends on degree.
struct SplatCloud
{
    std::string sourcePath;
    std::string importerName;

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> scales;      ///< log-space
    std::vector<glm::quat> rotations;   ///< xyzw, un-normalised
    std::vector<float>     alphas;      ///< logits
    std::vector<glm::vec3> colorsDC;    ///< SH degree-0 term

    /// Higher-order SH, laid out per splat as `shDim` coefficients, each an RGB
    /// triple, coefficient-major: [c0.r c0.g c0.b  c1.r c1.g c1.b  ...].
    /// Empty when shDegree == 0. Length == count * splatShDim(shDegree) * 3.
    std::vector<float>     sh;
    int                    shDegree = 0;

    AABB bounds;

    std::size_t count() const { return positions.size(); }
    bool        empty() const { return positions.empty(); }
    int         shDim() const { return splatShDim(shDegree); }

    void clear() { *this = SplatCloud{}; }

    /// Recompute `bounds` from the current positions.
    void updateBounds()
    {
        bounds = AABB{};
        for (const glm::vec3& p : positions) bounds.expand(p);
    }
};

} // namespace mv
