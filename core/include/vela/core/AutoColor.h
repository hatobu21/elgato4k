#pragma once
#include <array>
#include <cstdint>

namespace vela::core {

// Statistics the GPU analysis pass writes for one frame (read back two frames
// later, never waited on). Luma is the encoded (gamma / PQ) value in [0, 1].
struct ColorStats {
    static constexpr int kBins = 64;
    std::array<uint32_t, kBins> luma{};
    // Mean linear RGB of mid-tone pixels (luma 0.15..0.85), the grey-world sample.
    float midR = 0, midG = 0, midB = 0;
    // Mean linear RGB of the brightest unclipped pixels, the white-patch sample.
    float highR = 0, highG = 0, highB = 0;
    // Mean (max - min) / max of mid-tone pixels: 0 grey, 1 fully saturated.
    float meanChroma = 0;
    bool hdr = false;
};

// Parameters consumed by the colour apply shader. Levels and gamma work on
// encoded SDR values; gains on linear light; saturation around luma.
struct ColorParams {
    float blackPoint = 0, whitePoint = 1, gamma = 1;
    float gainR = 1, gainG = 1, gainB = 1;
    float saturation = 1;
    bool isIdentity(float tolerance = 1e-3f) const;
};

enum class AdaptSpeed : uint8_t { Slow = 0, Normal = 1, Fast = 2 };

struct AutoColorConfig {
    bool enabled = false;
    float strength = 0.6f;      // 0 = analysis only, 1 = full correction
    bool levels = true;         // black / white point and mid-tone gamma (SDR only)
    bool whiteBalance = true;
    bool vibrance = true;
    AdaptSpeed speed = AdaptSpeed::Normal;
};

// Conservative automatic correction for console capture: lift washed-out
// blacks, recover dim highlights, take out a mild colour cast and add a little
// saturation to flat pictures. Strong casts and very dark or bright scenes are
// treated as artistic intent and mostly left alone. Every output is clamped
// near identity and changes slowly, so it never visibly "pumps".
class AutoColor {
public:
    void configure(const AutoColorConfig& config);
    const AutoColorConfig& config() const { return config_; }
    void reset();

    // dtSeconds: time since the previous update. Returns the smoothed parameters.
    const ColorParams& update(const ColorStats& stats, double dtSeconds);
    const ColorParams& current() const { return current_; }
    const ColorParams& target() const { return target_; }
    bool lastWasSceneCut() const { return sceneCut_; }

    // Instantaneous target for one frame, before temporal smoothing.
    static ColorParams estimate(const ColorStats& stats, const AutoColorConfig& config);
    // Fraction of samples at or below `value` -> encoded luma (linear inside a bin).
    static float lumaPercentile(const std::array<uint32_t, ColorStats::kBins>& histogram, float fraction);

private:
    AutoColorConfig config_;
    ColorParams current_, target_;
    std::array<float, ColorStats::kBins> previous_{};
    bool havePrevious_ = false, sceneCut_ = false;
    double fastRemaining_ = 0;
};

} // namespace vela::core
