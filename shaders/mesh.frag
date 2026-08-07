#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D uBaseColor;
layout(set = 1, binding = 1) uniform sampler2D uNormal;
layout(set = 1, binding = 2) uniform sampler2D uMetalRough;  // g = roughness, b = metallic
layout(set = 1, binding = 3) uniform sampler2D uEmissive;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float distributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float geometrySmith(float NdotV, float NdotL, float roughness)
{
    float k  = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 applyNormalMap(vec3 N, vec4 T)
{
    vec3 t = normalize(T.xyz - N * dot(N, T.xyz));
    if (any(isnan(t)) || length(T.xyz) < 1e-5)
        return N;
    vec3 b = cross(N, t) * (T.w < 0.0 ? -1.0 : 1.0);
    vec3 n = texture(uNormal, vUV).xyz * 2.0 - 1.0;
    return normalize(mat3(t, b, N) * n);
}

// Narkowicz ACES approximation.
vec3 tonemapACES(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    int flags = int(pc.mrfs.z);

    vec4 base = pc.baseColor;
    if ((flags & MV_FLAG_BASECOLOR_TEX) != 0)
        base *= texture(uBaseColor, vUV);

    if ((flags & MV_FLAG_MASKED) != 0 && base.a < pc.emissive.a)
        discard;

    float metallic  = pc.mrfs.x;
    float roughness = pc.mrfs.y;
    if ((flags & MV_FLAG_MR_TEX) != 0)
    {
        vec3 mr    = texture(uMetalRough, vUV).rgb;
        roughness *= mr.g;
        metallic  *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic, 0.0, 1.0);

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    if ((flags & MV_FLAG_NORMAL_TEX) != 0)
        N = applyNormalMap(N, vTangent);

    vec3 V = normalize(G.cameraPos.xyz - vWorldPos);
    float NdotV = max(dot(N, V), 1e-4);

    // -------- debug views -------------------------------------------------
    int debugMode = int(G.params.y);
    if (debugMode == 1) { outColor = vec4(base.rgb, 1.0);                     return; }
    if (debugMode == 2) { outColor = vec4(N * 0.5 + 0.5, 1.0);                return; }
    if (debugMode == 3) { outColor = vec4(fract(vUV), 0.0, 1.0);              return; }
    if (debugMode == 4) { outColor = vec4(vec3(metallic), 1.0);               return; }
    if (debugMode == 5) { outColor = vec4(vec3(roughness), 1.0);              return; }

    if ((flags & MV_FLAG_UNLIT) != 0) { outColor = vec4(base.rgb, base.a);    return; }

    vec3 F0      = mix(vec3(0.04), base.rgb, metallic);
    vec3 diffuse = base.rgb * (1.0 - metallic);
    vec3 Lo      = vec3(0.0);

    int lightCount = min(int(G.params.x), MV_MAX_LIGHTS);
    for (int i = 0; i < lightCount; ++i)
    {
        Light li   = G.lights[i];
        int   type = int(li.positionType.w);

        vec3  L;
        float attenuation = 1.0;

        if (type == MV_LIGHT_DIRECTIONAL)
        {
            L = normalize(-li.directionRange.xyz);
        }
        else
        {
            vec3  toLight = li.positionType.xyz - vWorldPos;
            float dist    = length(toLight);
            L = toLight / max(dist, 1e-5);

            attenuation = 1.0 / max(dist * dist, 1e-4);

            float range = li.directionRange.w;
            if (range > 0.0)
            {
                float f = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
                attenuation *= f * f;
            }

            if (type == MV_LIGHT_SPOT)
            {
                float cd    = dot(normalize(-li.directionRange.xyz), L);
                float t     = clamp((cd - li.cone.y) / max(li.cone.x - li.cone.y, 1e-4), 0.0, 1.0);
                attenuation *= t * t;
            }
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || attenuation <= 0.0) continue;

        vec3  H     = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D = distributionGGX(NdotH, roughness * roughness);
        float Gt = geometrySmith(NdotV, NdotL, roughness);
        vec3  F = fresnelSchlick(VdotH, F0);

        vec3 spec = (D * Gt * F) / max(4.0 * NdotV * NdotL, 1e-5);
        vec3 kD   = (vec3(1.0) - F);

        vec3 radiance = li.colorIntensity.rgb * li.colorIntensity.a * attenuation;
        Lo += (kD * diffuse / PI + spec) * radiance * NdotL;
    }

    // Cheap hemispheric ambient so unlit sides never go fully black.
    float hemi    = N.y * 0.5 + 0.5;
    vec3  ambient = G.ambient.rgb * G.ambient.a * mix(0.4, 1.0, hemi) * diffuse;

    vec3 emissive = pc.emissive.rgb;
    if ((flags & MV_FLAG_EMISSIVE_TEX) != 0)
        emissive *= texture(uEmissive, vUV).rgb;

    vec3 color = Lo + ambient + emissive;
    color = tonemapACES(color * G.params.z);

    outColor = vec4(color, base.a);
}
