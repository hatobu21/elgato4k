#include "Check.h"
#include "vela/core/AutoColor.h"

#include <random>

using namespace vela::core;

namespace {
// Flat histogram between two encoded luma values.
ColorStats flat(float from, float to, float r = 0.2f, float g = 0.2f, float b = 0.2f) {
    ColorStats s;
    for (int i = 0; i < ColorStats::kBins; ++i) {
        const float centre = (i + 0.5f) / ColorStats::kBins;
        if (centre >= from && centre <= to) s.luma[static_cast<size_t>(i)] = 1000;
    }
    s.midR = r; s.midG = g; s.midB = b;
    s.highR = r * 4; s.highG = g * 4; s.highB = b * 4;
    s.meanChroma = 0.3f;
    return s;
}
AutoColorConfig full() { AutoColorConfig c; c.enabled = true; c.strength = 1; return c; }
float luma(const ColorParams& p) { return 0.2126f * p.gainR + 0.7152f * p.gainG + 0.0722f * p.gainB; }
} // namespace

TEST(autocolor_well_exposed_neutral_frame_is_left_alone) {
    const auto p = AutoColor::estimate(flat(0.0f, 1.0f), full());
    CHECK(p.isIdentity(0.02f));
}

TEST(autocolor_washed_out_frame_gets_levels) {
    const auto p = AutoColor::estimate(flat(0.15f, 0.80f), full());
    CHECK(p.blackPoint > 0.05f && p.blackPoint <= 0.06f);
    CHECK(p.whitePoint < 0.9f && p.whitePoint >= 0.88f);
}

TEST(autocolor_percentile_interpolates_inside_bins) {
    ColorStats s;
    s.luma[32] = 100;   // everything in [0.5, 0.515625)
    CHECK_NEAR(AutoColor::lumaPercentile(s.luma, 0.5f), 32.5 / 64, 1e-6);
    CHECK_NEAR(AutoColor::lumaPercentile(s.luma, 1.0f), 33.0 / 64, 1e-6);
}

TEST(autocolor_mild_blue_cast_is_reduced_without_brightness_change) {
    const auto p = AutoColor::estimate(flat(0, 1, 0.19f, 0.20f, 0.23f), full());
    CHECK(p.gainB < 1 && p.gainR > 1);
    CHECK_NEAR(luma(p), 1.0, 1e-4);
}

TEST(autocolor_strong_cast_is_treated_as_intent) {
    const auto sunset = AutoColor::estimate(flat(0, 1, 0.40f, 0.20f, 0.10f), full());
    CHECK_NEAR(sunset.gainR, 1, 0.01);
    CHECK_NEAR(sunset.gainB, 1, 0.01);
}

TEST(autocolor_hdr_skips_levels) {
    auto s = flat(0.15f, 0.80f);
    s.hdr = true;
    const auto p = AutoColor::estimate(s, full());
    CHECK(p.blackPoint == 0 && p.whitePoint == 1 && p.gamma == 1);
}

TEST(autocolor_flat_picture_gets_vibrance) {
    auto s = flat(0, 1);
    s.meanChroma = 0.05f;
    const auto p = AutoColor::estimate(s, full());
    CHECK(p.saturation > 1.05f && p.saturation <= 1.12f);
}

TEST(autocolor_strength_scales_towards_identity) {
    AutoColorConfig half = full();
    half.strength = 0.5f;
    const auto a = AutoColor::estimate(flat(0.15f, 0.80f), full());
    const auto b = AutoColor::estimate(flat(0.15f, 0.80f), half);
    CHECK_NEAR(b.blackPoint, a.blackPoint / 2, 1e-6);
}

TEST(autocolor_converges_with_time_constant) {
    AutoColor ac;
    ac.configure(full());   // Normal: tau 0.8 s
    const auto s = flat(0.15f, 0.80f);
    const float goal = AutoColor::estimate(s, full()).blackPoint;
    for (int i = 0; i < 48; ++i) ac.update(s, 1.0 / 60);   // 0.8 s
    CHECK_NEAR(ac.current().blackPoint / goal, 1 - std::exp(-1.0), 0.02);
}

TEST(autocolor_scene_cut_converges_fast) {
    AutoColor ac;
    ac.configure(full());
    for (int i = 0; i < 300; ++i) ac.update(flat(0.0f, 1.0f), 1.0 / 60);
    const auto cut = flat(0.15f, 0.55f);
    ac.update(cut, 1.0 / 60);
    CHECK(ac.lastWasSceneCut());
    for (int i = 0; i < 15; ++i) ac.update(cut, 1.0 / 60);   // 0.25 s
    CHECK_NEAR(ac.current().blackPoint, ac.target().blackPoint, 0.01);
}

TEST(autocolor_noise_does_not_pump) {
    AutoColor ac;
    ac.configure(full());
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
    float lo = 1, hi = 0;
    for (int i = 0; i < 600; ++i) {
        auto s = flat(0.15f + noise(rng), 0.80f + noise(rng), 0.2f + noise(rng), 0.2f, 0.2f);
        ac.update(s, 1.0 / 60);
        if (i > 400) { lo = std::min(lo, ac.current().gainR); hi = std::max(hi, ac.current().gainR); }
    }
    CHECK(hi - lo < 0.004f);
}

TEST(autocolor_disabled_returns_to_identity) {
    AutoColor ac;
    ac.configure(full());
    for (int i = 0; i < 120; ++i) ac.update(flat(0.15f, 0.80f), 1.0 / 60);
    AutoColorConfig off = full();
    off.enabled = false;
    ac.configure(off);
    for (int i = 0; i < 600; ++i) ac.update(flat(0.15f, 0.80f), 1.0 / 60);
    CHECK(ac.current().isIdentity(0.001f));
}
