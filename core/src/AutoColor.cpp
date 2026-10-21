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
} // namespace

bool ColorParams::isIdentity(float t) const {
    return std::fabs(blackPoint) < t && std::fabs(whitePoint - 1) < t && std::fabs(gamma - 1) < t &&
        std::fabs(gainR - 1) < t && std::fabs(gainG - 1) < t && std::fabs(gainB - 1) < t &&
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
    uint64_t total = 0;
    for (const auto count : stats.luma) total += count;
    if (total == 0) return target;

    if (config.levels && !stats.hdr) {
        const float low = lumaPercentile(stats.luma, 0.005f);
        const float high = lumaPercentile(stats.luma, 0.995f);
        const float median = lumaPercentile(stats.luma, 0.5f);
        // Raised blacks (wrong range, haze): pull down, never more than 0.06.
        if (low > 0.03f) target.blackPoint = std::min(low - 0.02f, 0.06f);
        // Dim highlights: stretch, but keep at least 12 % headroom.
        if (high < 0.92f) target.whitePoint = std::max(high + 0.04f, 0.88f);
        // Mid-tones inside 0.25..0.60 are normal exposure; outside, nudge
        // towards the nearest edge of that band, never further.
        const float mid = (median - target.blackPoint) / (target.whitePoint - target.blackPoint);
        const float goal = std::clamp(mid, 0.25f, 0.60f);
        if (mid > 0.05f && mid < 0.95f && goal != mid)
            target.gamma = std::clamp(std::log(goal) / std::log(mid), 0.88f, 1.12f);
    }

    if (config.whiteBalance) {
        // Blend grey world (mid-tones) with white patch (highlights), each
        // normalised to its own luma so brightness does not weigh in.
        const float midY = luma709(stats.midR, stats.midG, stats.midB);
        const float highY = luma709(stats.highR, stats.highG, stats.highB);
        if (midY > 1e-4f) {
            float r = stats.midR / midY, g = stats.midG / midY, b = stats.midB / midY;
            if (highY > 1e-4f) {
                r = 0.6f * r + 0.4f * stats.highR / highY;
                g = 0.6f * g + 0.4f * stats.highG / highY;
                b = 0.6f * b + 0.4f * stats.highB / highY;
            }
            const float cast = std::max({std::fabs(r - 1), std::fabs(g - 1), std::fabs(b - 1)});
            // Mild casts are corrected by half; a strong one (sunset, a red
            // alarm room) is taken as intent and fades out of the correction.
            const float weight = 0.5f * (1 - smoothstep(0.12f, 0.30f, cast));
            float gr = std::clamp(1 + (1 / std::max(r, 1e-3f) - 1) * weight, 0.92f, 1.08f);
            float gg = std::clamp(1 + (1 / std::max(g, 1e-3f) - 1) * weight, 0.92f, 1.08f);
            float gb = std::clamp(1 + (1 / std::max(b, 1e-3f) - 1) * weight, 0.92f, 1.08f);
            const float keep = luma709(gr, gg, gb);   // gains must not change brightness
            target.gainR = gr / keep;
            target.gainG = gg / keep;
            target.gainB = gb / keep;
        }
    }

    if (config.vibrance && stats.meanChroma < 0.18f)
        target.saturation = 1 + (0.18f - std::max(stats.meanChroma, 0.0f)) / 0.18f * 0.12f;

    const float s = std::clamp(config.strength, 0.0f, 1.0f);
    ColorParams out;
    out.blackPoint = lerp(0, target.blackPoint, s);
    out.whitePoint = lerp(1, target.whitePoint, s);
    out.gamma = lerp(1, target.gamma, s);
    out.gainR = lerp(1, target.gainR, s);
    out.gainG = lerp(1, target.gainG, s);
    out.gainB = lerp(1, target.gainB, s);
    out.saturation = lerp(1, target.saturation, s);
    return out;
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
}

const ColorParams& AutoColor::update(const ColorStats& stats, double dtSeconds) {
    sceneCut_ = false;
    uint64_t total = 0;
    for (const auto count : stats.luma) total += count;
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

    target_ = config_.enabled ? estimate(stats, config_) : ColorParams{};
    const double tau = fastRemaining_ > 0 ? kSceneCutTau : timeConstant(config_.speed);
    fastRemaining_ = std::max(0.0, fastRemaining_ - dtSeconds);
    const float alpha = static_cast<float>(1 - std::exp(-std::max(dtSeconds, 0.0) / tau));
    auto follow = [alpha](float& value, float goal) { value += (goal - value) * alpha; };
    follow(current_.blackPoint, target_.blackPoint);
    follow(current_.whitePoint, target_.whitePoint);
    follow(current_.gamma, target_.gamma);
    follow(current_.gainR, target_.gainR);
    follow(current_.gainG, target_.gainG);
    follow(current_.gainB, target_.gainB);
    follow(current_.saturation, target_.saturation);
    return current_;
}

} // namespace vela::core
