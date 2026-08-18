#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scene/SplatCloud.hpp"

namespace mv {

/// Loads Gaussian-splat assets (splat-flavoured .ply and Niantic .spz) into a
/// SplatCloud. Kept separate from ModelLoader because splats share none of the
/// mesh pipeline; the two are dispatched by the caller based on canLoad().
class SplatLoader
{
public:
    /// True if `path` is a splat asset this loader handles.
    ///
    /// For .spz this is decided by extension (+ magic at load time). For .ply
    /// the extension is ambiguous — a .ply can be a mesh, a plain point cloud
    /// or a splat — so this inspects the header and only returns true when the
    /// vertex element carries the 3DGS splat properties (f_dc_0/scale_0/rot_0).
    /// A non-splat .ply returns false so the caller routes it to ModelLoader.
    static bool canLoad(const std::filesystem::path& path);

    /// Returns true on success. `outCloud` is left untouched on failure and
    /// `outError` describes why. Supports splat-PLY (binary little-endian) and
    /// SPZ format versions 1 and 2. Newer SPZ (v3 smallest-three quaternions,
    /// v4 ZSTD multi-stream) is reported as an explicit, actionable error
    /// rather than mis-decoded.
    static bool load(const std::filesystem::path& path,
                     SplatCloud&                  outCloud,
                     std::string&                 outError);

    /// Extensions this loader may handle, lower-case, without dots.
    /// ("ply" appears here and in ModelLoader's list; canLoad() disambiguates.)
    static const std::vector<std::string>& importExtensions();
};

} // namespace mv
