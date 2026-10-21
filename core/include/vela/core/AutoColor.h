#pragma once
#include <array>
#include <cstddef>
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

// Parameters consumed by the colour apply shader. Levels, gamma, lift and
// shoulder work on encoded SDR values; gains on linear light; saturation
// around luma.
struct ColorParams {
    float blackPoint = 0, whitePoint = 1, gamma = 1;
    float lift = 0;         // shadow toe below encoded 0.5, max raise lift / 2 (SDR only)
    float shoulder = 0;     // highlight roll-down above encoded 0.5, max drop shoulder / 2 (SDR only)
    float gainR = 1, gainG = 1, gainB = 1;
    float saturation = 1;
    bool isIdentity(float tolerance = 1e-3f) const;
};

// Both curves leave 0, 0.5 and 1 in place; kept in step with ColorApply.hlsl.
float applyLift(float encoded, float lift);
float applyShoulder(float encoded, float shoulder);

enum class AdaptSpeed : uint8_t { Slow = 0, Normal = 1, Fast = 2 };

// A starting point for the fine-tune amounts, one per typical capture
// problem. Natural doubles as "no problem found" in the diagnosis.
enum class ColorPreset : uint8_t { Natural = 0, Washed, Overexposed, Oversaturated, Dark, Cast };
constexpr std::size_t kColorPresetCount = 6;

struct AutoColorConfig {
    bool enabled = false;
    ColorPreset preset = ColorPreset::Natural;
    float strength = 1;         // overall, 0 = analysis only
    // Per correction, 0 = off, 1 = the largest correction it ever makes.
    float levels = 0.5f;        // black / white point and mid-tone gamma (SDR)
    float highlights = 0.3f;    // roll down bright, near-clipped areas (SDR)
    float shadows = 0.2f;       // open up dark, crushed areas (SDR)
    float saturation = 0.3f;    // -1 calm an oversaturated picture .. +1 enrich a flat one
    float whiteBalance = 0.5f;
    AdaptSpeed speed = AdaptSpeed::Normal;
};

// The amounts a preset stands for (enabled, strength and speed untouched).
AutoColorConfig colorPresetAmounts(ColorPreset preset);
// Sets the preset and overwrites the five amounts with its values.
void applyColorPreset(AutoColorConfig& config, ColorPreset preset);
bool matchesPreset(const AutoColorConfig& config, float tolerance = 0.005f);

// How strongly the source picture shows each problem, 0..1, indexed by
// preset (Natural holds the bar a problem must clear to be recommended).
struct ColorDiagnosis {
    std::array<float, kColorPresetCount> scores{};
    ColorPreset recommended = ColorPreset::Natural;
};

// Conservative automatic correction for console capture: pull down
// washed-out blacks, recover dim highlights, roll down blown ones, open up
// crushed shadows, take out a mild colour cast and even out saturation. Strong casts and very dark or bright scenes are
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
    // Smoothed over about two seconds, measured on the source picture, so
    // it holds while a correction is applied.
    const ColorDiagnosis& diagnosis() const { return diagnosis_; }

    // Instantaneous target for one frame, before temporal smoothing.
    static ColorParams estimate(const ColorStats& stats, const AutoColorConfig& config);
    // Problem scores for one frame, before smoothing.
    static std::array<float, kColorPresetCount> diagnose(const ColorStats& stats);
    // Fraction of samples at or below `value` -> encoded luma (linear inside a bin).
    static float lumaPercentile(const std::array<uint32_t, ColorStats::kBins>& histogram, float fraction);

private:
    AutoColorConfig config_;
    ColorParams current_, target_;
    ColorDiagnosis diagnosis_;
    bool haveDiagnosis_ = false;
    std::array<float, ColorStats::kBins> previous_{};
    bool havePrevious_ = false, sceneCut_ = false;
    double fastRemaining_ = 0;
};

} // namespace vela::core
