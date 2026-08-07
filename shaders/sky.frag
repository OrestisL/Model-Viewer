#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

// Procedural gradient sky.
//
// Three bands -- ground, horizon, zenith -- blended by the view ray's
// elevation, with an optional sun disc taken from the first directional light
// so the sky and the lighting agree about where the sun is.
//
// Colours come from the globals block via skyColor(), shared with the ambient
// lighting in mesh.frag so the two cannot disagree.
//
//   pc.mrfs.w = draw sun disc (>0.5)

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 dir = normalize(vFarPoint - vNearPoint);

    vec3 color = skyColor(dir);

    // Sun disc from the first directional light.
    if (pc.mrfs.w > 0.5)
    {
        const int count = int(G.params.x);
        for (int i = 0; i < count && i < MV_MAX_LIGHTS; ++i)
        {
            if (int(G.lights[i].positionType.w) != MV_LIGHT_DIRECTIONAL) continue;

            // Lights point along their direction, so the sun sits opposite.
            const vec3  sunDir = normalize(-G.lights[i].directionRange.xyz);
            const float cosine = dot(dir, sunDir);

            const float disc  = smoothstep(0.9995, 0.9999, cosine);
            const float bloom = pow(clamp(cosine, 0.0, 1.0), 350.0) * 0.35;

            const vec3 sunColor = G.lights[i].colorIntensity.rgb;
            color += sunColor * (disc * 6.0 + bloom);
            break;
        }
    }

    outColor = vec4(color, 1.0);
}
