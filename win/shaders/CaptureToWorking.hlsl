// Raw capture bytes (NV12 or P010, exactly as the driver delivered them) ->
//   u0: working texture, RGBA16F, encoded R'G'B' (sRGB-like for SDR, PQ for HDR)
//   u1: optical flow input, B8G8R8A8, at flow resolution (every flowStep pixel)
#include "Common.hlsli"

ByteAddressBuffer raw : register(t0);
RWTexture2D<float4> working : register(u0);
RWTexture2D<float4> flowInput : register(u1);

cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint pitch;            // bytes per luma row
    uint chromaOffset;     // bytes to the interleaved CbCr plane
    uint p010;             // 0 NV12 (8-bit), 1 P010 (10-bit in the high bits of 16)
    uint bt2020;           // matrix: 0 BT.709, 1 BT.2020 NCL
    uint fullRangeChroma;  // Elgato non-MK.2 HDR quirk: limited luma, full chroma
    uint flowStep;         // 1 or 2 (4K flows at 1920x1080)
    uint writeFlow;        // 0 when frame generation is off
};

uint loadByte(uint offset) {
    return (raw.Load(offset & ~3u) >> ((offset & 3u) * 8u)) & 0xFFu;
}
uint loadWord(uint offset) {   // 16-bit aligned
    return (raw.Load(offset & ~3u) >> ((offset & 2u) * 8u)) & 0xFFFFu;
}

float lumaAt(uint2 p) {
    if (p010) {
        float v = float(loadWord(p.y * pitch + p.x * 2) >> 6);
        return (v - 64.0) / 876.0;
    }
    float v = float(loadByte(p.y * pitch + p.x));
    return (v - 16.0) / 219.0;
}

float2 chromaAt(int2 c) {
    c = clamp(c, int2(0, 0), int2(width / 2 - 1, height / 2 - 1));
    if (p010) {
        uint base = chromaOffset + uint(c.y) * pitch + uint(c.x) * 4;
        float2 v = float2(loadWord(base) >> 6, loadWord(base + 2) >> 6);
        return fullRangeChroma ? (v - 512.0) / 1023.0 : (v - 512.0) / 896.0;
    }
    uint base = chromaOffset + uint(c.y) * pitch + uint(c.x) * 2;
    float2 v = float2(loadByte(base), loadByte(base + 1));
    return fullRangeChroma ? (v - 128.0) / 255.0 : (v - 128.0) / 224.0;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    // 4:2:0 with left (co-sited horizontally, centred vertically) siting.
    float2 cpos = float2(id.x * 0.5, (id.y - 0.5) * 0.5);
    int2 c0 = int2(floor(cpos));
    float2 f = cpos - c0;
    float2 cbcr = lerp(lerp(chromaAt(c0), chromaAt(c0 + int2(1, 0)), f.x),
                       lerp(chromaAt(c0 + int2(0, 1)), chromaAt(c0 + int2(1, 1)), f.x), f.y);
    float3 rgb = saturate(yuvToRgb(lumaAt(id.xy), cbcr.x, cbcr.y, bt2020 != 0));
    working[id.xy] = float4(rgb, 1);

    if (writeFlow && (id.x % flowStep) == 0 && (id.y % flowStep) == 0) {
        // NVOF works on luma-like structure; encoded values are what it wants.
        flowInput[id.xy / flowStep] = float4(rgb, 1);
    }
}
