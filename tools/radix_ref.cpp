// CPU reference of the EXACT algorithm the GPU compute shaders will implement:
//  1. depth-key encoding: view-space z -> order-preserving uint32
//  2. 4-pass 8-bit LSD radix sort (stable counting sort per pass)
// Validated by comparing the resulting index order against std::sort on the
// same depths. If they match on the real cloud, the algorithm is correct.
#include "scene/SplatLoader.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>

// Order-preserving float->uint: ascending uint order == ascending float order.
static inline uint32_t keyFromFloat(float f)
{
    uint32_t u; std::memcpy(&u, &f, 4);
    // If sign bit set (negative), flip all bits; else flip only sign bit.
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s file.spz\n", argv[0]); return 2; }
    mv::SplatCloud c; std::string err;
    if (!mv::SplatLoader::load(argv[1], c, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const uint32_t n = (uint32_t)c.count();

    // Synthetic but realistic view-space depth: a linear functional of position
    // (stands in for (view*p).z). Uses the real cloud's spatial distribution.
    const float dx=0.31f, dy=-0.52f, dz=0.79f, dw=12.5f;
    std::vector<float> depth(n);
    for (uint32_t i=0;i<n;i++){ auto&p=c.positions[i]; depth[i]=dx*p.x+dy*p.y+dz*p.z+dw; }

    // --- reference: std::sort ascending by depth (current renderer behaviour) ---
    std::vector<uint32_t> ref(n); for(uint32_t i=0;i<n;i++) ref[i]=i;
    std::stable_sort(ref.begin(),ref.end(),[&](uint32_t a,uint32_t b){return depth[a]<depth[b];});

    // --- radix: 4 x 8-bit LSD over the order-preserving key ---
    std::vector<uint32_t> key(n), idxA(n), idxB(n);
    for(uint32_t i=0;i<n;i++){ key[i]=keyFromFloat(depth[i]); idxA[i]=i; }
    std::vector<uint32_t>* src=&idxA; std::vector<uint32_t>* dst=&idxB;
    for(int pass=0;pass<4;pass++){
        const int shift=pass*8;
        uint32_t hist[256]={0};
        for(uint32_t i=0;i<n;i++) hist[(key[(*src)[i]]>>shift)&0xFF]++;
        uint32_t base[256]; uint32_t acc=0;
        for(int b=0;b<256;b++){ base[b]=acc; acc+=hist[b]; }        // exclusive scan
        for(uint32_t i=0;i<n;i++){ uint32_t k=(key[(*src)[i]]>>shift)&0xFF; (*dst)[base[k]++]=(*src)[i]; } // stable scatter
        std::swap(src,dst);
    }
    std::vector<uint32_t>& radix=*src;

    // --- compare ---
    // Exact index-order match is the strong check. (Ties on identical depth may
    // legitimately differ between stable_sort and radix only if radix weren't
    // stable; it is, so orders must be identical.)
    uint32_t mism=0; for(uint32_t i=0;i<n;i++) if(radix[i]!=ref[i]) mism++;
    // Also verify monotonic non-decreasing depth in radix output.
    uint32_t inv=0; for(uint32_t i=1;i<n;i++) if(depth[radix[i]]<depth[radix[i-1]]) inv++;

    std::printf("splats            : %u\n", n);
    std::printf("radix vs stable   : %u mismatches\n", mism);
    std::printf("monotonic (depth) : %u inversions\n", inv);
    std::printf("%s\n", (mism==0 && inv==0) ? "ALGORITHM CORRECT" : "MISMATCH");
    return (mism==0 && inv==0)?0:1;
}
