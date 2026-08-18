# Gaussian Splatting support — Milestone 1 design (loaders + validation)

Scope of milestone 1: in-memory `SplatCloud` type, a `SplatLoader` that reads
splat-PLY and SPZ (v1/v2) into it, and a host-side validation harness. **No
renderer changes in this milestone.** Verified against real data:
`hornedlizard.spz` and `racoonfamily.spz` (both SPZ v2), plus a synthesized
splat-PLY.

## Why a parallel type instead of extending `Scene`

`Scene` is mesh-centric: `Scene::empty()` is `meshes.empty()`, `App::hasModel()`
is `!m_scene.empty()`, and the whole load/upload/draw path assumes vertices +
indices + materials + node hierarchy. A splat cloud has none of that — it is a
flat list of anisotropic Gaussians. Forcing it through `Scene` would mean every
mesh assumption needs a "...unless it's a splat" branch. Cleaner to add a
sibling `SplatCloud` and let the app hold either (milestone 2 decides the exact
switch in App/Renderer).

## SplatCloud (renderer-agnostic, mirrors how Scene is renderer-agnostic)

Stored **un-activated**, matching the reference `GaussianCloud` convention, so
one activation path in the future shader works for both source formats:

- `positions`  : vec3 per splat (world units)
- `scales`     : vec3 per splat, **log-space** (apply exp() at render)
- `rotations`  : quat per splat, **xyzw, un-normalized** (normalize at render)
- `alphas`     : float per splat, **logit** (apply sigmoid at render)
- `colorsDC`   : vec3 per splat — SH degree-0 term (base color)
- `sh`         : float per splat = `shDim*3`, layout **[S,C] coeff-major,
                 RGB interleaved** (coeff0 R,G,B, coeff1 R,G,B, ...), matching
                 the reference's `[N,S,C]` in-memory layout
- `shDegree`   : 0..3 (0 means colorsDC only)
- `count`, `bounds` (AABB), `sourcePath`, `importerName`

`shDim` by degree: 0→0, 1→3, 2→8, 3→15, 4→24 (`dimForDegree`).

## Format detection (branch by content, not extension)

- `.spz` → SPZ path (magic `0x5053474e` "NGSP" after gzip inflate).
- `.ply` → sniff ASCII header: if vertex element has `f_dc_0`, `scale_0`,
  `rot_0` → splat path; else return "not a splat" so the caller hands it to the
  mesh `ModelLoader`. (A `.ply` is legitimately a mesh OR a point cloud OR a
  splat — this is the agreed disambiguation.)

## Splat-PLY specifics (VERIFIED against spz reference loader)

- Must be `format binary_little_endian 1.0`. Every 3DGS property is `float32`;
  confirm all vertex properties are float before the fast bulk read.
- Property → field mapping:
  - position: `x, y, z`
  - scale:    `scale_0, scale_1, scale_2`  (log-space)
  - **rotation: PLY stores `rot_0..3` as WXYZ. Reorder to xyzw =
    {rot_1, rot_2, rot_3, rot_0}.** (SPZ is already xyzw — do NOT reorder there.)
  - alpha:    `opacity` (logit)
  - color DC: `f_dc_0, f_dc_1, f_dc_2`
  - SH rest:  `f_rest_0 .. f_rest_(3*shDim-1)`, present only for degree ≥ 1.
- **SH `f_rest` is channel-major on disk: [C,S] (all R coeffs, then G, then B).**
  Transpose to our [S,C] interleaved: for coeff j in 0..shDim-1 push
  `f_rest[j]` (R), `f_rest[j+shDim]` (G), `f_rest[j+2*shDim]` (B).
  shDim is inferred from how many `f_rest_i` exist / 3.
- `nx, ny, nz` present but unused. Ignore any extra elements after `vertex`.

## SPZ specifics (VERIFIED — decodes both sample files, 0 leftover bytes)

- File = gzip stream. Inflate, then the payload begins with a 16-byte legacy
  header (v1/v2): magic `uint32` = NGSP, `version uint32`, `numPoints uint32`,
  `shDegree u8`, `fractionalBits u8`, `flags u8`, reserved u8.
- Samples are **version 2, shDegree 3, fractionalBits 12**. `flags & 1` =
  antialiased (informational for now).
- Non-interleaved streams, in this exact order after the header:
  `positions (9B/pt) | alphas (1B) | colors (3B) | scales (3B) |
   rotations (3B for v1/v2) | sh (shDim*3 B)`.
- Unpack math (per splat):
  - position: 3 × 24-bit little-endian signed fixed-point, sign-extended,
    × `1/2^fractionalBits`.
  - scale (log): `byte/16 - 10`.
  - rotation (v1/v2 "first three"): `xyz = byte/127.5 - 1`; `w = sqrt(max(0,
    1 - |xyz|^2))`. (v3+ uses "smallest three" — out of milestone-1 scope.)
  - alpha (logit): `invSigmoid(byte/255)`, `invSigmoid(x)=log(x/(1-x))`.
  - color DC: `((byte/255) - 0.5) / 0.15`  (colorScale = 0.15).
  - SH: `(byte - 128) / 128`. SPZ SH is already in our [S,C]-interleaved order.
- `version == 1` stores positions as float16 (never publicly released) — cheap
  to support, low priority.

## Deliberately deferred (flagged, not silently dropped)

- **SPZ v3/v4**: v3 changes quaternion encoding (smallest-three); v4 changes the
  container (ZSTD, multi-stream TOC, 32-byte `NgspFileHeader`). Neither is in the
  sample set. Ship v1/v2 first; add v3/v4 once rendering works.
- Renderer (EWA projection, depth sort, SH eval, blending) = milestone 2+,
  inherently needs on-GPU iteration and can't be verified in this sandbox.

## Milestone-1 deliverables

1. `src/scene/SplatCloud.hpp`
2. `src/scene/SplatLoader.hpp` / `.cpp`  (PLY + SPZ v1/v2 → SplatCloud)
3. `tools/splat_inspect.cpp` — standalone, no Vulkan; prints count, shDegree,
   AABB, and a few decoded splats. Runs in CI/sandbox against real files.
4. zlib for SPZ gunzip — RESOLVED: no new external dependency. Assimp is
   fetched with `ASSIMP_BUILD_ZLIB ON`, which builds a `zlibstatic` target in
   the same tree; CMake now links that (with a `ZLIB::ZLIB` fallback for the
   case where Assimp is a system package). If you'd rather not couple to
   Assimp's bundled zlib, swapping in a vendored single-file inflate (miniz) or
   a system zlib is a one-line change at the `MV_ZLIB_TARGET` block. This is the
   one build-side choice worth a second opinion, since I can't compile the
   Windows/Vulkan target here.

## Verification performed (in a Linux sandbox, no GPU)

- SplatLoader.cpp + the harness compiled with a real system zlib and run
  against the actual Niantic samples: `hornedlizard.spz` (786,233 gaussians)
  and `racoonfamily.spz` (932,560), both SPZ v2, SH degree 3. Streams consume
  to the exact byte, quaternions come out unit-length, AABB/opacity/colour
  ranges are sane, and array sizes are internally consistent.
- Synthesized splat-PLY (SH degree 1) parses; the WXYZ->xyzw quaternion reorder
  yields identity as expected.
- Synthesized mesh-PLY is correctly rejected by canLoad() (routes to the mesh
  loader); a forged SPZ v4 is refused with a clear message.
- Not verifiable here: the real ModelViewer target (needs Vulkan SDK + glm via
  FetchContent) and, therefore, the CMake wiring on Windows. The loader *logic*
  is proven; the build integration is written but unbuilt.
