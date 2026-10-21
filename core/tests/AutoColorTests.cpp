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
// Adds `weight` samples to every bin whose centre lies in [from, to].
ColorStats& add(ColorStats& s, float from, float to, uint32_t weight) {
    for (int i = 0; i < ColorStats::kBins; ++i) {
        const float centre = (i + 0.5f) / ColorStats::kBins;
        if (centre >= from && centre <= to) s.luma[static_cast<size_t>(i)] += weight;
    }
    return s;
}
// Mostly bright: a full-range picture with a heavy mass above 0.85.
ColorStats overexposed() { auto s = flat(0, 1); return add(s, 0.85f, 1, 5000); }
AutoColorConfig full() { AutoColorConfig c; c.enabled = true; c.strength = 1; return c; }
AutoColorConfig preset(ColorPreset p) { auto c = full(); applyColorPreset(c, p); return c; }
ColorPreset settle(const ColorStats& s, double seconds, AutoColor& ac) {
    for (int i = 0; i < int(seconds * 60); ++i) ac.update(s, 1.0 / 60);
    return ac.diagnosis().recommended;
}
float luma(const ColorParams& p) { return 0.2126f * p.gainR + 0.7152f * p.gainG + 0.0722f * p.gainB; }
} // namespace

TEST(autocolor_well_exposed_neutral_frame_is_left_alone) {
    const auto p = AutoColor::estimate(flat(0.0f, 1.0f), full());
    CHECK(p.isIdentity(0.02f));
}

TEST(autocolor_washed_out_frame_gets_levels) {
    const auto p = AutoColor::estimate(flat(0.15f, 0.80f), preset(ColorPreset::Washed));
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
    auto c = full();
    c.saturation = 1;
    const auto p = AutoColor::estimate(s, c);
    CHECK(p.saturation > 1.08f && p.saturation <= 1.15f);
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

TEST(autocolor_presets_set_amounts_and_detect_tweaks) {
    auto c = full();
    c.strength = 0.7f;
    applyColorPreset(c, ColorPreset::Oversaturated);
    CHECK(c.preset == ColorPreset::Oversaturated);
    CHECK(c.saturation < 0 && c.strength == 0.7f && c.enabled);
    CHECK(matchesPreset(c));
    c.highlights += 0.1f;
    CHECK(!matchesPreset(c));
    CHECK(matchesPreset(AutoColorConfig{}));   // the default config is the Natural preset
}

TEST(autocolor_curves_keep_black_mid_white_and_stay_monotonic) {
    for (const float a : {0.0f, 0.08f, 0.16f}) {
        CHECK(applyLift(0, a) == 0 && applyLift(0.5f, a) == 0.5f && applyLift(1, a) == 1);
        CHECK(applyShoulder(0, a) == 0 && applyShoulder(0.5f, a) == 0.5f && applyShoulder(1, a) == 1);
        float lastL = 0, lastS = 0;
        for (int i = 1; i <= 1000; ++i) {
            const float v = i / 1000.0f;
            CHECK(applyLift(v, a) >= lastL && applyShoulder(v, a) >= lastS);
            lastL = applyLift(v, a);
            lastS = applyShoulder(v, a);
        }
    }
    CHECK_NEAR(applyLift(1.0f / 6, 0.16f), 1.0f / 6 + 0.08f, 1e-5);
    CHECK_NEAR(applyShoulder(5.0f / 6, 0.16f), 5.0f / 6 - 0.08f, 1e-5);
}

TEST(autocolor_highlights_roll_down_only_bright_pictures) {
    CHECK(AutoColor::estimate(flat(0, 1), preset(ColorPreset::Overexposed)).shoulder == 0);
    const auto p = AutoColor::estimate(overexposed(), preset(ColorPreset::Overexposed));
    CHECK(p.shoulder > 0.12f && p.shoulder <= 0.16f);
    CHECK(p.lift == 0);
    auto off = preset(ColorPreset::Overexposed);
    off.highlights = 0;
    CHECK(AutoColor::estimate(overexposed(), off).shoulder == 0);
}

TEST(autocolor_shadows_open_up_dark_pictures) {
    CHECK(AutoColor::estimate(flat(0, 1), preset(ColorPreset::Dark)).lift == 0);
    const auto natural = AutoColor::estimate(flat(0, 0.35f), full());
    const auto dark = AutoColor::estimate(flat(0, 0.35f), preset(ColorPreset::Dark));
    CHECK(dark.lift > 0.1f && dark.lift <= 0.16f);
    CHECK(natural.lift > 0 && natural.lift < dark.lift / 3);
}

TEST(autocolor_negative_saturation_calms_loud_pictures) {
    auto loud = flat(0, 1);
    loud.meanChroma = 0.6f;
    const auto calm = AutoColor::estimate(loud, preset(ColorPreset::Oversaturated));
    CHECK_NEAR(calm.saturation, 0.8, 1e-4);
    CHECK_NEAR(AutoColor::estimate(flat(0, 1), preset(ColorPreset::Oversaturated)).saturation, 0.95, 1e-4);
    CHECK(AutoColor::estimate(loud, full()).saturation == 1);   // positive amounts only fill in flat pictures
}

TEST(autocolor_white_balance_amount_scales_correction) {
    const auto stats = flat(0, 1, 0.19f, 0.20f, 0.23f);
    auto half = full();
    auto all = full();
    all.whiteBalance = 1;
    const auto a = AutoColor::estimate(stats, half), b = AutoColor::estimate(stats, all);
    CHECK(b.gainB < a.gainB && a.gainB < 1);
    CHECK_NEAR(luma(b), 1.0, 1e-4);
    const auto sunset = AutoColor::estimate(flat(0, 1, 0.40f, 0.20f, 0.10f), all);
    CHECK_NEAR(sunset.gainR, 1, 0.01);
}

TEST(autocolor_hdr_skips_tone_curves) {
    auto s = overexposed();
    s.hdr = true;
    const auto p = AutoColor::estimate(s, preset(ColorPreset::Overexposed));
    CHECK(p.shoulder == 0 && p.lift == 0);
    const auto d = AutoColor::diagnose(s);
    CHECK(d[size_t(ColorPreset::Overexposed)] == 0 && d[size_t(ColorPreset::Washed)] == 0);
}

TEST(autocolor_diagnosis_names_the_problem) {
    auto loud = flat(0, 1);
    loud.meanChroma = 0.6f;
    const std::pair<ColorStats, ColorPreset> cases[] = {
        {flat(0, 1), ColorPreset::Natural},
        {flat(0.15f, 0.80f), ColorPreset::Washed},
        {overexposed(), ColorPreset::Overexposed},
        {loud, ColorPreset::Oversaturated},
        {flat(0, 0.35f), ColorPreset::Dark},
        {flat(0, 1, 0.19f, 0.20f, 0.24f), ColorPreset::Cast},
        {flat(0, 1, 0.40f, 0.20f, 0.10f), ColorPreset::Natural},   // sunset: intent
    };
    for (const auto& [stats, expected] : cases) {
        AutoColor ac;
        ac.configure(full());
        CHECK(settle(stats, 0.5, ac) == expected);
    }
}

TEST(autocolor_recommendation_is_smoothed) {
    AutoColor ac;
    ac.configure(full());
    CHECK(settle(flat(0.15f, 0.80f), 3, ac) == ColorPreset::Washed);
    CHECK(settle(flat(0, 1), 0.5, ac) == ColorPreset::Washed);    // a short change does not flip it
    CHECK(settle(flat(0, 1), 5, ac) == ColorPreset::Natural);
}
