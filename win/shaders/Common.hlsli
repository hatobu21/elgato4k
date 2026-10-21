// Shared colour math. Encoded SDR is sRGB-like (the console's BT.709 output
// shown on a PC monitor); HDR is SMPTE ST 2084 (PQ) with BT.2020 primaries.
#ifndef VELA_COMMON_HLSLI
#define VELA_COMMON_HLSLI

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);

float3 srgbToLinear(float3 c) {
    c = saturate(c);
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}
float3 linearToSrgb(float3 c) {
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// PQ: encoded [0,1] <-> absolute nits [0,10000].
float3 pqToNits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float3 nitsToPq(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float3 bt709To2020(float3 c) {
    return float3(dot(c, float3(0.6274, 0.3293, 0.0433)),
                  dot(c, float3(0.0691, 0.9195, 0.0114)),
                  dot(c, float3(0.0164, 0.0880, 0.8956)));
}
float3 bt2020To709(float3 c) {
    return float3(dot(c, float3(1.6605, -0.5876, -0.0728)),
                  dot(c, float3(-0.1246, 1.1329, -0.0083)),
                  dot(c, float3(-0.0182, -0.1006, 1.1187)));
}

// Y'CbCr (already range-expanded: Y in [0,1], Cb/Cr in [-0.5,0.5]) -> R'G'B'.
float3 yuvToRgb(float y, float cb, float cr, bool bt2020) {
    if (bt2020) return float3(y + 1.4746 * cr, y - 0.16455 * cb - 0.57135 * cr, y + 1.8814 * cb);
    return float3(y + 1.5748 * cr, y - 0.1873 * cb - 0.4681 * cr, y + 1.8556 * cb);
}

// HDR10 -> SDR for an SDR display: luminance roll-off above a knee (extended
// Reinhard, the source peak lands exactly on SDR white), then BT.2020 ->
// BT.709 and sRGB encoding. 203 nits is SDR reference white (BT.2408).
float3 toneMapPqToSrgb(float3 pq, float peakNits) {
    float3 lin709 = max(bt2020To709(pqToNits(pq)), 0.0) / 203.0;
    float l = dot(lin709, kLuma709);
    const float knee = 0.6;
    float w = max((peakNits / 203.0 - knee) / (1.0 - knee), 1.0);
    float x = max(l - knee, 0.0) / (1.0 - knee);
    float mapped = l <= knee ? l : knee + (1.0 - knee) * x * (1.0 + x / (w * w)) / (1.0 + x);
    return linearToSrgb(lin709 * (l > 1e-5 ? mapped / l : 1.0));
}

#endif
