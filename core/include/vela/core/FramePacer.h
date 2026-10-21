#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "vela/core/RollingStats.h"
#include "vela/core/Time.h"

namespace vela::core {

// Even: generated and real frames are spaced T/n apart (T = source interval).
//       The real frame reaches the screen budget + (n-1)/n * T after arrival.
// LowLatency: generated frames are packed tighter so the real frame arrives
//       earlier; motion is slightly uneven, latency drops by (n-1)*(T/n - s).
enum class PacingMode : uint8_t { Even = 0, LowLatency = 1 };

struct PacerConfig {
    unsigned multiplier = 2;        // 1 = frame generation off
    PacingMode mode = PacingMode::Even;
    double compression = 0.35;      // LowLatency only: spacing = T/n * (1 - compression)
    double safetyMarginMs = 1.0;    // added to the observed P95 processing time
    double minGapMs = 0.5;          // two presents are never closer than this
};

struct PlannedFrame {
    Micros target = 0;              // host time the frame should be presented
    unsigned subframe = 0;          // 1..n-1 generated, n = the real frame
    bool real = false;
};

struct BatchPlan {
    std::vector<PlannedFrame> frames;   // presentation order
    unsigned droppedGenerated = 0;      // generated frames skipped to stay on time
    Micros interval = 0;                // source interval estimate used
    Micros budget = 0;                  // processing budget used
    double realLatencyMs = 0;           // planned arrival -> present of the real frame
};

// Schedules the frames of one interpolation batch (generated 1..n-1, then the
// real frame B) against B's capture arrival. Anchoring every batch to its own
// arrival time, instead of to the previous present, keeps latency from
// drifting upwards when one batch runs late: late generated frames are
// dropped, the real frame is never dropped.
class FramePacer {
public:
    void configure(const PacerConfig& config);
    const PacerConfig& config() const { return config_; }
    void reset();

    // arrival -> GPU completion of the whole batch for a finished batch.
    void observeProcessing(Micros arrival, Micros ready);
    BatchPlan plan(Micros arrival, Micros now);

    Micros sourceInterval() const;
    Micros processingBudget() const { return budget_; }

private:
    void observeArrival(Micros arrival);

    PacerConfig config_;
    std::deque<Micros> intervals_;
    Micros lastArrival_ = -1;
    Micros lastTarget_ = INT64_MIN;
    RollingStats processing_{120};
    Micros budget_ = 0;
};

} // namespace vela::core
