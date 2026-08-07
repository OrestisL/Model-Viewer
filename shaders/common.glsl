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
    vec4  params;         // x = lightCount, y = debugMode, z = exposure,
                          // w = unlit mode (0 or 1)
    vec4  skyZenith;      // rgb = colour, a = intensity
    vec4  skyHorizon;     // rgb = colour, a = horizon tightness
    vec4  skyGround;      // rgb = colour, a = sky drives ambient (0 or 1)
    mat4  lightViewProj;  // world -> shadow map clip space
    vec4  shadowParams;   // x = caster light index + 1 (0 = shadows off),
                          // y = depth bias, z = normal bias, w = shadow texel size
    Light lights[MV_MAX_LIGHTS];
} G;

// The sky gradient, shared by the sky pass and by ambient lighting so the
// background and the shading can never disagree about what the sky looks like.
vec3 skyColor(vec3 dir)
{
    const float tightness = max(G.skyHorizon.a, 0.05);
    const float up   = pow(clamp( dir.y, 0.0, 1.0), tightness);
    const float down = pow(clamp(-dir.y, 0.0, 1.0), tightness);

    vec3 c = mix(G.skyHorizon.rgb, G.skyZenith.rgb, up);
    c      = mix(c, G.skyGround.rgb, down);
    return c * G.skyZenith.a;
}

// Depth-compare sampler: the hardware does the depth test and bilinear filters
// the *result*, so one fetch already gives 2x2 percentage-closer filtering.
layout(set = 0, binding = 2) uniform sampler2DShadow uShadowMap;

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

// Fraction of light reaching a point: 1 fully lit, 0 fully shadowed.
//
// The normal-offset shift moves the lookup along the surface normal rather than
// along the light, which handles curved and thin geometry better than a plain
// depth bias and is what stops the acne pattern on rounded surfaces.
float shadowFactor(vec3 worldPos, vec3 N, vec3 L)
{
    if (G.shadowParams.x < 0.5) return 1.0;

    const float texel      = G.shadowParams.w;
    const float normalBias = G.shadowParams.z;

    // Surfaces edge-on to the light need the largest offset.
    const float slope  = clamp(1.0 - dot(N, L), 0.0, 1.0);
    const vec3  offset = N * (normalBias * texel * (1.0 + slope * 2.0));

    vec4 lightClip = G.lightViewProj * vec4(worldPos + offset, 1.0);
    vec3 proj      = lightClip.xyz / lightClip.w;

    // Outside the map, or behind the light, means unshadowed rather than dark.
    if (proj.z > 1.0 || proj.z < 0.0) return 1.0;

    vec2 uv = proj.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;

    const float depth = proj.z - G.shadowParams.y;

    // 3x3 taps over a hardware-PCF sampler give an effective 6x6 kernel.
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(uShadowMap, vec3(uv + vec2(x, y) * texel, depth));

    return sum / 9.0;
}

#endif // MV_COMMON_GLSL
