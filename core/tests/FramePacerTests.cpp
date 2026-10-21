#include "Check.h"
#include "vela/core/FramePacer.h"

#include <random>

using namespace vela::core;

namespace {
struct Run {
    std::vector<PlannedFrame> frames;
    std::vector<double> latencies;
    unsigned dropped = 0;
};

// Steady source; every batch takes `processingMs` on the GPU and is planned
// 1 ms after its arrival (CPU submit), like the engine does.
Run simulate(PacerConfig config, double fps, double processingMs, int batches, double jitterMs = 0) {
    FramePacer pacer;
    pacer.configure(config);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-jitterMs, jitterMs);
    Run run;
    for (int i = 0; i < batches; ++i) {
        const Micros arrival = fromMs(i * 1000.0 / fps + (jitterMs > 0 ? jitter(rng) : 0));
        const auto plan = pacer.plan(arrival, arrival + 1000);
        pacer.observeProcessing(arrival, arrival + fromMs(processingMs));
        if (i < 20) continue;   // warm-up: interval and budget not measured yet
        run.frames.insert(run.frames.end(), plan.frames.begin(), plan.frames.end());
        run.latencies.push_back(plan.realLatencyMs);
        run.dropped += plan.droppedGenerated;
    }
    return run;
}

double maxSpacingError(const std::vector<PlannedFrame>& frames, double expectedMs) {
    double worst = 0;
    for (size_t i = 1; i < frames.size(); ++i)
        worst = std::max(worst, std::fabs(toMs(frames[i].target - frames[i - 1].target) - expectedMs));
    return worst;
}
} // namespace

TEST(pacer_even_2x_at_60_spaces_frames_evenly) {
    PacerConfig c; c.multiplier = 2;
    const auto run = simulate(c, 60, 6.0, 200);
    CHECK(run.dropped == 0);
    CHECK_NEAR(maxSpacingError(run.frames, 1000.0 / 120), 0, 0.01);
    // budget = P95 processing (6) + margin (1); the real frame waits half an interval.
    CHECK_NEAR(run.latencies.back(), 7.0 + 1000.0 / 120, 0.05);
}

TEST(pacer_even_4x_at_60) {
    PacerConfig c; c.multiplier = 4;
    const auto run = simulate(c, 60, 6.0, 200);
    CHECK(run.dropped == 0);
    CHECK_NEAR(maxSpacingError(run.frames, 1000.0 / 240), 0, 0.01);
    CHECK_NEAR(run.latencies.back(), 7.0 + 3 * 1000.0 / 240, 0.05);
}

TEST(pacer_low_latency_trades_spacing_for_latency) {
    PacerConfig even; even.multiplier = 4;
    PacerConfig fast = even; fast.mode = PacingMode::LowLatency; fast.compression = 0.35;
    const auto a = simulate(even, 60, 6.0, 200);
    const auto b = simulate(fast, 60, 6.0, 200);
    const double saved = 3 * (1000.0 / 240) * 0.35;
    CHECK_NEAR(a.latencies.back() - b.latencies.back(), saved, 0.05);
    CHECK(b.dropped == 0);
}

TEST(pacer_targets_are_monotonic_under_jitter) {
    PacerConfig c; c.multiplier = 3; c.minGapMs = 0.5;
    const auto run = simulate(c, 60, 7.0, 600, 1.5);
    bool monotonic = true;
    for (size_t i = 1; i < run.frames.size(); ++i)
        if (run.frames[i].target - run.frames[i - 1].target < fromMs(0.5)) monotonic = false;
    CHECK(monotonic);
    // Every batch still ends with its real frame.
    unsigned real = 0;
    for (const auto& f : run.frames) real += f.real ? 1 : 0;
    CHECK(real == 580);
}

TEST(pacer_drops_late_generated_frames_but_never_the_real_one) {
    FramePacer pacer;
    PacerConfig c; c.multiplier = 4;
    pacer.configure(c);
    for (int i = 0; i < 30; ++i) {
        const Micros a = fromMs(i * 1000.0 / 60);
        pacer.plan(a, a + 1000);
        pacer.observeProcessing(a, a + fromMs(6));
    }
    // The next batch is planned very late: 2.2 subframes after its slot started.
    const Micros arrival = fromMs(30 * 1000.0 / 60);
    const Micros late = arrival + pacer.processingBudget() + fromMs(2.2 * 1000.0 / 240);
    const auto plan = pacer.plan(arrival, late);
    CHECK(plan.droppedGenerated == 2);
    CHECK(plan.frames.size() == 2);
    CHECK(plan.frames.back().real);
}

TEST(pacer_interval_ignores_a_dropped_capture_frame) {
    FramePacer pacer;
    PacerConfig c; c.multiplier = 2;
    pacer.configure(c);
    Micros t = 0;
    for (int i = 0; i < 20; ++i) {
        t += (i == 10) ? 33333 : 16667;     // one frame missing from the card
        pacer.plan(t, t + 1000);
    }
    CHECK_NEAR(toMs(pacer.sourceInterval()), 16.667, 0.01);
}

TEST(pacer_without_frame_generation_presents_immediately) {
    PacerConfig c; c.multiplier = 1;
    const auto run = simulate(c, 120, 4.0, 100);
    CHECK(run.frames.size() == 80);
    CHECK_NEAR(run.latencies.back(), 1.0, 0.001);  // planned at arrival + 1 ms submit
}

TEST(pacer_budget_falls_gradually) {
    FramePacer pacer;
    PacerConfig c; c.multiplier = 2;
    pacer.configure(c);
    Micros t = 0;
    for (int i = 0; i < 200; ++i) {
        t += 16667;
        pacer.plan(t, t + 1000);
        pacer.observeProcessing(t, t + fromMs(i < 150 ? 12 : 4));
    }
    // 50 fast batches: the P95 window still holds slow samples, budget stays up.
    CHECK(pacer.processingBudget() > fromMs(12));
}
