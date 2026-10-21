#include "vela/core/ContentCadence.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vela::core {
namespace {
constexpr size_t kWindow = 24;                 // accepted intervals for the median
constexpr Micros kPause = 100000;              // longer gaps are pauses, not cadence
constexpr Micros kStableFor = 1000000;         // a new detected rate must hold this long
constexpr double kRates[] = {24, 25, 30, 40, 48, 50, 60, 90, 100, 120};
} // namespace

double ContentCadence::snap(double fps) {
    for (const double r : kRates)
        if (std::fabs(fps - r) <= r * 0.06) return r;
    return std::round(fps);
}

void ContentCadence::configure(ContentRate mode, double signalFps) {
    if (mode != mode_ || std::fabs(signalFps - signalFps_) > 0.5) reset();
    mode_ = mode;
    signalFps_ = signalFps > 1 ? signalFps : 60;
}

void ContentCadence::reset() {
    intervals_.clear();
    lastAccepted_ = -1;
    detected_ = effective_ = candidate_ = 0;
    candidateSince_ = 0;
    repeats_ = 0;
}

double ContentCadence::contentFps() const {
    if (mode_ != ContentRate::Auto) return std::min(double(mode_), signalFps_);
    return effective_ > 0 ? std::min(effective_, signalFps_) : signalFps_;
}

CadenceDecision ContentCadence::onFrame(Micros arrival, bool changed) {
    CadenceDecision d;
    const double rate = contentFps();
    const Micros expected = Micros(1e6 / rate);
    const bool everyFrame = mode_ != ContentRate::Auto && double(mode_) >= signalFps_ * 0.95;

    if (lastAccepted_ < 0) {
        d.process = true;
    } else if (everyFrame) {
        d.process = true;
    } else if (mode_ == ContentRate::Auto) {
        d.process = changed;
    } else {
        // Fixed rate: a change sooner than 3/4 of the content interval is a
        // HUD or overlay running at the signal rate, not a new game frame.
        d.process = changed && arrival - lastAccepted_ >= expected * 3 / 4;
    }
    if (!d.process) {
        ++repeats_;
        return d;
    }

    if (lastAccepted_ >= 0) {
        const Micros gap = arrival - lastAccepted_;
        // After a pause (static menu, loading screen) the two pictures are
        // unrelated in time; interpolating between them only smears.
        d.reset = gap > expected * 5 / 2;
        if (gap < kPause) {
            intervals_.push_back(gap);
            if (intervals_.size() > kWindow) intervals_.pop_front();
        }
    }
    lastAccepted_ = arrival;

    if (intervals_.size() >= 8) {
        std::vector<Micros> v(intervals_.begin(), intervals_.end());
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        detected_ = std::min(snap(1e6 / double(v[v.size() / 2])), snap(signalFps_));
        if (detected_ != candidate_) {
            candidate_ = detected_;
            candidateSince_ = arrival;
        }
        if (effective_ == 0 || (candidate_ != effective_ && arrival - candidateSince_ >= kStableFor))
            effective_ = candidate_;
    }
    return d;
}

bool PictureCompare::changed(const Layout& f) {
    current_.clear();
    if (!f.data || !f.pitch || !f.width || !f.height) return true;
    const uint32_t step = f.p010 ? 8 : 4;           // every 4th sample
    const uint32_t offset = f.p010 ? 1 : 0;         // P010: high byte of each sample
    const uint32_t rowBytes = f.width * (f.p010 ? 2 : 1);
    auto sampleRows = [&](size_t base, uint32_t rows) {
        for (uint32_t y = 0; y < rows; y += 8) {
            const size_t row = base + size_t(y) * f.pitch;
            if (row + rowBytes > f.bytes) return;
            for (uint32_t x = offset; x < rowBytes; x += step) current_.push_back(f.data[row + x]);
        }
    };
    sampleRows(0, f.height);
    sampleRows(f.chromaOffset, f.height / 2);

    bool result = true;
    if (previous_.size() == current_.size() && !current_.empty()) {
        size_t differing = 0;
        for (size_t i = 0; i < current_.size(); ++i)
            if (std::abs(int(current_[i]) - int(previous_[i])) > 2) ++differing;
        fraction_ = double(differing) / double(current_.size());
        // 0.05 % of samples (about 200 for 4K) or 16, whichever is larger.
        result = differing > std::max<size_t>(16, current_.size() / 2000);
    } else {
        fraction_ = 1;
    }
    previous_.swap(current_);
    return result;
}

} // namespace vela::core
