// NVOF grid flow (S10.5 SHORT2, current -> previous, flow-input pixels) ->
// dense motion vectors at output resolution in output pixels, as DLSS-G and
// DLSS SR consume them. Bilinear between grid cells; cells whose cost is at
// or above the threshold contribute zero motion (DLSS-G then relies on its
// own analysis there instead of a wrong vector).
Texture2D<int2> rawFlow : register(t0);
Texture2D<uint> rawCost : register(t1);
RWTexture2D<float2> motion : register(u0);

cbuffer Constants : register(b0) {
    uint gridW;
    uint gridH;
    uint gridSize;
    uint flowW;          // flow input extent
    uint flowH;
    uint outW;
    uint outH;
    uint costThreshold;  // 0..255
};

float2 cell(int2 c) {
    c = clamp(c, int2(0, 0), int2(gridW - 1, gridH - 1));
    if (rawCost[c] >= costThreshold) return 0;
    return float2(rawFlow[c]) / 32.0;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outW || id.y >= outH) return;
    float2 scale = float2(flowW, flowH) / float2(outW, outH);
    float2 g = (float2(id.xy) + 0.5) * scale / gridSize - 0.5;
    int2 g0 = int2(floor(g));
    float2 f = g - g0;
    float2 v = lerp(lerp(cell(g0), cell(g0 + int2(1, 0)), f.x),
                    lerp(cell(g0 + int2(0, 1)), cell(g0 + int2(1, 1)), f.x), f.y);
    motion[id.xy] = v / scale;   // flow pixels -> output pixels
}
