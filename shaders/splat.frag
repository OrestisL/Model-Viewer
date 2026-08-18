#version 450

// Gaussian-splat fragment stage.
//
// Evaluates the 2D Gaussian at this fragment using the conic (inverse 2D
// covariance) and the pixel offset from the splat centre, then outputs a
// PREMULTIPLIED colour. The pipeline blends back-to-front with
// (ONE, ONE_MINUS_SRC_ALPHA), so the fragment must emit colour already scaled
// by alpha.

layout(location = 0) in vec3  vColor;
layout(location = 1) in float vOpacity;
layout(location = 2) in vec3  vConic;    // (a, b, c) = inverse 2D covariance
layout(location = 3) in vec2  vDelta;    // offset from centre, in pixels

layout(location = 0) out vec4 outColor;

void main()
{
    // power = -1/2 * d^T * conic * d
    float power = -0.5 * (vConic.x * vDelta.x * vDelta.x + vConic.z * vDelta.y * vDelta.y)
                  - vConic.y * vDelta.x * vDelta.y;

    if (power > 0.0) discard;   // outside the ellipse

    float alpha = min(0.99, vOpacity * exp(power));
    if (alpha < 1.0 / 255.0) discard;

    // Premultiplied output for back-to-front (ONE, ONE_MINUS_SRC_ALPHA) blending.
    outColor = vec4(vColor * alpha, alpha);
}
