#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// pc.mrfs.y = distance at which the grid has fully faded out

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vWorld;

layout(location = 0) out vec4 outColor;

void main()
{
    // Distance fade only. Smooth, monotonic, and independent of orientation
    // and sub-pixel position -- nothing here can change discontinuously from
    // one frame to the next.
    const float fade  = max(pc.mrfs.y, 1.0);
    const float dist  = length(vWorld - G.cameraPos.xyz);
    const float atten = 1.0 - smoothstep(fade * 0.55, fade, dist);

    const float alpha = vColor.a * atten;
    if (alpha <= 0.002) discard;

    outColor = vec4(vColor.rgb, alpha);
}
