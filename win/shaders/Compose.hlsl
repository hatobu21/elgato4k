// Present-time composition: scale one finished frame (real or generated)
// into the window's letterbox rectangle and draw the latency HUD on top.
// Runs only when the frame size differs from the back buffer or the HUD is
// on; otherwise the presenter copies the frame directly.
Texture2D<float4> frame : register(t0);
RWTexture2D<float4> output : register(u0);
SamplerState linearClamp : register(s0);

cbuffer Constants : register(b0) {
    uint dstW;
    uint dstH;
    int rectX;
    int rectY;
    uint rectW;
    uint rectH;
    uint hudOn;
    int hudX;
    int hudY;
    uint hudScale;      // pixels per font pixel
    uint hudChars;
    float hudWhite;     // 1.0 SDR, PQ code of ~203 nits for HDR
    uint text0, text1, text2, text3;   // glyph indices, 8 bits each, 16 max
};

// 3x5 font: 0-9, '.', 'm', 's', 'f', 'p', '/', ' ' (indices 0..16).
static const uint kGlyphs[17] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292, 0x7BEF, 0x7BCF,
    0x0002, 0x0FED, 0x388E, 0x39A4, 0x0D74, 0x12A4, 0x0000};

uint glyphAt(uint i) {
    uint word = i < 4 ? text0 : i < 8 ? text1 : i < 12 ? text2 : text3;
    return (word >> ((i & 3) * 8)) & 0xFF;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= dstW || id.y >= dstH) return;
    int2 p = int2(id.xy);
    float4 c = float4(0, 0, 0, 1);
    if (p.x >= rectX && p.y >= rectY && p.x < rectX + int(rectW) && p.y < rectY + int(rectH)) {
        float2 uv = (float2(p - int2(rectX, rectY)) + 0.5) / float2(rectW, rectH);
        c = frame.SampleLevel(linearClamp, uv, 0);
    }
    if (hudOn) {
        const int pad = int(hudScale) * 2;
        const int cellW = int(hudScale) * 4;
        int2 q = p - int2(hudX, hudY);
        int boxW = int(hudChars) * cellW - int(hudScale) + pad * 2;
        int boxH = int(hudScale) * 5 + pad * 2;
        if (q.x >= 0 && q.y >= 0 && q.x < boxW && q.y < boxH) {
            c.rgb *= 0.35;
            int2 t = q - pad;
            if (t.x >= 0 && t.y >= 0 && t.y < int(hudScale) * 5) {
                uint ch = uint(t.x / cellW);
                int gx = (t.x % cellW) / int(hudScale);
                int gy = t.y / int(hudScale);
                if (ch < hudChars && gx < 3) {
                    uint bits = kGlyphs[min(glyphAt(ch), 16u)];
                    if ((bits >> ((4 - gy) * 3 + (2 - gx))) & 1) c.rgb = hudWhite;
                }
            }
        }
    }
    output[id.xy] = c;
}
