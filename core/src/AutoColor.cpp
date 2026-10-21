#include "vela/core/AutoColor.h"

#include <algorithm>
#include <cmath>

namespace vela::core {
namespace {
float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float luma709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

double timeConstant(AdaptSpeed speed) {
    switch (speed) {
    case AdaptSpeed::Slow: return 2.0;
    case AdaptSpeed::Fast: return 0.3;
    default: return 0.8;
    }
}
constexpr double kSceneCutTau = 0.12;      // converge within a few frames after a cut
constexpr double kSceneCutHold = 0.4;      // seconds the fast constant stays active
constexpr float kSceneCutDistance = 0.6f;  // L1 distance of normalised histograms (0..2)
constexpr double kDiagnosisTau = 2.0;      // problem scores change no faster than this
constexpr float kRecommendBar = 0.5f;      // a problem must score above this to be recommended
constexpr float kRecommendMargin = 0.1f;   // and beat the current recommendation by this much
constexpr float kMaxLift = 0.16f;
constexpr float kMaxShoulder = 0.16f;

// levels, highlights, shadows, saturation, white balance
constexpr std::array<std::array<float, 5>, kColorPresetCount> kPresets = {{
    {0.5f, 0.3f, 0.2f, 0.3f, 0.5f},     // Natural
    {1.0f, 0.2f, 0.0f, 0.5f, 0.5f},     // Washed: raised blacks, dim whites
    {0.5f, 1.0f, 0.0f, 0.0f, 0.4f},     // Overexposed: large near-white areas
    {0.4f, 0.3f, 0.1f, -1.0f, 0.4f},    // Oversaturated
    {0.5f, 0.0f, 1.0f, 0.3f, 0.4f},     // Dark: crushed shadows
    {0.4f, 0.2f, 0.1f, 0.2f, 1.0f},     // Cast
}};

uint64_t totalOf(const std::array<uint32_t, ColorStats::kBins>& h) {
    uint64_t total = 0;
    for (const auto count : h) total += count;
    return total;
}
// Fraction of samples in bins [from, to).
float binFraction(const std::array<uint32_t, ColorStats::kBins>& h, int from, int to, uint64_t total) {
    uint64_t n = 0;
    for (int i = from; i < to; ++i) n += h[static_cast<size_t>(i)];
    return total ? static_cast<float>(static_cast<double>(n) / static_cast<double>(total)) : 0;
}

// Shape measures shared by the correction and the diagnosis (SDR luma).
struct Shape {
    float low, high, median, p75;
    float blown;    // 0..1, large near-white areas
    float dark;     // 0..1, low mid-tones or crushed shadows
};
Shape shapeOf(const ColorStats& stats, uint64_t total) {
    Shape sh;
    sh.low = AutoColor::lumaPercentile(stats.luma, 0.005f);
    sh.high = AutoColor::lumaPercentile(stats.luma, 0.995f);
    sh.median = AutoColor::lumaPercentile(stats.luma, 0.5f);
    sh.p75 = AutoColor::lumaPercentile(stats.luma, 0.75f);
    const float clipped = binFraction(stats.luma, 62, ColorStats::kBins, total);   // luma >= 0.97
    const float crushed = binFraction(stats.luma, 0, 2, total);                     // luma < 0.03
    sh.blown = std::max(smoothstep(0.04f, 0.15f, clipped), smoothstep(0.80f, 0.92f, sh.p75));
    sh.dark = std::max(1 - smoothstep(0.12f, 0.28f, sh.median), smoothstep(0.08f, 0.25f, crushed));
    return sh;
}

// Largest per-channel deviation of the scene's neutral estimate from grey:
// grey world (mid-tones) blended with white patch (highlights), each
// normalised to its own luma so brightness does not weigh in.
bool neutralOf(const ColorStats& stats, float& r, float& g, float& b) {
    const float midY = luma709(stats.midR, stats.midG, stats.midB);
    const float highY = luma709(stats.highR, stats.highG, stats.highB);
    if (midY <= 1e-4f) return false;
    r = stats.midR / midY; g = stats.midG / midY; b = stats.midB / midY;
    if (highY > 1e-4f) {
        r = 0.6f * r + 0.4f * stats.highR / highY;
        g = 0.6f * g + 0.4f * stats.highG / highY;
        b = 0.6f * b + 0.4f * stats.highB / highY;
    }
    return true;
}
} // namespace

float applyLift(float v, float lift) {
    if (v <= 0 || v >= 0.5f) return v;
    const float x = v / 0.5f;
    return v + lift * 0.5f * 6.75f * x * (1 - x) * (1 - x);
}

float applyShoulder(float v, float shoulder) {
    if (v <= 0.5f || v >= 1) return v;
    const float x = (v - 0.5f) / 0.5f;
    return v - shoulder * 0.5f * 6.75f * x * x * (1 - x);
}

AutoColorConfig colorPresetAmounts(ColorPreset preset) {
    const auto& a = kPresets[std::min(static_cast<size_t>(preset), kColorPresetCount - 1)];
    AutoColorConfig c;
    c.preset = preset;
    c.levels = a[0];
    c.highlights = a[1];
    c.shadows = a[2];
    c.saturation = a[3];
    c.whiteBalance = a[4];
    return c;
}

void applyColorPreset(AutoColorConfig& config, ColorPreset preset) {
    const AutoColorConfig a = colorPresetAmounts(preset);
    config.preset = a.preset;
    config.levels = a.levels;
    config.highlights = a.highlights;
    config.shadows = a.shadows;
    config.saturation = a.saturation;
    config.whiteBalance = a.whiteBalance;
}

bool matchesPreset(const AutoColorConfig& c, float t) {
    const AutoColorConfig a = colorPresetAmounts(c.preset);
    return std::fabs(c.levels - a.levels) < t && std::fabs(c.highlights - a.highlights) < t &&
        std::fabs(c.shadows - a.shadows) < t && std::fabs(c.saturation - a.saturation) < t &&
        std::fabs(c.whiteBalance - a.whiteBalance) < t;
}

bool ColorParams::isIdentity(float t) const {
    return std::fabs(blackPoint) < t && std::fabs(whitePoint - 1) < t && std::fabs(gamma - 1) < t &&
        std::fabs(lift) < t && std::fabs(shoulder) < t && std::fabs(gainR - 1) < t && std::fabs(gainG - 1) < t && std::fabs(gainB - 1) < t &&
        std::fabs(saturation - 1) < t;
}

float AutoColor::lumaPercentile(const std::array<uint32_t, ColorStats::kBins>& histogram, float fraction) {
    uint64_t total = 0;
    for (const auto count : histogram) total += count;
    if (total == 0) return 0;
    const double wanted = std::clamp(static_cast<double>(fraction), 0.0, 1.0) * static_cast<double>(total);
    uint64_t below = 0;
    for (int i = 0; i < ColorStats::kBins; ++i) {
        const uint64_t count = histogram[static_cast<size_t>(i)];
        if (count > 0 && static_cast<double>(below + count) >= wanted) {
            const double inside = (wanted - static_cast<double>(below)) / static_cast<double>(count);
            return static_cast<float>((i + std::clamp(inside, 0.0, 1.0)) / ColorStats::kBins);
        }
        below += count;
    }
    return 1;
}

ColorParams AutoColor::estimate(const ColorStats& stats, const AutoColorConfig& config) {
    ColorParams target;
    const uint64_t total = totalOf(stats.luma);
    if (total == 0) return target;
    const float levels = std::clamp(config.levels, 0.0f, 1.0f);
    const float highlights = std::clamp(config.highlights, 0.0f, 1.0f);
    const float shadows = std::clamp(config.shadows, 0.0f, 1.0f);
    const float saturation = std::clamp(config.saturation, -1.0f, 1.0f);
    const float wb = std::clamp(config.whiteBalance, 0.0f, 1.0f);

    if (!stats.hdr) {
        const Shape sh = shapeOf(stats, total);
        if (levels > 0) {
            float black = 0, white = 1, gamma = 1;
            // Raised blacks (wrong range, haze): pull down, never more than 0.06.
            if (sh.low > 0.03f) black = std::min(sh.low - 0.02f, 0.06f);
            // Dim highlights: stretch, but keep at least 12 % headroom.
            if (sh.high < 0.92f) white = std::max(sh.high + 0.04f, 0.88f);
            // Mid-tones inside 0.25..0.60 are normal exposure; outside, nudge
            // towards the nearest edge of that band, never further.
            const float mid = (sh.median - black) / (white - black);
            const float goal = std::clamp(mid, 0.25f, 0.60f);
            if (mid > 0.05f && mid < 0.95f && goal != mid)
                gamma = std::clamp(std::log(goal) / std::log(mid), 0.88f, 1.12f);
            target.blackPoint = lerp(0, black, levels);
            target.whitePoint = lerp(1, white, levels);
            target.gamma = lerp(1, gamma, levels);
        }
        // Detail that is left in near-white areas gets more room; crushed
        // shadows open up. Both only act when the picture asks for it.
        target.shoulder = kMaxShoulder * highlights * sh.blown;
        target.lift = kMaxLift * shadows * sh.dark;
    }

    float r, g, b;
    if (wb > 0 && neutralOf(stats, r, g, b)) {
        const float cast = std::max({std::fabs(r - 1), std::fabs(g - 1), std::fabs(b - 1)});
        // Mild casts are corrected in part; a strong one (sunset, a red alarm
        // room) is taken as intent and fades out of the correction. A higher
        // amount corrects more and accepts stronger casts as mistakes.
        const float weight = 0.8f * wb * (1 - smoothstep(0.12f + 0.10f * wb, 0.30f + 0.15f * wb, cast));
        const float limit = 0.04f + 0.08f * wb;
        const float gr = std::clamp(1 + (1 / std::max(r, 1e-3f) - 1) * weight, 1 - limit, 1 + limit);
        const float gg = std::clamp(1 + (1 / std::max(g, 1e-3f) - 1) * weight, 1 - limit, 1 + limit);
        const float gb = std::clamp(1 + (1 / std::max(b, 1e-3f) - 1) * weight, 1 - limit, 1 + limit);
        const float keep = luma709(gr, gg, gb);   // gains must not change brightness
        target.gainR = gr / keep;
        target.gainG = gg / keep;
        target.gainB = gb / keep;
    }

    const float chroma = std::max(stats.meanChroma, 0.0f);
    if (saturation > 0 && chroma < 0.18f)
        target.saturation = 1 + (0.18f - chroma) / 0.18f * 0.15f * saturation;
    else if (saturation < 0)   // always a little calmer, more when the picture is loud
        target.saturation = 1 + saturation * (0.05f + 0.15f * smoothstep(0.35f, 0.60f, chroma));

    const float s = std::clamp(config.strength, 0.0f, 1.0f);
    ColorParams out;
    out.blackPoint = lerp(0, target.blackPoint, s);
    out.whitePoint = lerp(1, target.whitePoint, s);
    out.gamma = lerp(1, target.gamma, s);
    out.lift = lerp(0, target.lift, s);
    out.shoulder = lerp(0, target.shoulder, s);
    out.gainR = lerp(1, target.gainR, s);
    out.gainG = lerp(1, target.gainG, s);
    out.gainB = lerp(1, target.gainB, s);
    out.saturation = lerp(1, target.saturation, s);
    return out;
}

std::array<float, kColorPresetCount> AutoColor::diagnose(const ColorStats& stats) {
    std::array<float, kColorPresetCount> score{};
    score[static_cast<size_t>(ColorPreset::Natural)] = kRecommendBar;
    const uint64_t total = totalOf(stats.luma);
    if (total == 0) return score;
    if (!stats.hdr) {
        const Shape sh = shapeOf(stats, total);
        // Washed out: raised blacks, or a narrow range that is not just a dark scene.
        const float narrow = 1 - smoothstep(0.55f, 0.75f, sh.high - sh.low);
        score[static_cast<size_t>(ColorPreset::Washed)] = std::max(smoothstep(0.03f, 0.07f, sh.low), narrow * (1 - sh.dark));
        score[static_cast<size_t>(ColorPreset::Overexposed)] = sh.blown;
        score[static_cast<size_t>(ColorPreset::Dark)] = sh.dark;
    }
    score[static_cast<size_t>(ColorPreset::Oversaturated)] = smoothstep(0.35f, 0.60f, stats.meanChroma);
    float r, g, b;
    if (neutralOf(stats, r, g, b)) {
        const float cast = std::max({std::fabs(r - 1), std::fabs(g - 1), std::fabs(b - 1)});
        // A very strong cast is more likely a sunset than a mistake.
        score[static_cast<size_t>(ColorPreset::Cast)] = smoothstep(0.04f, 0.12f, cast) * (1 - smoothstep(0.35f, 0.50f, cast));
    }
    return score;
}

void AutoColor::configure(const AutoColorConfig& config) {
    config_ = config;
    config_.strength = std::clamp(config_.strength, 0.0f, 1.0f);
}

void AutoColor::reset() {
    current_ = target_ = ColorParams{};
    previous_.fill(0);
    havePrevious_ = sceneCut_ = false;
    fastRemaining_ = 0;
    diagnosis_ = {};
    haveDiagnosis_ = false;
}

const ColorParams& AutoColor::update(const ColorStats& stats, double dtSeconds) {
    sceneCut_ = false;
    const uint64_t total = totalOf(stats.luma);
    if (total > 0) {
        std::array<float, ColorStats::kBins> normalised{};
        float distance = 0;
        for (size_t i = 0; i < normalised.size(); ++i) {
            normalised[i] = static_cast<float>(stats.luma[i]) / static_cast<float>(total);
            distance += std::fabs(normalised[i] - previous_[i]);
        }
        sceneCut_ = havePrevious_ && distance > kSceneCutDistance;
        previous_ = normalised;
        havePrevious_ = true;
    }
    if (sceneCut_) fastRemaining_ = kSceneCutHold;

    if (total > 0) {
        const auto instant = diagnose(stats);
        auto& d = diagnosis_;
        if (!haveDiagnosis_) {
            d.scores = instant;
        } else {
            const float a = static_cast<float>(1 - std::exp(-std::max(dtSeconds, 0.0) / kDiagnosisTau));
            for (size_t i = 0; i < kColorPresetCount; ++i) d.scores[i] += (instant[i] - d.scores[i]) * a;
        }
        size_t best = 0;
        for (size_t i = 1; i < kColorPresetCount; ++i)
            if (d.scores[i] > d.scores[best]) best = i;
        const size_t now = static_cast<size_t>(d.recommended);
        if (!haveDiagnosis_ || d.scores[best] > d.scores[now] + kRecommendMargin) d.recommended = static_cast<ColorPreset>(best);
        haveDiagnosis_ = true;
    }

    target_ = config_.enabled ? estimate(stats, config_) : ColorParams{};
    const double tau = fastRemaining_ > 0 ? kSceneCutTau : timeConstant(config_.speed);
    fastRemaining_ = std::max(0.0, fastRemaining_ - dtSeconds);
    const float alpha = static_cast<float>(1 - std::exp(-std::max(dtSeconds, 0.0) / tau));
    auto follow = [alpha](float& value, float goal) { value += (goal - value) * alpha; };
    follow(current_.blackPoint, target_.blackPoint);
    follow(current_.whitePoint, target_.whitePoint);
    follow(current_.gamma, target_.gamma);
    follow(current_.lift, target_.lift);
    follow(current_.shoulder, target_.shoulder);
    follow(current_.gainR, target_.gainR);
    follow(current_.gainG, target_.gainG);
    follow(current_.gainB, target_.gainB);
    follow(current_.saturation, target_.saturation);
    return current_;
}

} // namespace vela::core
