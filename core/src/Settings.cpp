#include "vela/core/Settings.h"

#include <algorithm>

namespace vela::core {

void Settings::clamp() {
    frameGen.multiplier = std::clamp(frameGen.multiplier, 2u, 6u);
    if (frameGen.pacing != PacingMode::Even && frameGen.pacing != PacingMode::LowLatency) frameGen.pacing = PacingMode::Even;
    frameGen.compression = std::clamp(frameGen.compression, 0.0f, 0.75f);
    if (frameGen.contentRate != ContentRate::Auto && frameGen.contentRate != ContentRate::Fps30 &&
        frameGen.contentRate != ContentRate::Fps60 && frameGen.contentRate != ContentRate::Fps120)
        frameGen.contentRate = ContentRate::Auto;

    latency.hudCorner = std::min(latency.hudCorner, 3u);
    latency.captureHardwareMs = std::clamp(latency.captureHardwareMs, 0.0f, 100.0f);
    latency.displayProcessingMs = std::clamp(latency.displayProcessingMs, 0.0f, 100.0f);

    superRes.quality = std::clamp(superRes.quality, 1u, 4u);
    superRes.sharpness = std::clamp(superRes.sharpness, 0.0f, 1.0f);

    if (hdr.mode > HdrMode::Off) hdr.mode = HdrMode::Auto;
    hdr.peakNits = std::clamp(hdr.peakNits, 400u, 2000u);
    hdr.contrast = std::min(hdr.contrast, 200u);
    hdr.saturation = std::min(hdr.saturation, 200u);
    hdr.middleGray = std::clamp(hdr.middleGray, 10u, 100u);

    color.strength = std::clamp(color.strength, 0.0f, 1.0f);
    if (color.speed > AdaptSpeed::Fast) color.speed = AdaptSpeed::Normal;

    capture.fps = std::clamp(capture.fps, 0.0, 240.0);
    capture.volume = std::clamp(capture.volume, 0.0f, 1.0f);
    if (display.sync > SyncMode::Tearing) display.sync = SyncMode::Vrr;
}

} // namespace vela::core
