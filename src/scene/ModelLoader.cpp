#include "scene/ModelLoader.hpp"

#include <algorithm>
#include <utility>
#include <string>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include <cmath>

#include <assimp/config.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <stb_image.h>

#include "core/Log.hpp"
#include "scene/UsdBackend.hpp"

namespace fs = std::filesystem;

namespace mv {
namespace {

// ---------------------------------------------------------------------------
// Assimp <-> glm helpers
// ---------------------------------------------------------------------------

glm::mat4 toGlm(const aiMatrix4x4& m)
{
    // aiMatrix4x4 is row-major, glm::mat4 is column-major.
    return glm::mat4{
        m.a1, m.b1, m.c1, m.d1,
        m.a2, m.b2, m.c2, m.d2,
        m.a3, m.b3, m.c3, m.d3,
        m.a4, m.b4, m.c4, m.d4};
}

glm::vec3 toGlm(const aiVector3D& v) { return {v.x, v.y, v.z}; }
glm::vec3 toGlm(const aiColor3D& c) { return {c.r, c.g, c.b}; }
glm::quat toGlm(const aiQuaternion& q) { return {q.w, q.x, q.y, q.z}; }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Whether to flip the V coordinate after import.
///
/// The question is not what the file format uses, but what Assimp hands over.
/// Assimp's convention is a bottom-left origin, and its importers convert into
/// it -- the glTF2 importer does `values[i].y = 1 - values[i].y` on every
/// texture coordinate, precisely because glTF's origin is top-left.
///
/// Our images are uploaded top-down, so a Vulkan sampler reads v = 0 as the
/// top row. That means the flip has to be undone for *every* format, glTF
/// included. Reasoning about the file format instead of the importer output is
/// what made this wrong: the earlier rule skipped the flip for glTF on the
/// grounds that glTF is already top-down, without accounting for Assimp having
/// already inverted it.
bool shouldFlipV(const ImportOptions& options, const fs::path& path)
{
    (void)path;
    switch (options.flipV)
    {
        case ImportOptions::FlipV::Always: return true;
        case ImportOptions::FlipV::Never:  return false;
        case ImportOptions::FlipV::Auto:   break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Keeps the Assimp importer (and therefore the aiScene) alive for re-export.
// ---------------------------------------------------------------------------

struct AssimpAsset final : SourceAsset
{
    Assimp::Importer importer;
    const aiScene*   scene = nullptr;
};

// ---------------------------------------------------------------------------
// Texture loading
// ---------------------------------------------------------------------------

class TextureCache
{
public:
    TextureCache(Scene& scene, const aiScene* ai, fs::path baseDir)
        : m_scene(scene), m_ai(ai), m_baseDir(std::move(baseDir)) {}

    /// Builds a copy of `colorIndex` with its alpha taken from `opacityIndex`.
    ///
    /// Returns a new texture rather than editing in place: the base colour may
    /// be shared with a material that has no opacity map, and giving that one
    /// a cut-out alpha would punch holes in it.
    int combineAlpha(int colorIndex, int opacityIndex)
    {
        if (colorIndex == kInvalidIndex || opacityIndex == kInvalidIndex)
            return colorIndex;

        const TextureData& color   = m_scene.textures[static_cast<size_t>(colorIndex)];
        const TextureData& opacity = m_scene.textures[static_cast<size_t>(opacityIndex)];

        if (color.width != opacity.width || color.height != opacity.height)
        {
            log::warn("Opacity map ", opacity.name, " is ", opacity.width, "x", opacity.height,
                      " but the base colour is ", color.width, "x", color.height,
                      "; cannot combine them");
            return colorIndex;
        }

        TextureData merged = color;
        merged.name        = color.name + "+alpha";

        // Greyscale maps put the same value in every channel; red is as good
        // as any and is what an 8-bit single-channel map decodes into.
        for (size_t i = 0; i + 3 < merged.pixels.size(); i += 4)
            merged.pixels[i + 3] = opacity.pixels[i];

        merged.hasAlpha = scanForAlpha(merged.pixels);

        m_scene.textures.push_back(std::move(merged));
        return static_cast<int>(m_scene.textures.size()) - 1;
    }

    int acquire(const aiMaterial* material, aiTextureType type, bool srgb)
    {
        if (material->GetTextureCount(type) == 0)
            return kInvalidIndex;

        aiString relative;
        if (material->GetTexture(type, 0, &relative) != AI_SUCCESS)
            return kInvalidIndex;

        // The colour space is part of the identity, not just the path.
        //
        // One image is routinely referenced by two slots -- an ORM map used for
        // both occlusion and metal-roughness, or an image serving as base
        // colour in one material and as a mask in another. Base colour is
        // decoded as sRGB and data maps as linear. Keying on the path alone
        // hands whichever was requested first to both, so one of them is
        // decoded in the wrong colour space. In a GLB every embedded texture
        // is named "*0", "*1", ... so these collisions are the norm rather
        // than the exception.
        const std::string key = std::string(relative.C_Str()) + (srgb ? "|srgb" : "|linear");
        if (relative.length == 0) return kInvalidIndex;

        if (auto it = m_cache.find(key); it != m_cache.end())
            return it->second;

        // The path, not the cache key: the key carries a colour-space suffix
        // that is meaningless to the filesystem.
        const std::string path = relative.C_Str();

        int index = kInvalidIndex;
        if (const aiTexture* embedded = m_ai->GetEmbeddedTexture(relative.C_Str()))
            index = loadEmbedded(*embedded, path, srgb);
        else
            index = loadFromDisk(path, srgb);

        m_cache.emplace(key, index);
        return index;
    }

private:
    int loadEmbedded(const aiTexture& tex, const std::string& name, bool srgb)
    {
        if (tex.mHeight == 0)
        {
            // Compressed payload (png/jpg/...) of mWidth bytes.
            return decode(reinterpret_cast<const uint8_t*>(tex.pcData), tex.mWidth, name, srgb);
        }

        // Uncompressed ARGB8888 as aiTexel{b,g,r,a}.
        TextureData data;
        data.name   = name;
        data.width  = tex.mWidth;
        data.height = tex.mHeight;
        data.srgb   = srgb;
        data.pixels.resize(static_cast<size_t>(tex.mWidth) * tex.mHeight * 4);

        for (size_t i = 0; i < data.pixels.size() / 4; ++i)
        {
            data.pixels[i * 4 + 0] = tex.pcData[i].r;
            data.pixels[i * 4 + 1] = tex.pcData[i].g;
            data.pixels[i * 4 + 2] = tex.pcData[i].b;
            data.pixels[i * 4 + 3] = tex.pcData[i].a;
        }

        data.hasAlpha = scanForAlpha(data.pixels);

        m_scene.textures.push_back(std::move(data));
        return static_cast<int>(m_scene.textures.size()) - 1;
    }

    int loadFromDisk(const std::string& relative, bool srgb)
    {
        const fs::path resolved = resolve(relative);
        if (resolved.empty())
        {
            log::warn("Texture not found: ", relative);
            return kInvalidIndex;
        }

        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(resolved.string().c_str(), &w, &h, &channels, 4);
        if (!pixels)
        {
            log::warn("Failed to decode texture ", resolved.string(), ": ", stbi_failure_reason());
            return kInvalidIndex;
        }

        TextureData data;
        data.name   = resolved.filename().string();
        data.width  = static_cast<uint32_t>(w);
        data.height = static_cast<uint32_t>(h);
        data.srgb   = srgb;
        data.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        data.hasAlpha = scanForAlpha(data.pixels);
        stbi_image_free(pixels);

        m_scene.textures.push_back(std::move(data));
        return static_cast<int>(m_scene.textures.size()) - 1;
    }

    /// True when any pixel is meaningfully transparent.
    ///
    /// A tolerance rather than != 255: 8-bit authoring and JPEG-ish round
    /// trips leave stray 253s in images that are conceptually opaque, and
    /// treating those as cut-outs would push everything into the blended pass.
    static bool scanForAlpha(const std::vector<uint8_t>& rgba)
    {
        for (size_t i = 3; i < rgba.size(); i += 4)
            if (rgba[i] < 250) return true;
        return false;
    }

    int decode(const uint8_t* bytes, size_t size, const std::string& name, bool srgb)
    {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &channels, 4);
        if (!pixels)
        {
            log::warn("Failed to decode embedded texture ", name, ": ", stbi_failure_reason());
            return kInvalidIndex;
        }

        TextureData data;
        data.name   = name;
        data.width  = static_cast<uint32_t>(w);
        data.height = static_cast<uint32_t>(h);
        data.srgb   = srgb;
        data.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        data.hasAlpha = scanForAlpha(data.pixels);
        stbi_image_free(pixels);

        m_scene.textures.push_back(std::move(data));
        return static_cast<int>(m_scene.textures.size()) - 1;
    }

    /// Exporters write all kinds of paths; try the usual suspects.
    fs::path resolve(const std::string& relative) const
    {
        std::string cleaned = relative;
        std::replace(cleaned.begin(), cleaned.end(), '\\', '/');

        const fs::path asGiven{cleaned};
        std::vector<fs::path> candidates{
            m_baseDir / asGiven,
            asGiven,
            m_baseDir / asGiven.filename(),
            m_baseDir / "textures" / asGiven.filename(),
            m_baseDir / "Textures" / asGiven.filename(),
            m_baseDir / "maps" / asGiven.filename(),
            m_baseDir.parent_path() / "textures" / asGiven.filename()};

        std::error_code ec;
        for (const fs::path& candidate : candidates)
            if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec))
                return candidate;

        return {};
    }

    Scene&                               m_scene;
    const aiScene*                       m_ai;
    fs::path                             m_baseDir;
    std::unordered_map<std::string, int> m_cache;
};

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

void convertMaterials(const aiScene* ai, Scene& scene, TextureCache& textures)
{
    scene.materials.reserve(ai->mNumMaterials);

    for (unsigned int i = 0; i < ai->mNumMaterials; ++i)
    {
        const aiMaterial* src = ai->mMaterials[i];
        Material          dst;

        aiString name;
        if (src->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0)
            dst.name = name.C_Str();

        aiColor4D baseColor;
        if (src->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS)
            dst.baseColorFactor = {baseColor.r, baseColor.g, baseColor.b, baseColor.a};
        else if (aiColor3D diffuse; src->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS)
            dst.baseColorFactor = glm::vec4(toGlm(diffuse), 1.0f);

        if (float opacity = 1.0f; src->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
            dst.baseColorFactor.a *= opacity;

        if (float metallic = 0.0f; src->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS)
            dst.metallicFactor = metallic;

        if (float roughness = 0.0f; src->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS)
            dst.roughnessFactor = roughness;
        else if (float shininess = 0.0f; src->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f)
            dst.roughnessFactor = glm::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.04f, 1.0f);

        if (aiColor3D emissive; src->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
            dst.emissiveFactor = toGlm(emissive);
        if (float strength = 1.0f; src->Get(AI_MATKEY_EMISSIVE_INTENSITY, strength) == AI_SUCCESS)
            dst.emissiveFactor *= strength;

        if (int twoSided = 0; src->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS)
            dst.doubleSided = twoSided != 0;

        if (int unlit = 0; src->Get("$mat.gltf.unlit", 0, 0, unlit) == AI_SUCCESS)
            dst.unlit = unlit != 0;

        bool alphaModeDeclared = false;
        if (aiString mode; src->Get("$mat.gltf.alphaMode", 0, 0, mode) == AI_SUCCESS)
        {
            const std::string m = mode.C_Str();
            if (m == "MASK")       { dst.alphaMode = AlphaMode::Mask;  alphaModeDeclared = true; }
            else if (m == "BLEND") { dst.alphaMode = AlphaMode::Blend; alphaModeDeclared = true; }
            else if (m == "OPAQUE") alphaModeDeclared = true;
        }
        else if (dst.baseColorFactor.a < 0.999f)
        {
            dst.alphaMode      = AlphaMode::Blend;
            alphaModeDeclared  = true;
        }

        if (float cutoff = 0.5f; src->Get("$mat.gltf.alphaCutoff", 0, 0, cutoff) == AI_SUCCESS)
            dst.alphaCutoff = cutoff;

        dst.baseColorTexture = textures.acquire(src, aiTextureType_BASE_COLOR, true);
        if (dst.baseColorTexture == kInvalidIndex)
            dst.baseColorTexture = textures.acquire(src, aiTextureType_DIFFUSE, true);

        dst.normalTexture = textures.acquire(src, aiTextureType_NORMALS, false);
        if (dst.normalTexture == kInvalidIndex)
            dst.normalTexture = textures.acquire(src, aiTextureType_HEIGHT, false);

        dst.metalRoughTexture = textures.acquire(src, aiTextureType_METALNESS, false);
        if (dst.metalRoughTexture == kInvalidIndex)
            dst.metalRoughTexture = textures.acquire(src, aiTextureType_DIFFUSE_ROUGHNESS, false);
        if (dst.metalRoughTexture == kInvalidIndex)
            dst.metalRoughTexture = textures.acquire(src, aiTextureType_UNKNOWN, false);

        dst.emissiveTexture = textures.acquire(src, aiTextureType_EMISSIVE, true);

        // FBX and OBJ keep transparency in a separate opacity map rather than
        // in the base colour's alpha channel, which is how Mixamo and most DCC
        // exporters write hair and eyelashes. Fold it in so there is something
        // for the mask to test against.
        const int opacityTexture = textures.acquire(src, aiTextureType_OPACITY, false);
        if (opacityTexture != kInvalidIndex)
        {
            const int merged = textures.combineAlpha(dst.baseColorTexture, opacityTexture);
            if (merged != dst.baseColorTexture)
            {
                dst.baseColorTexture = merged;
                log::info("Material '", dst.name, "': folded opacity map into base colour alpha");
            }
        }

        // What Assimp actually offers for this material, so a missing alpha
        // source is visible rather than guessed at.
        {
            std::string available;
            const std::pair<aiTextureType, const char*> kTypes[] = {
                {aiTextureType_BASE_COLOR, "baseColor"}, {aiTextureType_DIFFUSE, "diffuse"},
                {aiTextureType_NORMALS, "normals"},      {aiTextureType_METALNESS, "metalness"},
                {aiTextureType_DIFFUSE_ROUGHNESS, "roughness"}, {aiTextureType_OPACITY, "opacity"},
                {aiTextureType_EMISSIVE, "emissive"},    {aiTextureType_LIGHTMAP, "lightmap"},
                {aiTextureType_SPECULAR, "specular"},    {aiTextureType_UNKNOWN, "unknown"},
            };
            for (const auto& [type, label] : kTypes)
                if (src->GetTextureCount(type) > 0)
                    available += std::string(available.empty() ? "" : ", ") + label +
                                 "x" + std::to_string(src->GetTextureCount(type));

            const bool baseHasAlpha =
                dst.baseColorTexture != kInvalidIndex &&
                scene.textures[static_cast<size_t>(dst.baseColorTexture)].hasAlpha;

            log::info("Material '", dst.name, "': assimp textures [", available,
                      "]  baseColourHasAlpha=", baseHasAlpha ? "yes" : "no");
        }

        // Only glTF carries an alpha mode. Every other format leaves it to the
        // renderer to notice, and the signal authors rely on is an alpha
        // channel in the base colour texture -- which is how hair cards and
        // eyelashes are made. Without this they render as opaque quads with
        // visible rectangular edges.
        if (!alphaModeDeclared && dst.baseColorTexture != kInvalidIndex &&
            static_cast<size_t>(dst.baseColorTexture) < scene.textures.size() &&
            scene.textures[static_cast<size_t>(dst.baseColorTexture)].hasAlpha)
        {
            // Mask rather than Blend: cut-outs need no sorting and do not
            // interact badly with the depth buffer, and foliage, hair and
            // lashes are overwhelmingly cut-outs rather than true glass.
            dst.alphaMode = AlphaMode::Mask;
            log::info("Material '", dst.name,
                      "': base colour texture has alpha and the format declares no "
                      "alpha mode; treating as MASK");
        }

        // Which image landed in which slot, so a mix-up is visible in the log
        // rather than only in the render.
        log::info("Material '", dst.name, "': base=", dst.baseColorTexture,
                  " normal=", dst.normalTexture,
                  " metalRough=", dst.metalRoughTexture,
                  " emissive=", dst.emissiveTexture,
                  " alpha=", dst.alphaMode == AlphaMode::Opaque ? "opaque"
                           : dst.alphaMode == AlphaMode::Mask   ? "mask" : "blend");

        // A texture-less material with an untouched base colour is what the
        // colour wheel in the UI drives; flag it by leaving the factor white.
        scene.materials.push_back(std::move(dst));
    }
}

void convertMeshes(const aiScene* ai, Scene& scene, bool flipV)
{
    size_t totalVertices = 0, totalIndices = 0;
    for (unsigned int i = 0; i < ai->mNumMeshes; ++i)
    {
        totalVertices += ai->mMeshes[i]->mNumVertices;
        totalIndices  += static_cast<size_t>(ai->mMeshes[i]->mNumFaces) * 3;
    }
    scene.vertices.reserve(totalVertices);
    scene.indices.reserve(totalIndices);
    scene.meshes.reserve(ai->mNumMeshes);

    for (unsigned int m = 0; m < ai->mNumMeshes; ++m)
    {
        const aiMesh* src = ai->mMeshes[m];

        Mesh mesh;
        mesh.name         = src->mName.C_Str();
        mesh.material     = static_cast<int>(src->mMaterialIndex);
        mesh.vertexOffset = static_cast<int32_t>(scene.vertices.size());
        mesh.firstIndex   = static_cast<uint32_t>(scene.indices.size());
        mesh.vertexCount  = src->mNumVertices;

        // Which UV set to read.
        //
        // A mesh may carry several, and glTF lets every texture name the one it
        // wants (TEXCOORD_0, TEXCOORD_1, ...). Assimp exposes that per texture
        // as UVWSRC. Always reading channel 0 is right for single-set models
        // and silently wrong for the rest -- the texture lands on geometry
        // unwrapped for something else, which looks like scrambled UVs rather
        // than like a missing setting.
        unsigned int uvChannel = 0;
        if (src->mMaterialIndex < ai->mNumMaterials)
        {
            const aiMaterial* material = ai->mMaterials[src->mMaterialIndex];
            unsigned int      wanted   = 0;

            // Base colour first, then legacy diffuse: whichever names a set,
            // that is the one the visible texture is authored against.
            if (material->Get(AI_MATKEY_UVWSRC(aiTextureType_BASE_COLOR, 0), wanted) == AI_SUCCESS ||
                material->Get(AI_MATKEY_UVWSRC(aiTextureType_DIFFUSE, 0), wanted) == AI_SUCCESS)
            {
                uvChannel = wanted;
            }
        }

        // Fall back rather than read past the end if the material names a set
        // the mesh does not actually have.
        if (!src->HasTextureCoords(uvChannel))
        {
            if (uvChannel != 0)
                log::warn("Mesh '", mesh.name, "' has no UV set ", uvChannel,
                          "; using set 0");
            uvChannel = 0;
        }

        unsigned int uvSetCount = 0;
        while (uvSetCount < AI_MAX_NUMBER_OF_TEXTURECOORDS &&
               src->HasTextureCoords(uvSetCount))
            ++uvSetCount;

        if (uvSetCount > 1)
            log::info("Mesh '", mesh.name, "' has ", uvSetCount,
                      " UV sets; using set ", uvChannel);

        // UV bounds, so what Assimp hands us can be compared against what the
        // file declares. If these disagree, the coordinates are being altered
        // in import rather than being wrong on disk.
        if (src->HasTextureCoords(uvChannel) && src->mNumVertices > 0)
        {
            float minU =  1e30f, minV =  1e30f;
            float maxU = -1e30f, maxV = -1e30f;
            for (unsigned int v = 0; v < src->mNumVertices; ++v)
            {
                const aiVector3D& uv = src->mTextureCoords[uvChannel][v];
                minU = std::min(minU, uv.x); maxU = std::max(maxU, uv.x);
                minV = std::min(minV, uv.y); maxV = std::max(maxV, uv.y);
            }
            log::info("Mesh '", mesh.name, "' UV range: u[", minU, ", ", maxU,
                      "] v[", minV, ", ", maxV, "]  verts=", src->mNumVertices);
        }

        const bool hasUV      = src->HasTextureCoords(uvChannel);
        const bool hasNormals = src->HasNormals();
        const bool hasTangent = src->HasTangentsAndBitangents();

        for (unsigned int v = 0; v < src->mNumVertices; ++v)
        {
            Vertex vertex;
            vertex.position = toGlm(src->mVertices[v]);
            if (hasNormals) vertex.normal = toGlm(src->mNormals[v]);

            if (hasTangent)
            {
                const glm::vec3 t = toGlm(src->mTangents[v]);
                const glm::vec3 b = toGlm(src->mBitangents[v]);
                const float sign  = (glm::dot(glm::cross(vertex.normal, t), b) < 0.0f) ? -1.0f : 1.0f;
                vertex.tangent    = glm::vec4(t, sign);
            }

            if (hasUV)
            {
                vertex.uv = {src->mTextureCoords[uvChannel][v].x,
                             flipV ? 1.0f - src->mTextureCoords[uvChannel][v].y
                                   : src->mTextureCoords[uvChannel][v].y};
            }

            mesh.bounds.expand(vertex.position);
            scene.vertices.push_back(vertex);
        }

        for (unsigned int f = 0; f < src->mNumFaces; ++f)
        {
            const aiFace& face = src->mFaces[f];
            if (face.mNumIndices != 3) continue;   // aiProcess_SortByPType keeps only triangles
            scene.indices.push_back(face.mIndices[0]);
            scene.indices.push_back(face.mIndices[1]);
            scene.indices.push_back(face.mIndices[2]);
        }

        mesh.indexCount = static_cast<uint32_t>(scene.indices.size()) - mesh.firstIndex;
        scene.meshes.push_back(std::move(mesh));
    }
}

std::unordered_map<std::string, int> convertNodes(const aiScene* ai, Scene& scene)
{
    std::unordered_map<std::string, int> nameToNode;

    struct Pending { const aiNode* node; int parent; };
    std::vector<Pending> stack{{ai->mRootNode, kInvalidIndex}};

    while (!stack.empty())
    {
        const Pending pending = stack.back();
        stack.pop_back();

        const int index = static_cast<int>(scene.nodes.size());
        scene.nodes.emplace_back();

        Node& node  = scene.nodes.back();
        node.name   = pending.node->mName.C_Str();
        node.parent = pending.parent;

        aiVector3D   position, scaling;
        aiQuaternion rotation;
        pending.node->mTransformation.Decompose(scaling, rotation, position);
        node.translation = toGlm(position);
        node.rotation    = toGlm(rotation);
        node.scale       = toGlm(scaling);

        node.meshes.assign(pending.node->mMeshes,
                           pending.node->mMeshes + pending.node->mNumMeshes);

        if (pending.parent == kInvalidIndex)
            scene.roots.push_back(index);
        else
            scene.nodes[static_cast<size_t>(pending.parent)].children.push_back(index);

        if (!node.name.empty())
            nameToNode.emplace(node.name, index);

        for (unsigned int c = 0; c < pending.node->mNumChildren; ++c)
            stack.push_back({pending.node->mChildren[c], index});
    }

    // Re-link children in source order (the stack above reverses them).
    for (Node& node : scene.nodes)
        std::sort(node.children.begin(), node.children.end());

    return nameToNode;
}

void convertSkins(const aiScene*                              ai,
                  Scene&                                      scene,
                  const std::unordered_map<std::string, int>& nameToNode)
{
    uint32_t boneCursor = 0;

    for (unsigned int m = 0; m < ai->mNumMeshes; ++m)
    {
        const aiMesh* src = ai->mMeshes[m];
        if (!src->HasBones()) continue;

        Skin skin;
        skin.name = std::string(src->mName.C_Str()) + "_skin";
        skin.joints.reserve(src->mNumBones);
        skin.inverseBind.reserve(src->mNumBones);
        skin.gpuOffset = boneCursor;

        Mesh& mesh = scene.meshes[m];

        // Track how many influences each vertex already has.
        std::vector<uint32_t> influenceCount(src->mNumVertices, 0);

        for (unsigned int b = 0; b < src->mNumBones; ++b)
        {
            const aiBone* bone = src->mBones[b];
            const auto    it   = nameToNode.find(bone->mName.C_Str());
            const int     node = (it != nameToNode.end()) ? it->second : kInvalidIndex;

            if (node == kInvalidIndex)
                log::warn("Bone '", bone->mName.C_Str(), "' has no matching node; it will not animate");

            const uint32_t jointIndex = static_cast<uint32_t>(skin.joints.size());
            skin.joints.push_back(node);
            skin.inverseBind.push_back(toGlm(bone->mOffsetMatrix));

            for (unsigned int w = 0; w < bone->mNumWeights; ++w)
            {
                const aiVertexWeight& vw = bone->mWeights[w];
                if (vw.mVertexId >= src->mNumVertices || vw.mWeight <= 0.0f) continue;

                uint32_t& slot = influenceCount[vw.mVertexId];
                if (slot >= kMaxBoneInfluences) continue;   // aiProcess_LimitBoneWeights caps this

                Vertex& vertex = scene.vertices[static_cast<size_t>(mesh.vertexOffset) + vw.mVertexId];
                vertex.joints[slot]  = jointIndex;
                vertex.weights[slot] = vw.mWeight;
                ++slot;
            }
        }

        // Renormalise in case the source data does not sum to one.
        for (uint32_t v = 0; v < src->mNumVertices; ++v)
        {
            Vertex& vertex = scene.vertices[static_cast<size_t>(mesh.vertexOffset) + v];
            const float sum = vertex.weights.x + vertex.weights.y + vertex.weights.z + vertex.weights.w;
            if (sum > 1e-5f) vertex.weights /= sum;
        }

        boneCursor += static_cast<uint32_t>(skin.joints.size());
        mesh.skin = static_cast<int>(scene.skins.size());
        scene.skins.push_back(std::move(skin));
    }

    scene.totalBoneSlots = boneCursor;
}

void convertAnimations(const aiScene*                              ai,
                       Scene&                                      scene,
                       const std::unordered_map<std::string, int>& nameToNode)
{
    scene.animations.reserve(ai->mNumAnimations);

    for (unsigned int a = 0; a < ai->mNumAnimations; ++a)
    {
        const aiAnimation* src = ai->mAnimations[a];

        Animation animation;
        animation.name = (src->mName.length > 0) ? src->mName.C_Str()
                                                 : ("animation " + std::to_string(a));
        animation.ticksPerSecond = (src->mTicksPerSecond > 0.0)
                                       ? static_cast<float>(src->mTicksPerSecond)
                                       : 25.0f;
        animation.duration = static_cast<float>(src->mDuration) / animation.ticksPerSecond;
        animation.channels.reserve(src->mNumChannels);

        for (unsigned int c = 0; c < src->mNumChannels; ++c)
        {
            const aiNodeAnim* srcChannel = src->mChannels[c];
            const auto        it         = nameToNode.find(srcChannel->mNodeName.C_Str());
            if (it == nameToNode.end())
            {
                log::warn("Animation channel targets unknown node '",
                          srcChannel->mNodeName.C_Str(), "'");
                continue;
            }

            AnimationChannel channel;
            channel.node = it->second;

            const float tps = animation.ticksPerSecond;

            channel.positions.reserve(srcChannel->mNumPositionKeys);
            for (unsigned int k = 0; k < srcChannel->mNumPositionKeys; ++k)
                channel.positions.push_back({static_cast<float>(srcChannel->mPositionKeys[k].mTime) / tps,
                                             toGlm(srcChannel->mPositionKeys[k].mValue)});

            channel.rotations.reserve(srcChannel->mNumRotationKeys);
            for (unsigned int k = 0; k < srcChannel->mNumRotationKeys; ++k)
                channel.rotations.push_back({static_cast<float>(srcChannel->mRotationKeys[k].mTime) / tps,
                                             toGlm(srcChannel->mRotationKeys[k].mValue)});

            channel.scales.reserve(srcChannel->mNumScalingKeys);
            for (unsigned int k = 0; k < srcChannel->mNumScalingKeys; ++k)
                channel.scales.push_back({static_cast<float>(srcChannel->mScalingKeys[k].mTime) / tps,
                                          toGlm(srcChannel->mScalingKeys[k].mValue)});

            animation.channels.push_back(std::move(channel));
        }

        if (!animation.channels.empty())
            scene.animations.push_back(std::move(animation));
    }
}

void convertLights(const aiScene*                              ai,
                   Scene&                                      scene,
                   const std::unordered_map<std::string, int>& nameToNode)
{
    for (unsigned int i = 0; i < ai->mNumLights; ++i)
    {
        const aiLight* src = ai->mLights[i];

        Light light;
        light.name = src->mName.C_Str();

        switch (src->mType)
        {
            case aiLightSource_DIRECTIONAL: light.type = LightType::Directional; break;
            case aiLightSource_SPOT:        light.type = LightType::Spot;        break;
            case aiLightSource_POINT:       light.type = LightType::Point;       break;
            default:
                log::warn("Unsupported light type on '", light.name, "', importing as point");
                light.type = LightType::Point;
                break;
        }

        const auto it = nameToNode.find(light.name);
        light.node = (it != nameToNode.end()) ? it->second : kInvalidIndex;

        light.color     = toGlm(src->mColorDiffuse);
        light.intensity = glm::max(glm::max(light.color.r, light.color.g), light.color.b);
        if (light.intensity > 1e-4f) light.color /= light.intensity;
        else                         { light.color = glm::vec3(1.0f); light.intensity = 1.0f; }

        light.position  = toGlm(src->mPosition);
        light.direction = toGlm(src->mDirection);
        if (glm::length(light.direction) < 1e-5f)
            light.direction = glm::vec3(0.0f, -1.0f, 0.0f);

        light.innerCone = src->mAngleInnerCone;
        light.outerCone = glm::max(src->mAngleOuterCone, src->mAngleInnerCone + 1e-3f);

        // Assimp exposes attenuation coefficients; derive a usable range.
        if (src->mAttenuationQuadratic > 1e-6f)
            light.range = glm::sqrt(1.0f / (src->mAttenuationQuadratic * 0.01f));

        scene.lights.push_back(std::move(light));
    }
}

void convertCameras(const aiScene*                              ai,
                    Scene&                                      scene,
                    const std::unordered_map<std::string, int>& nameToNode)
{
    for (unsigned int i = 0; i < ai->mNumCameras; ++i)
    {
        const aiCamera* src = ai->mCameras[i];

        CameraDef camera;
        camera.name = src->mName.C_Str();

        const auto it = nameToNode.find(camera.name);
        camera.node = (it != nameToNode.end()) ? it->second : kInvalidIndex;

        camera.aspectRatio = src->mAspect;
        camera.znear       = glm::max(src->mClipPlaneNear, 1e-3f);
        camera.zfar        = glm::max(src->mClipPlaneFar, camera.znear * 10.0f);

        // mHorizontalFOV is the *half* horizontal angle.
        const float aspect = (src->mAspect > 1e-3f) ? src->mAspect : (16.0f / 9.0f);
        camera.yfov = 2.0f * std::atan(std::tan(src->mHorizontalFOV) / aspect);

        camera.position = toGlm(src->mPosition);
        camera.lookAt   = camera.position + toGlm(src->mLookAt);
        camera.up       = toGlm(src->mUp);

        scene.cameras.push_back(std::move(camera));
    }
}

unsigned int postProcessFlags(const ImportOptions& options)
{
    unsigned int flags =
        aiProcess_Triangulate |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_LimitBoneWeights |
        aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType |
        aiProcess_GenUVCoords |
        aiProcess_TransformUVCoords |
        aiProcess_FindDegenerates |
        aiProcess_PopulateArmatureData |
        aiProcess_ValidateDataStructure;

    if (options.generateNormals)   flags |= aiProcess_GenSmoothNormals;
    if (options.optimizeMeshes)    flags |= aiProcess_OptimizeMeshes;
    if (options.repairInvalidData) flags |= aiProcess_FindInvalidData;

    return flags;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace {

/// True when the linked Assimp advertises this file's extension. USD only
/// shows up here in builds configured with -DMV_ENABLE_USD=ON.
bool assimpSupports(const fs::path& path)
{
    std::string ext = path.extension().string();
    if (ext.empty()) return false;
    if (ext.front() == '.') ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    Assimp::Importer probe;
    return probe.IsExtensionSupported(ext);
}

} // namespace

bool ModelLoader::load(const fs::path&      path,
                       const ImportOptions& options,
                       Scene&               outScene,
                       std::string&         outError)
{
    std::error_code ec;
    if (!fs::exists(path, ec))
    {
        outError = "File does not exist: " + path.string();
        return false;
    }

    // Assimp claims USD when built with ASSIMP_BUILD_USD_IMPORTER, in which
    // case it is preferred: the file then travels the same path as every other
    // format. The standalone backend is only a fallback for builds without it.
    if (UsdBackend::handles(path) && !assimpSupports(path))
        return UsdBackend::load(path, outScene, outError);

    // Refuse unknown extensions before handing anything to the importer.
    //
    // Assimp will otherwise fall back to sniffing file contents and may pick a
    // loader that was never meant for the data. Deciding here, from the
    // extension, keeps a bad guess from ever reaching parser code -- and an
    // importer that crashes rather than returning an error cannot be recovered
    // from once it has been entered.
    if (!assimpSupports(path))
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".blend")
        {
            outError = "Blender .blend files are not supported. Assimp's reader "
                       "only handles the pre-2.8 format. Export glTF or FBX from "
                       "Blender instead.";
        }
        else
        {
            outError = "Unsupported file type: " +
                       (ext.empty() ? std::string("(no extension)") : ext);
        }
        return false;
    }

    auto asset = std::make_shared<AssimpAsset>();
    asset->importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS,
                                       static_cast<int>(kMaxBoneInfluences));
    asset->importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                                       aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    asset->importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    // Assimp swallows most import failures and reports them through
    // GetErrorString, but not all of them: some importers let exceptions
    // escape, and a corrupt file can provoke a bad_alloc from a bogus size
    // read out of the header. Losing the whole application because one file
    // was malformed is not acceptable in a viewer.
    try
    {
        asset->scene = asset->importer.ReadFile(path.string(), postProcessFlags(options));
    }
    catch (const std::exception& e)
    {
        outError = std::string("Importer threw: ") + e.what();
        return false;
    }
    catch (...)
    {
        outError = "Importer threw an unknown exception";
        return false;
    }

    if (!asset->scene || !asset->scene->mRootNode)
    {
        outError = asset->importer.GetErrorString();
        if (outError.empty()) outError = "Assimp returned no scene";

        // Give the specific advice rather than "unknown file format".
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".blend")
        {
            outError = "Blender .blend files are not supported. Assimp's reader "
                       "only handles the pre-2.8 format and is unreliable on "
                       "anything newer. Export glTF or FBX from Blender instead.";
        }
        return false;
    }

    if (asset->scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
        log::warn("Scene is flagged incomplete; some data may be missing");

    const aiScene* ai = asset->scene;

    Scene scene;
    scene.sourcePath   = path.string();
    scene.importerName = "assimp";
    scene.unitScale    = options.importScale;

    const bool flipV = shouldFlipV(options, path);

    TextureCache textures(scene, ai, path.parent_path());
    convertMaterials(ai, scene, textures);
    convertMeshes(ai, scene, flipV);

    const auto nameToNode = convertNodes(ai, scene);

    convertSkins(ai, scene, nameToNode);
    convertAnimations(ai, scene, nameToNode);
    convertLights(ai, scene, nameToNode);
    convertCameras(ai, scene, nameToNode);

    if (options.importScale != 1.0f)
    {
        for (int root : scene.roots)
        {
            Node& node = scene.nodes[static_cast<size_t>(root)];
            node.scale       *= options.importScale;
            node.translation *= options.importScale;
        }
    }

    scene.updateBounds();
    scene.source = std::move(asset);

    log::info("Imported ", path.filename().string(), ": ",
              scene.meshes.size(), " meshes, ",
              scene.vertices.size(), " vertices, ",
              scene.materials.size(), " materials, ",
              scene.textures.size(), " textures, ",
              scene.animations.size(), " animations, ",
              scene.lights.size(), " lights, ",
              scene.cameras.size(), " cameras");

    outScene = std::move(scene);
    return true;
}

const std::vector<ExportFormat>& ModelLoader::exportFormats()
{
    static const std::vector<ExportFormat> formats = [] {
        std::vector<ExportFormat> result;
        Assimp::Exporter          exporter;

        const size_t count = exporter.GetExportFormatCount();
        result.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            const aiExportFormatDesc* desc = exporter.GetExportFormatDescription(i);
            if (!desc) continue;
            result.push_back({desc->id, desc->description, desc->fileExtension});
        }

        std::sort(result.begin(), result.end(),
                  [](const ExportFormat& a, const ExportFormat& b) { return a.id < b.id; });
        return result;
    }();

    return formats;
}

bool ModelLoader::exportScene(const Scene&       scene,
                              const std::string& formatId,
                              const fs::path&    path,
                              std::string&       outError)
{
    const auto* asset = dynamic_cast<const AssimpAsset*>(scene.source.get());
    if (!asset || !asset->scene)
    {
        outError = "This scene was not imported through Assimp, so it cannot be re-exported. "
                   "(USD-imported scenes need a conversion pass that is not implemented yet.)";
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);

    Assimp::Exporter exporter;
    const aiReturn   result = exporter.Export(asset->scene, formatId, path.string(),
                                              aiProcess_ValidateDataStructure);

    if (result != AI_SUCCESS)
    {
        outError = exporter.GetErrorString();
        if (outError.empty()) outError = "Assimp export failed";
        return false;
    }

    log::info("Exported to ", path.string(), " (", formatId, ")");
    return true;
}

const std::vector<std::string>& ModelLoader::importExtensions()
{
    static const std::vector<std::string> extensions = [] {
        Assimp::Importer importer;
        aiString         raw;
        importer.GetExtensionList(raw);       // "*.obj;*.fbx;..."

        std::vector<std::string> result;
        std::string              token;
        for (const char* c = raw.C_Str(); ; ++c)
        {
            if (*c == ';' || *c == '\0')
            {
                if (token.size() > 2 && token.rfind("*.", 0) == 0)
                    result.push_back(lower(token.substr(2)));
                token.clear();
                if (*c == '\0') break;
            }
            else
            {
                token.push_back(*c);
            }
        }

        // Only advertise USD from the standalone backend if Assimp did not
        // already list it; otherwise it appears twice.
        for (const std::string& ext : UsdBackend::extensions())
            result.push_back(ext);

        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }();

    return extensions;
}

bool ModelLoader::canLoad(const fs::path& path)
{
    if (!path.has_extension()) return false;
    const std::string ext = lower(path.extension().string().substr(1));
    const auto&       all = importExtensions();
    return std::find(all.begin(), all.end(), ext) != all.end();
}

} // namespace mv
