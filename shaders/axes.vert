#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// Origin axis arrows, built procedurally so there is no vertex buffer to own.
// Each arrow is a cylindrical shaft, a conical head and a disc capping the
// base of the cone; all three come out of gl_VertexIndex alone.
//
//   pc.mrfs.x = arrow length, world units
//   pc.mrfs.y = shaft radius as a fraction of that length
//
// Vertex count must match kAxisVertexCount in src/vk/Renderer.cpp.

const int SEG      = 16;
const int SHAFT_V  = SEG * 6;   // two triangles per segment
const int CONE_V   = SEG * 3;
const int CAP_V    = SEG * 3;
const int PER_AXIS = SHAFT_V + CONE_V + CAP_V;

const float kHeadStart  = 0.78;  // where the cone begins, along the arrow
const float kHeadRadius = 2.6;   // cone radius as a multiple of the shaft's

layout(location = 0) out vec3 vColor;

// A point on the ring around the canonical +X axis.
vec3 ring(int i, float radius)
{
    float a = 6.28318530718 * float(i) / float(SEG);
    return vec3(0.0, cos(a) * radius, sin(a) * radius);
}

// Rotates the canonical +X arrow onto axis 0 (X), 1 (Y) or 2 (Z).
vec3 toAxis(int axis, vec3 p)
{
    if (axis == 0) return p.xyz;
    if (axis == 1) return p.yxz;
    return p.yzx;
}

void main()
{
    int axis  = gl_VertexIndex / PER_AXIS;
    int local = gl_VertexIndex % PER_AXIS;

    float len      = pc.mrfs.x;
    float shaftR   = pc.mrfs.y * len;
    float headR    = shaftR * kHeadRadius;
    float headBase = len * kHeadStart;

    vec3 baseC = vec3(headBase, 0.0, 0.0);
    vec3 p;

    if (local < SHAFT_V)
    {
        int seg    = local / 6;
        int corner = local % 6;

        vec3 a0 = ring(seg,     shaftR);
        vec3 a1 = ring(seg + 1, shaftR);
        vec3 b0 = baseC + a0;
        vec3 b1 = baseC + a1;

        vec3 quad[6] = vec3[6](a0, b0, b1, a0, b1, a1);
        p = quad[corner];
    }
    else if (local < SHAFT_V + CONE_V)
    {
        int t      = local - SHAFT_V;
        int seg    = t / 3;
        int corner = t % 3;

        vec3 tip = vec3(len, 0.0, 0.0);
        vec3 c0  = baseC + ring(seg,     headR);
        vec3 c1  = baseC + ring(seg + 1, headR);

        vec3 tri[3] = vec3[3](tip, c0, c1);
        p = tri[corner];
    }
    else
    {
        int t      = local - SHAFT_V - CONE_V;
        int seg    = t / 3;
        int corner = t % 3;

        vec3 c0 = baseC + ring(seg,     headR);
        vec3 c1 = baseC + ring(seg + 1, headR);

        vec3 tri[3] = vec3[3](baseC, c1, c0);
        p = tri[corner];
    }

    const vec3 kColors[3] = vec3[3](
        vec3(0.90, 0.22, 0.24),    // X -- red
        vec3(0.36, 0.78, 0.30),    // Y -- green
        vec3(0.26, 0.50, 0.95));   // Z -- blue

    vColor      = kColors[axis];
    gl_Position = G.viewProj * vec4(toAxis(axis, p), 1.0);
}
