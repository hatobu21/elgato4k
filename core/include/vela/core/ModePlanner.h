#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "vela/core/Settings.h"

namespace vela::core {

struct CaptureMode {
    uint32_t width = 0, height = 0;
    double fps = 0;
    bool hdr = false;                   // P010 PQ signal
};

struct DisplayInfo {
    uint32_t width = 0, height = 0;
    double refreshHz = 60;
    bool hdr = false;                   // Windows HDR is on for this monitor
    bool vrr = false;
};

struct GpuCaps {
    unsigned fgMaxMultiplier = 0;       // DLSS-G MultiFrameCountMax + 1; 0 = no DLSS-G
    bool videoSr = false;               // RTX Video SDK super resolution
    bool dlssSr = false;
    bool trueHdr = false;               // RTX Video SDK TrueHDR
    bool opticalFlow = false;           // NVOF
    bool hags = false;                  // DLSS-G requires hardware GPU scheduling
    uint64_t vramMB = 0;
};

enum class HdrPath : uint8_t {
    Sdr = 0,            // SDR in, SDR out
    Passthrough,        // HDR10 in, HDR10 out
    RtxHdr,             // SDR in, RTX Video HDR, HDR10 out
    ToneMap,            // HDR10 in, SDR display: tone mapped on the GPU
};
const char* hdrPathKey(HdrPath path);

enum class SrPath : uint8_t { None = 0, VideoSr, Dlss };
const char* srPathKey(SrPath path);

struct Plan {
    uint32_t outputWidth = 0, outputHeight = 0;
    SrPath superRes = SrPath::None;
    HdrPath hdr = HdrPath::Sdr;
    std::vector<unsigned> multipliers;  // usable frame generation multipliers
    unsigned multiplier = 1;            // applied (1 = off)
    double outputFps = 0;
    bool frameGenBlocked = false;       // requested but unusable
    double vramEstimateMB = 0;
    std::vector<std::string> notes;     // short reasons, shown in the UI
};

// Pure decision function: what the pipeline will run for this source, display,
// GPU and settings. The engine re-plans on any change and rebuilds only the
// parts whose decision changed.
Plan planPipeline(const CaptureMode& capture, const DisplayInfo& display, const GpuCaps& caps, const Settings& settings);

} // namespace vela::core
