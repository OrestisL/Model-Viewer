#include "scene/SplatLoader.hpp"

#include "core/Log.hpp"
#include "core/Utf8.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>

#include <zlib.h>

namespace fs = std::filesystem;

namespace mv {
namespace {

// ---------------------------------------------------------------------------
// Shared activation-free helpers. Values are decoded to the same un-activated
// convention SplatCloud documents (log scale, logit alpha, xyzw quaternion).
// ---------------------------------------------------------------------------

constexpr float kColorScale = 0.15f;   // SPZ colour quantisation constant

float invSigmoid(float x)
{
    x = std::min(std::max(x, 1e-6f), 1.0f - 1e-6f);
    return std::log(x / (1.0f - x));
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extensionOf(const fs::path& path)
{
    std::string ext = pathToUtf8(path.extension());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    return lower(ext);
}

// ---------------------------------------------------------------------------
// PLY
// ---------------------------------------------------------------------------

struct PlyHeader
{
    bool                     ok             = false;
    bool                     binaryLE       = false;
    bool                     allFloat       = true;   // every vertex prop is float32
    uint64_t                 vertexCount    = 0;
    std::size_t              dataOffset     = 0;       // byte offset of vertex data
    std::vector<std::string> propertyOrder;           // vertex property names, in order
    std::unordered_map<std::string, int> propertyIndex;
};

// Parse just enough of a PLY header to decide splat-ness and locate data.
// Only the `vertex` element's float properties are needed; any element after
// vertex is ignored (its data is skipped by not reading past vertexCount).
PlyHeader parsePlyHeader(std::istream& in)
{
    PlyHeader h;
    std::string line;

    if (!std::getline(in, line)) return h;
    // Tolerate a trailing '\r' from CRLF files.
    auto rstrip = [](std::string& s) { if (!s.empty() && s.back() == '\r') s.pop_back(); };
    rstrip(line);
    if (line != "ply") return h;

    bool inVertex = false;
    std::size_t headerBytes = line.size() + 1;  // include the '\n' we consumed

    while (std::getline(in, line))
    {
        headerBytes += line.size() + 1;
        rstrip(line);

        std::istringstream ls(line);
        std::string tok;
        ls >> tok;

        if (tok == "format")
        {
            std::string fmt;
            ls >> fmt;
            h.binaryLE = (fmt == "binary_little_endian");
        }
        else if (tok == "comment" || tok == "obj_info")
        {
            continue;
        }
        else if (tok == "element")
        {
            std::string name;
            uint64_t    count = 0;
            ls >> name >> count;
            inVertex = (name == "vertex");
            if (inVertex) h.vertexCount = count;
        }
        else if (tok == "property" && inVertex)
        {
            std::string type, name;
            ls >> type;
            if (type == "list")
            {
                // A list property on the vertex element isn't part of the 3DGS
                // layout; its presence breaks the fixed-stride float read.
                h.allFloat = false;
                continue;
            }
            ls >> name;
            if (type != "float" && type != "float32") h.allFloat = false;
            h.propertyIndex[name] = static_cast<int>(h.propertyOrder.size());
            h.propertyOrder.push_back(name);
        }
        else if (tok == "end_header")
        {
            h.dataOffset = headerBytes;
            h.ok = true;
            return h;
        }
    }
    return h;   // ok stays false if end_header was never seen
}

bool plyHasSplatProps(const PlyHeader& h)
{
    return h.propertyIndex.count("f_dc_0") && h.propertyIndex.count("scale_0") &&
           h.propertyIndex.count("rot_0");
}

bool loadPly(const fs::path& path, SplatCloud& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Cannot open file: " + pathToUtf8(path); return false; }

    PlyHeader h = parsePlyHeader(in);
    if (!h.ok)          { err = "Malformed PLY header.";                     return false; }
    if (!plyHasSplatProps(h))
    {
        err = "PLY is not a Gaussian splat (no f_dc_0/scale_0/rot_0). Load it "
              "as a mesh instead.";
        return false;
    }
    if (!h.binaryLE)    { err = "Only binary_little_endian splat PLY is supported."; return false; }
    if (!h.allFloat)    { err = "Splat PLY has non-float vertex properties; unsupported layout."; return false; }
    if (h.vertexCount == 0) { err = "Splat PLY has no vertices."; return false; }

    const int stride = static_cast<int>(h.propertyOrder.size());
    auto idx = [&](const char* name) -> int {
        auto it = h.propertyIndex.find(name);
        return it == h.propertyIndex.end() ? -1 : it->second;
    };

    const std::array<int, 3> posI  = {idx("x"), idx("y"), idx("z")};
    const std::array<int, 3> sclI  = {idx("scale_0"), idx("scale_1"), idx("scale_2")};
    // PLY stores the quaternion as WXYZ (rot_0..3). Reorder to xyzw here.
    const std::array<int, 4> rotI  = {idx("rot_1"), idx("rot_2"), idx("rot_3"), idx("rot_0")};
    const int                alpI  = idx("opacity");
    const std::array<int, 3> dcI   = {idx("f_dc_0"), idx("f_dc_1"), idx("f_dc_2")};

    for (int v : posI) if (v < 0) { err = "Splat PLY missing a position property."; return false; }
    for (int v : sclI) if (v < 0) { err = "Splat PLY missing a scale property.";    return false; }
    for (int v : rotI) if (v < 0) { err = "Splat PLY missing a rotation property."; return false; }
    for (int v : dcI)  if (v < 0) { err = "Splat PLY missing an f_dc property.";     return false; }
    if (alpI < 0)                 { err = "Splat PLY missing opacity.";              return false; }

    // Higher-order SH: count contiguous f_rest_i. On disk these are channel-major
    // ([C,S]: all R coeffs, then G, then B). shDim = total / 3.
    std::vector<int> restI;
    for (int i = 0; ; ++i)
    {
        int j = idx(("f_rest_" + std::to_string(i)).c_str());
        if (j < 0) break;
        restI.push_back(j);
    }
    const int shDim    = static_cast<int>(restI.size() / 3);
    const int shDegree = splatDegreeForDim(shDim);

    // Bulk-read the whole vertex block: every property is float32.
    const std::size_t floatsPerVertex = static_cast<std::size_t>(stride);
    const std::size_t totalFloats      = floatsPerVertex * h.vertexCount;
    std::vector<float> data(totalFloats);

    in.seekg(static_cast<std::streamoff>(h.dataOffset), std::ios::beg);
    in.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(totalFloats * sizeof(float)));
    if (!in) { err = "Unexpected end of splat PLY data."; return false; }

    const std::size_t n = h.vertexCount;
    out.clear();
    out.shDegree = shDegree;
    out.positions.resize(n);
    out.scales.resize(n);
    out.rotations.resize(n);
    out.alphas.resize(n);
    out.colorsDC.resize(n);
    out.sh.resize(n * static_cast<std::size_t>(shDim) * 3);

    for (std::size_t i = 0; i < n; ++i)
    {
        const float* r = &data[i * floatsPerVertex];

        // 3DGS training-output PLY is authored Y-down / Z-forward, which is
        // flipped on Y and Z relative to the Y-up world the viewer renders (and
        // relative to SPZ, which the reference already stores in RDF). Negate Y
        // and Z on the position, and flip the matching quaternion components so
        // the covariance orientation follows -- since the covariance is rebuilt
        // from scale+rotation on upload, flipping the quaternion is sufficient
        // and the scales are untouched.
        //
        // Flipping axes {y,z} negates the quaternion's {y,z} components (a
        // reflection about the X axis, applied to the rotation). If the truck
        // ends up over-rotated instead of upright, switch to Y-only: negate
        // only position.y and quaternion.y (leave z as read).
        out.positions[i] = {r[posI[0]], -r[posI[1]], -r[posI[2]]};
        out.scales[i]    = {r[sclI[0]], r[sclI[1]], r[sclI[2]]};
        out.rotations[i] = glm::quat(r[rotI[3]], r[rotI[0]], -r[rotI[1]], -r[rotI[2]]); // (w, x, -y, -z)
        out.alphas[i]    = r[alpI];
        out.colorsDC[i]  = {r[dcI[0]], r[dcI[1]], r[dcI[2]]};

        // Transpose channel-major [C,S] -> coeff-major interleaved [S,C=rgb].
        float* dst = &out.sh[i * static_cast<std::size_t>(shDim) * 3];
        for (int c = 0; c < shDim; ++c)
        {
            dst[c * 3 + 0] = r[restI[c]];
            dst[c * 3 + 1] = r[restI[c + shDim]];
            dst[c * 3 + 2] = r[restI[c + 2 * shDim]];
        }
    }

    out.sourcePath   = pathToUtf8(path);
    out.importerName = "splat-ply";
    out.updateBounds();
    return true;
}

// ---------------------------------------------------------------------------
// SPZ (versions 1 and 2)
// ---------------------------------------------------------------------------

constexpr uint32_t kNgspMagic = 0x5053474eu;   // "NGSP" little-endian

bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err)
{
    z_stream zs{};
    // 15 window bits + 16 tells zlib to expect a gzip header.
    if (inflateInit2(&zs, 15 + 16) != Z_OK) { err = "zlib init failed."; return false; }

    zs.next_in  = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());

    std::vector<uint8_t> buf(1u << 16);
    out.clear();
    int ret = Z_OK;
    do
    {
        zs.next_out  = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END)
        {
            inflateEnd(&zs);
            err = "SPZ gzip decompression failed.";
            return false;
        }
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return true;
}

template <typename T>
bool readStream(const uint8_t*& p, const uint8_t* end, std::size_t bytes,
                std::vector<T>& dst, std::size_t elemCount, std::string& err)
{
    if (static_cast<std::size_t>(end - p) < bytes) { err = "SPZ stream truncated."; return false; }
    dst.resize(elemCount);
    std::memcpy(dst.data(), p, bytes);
    p += bytes;
    return true;
}

bool loadSpz(const fs::path& path, SplatCloud& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Cannot open file: " + pathToUtf8(path); return false; }
    std::vector<uint8_t> gz((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (gz.size() < 2 || gz[0] != 0x1f || gz[1] != 0x8b)
    {
        err = "Not a gzip-compressed SPZ file.";
        return false;
    }

    std::vector<uint8_t> data;
    if (!gunzip(gz, data, err)) return false;
    if (data.size() < 16) { err = "SPZ payload too small for header."; return false; }

    // 16-byte legacy header (v1/v2).
    uint32_t magic, version, numPoints;
    std::memcpy(&magic,     data.data() + 0, 4);
    std::memcpy(&version,   data.data() + 4, 4);
    std::memcpy(&numPoints, data.data() + 8, 4);
    const uint8_t shDegreeByte = data[12];
    const uint8_t fractionalBits = data[13];
    // data[14] = flags, data[15] = reserved

    if (magic != kNgspMagic) { err = "SPZ magic mismatch (not an NGSP stream)."; return false; }
    if (version == 0 || version > 4) { err = "Unsupported SPZ version: " + std::to_string(version); return false; }
    if (version >= 3)
    {
        err = "SPZ version " + std::to_string(version) +
              " uses a newer encoding (smallest-three quaternions / ZSTD "
              "multi-stream) not yet supported. Versions 1 and 2 are supported.";
        return false;
    }
    if (shDegreeByte > 4) { err = "SPZ has an unsupported SH degree."; return false; }

    const int      shDegree = shDegreeByte;
    const int      shDim    = splatShDim(shDegree);
    const std::size_t n     = numPoints;
    const bool usesFloat16  = (version == 1);   // v1 positions are float16

    // Non-interleaved streams, in file order:
    // positions | alphas | colors | scales | rotations | sh
    const uint8_t* p   = data.data() + 16;
    const uint8_t* end = data.data() + data.size();

    std::vector<uint8_t> posBytes, alphaBytes, colorBytes, scaleBytes, rotBytes, shBytes;
    const std::size_t posStride = usesFloat16 ? 6u : 9u;   // 3 * (2 or 3) bytes
    if (!readStream(p, end, n * posStride, posBytes, n * posStride, err)) return false;
    if (!readStream(p, end, n * 1,         alphaBytes, n * 1,        err)) return false;
    if (!readStream(p, end, n * 3,         colorBytes, n * 3,        err)) return false;
    if (!readStream(p, end, n * 3,         scaleBytes, n * 3,        err)) return false;
    if (!readStream(p, end, n * 3,         rotBytes,   n * 3,        err)) return false;   // v1/v2: 3 bytes
    const std::size_t shPer = static_cast<std::size_t>(shDim) * 3;
    if (!readStream(p, end, n * shPer,     shBytes,    n * shPer,    err)) return false;

    out.clear();
    out.shDegree = shDegree;
    out.positions.resize(n);
    out.scales.resize(n);
    out.rotations.resize(n);
    out.alphas.resize(n);
    out.colorsDC.resize(n);
    out.sh.resize(n * shPer);

    const float posScale = 1.0f / static_cast<float>(1 << fractionalBits);

    auto decodeHalf = [](uint16_t hbits) -> float {
        // Minimal IEEE half -> float; positions only, v1 (never publicly shipped).
        uint32_t sign = (hbits & 0x8000u) << 16;
        uint32_t exp  = (hbits >> 10) & 0x1f;
        uint32_t mant = hbits & 0x3ffu;
        uint32_t bits;
        if (exp == 0)
        {
            if (mant == 0) { bits = sign; }
            else
            {
                exp = 127 - 15 + 1;
                while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
                mant &= 0x3ffu;
                bits = sign | (exp << 23) | (mant << 13);
            }
        }
        else if (exp == 0x1f) { bits = sign | 0x7f800000u | (mant << 13); }
        else { bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13); }
        float f; std::memcpy(&f, &bits, 4); return f;
    };

    for (std::size_t i = 0; i < n; ++i)
    {
        // Position
        glm::vec3 pos;
        if (usesFloat16)
        {
            const uint8_t* b = &posBytes[i * 6];
            for (int k = 0; k < 3; ++k)
            {
                uint16_t hb = static_cast<uint16_t>(b[k * 2] | (b[k * 2 + 1] << 8));
                pos[k] = decodeHalf(hb);
            }
        }
        else
        {
            const uint8_t* b = &posBytes[i * 9];
            for (int k = 0; k < 3; ++k)
            {
                int32_t f = b[k * 3] | (b[k * 3 + 1] << 8) | (b[k * 3 + 2] << 16);
                if (f & 0x800000) f |= static_cast<int32_t>(0xff000000u); // sign-extend 24->32
                pos[k] = static_cast<float>(f) * posScale;
            }
        }
        out.positions[i] = pos;

        // Scale (log-space): byte/16 - 10
        const uint8_t* s = &scaleBytes[i * 3];
        out.scales[i] = {s[0] / 16.0f - 10.0f, s[1] / 16.0f - 10.0f, s[2] / 16.0f - 10.0f};

        // Rotation (first-three): xyz = byte/127.5 - 1, w reconstructed.
        const uint8_t* r = &rotBytes[i * 3];
        glm::vec3 xyz{r[0] / 127.5f - 1.0f, r[1] / 127.5f - 1.0f, r[2] / 127.5f - 1.0f};
        float w = std::sqrt(std::max(0.0f, 1.0f - (xyz.x * xyz.x + xyz.y * xyz.y + xyz.z * xyz.z)));
        out.rotations[i] = glm::quat(w, xyz.x, xyz.y, xyz.z);  // (w,x,y,z)

        // Alpha: stored as a sigmoid-ed byte; keep the logit to match convention.
        out.alphas[i] = invSigmoid(alphaBytes[i] / 255.0f);

        // Colour DC: ((byte/255) - 0.5) / 0.15
        const uint8_t* c = &colorBytes[i * 3];
        out.colorsDC[i] = {((c[0] / 255.0f) - 0.5f) / kColorScale,
                           ((c[1] / 255.0f) - 0.5f) / kColorScale,
                           ((c[2] / 255.0f) - 0.5f) / kColorScale};

        // SH: (byte - 128)/128, already in coeff-major interleaved order.
        float* dst = &out.sh[i * shPer];
        const uint8_t* sh = &shBytes[i * shPer];
        for (std::size_t k = 0; k < shPer; ++k)
            dst[k] = (static_cast<float>(sh[k]) - 128.0f) / 128.0f;
    }

    out.sourcePath   = pathToUtf8(path);
    out.importerName = "spz";
    out.updateBounds();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

const std::vector<std::string>& SplatLoader::importExtensions()
{
    static const std::vector<std::string> exts = {"ply", "spz"};
    return exts;
}

bool SplatLoader::canLoad(const fs::path& path)
{
    const std::string ext = extensionOf(path);
    if (ext == "spz") return true;
    if (ext != "ply") return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    PlyHeader h = parsePlyHeader(in);
    return h.ok && plyHasSplatProps(h);
}

bool SplatLoader::load(const fs::path& path, SplatCloud& outCloud, std::string& outError)
{
    const std::string ext = extensionOf(path);
    bool ok = false;
    if (ext == "spz")      ok = loadSpz(path, outCloud, outError);
    else if (ext == "ply") ok = loadPly(path, outCloud, outError);
    else { outError = "Unsupported splat extension: " + ext; return false; }

    if (ok)
        log::info("Imported splat ", pathToUtf8(path.filename()), ": ",
                  std::to_string(outCloud.count()), " gaussians, SH degree ",
                  std::to_string(outCloud.shDegree));
    return ok;
}

} // namespace mv
