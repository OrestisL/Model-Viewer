#version 450

// Gaussian-splat vertex stage.
//
// Draws one screen-aligned quad per splat (6 verts, no vertex buffer -- corners
// come from gl_VertexIndex). Each 3D Gaussian is projected to a 2D screen-space
// ellipse using the EWA-splatting approximation: the 3D covariance is pushed
// through the projection Jacobian to a 2D covariance, whose inverse (the
// "conic") the fragment stage uses to evaluate the Gaussian falloff.
//
// Per-splat data comes from an SSBO (set 1, binding 0); the per-frame sorted
// draw order comes from a second SSBO (set 1, binding 1); the higher-order
// spherical-harmonic coefficients come from a third (set 1, binding 2). The DC
// (degree-0) SH term is carried in GpuSplat.color; the view-dependent higher
// orders are evaluated here per splat against the view direction.

// Only the leading fields of the shared GlobalsBlock (common.glsl) are needed
// here, and they sit at the front of the std140 block, so this truncated
// declaration is layout-compatible with the full UBO bound at set 0, binding 0.
layout(set = 0, binding = 0) uniform GlobalsBlock
{
    mat4 view;
    mat4 proj;
    // ...remaining fields exist in the buffer but are unused here.
} G;

// std430: 4 * vec4 = 64 bytes. Must match GpuSplat in SplatRenderer.cpp.
struct GpuSplat
{
    vec4 posOpacity;  // xyz = world position, w = opacity (0..1)
    vec4 color;       // rgb = DC SH coefficient (f_dc), w = unused
    vec4 cov0;        // 3D covariance: xx, xy, xz, yy
    vec4 cov1;        //                yz, zz, (unused, unused)
};

layout(std430, set = 1, binding = 0) readonly buffer SplatBlock { GpuSplat splats[]; };
layout(std430, set = 1, binding = 1) readonly buffer OrderBlock { uint order[]; };
// Higher-order SH: per splat, shDim coefficients, each an RGB triple, coeff-
// major: flat index g = base + k*3 + channel, base = sidx * shDim * 3.
// Coefficients are snorm8-quantised over [-1,1] and packed 4 per uint (they sit
// within [-0.75,0.875] in practice, so [-1,1] never clips). This is a 4x memory
// saving over float32 with visually negligible error (~1/254). unpackSnorm4x8
// needs no extension.
layout(std430, set = 1, binding = 2) readonly buffer ShBlock { uint shPacked[]; };

float shAt(uint g)   // dequantised coefficient at flat index g
{
    return unpackSnorm4x8(shPacked[g >> 2u])[g & 3u];
}

layout(push_constant) uniform SplatPush
{
    vec2  viewport;   // pixels
    float scaleMod;   // global splat size multiplier (debug knob; 1.0 = normal)
    uint  shDegree;   // 0..3 spherical-harmonics degree to evaluate
} sp;

layout(location = 0) out vec3 vColor;
layout(location = 1) out float vOpacity;
layout(location = 2) out vec3 vConic;    // inverse 2D covariance: (a, b, c)
layout(location = 3) out vec2 vDelta;    // offset from centre, in pixels

// Real spherical-harmonics constants (match the CPU reference in tools/sh_ref).
const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;
const float SH_C2[5] = float[5]( 1.0925484305920792, -1.0925484305920792,
                                 0.31539156525252005, -1.0925484305920792,
                                 0.5462742152960396);
const float SH_C3[7] = float[7](-0.5900435899266435, 2.890611442640554,
                                -0.4570457994644658, 0.3731763325901154,
                                -0.4570457994644658, 1.445305721320277,
                                -0.5900435899266435);

int shDimForDegree(uint deg)
{
    return deg == 1u ? 3 : deg == 2u ? 8 : deg == 3u ? 15 : 0;
}

// dc = degree-0 coefficient (f_dc); dir = normalised camera->splat direction.
// `base` indexes the higher-order coeffs for this splat in sh[].
vec3 evalSH(uint deg, vec3 dir, vec3 dc, uint base)
{
    vec3 result = SH_C0 * dc;
    if (deg >= 1u)
    {
        float x = dir.x, y = dir.y, z = dir.z;
        #define COEF(k) vec3(shAt(base + uint(k)*3u + 0u), shAt(base + uint(k)*3u + 1u), shAt(base + uint(k)*3u + 2u))
        result += -SH_C1 * y * COEF(0) + SH_C1 * z * COEF(1) - SH_C1 * x * COEF(2);
        if (deg >= 2u)
        {
            float xx=x*x, yy=y*y, zz=z*z, xy=x*y, yz=y*z, xz=x*z;
            result += SH_C2[0]*xy*COEF(3) + SH_C2[1]*yz*COEF(4)
                    + SH_C2[2]*(2.0*zz-xx-yy)*COEF(5)
                    + SH_C2[3]*xz*COEF(6) + SH_C2[4]*(xx-yy)*COEF(7);
            if (deg >= 3u)
            {
                result += SH_C3[0]*y*(3.0*xx-yy)*COEF(8)
                        + SH_C3[1]*xy*z*COEF(9)
                        + SH_C3[2]*y*(4.0*zz-xx-yy)*COEF(10)
                        + SH_C3[3]*z*(2.0*zz-3.0*xx-3.0*yy)*COEF(11)
                        + SH_C3[4]*x*(4.0*zz-xx-yy)*COEF(12)
                        + SH_C3[5]*z*(xx-yy)*COEF(13)
                        + SH_C3[6]*x*(xx-3.0*yy)*COEF(14);
            }
        }
        #undef COEF
    }
    return max(result + vec3(0.5), vec3(0.0));
}

// The two triangles of the quad, in clip-space corner signs.
const vec2 kCorners[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));

void main()
{
    const uint instance = uint(gl_InstanceIndex);
    const uint sidx     = order[instance];
    GpuSplat s = splats[sidx];

    // View-space centre.
    vec4 posV = G.view * vec4(s.posOpacity.xyz, 1.0);

    // Cull splats at or behind the camera: emit a degenerate, off-screen vertex.
    if (posV.z >= -0.001)
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // clipped (z/w = 2 > 1)
        vOpacity = 0.0; vColor = vec3(0.0); vConic = vec3(0.0); vDelta = vec2(0.0);
        return;
    }

    // Focal lengths recovered from the projection matrix, robust to the
    // project's exact clip conventions (Y may be flipped in proj).
    const float focalX = 0.5 * sp.viewport.x * abs(G.proj[0][0]);
    const float focalY = 0.5 * sp.viewport.y * abs(G.proj[1][1]);

    // Reassemble the symmetric 3D covariance.
    mat3 Vrk = mat3(
        s.cov0.x, s.cov0.y, s.cov0.z,
        s.cov0.y, s.cov0.w, s.cov1.x,
        s.cov0.z, s.cov1.x, s.cov1.y);

    // Projection Jacobian at the splat centre. t.z < 0 in front of the camera.
    // Clamp x/y extent so splats near the frustum edge don't explode.
    const float tz  = posV.z;
    const float lim = 1.3;
    float tx = clamp(posV.x / tz, -lim, lim) * tz;
    float ty = clamp(posV.y / tz, -lim, lim) * tz;

    mat3 J = mat3(
        focalX / tz, 0.0,          -(focalX * tx) / (tz * tz),
        0.0,          focalY / tz, -(focalY * ty) / (tz * tz),
        0.0,          0.0,          0.0);

    mat3 W = mat3(G.view);          // rotation part of the view matrix
    mat3 T = W * J;

    mat3 cov = transpose(T) * transpose(Vrk) * T;

    // Low-pass dilation: guarantees every splat covers at least ~1px so thin
    // ellipses don't fall through the sampling grid.
    // Low-pass floor (px^2). Guarantees each splat covers ~sub-pixel area so
    // thin ellipses don't fall through the sampling grid. Kept small so that
    // zoomed-out clouds don't collapse into equal-size round blobs -- a larger
    // value (e.g. 0.3) makes the whole cloud read as a fuzzy sphere until you
    // zoom in far enough for the true projected covariance to exceed the floor.
    cov[0][0] += 0.15;
    cov[1][1] += 0.15;

    float a = cov[0][0];
    float b = cov[0][1];
    float c = cov[1][1];

    float det = a * c - b * b;
    if (det <= 0.0)
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vOpacity = 0.0; vColor = vec3(0.0); vConic = vec3(0.0); vDelta = vec2(0.0);
        return;
    }

    // Conic = inverse of the 2x2 covariance [[a,b],[b,c]].
    float invDet = 1.0 / det;
    vec3 conic = vec3(c * invDet, -b * invDet, a * invDet);

    // Screen-space extent: 3 sigma along the major axis.
    float mid    = 0.5 * (a + c);
    float lambda = mid + sqrt(max(0.1, mid * mid - det));
    float radius = ceil(3.0 * sqrt(lambda)) * sp.scaleMod;   // pixels

    // Clip-space centre, then push the corner out by `radius` pixels.
    vec4 centreClip = G.proj * posV;
    vec2 corner     = kCorners[gl_VertexIndex];
    vec2 offsetNdc  = corner * radius * 2.0 / sp.viewport;   // pixel->NDC

    gl_Position = vec4(centreClip.xy / centreClip.w + offsetNdc, 0.0, 1.0) * centreClip.w;
    // (multiply back by w so perspective divide restores centre + offset)
    gl_Position.z = centreClip.z;
    gl_Position.w = centreClip.w;

    // View-dependent colour from spherical harmonics. dir is the camera->splat
    // direction in world space; camera world position is the translation of the
    // inverse (rigid) view matrix: -R^T * t.
    vec3 camPos = -transpose(mat3(G.view)) * vec3(G.view[3]);
    vec3 dir    = normalize(s.posOpacity.xyz - camPos);
    uint shBase = sidx * uint(shDimForDegree(sp.shDegree)) * 3u;

    vColor   = evalSH(sp.shDegree, dir, s.color.rgb, shBase);
    vOpacity = s.posOpacity.w;
    vConic   = conic;
    vDelta   = corner * radius;   // pixel offset the fragment evaluates against
}
