#pragma once
#include <cstdint>
#include <string>

#include "vela/core/AutoColor.h"
#include "vela/core/FramePacer.h"

namespace vela::core {

// The complete user configuration, in the priority order of the product:
// frame generation, latency, super resolution, HDR, auto colour. The Windows
// host maps it to and from the UI's JSON; clamp() is the single place values
// are validated, whatever wrote them.

struct FrameGenSettings {
    bool enabled = true;
    unsigned multiplier = 2;            // 2..6, further limited by GPU caps and display refresh
    PacingMode pacing = PacingMode::Even;
    float compression = 0.35f;          // LowLatency pacing only
    bool reflex = true;                 // NVIDIA Reflex low-latency mode on the present queue
    bool performanceMode = false;       // DLSS-G "Lite" candidate blending, runtimes that support it
};

struct LatencySettings {
    bool hud = false;                   // on-video overlay
    unsigned hudCorner = 1;             // 0 TL, 1 TR, 2 BL, 3 BR
    float captureHardwareMs = 0;        // calibration for the card, 0 = not calibrated
    float displayProcessingMs = 0;
};

// The engine follows the picture: RTX Video SR (built for video, 8-bit SDR
// only) for SDR, DLSS SR (linear FP16) for HDR10. No user choice needed.
struct SuperResSettings {
    bool enabled = true;                // only acts when the display is larger than the source
    unsigned quality = 4;               // RTX Video SR quality 1..4
    float sharpness = 0.0f;             // DLSS SR, 0..1
};

enum class HdrMode : uint8_t { Auto = 0, Off = 1 };
struct HdrSettings {
    HdrMode mode = HdrMode::Auto;       // Auto: HDR10 passthrough / RTX HDR / tone map as the plan decides
    bool rtxHdr = true;                 // SDR source on an HDR display -> RTX Video HDR
    unsigned peakNits = 1000;           // 400..2000
    unsigned contrast = 100;            // 0..200
    unsigned saturation = 100;          // 0..200
    unsigned middleGray = 50;           // 10..100
};

struct CaptureSettings {
    std::wstring devicePath;            // empty: first Elgato device found
    unsigned width = 0, height = 0;     // 0: best mode for the signal
    double fps = 0;
    bool preferHdr = true;              // P010 when the source sends HDR
    bool audio = true;
    float volume = 1.0f;
};

enum class SyncMode : uint8_t { Vrr = 0, VSync = 1, Tearing = 2 };
struct DisplaySettings {
    SyncMode sync = SyncMode::Vrr;
    bool fullscreen = true;
};

struct Settings {
    FrameGenSettings frameGen;
    LatencySettings latency;
    SuperResSettings superRes;
    HdrSettings hdr;
    AutoColorConfig color{true, 0.5f, true, true, true, AdaptSpeed::Normal};
    CaptureSettings capture;
    DisplaySettings display;

    void clamp();
};

} // namespace vela::core
