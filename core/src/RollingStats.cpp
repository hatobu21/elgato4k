#include "vela/core/RollingStats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vela::core {
namespace {
size_t rankIndex(double p, size_t n) {
    // Nearest rank: the smallest value with at least p% of samples <= it.
    const double rank = std::ceil(std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(n));
    return static_cast<size_t>(std::max(rank, 1.0)) - 1;
}
} // namespace

RollingStats::RollingStats(size_t capacity) : samples_(std::max<size_t>(capacity, 1)) {}

void RollingStats::add(double value) {
    samples_[head_] = value;
    head_ = (head_ + 1) % samples_.size();
    if (size_ < samples_.size()) ++size_;
    last_ = value;
}

void RollingStats::clear() {
    head_ = size_ = 0;
    last_ = 0;
}

double RollingStats::percentile(double p) const {
    if (size_ == 0) return 0;
    std::vector<double> copy(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(size_));
    const auto at = copy.begin() + static_cast<std::ptrdiff_t>(rankIndex(p, size_));
    std::nth_element(copy.begin(), at, copy.end());
    return *at;
}

RollingStats::Summary RollingStats::summary() const {
    Summary s;
    s.count = size_;
    if (size_ == 0) return s;
    std::vector<double> copy(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(size_));
    std::sort(copy.begin(), copy.end());
    s.mean = std::accumulate(copy.begin(), copy.end(), 0.0) / static_cast<double>(size_);
    s.min = copy.front();
    s.max = copy.back();
    s.p50 = copy[rankIndex(50, size_)];
    s.p95 = copy[rankIndex(95, size_)];
    s.p99 = copy[rankIndex(99, size_)];
    return s;
}

} // namespace vela::core
