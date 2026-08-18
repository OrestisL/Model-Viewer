// Shared declarations for the Gaussian-splat GPU radix sort.
//
// Sorts splat indices back-to-front by view-space depth so the splat graphics
// pass can blend them in order. 4-pass, 8-bit LSD radix over a 32-bit key. The
// exact same algorithm is validated on the CPU in tools/radix_ref.cpp against
// the real sample clouds (0 mismatches vs std::stable_sort).
//
// Layout of the tile-histogram matrix `gTileHist` is BIN-MAJOR:
//   gTileHist[bin * numTiles + tile]
// so that the per-bin running prefix over tiles is contiguous, and the global
// base offset for (tile, bin) is:
//   digitBase[bin] + (exclusive prefix of gTileHist[bin][*] up to `tile`)
// where digitBase[bin] = exclusive scan over bins of the per-bin totals.

#ifndef MV_SPLAT_SORT_GLSL
#define MV_SPLAT_SORT_GLSL

#define RADIX          256u    // 8-bit digit
#define WG_SIZE        256u    // threads per workgroup
#define ELEMS_PER_TILE 512u    // elements per workgroup-tile (kept small so the
                               // stable serial scatter per tile is short and
                               // there are many tiles to fill the GPU)

// Push constants shared by the sort dispatches.
layout(push_constant) uniform SortPush
{
    uint numElements;   // splat count
    uint numTiles;      // ceil(numElements / ELEMS_PER_TILE)
    uint shift;         // current radix pass digit shift (0, 8, 16, 24)
    uint srcIsA;        // 1 => src index buffer is index0 (A), dst is index1 (B)
                        // 0 => src is index1 (B), dst is index0 (A)
} pc;

// Order-preserving float -> uint: ascending uint order == ascending float order.
// Ascending sort of these keys reproduces the ascending view-z (back-to-front)
// order the CPU path used.
uint keyFromDepth(float z)
{
    uint u = floatBitsToUint(z);
    return ((u & 0x80000000u) != 0u) ? ~u : (u | 0x80000000u);
}

#endif
