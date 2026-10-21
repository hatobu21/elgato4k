// Auto colour statistics over every 4th pixel of the working texture.
// Output layout (uint words), matching vela::core::ColorStats:
//   [0..63]  encoded luma histogram
//   [64..66] mid-tone linear RGB sums (x1024)   [67] mid-tone count
//   [68..70] highlight linear RGB sums (x1024)  [71] highlight count
//   [72]     chroma sum (x1024)                 [73] chroma count
#include "Common.hlsli"

Texture2D<float4> working : register(t0);
RWByteAddressBuffer stats : register(u0);

cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint hdr;       // working values are PQ
};

groupshared uint gHist[64];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 64) gHist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint2 p = id.xy * 4;
    if (p.x < width && p.y < height) {
        float3 e = working[p].rgb;
        float3 lin = hdr ? min(pqToNits(e) / 203.0, 4.0) : srgbToLinear(e);
        float y = dot(e, hdr ? kLuma2020 : kLuma709);
        InterlockedAdd(gHist[min(uint(y * 64.0), 63u)], 1);
        if (y > 0.15 && y < 0.85) {
            uint3 q = uint3(lin * 1024.0);
            stats.InterlockedAdd(64 * 4, q.r);
            stats.InterlockedAdd(65 * 4, q.g);
            stats.InterlockedAdd(66 * 4, q.b);
            stats.InterlockedAdd(67 * 4, 1);
            float mx = max(e.r, max(e.g, e.b)), mn = min(e.r, min(e.g, e.b));
            stats.InterlockedAdd(72 * 4, uint((mx > 1e-4 ? (mx - mn) / mx : 0) * 1024.0));
            stats.InterlockedAdd(73 * 4, 1);
        } else if (y >= 0.85 && max(e.r, max(e.g, e.b)) < 0.995) {
            uint3 q = uint3(lin * 1024.0);
            stats.InterlockedAdd(68 * 4, q.r);
            stats.InterlockedAdd(69 * 4, q.g);
            stats.InterlockedAdd(70 * 4, q.b);
            stats.InterlockedAdd(71 * 4, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 64 && gHist[gi]) stats.InterlockedAdd(gi * 4, gHist[gi]);
}
