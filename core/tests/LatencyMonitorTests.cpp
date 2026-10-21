#include "Check.h"
#include "vela/core/LatencyMonitor.h"

using namespace vela::core;

namespace {
constexpr size_t idx(GpuStage s) { return static_cast<size_t>(s); }
}

TEST(latency_monitor_rates_and_pipeline) {
    LatencyMonitor m;
    m.configure({3.0, 2.0, 120.0});
    m.setFrameGeneration(2);
    Micros now = 0;
    for (int i = 0; i < 120; ++i) {
        const Micros arrival = fromMs(i * 1000.0 / 60);
        m.onSignalFrame(arrival);
        m.onSourceFrame(arrival, 1.1);
        std::array<double, idx(GpuStage::Count)> stages;
        stages.fill(-1);
        stages[idx(GpuStage::Upload)] = 0.2;
        stages[idx(GpuStage::Flow)] = 0.9;
        stages[idx(GpuStage::FrameGen)] = 2.5;
        m.onGpuBatch(arrival, stages, 5.0);
        // Generated frame half an interval before the real one, real at +20 ms.
        m.onPresent(arrival + fromMs(20 - 1000.0 / 120), arrival, true);
        m.onPresent(arrival + fromMs(20), arrival, false);
        now = arrival + fromMs(20);
    }
    const auto s = m.snapshot(now);
    CHECK_NEAR(s.sourceFps, 60, 0.5);
    CHECK_NEAR(s.signalFps, 60, 0.5);
    CHECK_NEAR(s.outputFps, 120, 1.0);
    CHECK_NEAR(s.pipeline.p50, 20, 0.01);
    CHECK_NEAR(s.sourceInterval.p50, 16.667, 0.01);
    CHECK_NEAR(s.scanoutMs, 500.0 / 120, 1e-6);
    CHECK_NEAR(s.endToEndMs, 3 + 20 + 500.0 / 120 + 2, 0.01);
    CHECK_NEAR(s.fgAddedMs, 16.667 / 2 + 2.5, 0.01);
    CHECK(s.calibrated);
    CHECK(s.presented == 240 && s.presentedGenerated == 120);
    CHECK(s.stages[idx(GpuStage::SuperRes)].p50 < 0);           // never reported -> inactive
    CHECK_NEAR(s.stages[idx(GpuStage::Flow)].p50, 0.9, 1e-9);
}

TEST(latency_monitor_marks_stale_stages_inactive) {
    LatencyMonitor m;
    std::array<double, idx(GpuStage::Count)> stages;
    stages.fill(-1);
    stages[idx(GpuStage::SuperRes)] = 1.5;
    m.onGpuBatch(fromMs(100), stages, 1.5);
    CHECK(m.snapshot(fromMs(500)).stages[idx(GpuStage::SuperRes)].p50 > 0);
    CHECK(m.snapshot(fromMs(1500)).stages[idx(GpuStage::SuperRes)].p50 < 0);
}

TEST(latency_monitor_no_fg_adds_nothing) {
    LatencyMonitor m;
    m.setFrameGeneration(1);
    for (int i = 0; i < 30; ++i) m.onSourceFrame(fromMs(i * 16.667), 1.0);
    CHECK(m.snapshot(fromMs(500)).fgAddedMs == 0);
    CHECK(!m.snapshot(fromMs(500)).calibrated);
}
