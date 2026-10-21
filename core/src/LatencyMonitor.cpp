#include "vela/core/LatencyMonitor.h"

namespace vela::core {
namespace {
constexpr Micros kRateWindow = 1000000;     // fps over the last second
constexpr Micros kStageStale = 1000000;     // a stage unseen for 1 s is inactive

void trim(std::deque<Micros>& times, Micros now) {
    while (!times.empty() && times.front() < now - kRateWindow) times.pop_front();
}
} // namespace

const char* gpuStageKey(GpuStage stage) {
    switch (stage) {
    case GpuStage::Upload: return "upload";
    case GpuStage::Flow: return "flow";
    case GpuStage::Color: return "color";
    case GpuStage::SuperRes: return "superRes";
    case GpuStage::Hdr: return "hdr";
    case GpuStage::FrameGen: return "frameGen";
    case GpuStage::Output: return "output";
    default: return "unknown";
    }
}

void LatencyMonitor::reset() {
    sourceInterval_.clear();
    captureCopy_.clear();
    pipeline_.clear();
    presentInterval_.clear();
    gpuTotal_.clear();
    for (auto& s : stages_) s.clear();
    stageSeen_.fill(0);
    signalTimes_.clear();
    sourceTimes_.clear();
    presentTimes_.clear();
    lastSource_ = lastPresent_ = -1;
    sourceFrames_ = droppedSource_ = presented_ = presentedGenerated_ = droppedGenerated_ = 0;
}

void LatencyMonitor::onSignalFrame(Micros arrival) {
    signalTimes_.push_back(arrival);
    trim(signalTimes_, arrival);
}

void LatencyMonitor::onSourceFrame(Micros arrival, double copyMs) {
    if (lastSource_ >= 0 && arrival > lastSource_) sourceInterval_.add(toMs(arrival - lastSource_));
    lastSource_ = arrival;
    captureCopy_.add(copyMs);
    sourceTimes_.push_back(arrival);
    trim(sourceTimes_, arrival);
    ++sourceFrames_;
}

void LatencyMonitor::onGpuBatch(Micros now, const std::array<double, static_cast<size_t>(GpuStage::Count)>& stageMs, double totalMs) {
    for (size_t i = 0; i < stageMs.size(); ++i) {
        if (stageMs[i] < 0) continue;
        stages_[i].add(stageMs[i]);
        stageSeen_[i] = now;
    }
    if (totalMs >= 0) gpuTotal_.add(totalMs);
}

void LatencyMonitor::onPresent(Micros presentTime, Micros sourceArrival, bool generated) {
    if (lastPresent_ >= 0 && presentTime > lastPresent_) presentInterval_.add(toMs(presentTime - lastPresent_));
    lastPresent_ = presentTime;
    presentTimes_.push_back(presentTime);
    trim(presentTimes_, presentTime);
    ++presented_;
    if (generated) {
        ++presentedGenerated_;
    } else if (presentTime >= sourceArrival) {
        // Only real frames: a generated frame mixes two source frames, so
        // "its" age is not an input-to-photon latency.
        pipeline_.add(toMs(presentTime - sourceArrival));
    }
}

double LatencyMonitor::rate(const std::deque<Micros>& times, Micros now) {
    size_t count = 0;
    Micros first = -1, last = -1;
    for (const Micros t : times) {
        if (t < now - kRateWindow) continue;
        if (first < 0) first = t;
        last = t;
        ++count;
    }
    if (count < 2 || last <= first) return 0;
    return static_cast<double>(count - 1) * 1e6 / static_cast<double>(last - first);
}

LatencySnapshot LatencyMonitor::snapshot(Micros now) const {
    LatencySnapshot s;
    s.signalFps = rate(signalTimes_, now);
    s.sourceFps = rate(sourceTimes_, now);
    s.outputFps = rate(presentTimes_, now);
    s.sourceInterval = sourceInterval_.summary();
    s.captureCopy = captureCopy_.summary();
    s.pipeline = pipeline_.summary();
    s.presentInterval = presentInterval_.summary();
    s.gpuTotal = gpuTotal_.summary();
    for (size_t i = 0; i < stages_.size(); ++i) {
        if (stages_[i].size() == 0 || now - stageSeen_[i] > kStageStale) continue;
        const auto summary = stages_[i].summary();
        s.stages[i] = {summary.p50, summary.p95};
    }

    s.scanoutMs = config_.refreshHz > 0 ? 500.0 / config_.refreshHz : 0;
    s.calibrated = config_.captureHardwareMs > 0;
    s.endToEndMs = config_.captureHardwareMs + s.pipeline.p50 + s.scanoutMs + config_.displayProcessingMs;

    const auto& fg = s.stages[static_cast<size_t>(GpuStage::FrameGen)];
    if (multiplier_ > 1 && s.sourceInterval.count > 0) {
        // The real frame waits (n-1)/n of a source interval behind its own
        // generated frames, plus the generation work itself.
        const double n = static_cast<double>(multiplier_);
        s.fgAddedMs = (n - 1) / n * s.sourceInterval.p50 + (fg.p50 > 0 ? fg.p50 : 0);
    }

    s.sourceFrames = sourceFrames_;
    s.droppedSource = droppedSource_;
    s.presented = presented_;
    s.presentedGenerated = presentedGenerated_;
    s.droppedGenerated = droppedGenerated_;
    return s;
}

} // namespace vela::core
