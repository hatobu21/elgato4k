#include "Check.h"
#include "vela/core/ModePlanner.h"

using namespace vela::core;

namespace {
GpuCaps rtx5070ti() {
    GpuCaps c;
    c.fgMaxMultiplier = 4;
    c.videoSr = c.dlssSr = c.trueHdr = c.opticalFlow = c.hags = true;
    c.vramMB = 16303;
    return c;
}
bool has(const std::vector<unsigned>& v, unsigned x) { return std::find(v.begin(), v.end(), x) != v.end(); }
}

TEST(plan_ps5_4k60_hdr_on_4k144_hdr) {
    Settings s;
    s.frameGen.multiplier = 4;
    const auto p = planPipeline({3840, 2160, 60, true}, {3840, 2160, 144, true, true}, rtx5070ti(), s);
    CHECK(p.hdr == HdrPath::Passthrough);
    CHECK(p.superRes == SrPath::None);
    CHECK(p.multipliers.size() == 1 && p.multipliers[0] == 2);
    CHECK(p.multiplier == 2);
    CHECK_NEAR(p.outputFps, 120, 1e-9);
    CHECK(!p.notes.empty());   // tells the user 4x was limited
}

TEST(plan_1440p120_sdr_upscaled_on_4k240) {
    Settings s;
    const auto p = planPipeline({2560, 1440, 120, false}, {3840, 2160, 240, false, true}, rtx5070ti(), s);
    CHECK(p.superRes == SrPath::VideoSr);
    CHECK(p.outputWidth == 3840 && p.outputHeight == 2160);
    CHECK(p.hdr == HdrPath::Sdr);
    CHECK(p.multiplier == 2);
}

TEST(plan_switch2_1080p60_sdr_gets_rtx_hdr_and_4x) {
    Settings s;
    s.frameGen.multiplier = 6;
    const auto p = planPipeline({1920, 1080, 60, false}, {3840, 2160, 240, true, true}, rtx5070ti(), s);
    CHECK(p.hdr == HdrPath::RtxHdr);
    CHECK(p.superRes == SrPath::VideoSr);   // SR runs before RTX HDR, on SDR
    CHECK(has(p.multipliers, 2) && has(p.multipliers, 3) && has(p.multipliers, 4));
    CHECK(p.multiplier == 4);
}

TEST(plan_hdr_source_upscales_with_dlss) {
    Settings s;
    const auto p = planPipeline({1920, 1080, 60, true}, {3840, 2160, 120, true, true}, rtx5070ti(), s);
    CHECK(p.superRes == SrPath::Dlss);
    CHECK(p.outputWidth == 3840);
}

TEST(plan_tone_mapped_hdr_upscales_as_sdr) {
    Settings s;
    const auto p = planPipeline({1920, 1080, 60, true}, {3840, 2160, 120, false, false}, rtx5070ti(), s);
    CHECK(p.hdr == HdrPath::ToneMap);
    CHECK(p.superRes == SrPath::VideoSr);
}

TEST(plan_missing_sr_runtime_keeps_source_size) {
    auto caps = rtx5070ti();
    caps.videoSr = false;
    Settings s;
    const auto p = planPipeline({1920, 1080, 60, false}, {3840, 2160, 120, false, false}, caps, s);
    CHECK(p.superRes == SrPath::None);
    CHECK(p.outputWidth == 1920);
}

TEST(plan_hdr_source_on_sdr_display_tone_maps) {
    Settings s;
    const auto p = planPipeline({3840, 2160, 60, true}, {3840, 2160, 120, false, false}, rtx5070ti(), s);
    CHECK(p.hdr == HdrPath::ToneMap);
}

TEST(plan_without_hags_blocks_frame_generation) {
    auto caps = rtx5070ti();
    caps.hags = false;
    Settings s;
    const auto p = planPipeline({3840, 2160, 60, true}, {3840, 2160, 144, true, true}, caps, s);
    CHECK(p.frameGenBlocked);
    CHECK(p.multiplier == 1);
}

TEST(plan_tearing_lifts_refresh_cap) {
    Settings s;
    s.display.sync = SyncMode::Tearing;
    s.frameGen.multiplier = 4;
    const auto p = planPipeline({3840, 2160, 60, true}, {3840, 2160, 144, true, false}, rtx5070ti(), s);
    CHECK(p.multiplier == 4);
}

TEST(plan_frame_generation_off) {
    Settings s;
    s.frameGen.enabled = false;
    const auto p = planPipeline({3840, 2160, 60, true}, {3840, 2160, 144, true, true}, rtx5070ti(), s);
    CHECK(p.multiplier == 1 && !p.frameGenBlocked);
    CHECK_NEAR(p.outputFps, 60, 1e-9);
}

TEST(settings_clamp) {
    Settings s;
    s.frameGen.multiplier = 9;
    s.hdr.peakNits = 50;
    s.color.strength = 3;
    s.clamp();
    CHECK(s.frameGen.multiplier == 6);
    CHECK(s.hdr.peakNits == 400);
    CHECK(s.color.strength == 1.0f);
}
