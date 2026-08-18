# Gaussian Splatting — Milestone 3: GPU radix sort

Replaces the per-frame CPU `std::sort` (which dominated frame time: truck PLY
~20fps, racoon ~45fps) with a GPU radix sort. The CPU sort remains as a runtime
fallback.

## Algorithm (validated on the CPU against real clouds)

4-pass, 8-bit LSD radix over a 32-bit key:
- **Key**: view-space depth of each splat, mapped through an order-preserving
  float->uint transform so ascending uint order == ascending view z == the
  back-to-front order the CPU path used.
- Each pass: **histogram** (per-tile digit counts, bin-major) -> **scan**
  (tile histogram -> global base offsets) -> **stable scatter**.

`tools/radix_ref.cpp` implements the exact same key encoding and radix logic on
the CPU and compares against `std::stable_sort` on the real sample clouds:
**0 mismatches, 0 depth inversions** on both racoonfamily (932,560) and
hornedlizard (786,233). So the algorithm and key encoding are proven; the
compute shaders mirror that code line-for-line.

## GPU implementation

Shaders (all compile to SPIR-V, Vulkan 1.3, verified here):
- `shaders/splat_sort.glsl` — shared constants + key transform.
- `splat_sort_key.comp` — depth key + identity indices.
- `splat_sort_histogram.comp` — per-tile histogram (shared-mem atomics).
- `splat_sort_scan.comp` — single-workgroup global offset scan.
- `splat_sort_scatter.comp` — stable per-tile scatter.

C++: `src/vk/SplatSorter.{hpp,cpp}` — the project's first compute pipelines.
One unified set-0 descriptor (5 SSBOs: splat, keys, index0, index1, tileHist);
the src/dst index buffer per pass is chosen by a push-constant flag, so there is
a single descriptor set and no per-pass rebinding. Buffers and descriptor sets
are **double-buffered per frame-in-flight** so frame N's sort can't race frame
N-1's graphics read. Result always lands in index0 (even pass count).

Integration: the sort is recorded in `Renderer::endFrame` BEFORE
`vkCmdBeginRendering` (compute can't run inside dynamic rendering), with a
compute->vertex barrier; the splat graphics pass then reads the sorted index
buffer as its draw order.

## Design choices favouring correctness over peak speed

Because none of the Vulkan/compute code can be run in the authoring sandbox, the
design deliberately trades some throughput for simpler, easier-to-verify logic:
- The **scatter is serial within a tile** (one thread scatters the tile's
  elements in order). Stability is thus obvious; parallelism comes from having
  many tiles (ELEMS_PER_TILE = 512 -> ~2000 tiles at 1M splats). A parallel
  stable per-tile scatter is a later optimisation.
- The **scan is a single workgroup**. Fine for the tile counts here.

Even so this removes the entire per-frame CPU sort and per-frame index upload,
which was the actual bottleneck.

## Runtime fallback (important)

The Model panel has a **"GPU radix sort"** checkbox (default ON). Turning it off
switches to the CPU `std::sort` path. This is the first thing to try if splats
render as garbage, flicker, or sort incorrectly with the GPU path: if CPU-sort
looks right and GPU-sort doesn't, the bug is in the GPU sort, not the renderer.

## Known-issue fix (post-first-run)

First GPU run showed flashing / mis-sorted splats. Root cause: Renderer's
globals UBO descriptor binding was declared `VERTEX | FRAGMENT` only, but the
sort's key pass reads the view matrix from it in the COMPUTE stage. Reading a
descriptor from a stage absent in its `stageFlags` is undefined behaviour, which
corrupted the depth keys nondeterministically -> flashing + wrong order for both
PLY and SPZ. Fixed by adding `VK_SHADER_STAGE_COMPUTE_BIT` to that binding.
(Vulkan validation layers flag this immediately -- worth keeping them on.)

## What could NOT be verified here (needs your GPU)

Shaders compile and the algorithm is CPU-proven, but the Vulkan plumbing
(compute pipelines, descriptor writes, barriers, dispatch sizing) has never been
run. Likely first issues to check with validation layers on:
1. **Barriers** — inter-pass compute barriers and the final compute->vertex
   barrier. If you see corruption that changes frame-to-frame, suspect a missing
   barrier.
2. **The scan/scatter offsets** — if the sort is wrong but stable, re-check the
   bin-major indexing (bin * numTiles + tile) in scan vs scatter.
3. **Globals set at set 1** — the key pass reads the view matrix from Renderer's
   globals set; confirm binding 0 there is the globals UBO.
4. Enable the Vulkan validation layers for the first run — they will catch
   descriptor/barrier/layout mistakes immediately.

## Later

- Parallel stable scatter + multi-workgroup scan (throughput).
- Sort key could pack tie-breaking; not needed for correctness.
- Milestone 4: full view-dependent SH; SPZ v3/v4.
