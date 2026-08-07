#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// Real line geometry, not a screen-space pattern.
//
// The previous grid reconstructed a world position per pixel and derived
// coverage from screen-space derivatives. That is what made it flicker: the
// derivative estimate depends on line orientation and sub-pixel phase, so
// coverage moved around as the camera turned. Lines that actually exist as
// geometry are rasterised and antialiased by MSAA like anything else, with no
// coverage maths of our own to get wrong.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorld;

void main()
{
    vColor      = inColor;
    vWorld      = inPosition;
    gl_Position = G.viewProj * vec4(inPosition, 1.0);
}
