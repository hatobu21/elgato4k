#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "vela/core/Time.h"

namespace vela::core {

// Games often render below the HDMI rate: a 30 fps game in a 60 Hz signal
// sends every picture twice, a 40 fps mode in 120 Hz sends it three times.
// Frame generation must interpolate between *different* pictures, so the
// repeats are found and skipped, and the multiplier applies to the content
// rate (30 fps x 4 = 120 fps out).
enum class ContentRate : uint8_t { Auto = 0, Fps30 = 30, Fps60 = 60, Fps120 = 120 };

struct CadenceDecision {
    bool process = false;   // a new picture: run the pipeline on it
    bool reset = false;     // long gap since the previous picture: drop temporal history
};

class ContentCadence {
public:
    void configure(ContentRate mode, double signalFps);
    void reset();

    // One call per captured signal frame. `changed`: the picture differs
    // from the previous signal frame (PictureCompare).
    CadenceDecision onFrame(Micros arrival, bool changed);

    // Rate the pipeline plans with: the fixed rate, or the detected rate once
    // it has been stable for a second; the signal rate until then.
    double contentFps() const;
    double detectedFps() const { return detected_; }   // 0 until measured
    double signalFps() const { return signalFps_; }
    uint64_t repeats() const { return repeats_; }
    ContentRate mode() const { return mode_; }

    // Nearest common game rate within 6 %, else the value itself.
    static double snap(double fps);

private:
    ContentRate mode_ = ContentRate::Auto;
    double signalFps_ = 60;
    std::deque<Micros> intervals_;      // between accepted pictures, pauses excluded
    Micros lastAccepted_ = -1;
    double detected_ = 0, effective_ = 0, candidate_ = 0;
    Micros candidateSince_ = 0;
    uint64_t repeats_ = 0;
};

// Sparse comparison of two captured frames in their raw NV12 / P010 bytes:
// every 8th luma and chroma row, every 4th sample, high byte only for P010.
// About 0.4 M byte compares for a 4K frame. Console repeats are bit exact,
// so the threshold only has to absorb capture noise, not real change.
class PictureCompare {
public:
    struct Layout {
        const uint8_t* data = nullptr;
        size_t bytes = 0;
        uint32_t pitch = 0, chromaOffset = 0, width = 0, height = 0;
        bool p010 = false;
    };
    // Samples `frame` and reports whether it differs from the previous call.
    // The first frame, and any size change, count as different.
    bool changed(const Layout& frame);
    void reset() { previous_.clear(); }
    double lastChangedFraction() const { return fraction_; }

private:
    std::vector<uint8_t> previous_, current_;
    double fraction_ = 0;
};

} // namespace vela::core
