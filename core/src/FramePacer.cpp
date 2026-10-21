#include "vela/core/FramePacer.h"

#include <algorithm>

namespace vela::core {
namespace {
constexpr Micros kDefaultInterval = 16667;   // 60 Hz until measured
constexpr Micros kDefaultBudget = 8000;      // until 8 batches were observed
constexpr size_t kIntervalWindow = 32;
} // namespace

void FramePacer::configure(const PacerConfig& config) {
    const bool rescheduled = config.multiplier != config_.multiplier || config.mode != config_.mode;
    config_ = config;
    config_.multiplier = std::clamp(config_.multiplier, 1u, 6u);
    config_.compression = std::clamp(config_.compression, 0.0, 0.75);
    config_.safetyMarginMs = std::clamp(config_.safetyMarginMs, 0.0, 10.0);
    config_.minGapMs = std::clamp(config_.minGapMs, 0.1, 4.0);
    if (rescheduled) lastTarget_ = INT64_MIN;
}

void FramePacer::reset() {
    intervals_.clear();
    lastArrival_ = -1;
    lastTarget_ = INT64_MIN;
    processing_.clear();
    budget_ = 0;
}

void FramePacer::observeArrival(Micros arrival) {
    if (lastArrival_ >= 0 && arrival > lastArrival_) {
        const Micros delta = arrival - lastArrival_;
        // A gap over 200 ms is a signal loss or a pause, not a frame interval.
        if (delta < 200000) {
            intervals_.push_back(delta);
            if (intervals_.size() > kIntervalWindow) intervals_.pop_front();
        }
    }
    lastArrival_ = arrival;
}

Micros FramePacer::sourceInterval() const {
    if (intervals_.size() < 2) return kDefaultInterval;
    // Median: a dropped capture frame doubles one interval and must not move it.
    std::vector<Micros> copy(intervals_.begin(), intervals_.end());
    std::nth_element(copy.begin(), copy.begin() + static_cast<std::ptrdiff_t>(copy.size() / 2), copy.end());
    return std::clamp<Micros>(copy[copy.size() / 2], 4000, 50000);
}

void FramePacer::observeProcessing(Micros arrival, Micros ready) {
    if (ready > arrival) processing_.add(toMs(ready - arrival));
}

BatchPlan FramePacer::plan(Micros arrival, Micros now) {
    observeArrival(arrival);
    BatchPlan out;
    out.interval = sourceInterval();
    const unsigned n = config_.multiplier;
    const Micros minGap = fromMs(config_.minGapMs);

    // Processing budget: rises at once, falls by at most 1/8 of a subframe per
    // batch, so a short fast stretch does not pull batches into each other.
    const Micros measured = processing_.size() >= 8
        ? fromMs(processing_.percentile(95) + config_.safetyMarginMs) : kDefaultBudget;
    const Micros fallStep = std::max<Micros>(out.interval / static_cast<Micros>(n) / 8, 100);
    budget_ = budget_ == 0 ? measured : std::max(measured, budget_ - fallStep);
    out.budget = budget_;

    if (n == 1) {
        // No generation: present as soon as the GPU is done.
        const Micros target = std::max(now, lastTarget_ == INT64_MIN ? now : lastTarget_ + minGap);
        out.frames.push_back({target, 1, true});
        lastTarget_ = target;
        out.realLatencyMs = toMs(target - arrival);
        return out;
    }

    const double share = config_.mode == PacingMode::Even ? 1.0 : 1.0 - config_.compression;
    const Micros spacing = std::max<Micros>(minGap, static_cast<Micros>(
        static_cast<double>(out.interval) / n * share));
    const Micros start = arrival + budget_;
    const Micros earliest = lastTarget_ == INT64_MIN ? INT64_MIN : lastTarget_ + minGap;

    for (unsigned k = 1; k < n; ++k) {
        const Micros target = start + static_cast<Micros>(k - 1) * spacing;
        // Too close to the previous batch, or already half a slot late.
        if (target < earliest || target < now - spacing / 2) {
            ++out.droppedGenerated;
            continue;
        }
        out.frames.push_back({target, k, false});
    }
    Micros realTarget = start + static_cast<Micros>(n - 1) * spacing;
    if (!out.frames.empty()) realTarget = std::max(realTarget, out.frames.back().target + minGap);
    if (earliest != INT64_MIN) realTarget = std::max(realTarget, earliest);
    out.frames.push_back({realTarget, n, true});
    lastTarget_ = realTarget;
    out.realLatencyMs = toMs(realTarget - arrival);
    return out;
}

} // namespace vela::core
