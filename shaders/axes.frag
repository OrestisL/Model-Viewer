#version 450

// The axis arrows are a navigation aid, not part of the scene, so they are
// deliberately unlit -- flat colour reads unambiguously from every angle.

layout(location = 0) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(vColor, 1.0);
}
