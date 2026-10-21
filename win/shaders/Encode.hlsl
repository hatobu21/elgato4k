// Linear intermediate -> output encoding.
//   inputKind 0: scRGB, BT.709 linear, 1.0 = 80 nits (RTX Video HDR output)
//   inputKind 1: BT.709 linear, 1.0 = SDR white (DLSS SR output, SDR path)
//   inputKind 2: BT.2020 linear, 1.0 = 100 nits (DLSS SR output, HDR path)
//   outputKind 0: sRGB (RGBA8)   outputKind 1: PQ BT.2020 (RGB10A2)
#include "Common.hlsli"

Texture2D<float4> source : register(t0);
RWTexture2D<float4> output : register(u0);

cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint inputKind;
    uint outputKind;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float3 c = max(source[id.xy].rgb, 0.0);
    if (outputKind == 0) {
        output[id.xy] = float4(linearToSrgb(inputKind == 2 ? bt2020To709(c) * 100.0 / 203.0 : c), 1);
        return;
    }
    float3 nits2020 = inputKind == 0 ? bt709To2020(c * 80.0)
                    : inputKind == 1 ? bt709To2020(c * 203.0)
                    : c * 100.0;
    output[id.xy] = float4(nitsToPq(nits2020), 1);
}
