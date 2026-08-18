# Gaussian Splatting — Milestone 4: view-dependent colour (spherical harmonics)

Until now the splat renderer used only the SH degree-0 (DC) term as a flat,
view-independent colour, so colours looked flat even though both sample clouds
carry degree-3 SH (45 higher-order coefficients per splat). This milestone
evaluates the full SH per splat against the view direction.

## What changed

- **Upload**: `GpuSplat.color` now carries the raw DC coefficient (`f_dc`)
  instead of a pre-baked flat colour. The higher-order coefficients (the
  `SplatCloud.sh` array, coeff-major RGB interleaved) are uploaded to a new
  device-local SSBO bound at splat set 1, binding 2. At degree 0 a 1-float dummy
  is bound so the descriptor stays valid.
- **Shader** (`splat.vert`): evaluates real spherical harmonics up to the cloud's
  degree, against `dir = normalize(splatPos - cameraPos)` (camera world position
  derived from the view matrix as `-R^T t`). Result: `max(0.5 + SH(dir), 0)`.
  Colour is per splat, so this is done once in the vertex stage.
- **Push constant** gained `shDegree` (replacing the unused pad word).
- **UI**: the Model panel has a "View-dependent colour (SH)" checkbox
  (on by default). Off forces degree 0 — identical to the previous flat look —
  which is handy for an A/B comparison.

## Validation (CPU, against real clouds)

`tools/sh_ref.cpp` implements the identical SH evaluation on the CPU and checks:
- **Degree-0 invariant**: evaluating at degree 0 reproduces the old flat colour
  `0.5 + C0*f_dc` to 0.0 error — so turning SH off is a bit-exact match to the
  previous renderer, and there is no regression for degree-0 assets.
- **No NaN/Inf** across ~170k samples over six view directions on both
  racoonfamily and hornedlizard.
- Colour range lands around [-0.9, 1.6] before the `max(...,0)` clamp, mean
  ~0.5 — the expected spread for SH-encoded colour.

The `splat.vert` SH constants and formula match this reference exactly.

## Memory (snorm8 SH)

The higher-order SH coefficients are stored as **snorm8** (1 byte/coeff, quantised
over [-1,1], packed 4 per uint and unpacked in-shader with `unpackSnorm4x8`).
That is 45 bytes/splat at degree 3 (~42 MB for the 932k racoon cloud) -- 4x
smaller than float32, needing no device extension.

Measured on the real clouds: SH coefficients lie within [-0.75, 0.875] (0%
clipping at [-1,1]), and snorm8 round-trips them with max error ~0.0039 (half a
quantisation step) -- visually negligible, and it matches the precision SPZ
itself uses on disk. The pack/unpack bit layout was verified against
`unpackSnorm4x8` semantics to zero error (tools/pack_check-style test).

The DC term stays float (in GpuSplat.color), so the "SH off == flat" invariant
remains bit-exact.

## Not verified here

As with the renderer and sort, the C++/descriptor plumbing hasn't been run in
the authoring sandbox (no GPU). The shader compiles to SPIR-V and the maths is
CPU-proven. First-run checks: with SH off the image must match the old flat
look; with SH on, colours should shift subtly as you orbit (specular/anisotropic
response). If colours look wrong only with SH on, suspect the `dir` sign
(camera->splat vs splat->camera) or the SH buffer indexing
(`base = sidx * shDim * 3`).
