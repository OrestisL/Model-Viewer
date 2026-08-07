#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// Fullscreen triangle. The view ray is reconstructed here rather than in the
// fragment shader so the matrix inverse runs three times per frame instead of
// once per pixel.

layout(location = 0) out vec3 vNearPoint;
layout(location = 1) out vec3 vFarPoint;

const vec2 kTriangle[3] = vec2[3](
    vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

vec3 unproject(vec2 ndc, float depth, mat4 invViewProj)
{
    vec4 p = invViewProj * vec4(ndc, depth, 1.0);
    return p.xyz / p.w;
}

void main()
{
    const vec2 ndc = kTriangle[gl_VertexIndex];
    const mat4 invViewProj = inverse(G.viewProj);

    vNearPoint = unproject(ndc, 0.0, invViewProj);
    vFarPoint  = unproject(ndc, 1.0, invViewProj);

    gl_Position = vec4(ndc, 0.0, 1.0);
}
