#pragma once
#include <cstddef>
#include <vector>

namespace vela::core {

// Fixed-capacity window of the most recent samples. add() is O(1) and runs on
// the frame path; summary() copies and partially sorts, so it belongs on the
// telemetry path (about 10 Hz), never per frame.
class RollingStats {
public:
    explicit RollingStats(size_t capacity = 600);

    void add(double value);
    void clear();
    size_t size() const { return size_; }
    size_t capacity() const { return samples_.size(); }
    double last() const { return last_; }

    struct Summary {
        size_t count = 0;
        double mean = 0, p50 = 0, p95 = 0, p99 = 0, min = 0, max = 0;
    };
    Summary summary() const;
    // Nearest-rank percentile, p in [0, 100]. Returns 0 for an empty window.
    double percentile(double p) const;

private:
    std::vector<double> samples_;
    size_t head_ = 0, size_ = 0;
    double last_ = 0;
};

} // namespace vela::core
