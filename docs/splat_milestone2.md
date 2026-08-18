# Gaussian Splatting support — Milestone 2 (renderer, first cut)

Scope: a minimal but complete draw path that puts a loaded SplatCloud on screen.
SH degree 0 (flat, view-independent colour), per-frame CPU depth sort,
back-to-front premultiplied-alpha blending. Full SH evaluation and a GPU sort
are later milestones; the data path is shaped so they slot in without a rewrite.

## What was added

Shaders (compile-verified to SPIR-V, Vulkan 1.2, with glslangValidator):
- `shaders/splat.vert` — projects each 3D Gaussian to a screen-space ellipse via
  the EWA/projection-Jacobian approximation, emits the conic (inverse 2D
  covariance) and a per-corner pixel offset. Draws one quad (6 verts, no vertex
  buffer) per splat; corners come from gl_VertexIndex, per-splat data from an
  SSBO indexed through a per-frame sorted-order SSBO.
- `shaders/splat.frag` — evaluates the Gaussian falloff from the conic and
  outputs PREMULTIPLIED colour for (ONE, ONE_MINUS_SRC_ALPHA) blending.

C++:
- `src/vk/SplatRenderer.{hpp,cpp}` — self-contained sibling to the mesh path.
  Owns the splat SSBO + pipeline + per-frame order buffers. On upload it
  pre-activates colour (0.5 + C0·f_dc) and opacity (sigmoid) and precomputes
  each splat's 3D covariance Σ = R·S²·Rᵀ from scale+rotation. Each frame it
  depth-sorts on the CPU, writes the order buffer, and records the draw.
- Renderer integration: `uploadSplats`/`clearSplats`, init of the SplatRenderer
  (reusing the set-0 globals layout, same colour/depth attachments), and a
  record call in `endFrame` after the mesh scene and before grid/axes.
- App integration: `loadModel` dispatches to `loadSplat` when
  `SplatLoader::canLoad(path)`; mesh and splat loads clear each other;
  `hasModel`/`focusCamera`/`modelPath`/window title account for splats.
- UI: the Model panel shows splat stats (count, SH degree, importer) and a
  "Splat size" debug slider (`RenderSettings::splatScale`). The Open dialog now
  offers `.spz`.

## Data path (GpuSplat, std430, 64 bytes — matches splat.vert)

    vec4 posOpacity;  // xyz world pos, w opacity (sigmoid-activated)
    vec4 color;       // rgb (SH0-activated), w unused
    vec4 cov0;        // Σ: xx, xy, xz, yy
    vec4 cov1;        //    yz, zz, pad, pad

Sort: view-space z ascending (farthest first) → back-to-front over-blend.

## NOT verifiable in the authoring sandbox

There is no GPU/Vulkan/display here, so only the shaders were machine-checked
(they compile). The C++ is written to the project's exact patterns and reviewed
statically, but its first real run is on your machine. Expect to iterate on:

1. **View-space z sign / culling.** splat.vert culls with `posV.z >= -0.001`
   assuming a right-handed view looking down -z (GLM default). If nothing draws
   at all, flip this test first.
2. **Projection Y flip.** Focal lengths use `abs(G.proj[1][1])`. If splats are
   mirrored vertically or the ellipse orientation looks wrong, the sign/flip in
   the Jacobian's y-row is the place to look.
3. **Blend / premultiply.** Frag outputs `vColor*alpha`; pipeline is
   (ONE, ONE_MINUS_SRC_ALPHA). If the cloud looks washed out or too dark, this
   pairing is the suspect.
4. **Sort direction.** If near splats are hidden behind far ones, reverse the
   comparator (ascending vs descending view z).
5. **Splat size.** The "Splat size" slider scales the 3σ quad extent; use it to
   sanity-check that ellipses are the right footprint before trusting the math.

## Performance note (expected, not a bug)

The depth sort is `std::sort` over all splats every frame on the CPU — fine for
correctness and for the ~800k–930k-splat sample clouds at interactive-ish rates,
but it is the obvious bottleneck. Milestone 3: move the sort to the GPU (radix)
and evaluate SH per-frame against the view direction for degree ≥ 1.

## Later milestones

- M3: GPU radix sort; full spherical-harmonics view-dependent colour.
- M4: SPZ v3/v4 (smallest-three quaternions, ZSTD multi-stream).
