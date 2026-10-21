#include "Check.h"
#include "vela/core/ContentCadence.h"

using namespace vela::core;

namespace {
struct Run {
    int processed = 0, resets = 0;
    std::vector<bool> pattern;
};
// `period`: the source repeats each picture `period` times. `hud`: something
// small changes on every signal frame (a 60 fps HUD over a 30 fps game).
Run feed(ContentCadence& c, double signalFps, int period, int frames, Micros start = 0, bool hud = false) {
    Run r;
    for (int i = 0; i < frames; ++i) {
        const Micros t = start + Micros(i * 1e6 / signalFps);
        const bool changed = hud || i % period == 0;
        const auto d = c.onFrame(t, changed);
        r.pattern.push_back(d.process);
        r.processed += d.process;
        r.resets += d.reset;
    }
    return r;
}
}

TEST(cadence_auto_detects_30_in_60) {
    ContentCadence c;
    c.configure(ContentRate::Auto, 60);
    const auto r = feed(c, 60, 2, 240);
    CHECK(r.processed == 120);
    CHECK_NEAR(c.detectedFps(), 30, 1e-9);
    CHECK_NEAR(c.contentFps(), 30, 1e-9);
    CHECK(c.repeats() == 120);
}

TEST(cadence_auto_full_rate_60) {
    ContentCadence c;
    c.configure(ContentRate::Auto, 60);
    const auto r = feed(c, 60, 1, 120);
    CHECK(r.processed == 120);
    CHECK_NEAR(c.contentFps(), 60, 1e-9);
}

TEST(cadence_auto_detects_40_in_120) {
    ContentCadence c;
    c.configure(ContentRate::Auto, 120);
    feed(c, 120, 3, 360);
    CHECK_NEAR(c.contentFps(), 40, 1e-9);
}

TEST(cadence_auto_switch_waits_one_second) {
    ContentCadence c;
    c.configure(ContentRate::Auto, 60);
    feed(c, 60, 2, 240);                          // 4 s of 30 fps
    CHECK_NEAR(c.contentFps(), 30, 1e-9);
    feed(c, 60, 1, 30, Micros(4e6));              // 0.5 s of 60 fps: not yet
    CHECK_NEAR(c.contentFps(), 30, 1e-9);
    feed(c, 60, 1, 120, Micros(4.5e6));           // 2 more seconds
    CHECK_NEAR(c.contentFps(), 60, 1e-9);
}

TEST(cadence_fixed_30_ignores_a_60fps_hud) {
    ContentCadence c;
    c.configure(ContentRate::Fps30, 60);
    const auto r = feed(c, 60, 2, 240, 0, true);  // every signal frame "changes"
    CHECK(r.processed == 120);
    bool alternating = true;
    for (size_t i = 2; i < r.pattern.size(); ++i) alternating &= r.pattern[i] != r.pattern[i - 1];
    CHECK(alternating);
}

TEST(cadence_fixed_120_processes_everything) {
    ContentCadence c;
    c.configure(ContentRate::Fps120, 120);
    const auto r = feed(c, 120, 1, 240);
    CHECK(r.processed == 240);
}

TEST(cadence_fixed_rate_is_capped_by_signal) {
    ContentCadence c;
    c.configure(ContentRate::Fps120, 60);
    CHECK_NEAR(c.contentFps(), 60, 1e-9);
}

TEST(cadence_pause_requests_reset) {
    ContentCadence c;
    c.configure(ContentRate::Auto, 60);
    feed(c, 60, 2, 120);
    // two seconds of a static menu, then motion again
    for (int i = 0; i < 120; ++i) c.onFrame(Micros(2e6 + i * 16667), false);
    const auto d = c.onFrame(Micros(4e6), true);
    CHECK(d.process && d.reset);
    CHECK_NEAR(c.contentFps(), 30, 1e-9);         // the pause does not count as cadence
}

TEST(cadence_snap) {
    CHECK_NEAR(ContentCadence::snap(29.97), 30, 1e-9);
    CHECK_NEAR(ContentCadence::snap(59.94), 60, 1e-9);
    CHECK_NEAR(ContentCadence::snap(41.0), 40, 1e-9);
    CHECK_NEAR(ContentCadence::snap(75.0), 75, 1e-9);
}

TEST(picture_compare_nv12) {
    const uint32_t w = 64, h = 32, pitch = 64;
    std::vector<uint8_t> a(pitch * h * 3 / 2, 100);
    PictureCompare pc;
    PictureCompare::Layout l{a.data(), a.size(), pitch, pitch * h, w, h, false};
    CHECK(pc.changed(l));                         // first frame
    CHECK(!pc.changed(l));                        // identical repeat
    for (uint32_t y = 0; y < 16; ++y)             // capture noise: +-2 levels
        for (uint32_t x = 0; x < w; ++x) a[y * pitch + x] = uint8_t(100 + (x % 3) - 1);
    CHECK(!pc.changed(l));
    for (uint32_t y = 0; y < h; ++y)              // a real change
        for (uint32_t x = 0; x < w; ++x) a[y * pitch + x] = 160;
    CHECK(pc.changed(l));
    CHECK(pc.lastChangedFraction() > 0.5);
}

TEST(picture_compare_p010_uses_high_bytes) {
    const uint32_t w = 64, h = 32, pitch = 128;
    std::vector<uint8_t> a(pitch * h * 3 / 2, 0);
    PictureCompare pc;
    PictureCompare::Layout l{a.data(), a.size(), pitch, pitch * h, w, h, true};
    pc.changed(l);
    for (size_t i = 0; i < a.size(); i += 2) a[i] = 0xC0;   // low bits only (noise)
    CHECK(!pc.changed(l));
    for (size_t i = 1; i < a.size(); i += 2) a[i] = 0x80;   // high byte: real change
    CHECK(pc.changed(l));
}
