#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// Depth-only pass from the light's point of view.
//
// Skinning has to match mesh.vert exactly: a skinned mesh that moves in the
// main pass but not here would cast its bind-pose shadow.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;
layout(location = 4) in uvec4 inJoints;
layout(location = 5) in vec4 inWeights;

void main()
{
    vec4 local = vec4(inPosition, 1.0);

    const int skinOffset = int(pc.mrfs.w);
    if (skinOffset >= 0)
    {
        mat4 skin =
            inWeights.x * bones.m[skinOffset + int(inJoints.x)] +
            inWeights.y * bones.m[skinOffset + int(inJoints.y)] +
            inWeights.z * bones.m[skinOffset + int(inJoints.z)] +
            inWeights.w * bones.m[skinOffset + int(inJoints.w)];

        if (inWeights.x + inWeights.y + inWeights.z + inWeights.w > 0.0)
            local = skin * local;
    }

    gl_Position = G.lightViewProj * pc.model * local;
}
