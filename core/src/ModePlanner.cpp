#include "vela/core/ModePlanner.h"

#include <algorithm>
#include <cmath>

namespace vela::core {

const char* hdrPathKey(HdrPath path) {
    switch (path) {
    case HdrPath::Passthrough: return "passthrough";
    case HdrPath::RtxHdr: return "rtxHdr";
    case HdrPath::ToneMap: return "toneMap";
    default: return "sdr";
    }
}

const char* srPathKey(SrPath path) {
    switch (path) {
    case SrPath::VideoSr: return "videoSr";
    case SrPath::Dlss: return "dlss";
    default: return "none";
    }
}

namespace {
uint32_t even(double v) { return static_cast<uint32_t>(std::lround(v / 2.0)) * 2; }

HdrPath chooseHdr(const CaptureMode& capture, const DisplayInfo& display, const GpuCaps& caps,
                  const HdrSettings& hdr, std::vector<std::string>& notes) {
    if (capture.hdr) {
        if (hdr.mode == HdrMode::Off || !display.hdr) {
            if (!display.hdr && hdr.mode != HdrMode::Off) notes.push_back("显示器未开启 HDR，HDR 信号将映射为 SDR");
            return HdrPath::ToneMap;
        }
        return HdrPath::Passthrough;
    }
    if (hdr.mode == HdrMode::Auto && display.hdr && hdr.rtxHdr) {
        if (caps.trueHdr) return HdrPath::RtxHdr;
        notes.push_back("未找到 RTX Video HDR 组件，SDR 信号按 SDR 输出");
    }
    return HdrPath::Sdr;
}

SrPath chooseSr(bool hdrInput, const GpuCaps& caps, std::vector<std::string>& notes) {
    // RTX Video SR takes 8-bit SDR only; HDR10 frames go to DLSS SR.
    if (!hdrInput && caps.videoSr) return SrPath::VideoSr;
    if (hdrInput && caps.dlssSr) return SrPath::Dlss;
    notes.push_back(hdrInput ? "未找到 DLSS 超分组件，HDR 画面按原分辨率输出" : "未找到 RTX Video 超分组件，按原分辨率输出");
    return SrPath::None;
}
} // namespace

Plan planPipeline(const CaptureMode& capture, const DisplayInfo& display, const GpuCaps& caps, const Settings& settings) {
    Plan plan;
    plan.hdr = chooseHdr(capture, display, caps, settings.hdr, plan.notes);
    // RTX HDR converts after SR, so SR still sees the SDR picture.
    const bool hdrAtSr = plan.hdr == HdrPath::Passthrough;

    plan.outputWidth = capture.width;
    plan.outputHeight = capture.height;
    if (capture.width && capture.height && display.width && display.height) {
        const double scale = std::min(double(display.width) / capture.width, double(display.height) / capture.height);
        if (settings.superRes.enabled && scale > 1.05) {
            plan.superRes = chooseSr(hdrAtSr, caps, plan.notes);
            if (plan.superRes != SrPath::None) {
                plan.outputWidth = even(capture.width * scale);
                plan.outputHeight = even(capture.height * scale);
            }
        }
    }

    const double fps = capture.fps > 0 ? capture.fps : 60.0;
    const auto& fg = settings.frameGen;
    if (caps.fgMaxMultiplier < 2) {
        if (fg.enabled) plan.notes.push_back("当前驱动或运行库不提供 DLSS 帧生成");
    } else if (!caps.hags) {
        if (fg.enabled) plan.notes.push_back("DLSS 帧生成需要开启“硬件加速 GPU 计划”");
    } else if (!caps.opticalFlow) {
        if (fg.enabled) plan.notes.push_back("光流接口不可用，无法为帧生成提供运动矢量");
    } else {
        const bool capByRefresh = settings.display.sync != SyncMode::Tearing;
        for (unsigned m = 2; m <= std::min(caps.fgMaxMultiplier, 6u); ++m)
            if (!capByRefresh || fps * m <= display.refreshHz * 1.01) plan.multipliers.push_back(m);
        if (plan.multipliers.empty() && fg.enabled)
            plan.notes.push_back("显示器刷新率不足以容纳 2× 补帧");
    }
    if (fg.enabled && !plan.multipliers.empty()) {
        plan.multiplier = plan.multipliers.front();
        for (const unsigned m : plan.multipliers)
            if (m <= fg.multiplier) plan.multiplier = m;
        if (plan.multiplier != fg.multiplier) plan.notes.push_back("补帧倍数已按刷新率限制为 " + std::to_string(plan.multiplier) + "×");
    }
    plan.frameGenBlocked = fg.enabled && plan.multiplier < 2;
    plan.outputFps = fps * plan.multiplier;

    // Rough VRAM estimate (MB) so the UI can warn before a 16 GB card runs out.
    const double inPx = double(capture.width) * capture.height;
    const double outPx = double(plan.outputWidth) * plan.outputHeight;
    double bytes = inPx * (8 /*linear FP16*/ + 4 /*flow input x2*/ * 2 + 1.5 * 3 /*upload ring*/);
    bytes += outPx * 4 * 3;                                     // real frame ring
    if (plan.superRes != SrPath::None) bytes += outPx * 16;     // SR feature state
    if (plan.hdr == HdrPath::RtxHdr) bytes += outPx * 8;        // scRGB intermediate
    if (plan.multiplier > 1) bytes += outPx * (4.0 * 2 * (plan.multiplier - 1) + 8 /*mvecs*/ + 4 /*depth*/ + 40 /*DLSS-G*/);
    plan.vramEstimateMB = bytes / (1024.0 * 1024.0);
    return plan;
}

} // namespace vela::core
