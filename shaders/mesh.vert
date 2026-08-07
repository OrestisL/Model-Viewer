#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3  inPos;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec4  inTangent;   // xyz = tangent, w = handedness
layout(location = 3) in vec2  inUV;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4  inWeights;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main()
{
    mat4 skin = mat4(1.0);

    int skinOffset = int(pc.mrfs.w);
    if (skinOffset >= 0)
    {
        skin =
            inWeights.x * bones.m[skinOffset + int(inJoints.x)] +
            inWeights.y * bones.m[skinOffset + int(inJoints.y)] +
            inWeights.z * bones.m[skinOffset + int(inJoints.z)] +
            inWeights.w * bones.m[skinOffset + int(inJoints.w)];

        // Guard against meshes whose weights do not sum to 1.
        float wsum = dot(inWeights, vec4(1.0));
        if (wsum < 1e-4) skin = mat4(1.0);
    }

    mat4 model  = pc.model * skin;
    vec4 world  = model * vec4(inPos, 1.0);

    mat3 normalMat = transpose(inverse(mat3(model)));

    vWorldPos = world.xyz;
    vNormal   = normalize(normalMat * inNormal);
    vTangent  = vec4(normalize(normalMat * inTangent.xyz), inTangent.w);
    vUV       = inUV;

    gl_Position = G.viewProj * world;
}
