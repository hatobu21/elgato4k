// Auto colour correction + conversion of the working texture into the
// format the next stage wants.
//   mode 0: SDR  -> sRGB (RGBA8)
//   mode 1: SDR  -> linear BT.709, 1.0 = SDR white (FP16, DLSS SR input)
//   mode 2: HDR  -> PQ BT.2020 (RGB10A2, HDR10 output)
//   mode 3: HDR  -> linear BT.2020, 1.0 = 100 nits (FP16, DLSS SR input)
//   mode 4: HDR  -> tone mapped sRGB (RGBA8, SDR display)
#include "Common.hlsli"

Texture2D<float4> working : register(t0);
RWTexture2D<float4> output : register(u0);

cbuffer Constants : register(b0) {
    uint width;
    uint height;
    uint mode;
    float blackPoint;
    float whitePoint;
    float gamma;
    float gainR;
    float gainG;
    float gainB;
    float saturation;
    float peakNits;      // tone mapping source peak (mode 4)
};

float3 adjustSaturation(float3 lin, float3 weights) {
    float y = dot(lin, weights);
    return max(y + (lin - y) * saturation, 0.0);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float3 e = working[id.xy].rgb;
    float3 gains = float3(gainR, gainG, gainB);

    if (mode <= 1) {
        // Levels and mid-tone gamma on encoded values, gains and saturation on light.
        e = pow(saturate((e - blackPoint) / max(whitePoint - blackPoint, 1e-3)), gamma);
        float3 lin = adjustSaturation(srgbToLinear(e) * gains, kLuma709);
        output[id.xy] = mode == 0 ? float4(linearToSrgb(lin), 1) : float4(lin, 1);
        return;
    }
    // HDR: no levels (PQ is absolute); white balance and saturation only.
    float3 nits = adjustSaturation(pqToNits(e) * gains, kLuma2020);
    if (mode == 2) output[id.xy] = float4(nitsToPq(nits), 1);
    else if (mode == 3) output[id.xy] = float4(nits / 100.0, 1);
    else output[id.xy] = float4(toneMapPqToSrgb(nitsToPq(nits), peakNits), 1);
}
