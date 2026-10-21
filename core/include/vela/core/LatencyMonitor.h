#pragma once
#include <array>
#include <cstdint>
#include <deque>

#include "vela/core/RollingStats.h"
#include "vela/core/Time.h"

namespace vela::core {

// GPU stages measured with D3D12 timestamp queries. Durations are in the GPU
// clock domain and are reported as milliseconds; an inactive stage is < 0.
enum class GpuStage : uint8_t {
    Upload = 0,     // capture bytes -> linear RGB (+ flow input, + histogram)
    Flow,           // NVOF optical flow + densify to motion vectors
    Color,          // auto colour apply + working-format encode
    SuperRes,       // RTX Video SR / DLSS SR
    Hdr,            // RTX Video HDR (SDR -> HDR) or HDR tone mapping
    FrameGen,       // all DLSS-G evaluations of one batch
    Output,         // copy/scale into the swap chain back buffer
    Count
};
const char* gpuStageKey(GpuStage stage);

struct LatencyConfig {
    // Capture card internal delay (HDMI in -> driver callback). Not measurable
    // in software; 0 means "not calibrated" and the estimate says so.
    double captureHardwareMs = 0;
    // Monitor input lag beyond scan-out (vendor spec or a user measurement).
    double displayProcessingMs = 0;
    double refreshHz = 60;
};

struct LatencySnapshot {
    struct Stage { double p50 = -1, p95 = -1; };   // -1: stage inactive

    double sourceFps = 0, outputFps = 0;
    RollingStats::Summary sourceInterval;   // ms between capture callbacks
    RollingStats::Summary captureCopy;      // ms, CPU: callback -> upload ring
    RollingStats::Summary pipeline;         // ms, capture callback -> Present of the real frame
    RollingStats::Summary presentInterval;  // ms between any two presents
    RollingStats::Summary gpuTotal;         // ms, GPU span of one batch
    std::array<Stage, static_cast<size_t>(GpuStage::Count)> stages;

    double scanoutMs = 0;       // average half refresh period
    double endToEndMs = 0;      // estimate: hardware + pipeline P50 + scan-out + display
    double fgAddedMs = 0;       // estimate of what frame generation adds
    bool calibrated = false;    // captureHardwareMs was set

    uint64_t sourceFrames = 0, droppedSource = 0;
    uint64_t presented = 0, presentedGenerated = 0, droppedGenerated = 0;
};

// Collects per-frame timing from the capture, GPU and present threads.
// Callers serialise access (the engine owns one instance behind its mutex).
class LatencyMonitor {
public:
    void configure(const LatencyConfig& config) { config_ = config; }
    const LatencyConfig& config() const { return config_; }
    void reset();

    void setFrameGeneration(unsigned multiplier) { multiplier_ = multiplier; }

    void onSourceFrame(Micros arrival, double copyMs);
    void onSourceDropped(uint64_t count) { droppedSource_ += count; }
    void onGpuBatch(Micros now, const std::array<double, static_cast<size_t>(GpuStage::Count)>& stageMs, double totalMs);
    void onPresent(Micros presentTime, Micros sourceArrival, bool generated);
    void onGeneratedDropped(unsigned count) { droppedGenerated_ += count; }

    LatencySnapshot snapshot(Micros now) const;

private:
    static double rate(const std::deque<Micros>& times, Micros now);

    LatencyConfig config_;
    unsigned multiplier_ = 1;
    RollingStats sourceInterval_{240}, captureCopy_{240}, pipeline_{240}, presentInterval_{960}, gpuTotal_{240};
    std::array<RollingStats, static_cast<size_t>(GpuStage::Count)> stages_{
        RollingStats(240), RollingStats(240), RollingStats(240), RollingStats(240),
        RollingStats(240), RollingStats(240), RollingStats(240)};
    std::array<Micros, static_cast<size_t>(GpuStage::Count)> stageSeen_{};
    std::deque<Micros> sourceTimes_, presentTimes_;
    Micros lastSource_ = -1, lastPresent_ = -1;
    uint64_t sourceFrames_ = 0, droppedSource_ = 0, presented_ = 0, presentedGenerated_ = 0, droppedGenerated_ = 0;
};

} // namespace vela::core
