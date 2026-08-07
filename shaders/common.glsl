// Shared declarations. Must stay in sync with src/vk/Renderer.hpp.
#ifndef MV_COMMON_GLSL
#define MV_COMMON_GLSL

#define MV_MAX_LIGHTS 16

// Material flag bits (packed into pc.mrfs.z)
#define MV_FLAG_BASECOLOR_TEX 1
#define MV_FLAG_NORMAL_TEX    2
#define MV_FLAG_MR_TEX        4
#define MV_FLAG_EMISSIVE_TEX  8
#define MV_FLAG_UNLIT         16
#define MV_FLAG_MASKED        32

// Light types
#define MV_LIGHT_DIRECTIONAL 0
#define MV_LIGHT_POINT       1
#define MV_LIGHT_SPOT        2

struct Light
{
    vec4 positionType;    // xyz = world position,  w = type
    vec4 directionRange;  // xyz = world direction, w = range (0 = infinite)
    vec4 colorIntensity;  // rgb = colour,          a = intensity
    vec4 cone;            // x = cos(inner), y = cos(outer), zw = unused
};

layout(set = 0, binding = 0) uniform GlobalsBlock
{
    mat4  view;
    mat4  proj;
    mat4  viewProj;
    vec4  cameraPos;      // xyz = eye,  w = unused
    vec4  ambient;        // rgb = colour, a = intensity
    vec4  params;         // x = lightCount, y = debugMode, z = exposure, w = time
    Light lights[MV_MAX_LIGHTS];
} G;

layout(std430, set = 0, binding = 1) readonly buffer BoneBlock
{
    mat4 m[];
} bones;

layout(push_constant) uniform PushBlock
{
    mat4 model;
    vec4 baseColor;
    vec4 mrfs;      // x = metallic, y = roughness, z = flags, w = skin offset (-1 = none)
    vec4 emissive;  // rgb = emissive colour, a = alpha cutoff
} pc;

#endif // MV_COMMON_GLSL
