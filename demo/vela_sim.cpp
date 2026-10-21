// vela_sim: runs the real pacing, latency and planning code of the core
// against a simulated capture card and GPU, and prints what each console
// setup would look like. The GPU stage costs are estimates (see kCost below);
// the scheduling, dropping and statistics are the production code.
//
//   vela_sim            all scenarios
//   vela_sim --color    auto colour convergence trace
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "vela/core/AutoColor.h"
#include "vela/core/FramePacer.h"
#include "vela/core/LatencyMonitor.h"
#include "vela/core/ModePlanner.h"

using namespace vela::core;

namespace {

// Estimated RTX 5070 Ti stage costs at 3840x2160 output, in ms. Derived from
// the upstream RTX 5070 capture measurements (DLSS-G 4X batch 5.6 ms: first
// frame 2.6, later 1.5) scaled by 0.8 for the larger GPU; replace with the
// numbers the in-app latency panel reports once the build runs on hardware.
struct Cost {
    double uploadPer4k = 0.20, flow = 1.0, color = 0.15, videoSr = 1.6, dlssSr = 1.3,
           rtxHdr = 0.8, toneMap = 0.1, fgFirst = 2.1, fgNext = 1.2, outputPerFrame = 0.05,
           captureCopyPer4kNv12 = 1.0, cpuSubmit = 0.8, presentCall = 0.2;
} kCost;

struct Scenario {
    const char* name;
    CaptureMode capture;
    DisplayInfo display;
    unsigned multiplier;
    PacingMode pacing;
    double cardMs;      // assumed capture card latency for the end-to-end line
};

struct Result {
    Plan plan;
    LatencySnapshot snap;
    double lowestLatency = 0;
};

Result run(const Scenario& sc, int seconds = 20) {
    Settings settings;
    settings.frameGen.enabled = sc.multiplier > 1;
    settings.frameGen.multiplier = std::max(sc.multiplier, 2u);
    settings.frameGen.pacing = sc.pacing;
    GpuCaps caps;
    caps.fgMaxMultiplier = 6;   // RTX 50 with a DLSS 4.5 runtime reports MultiFrameCountMax 5
    caps.videoSr = caps.dlssSr = caps.trueHdr = caps.opticalFlow = caps.hags = true;
    Result r;
    r.plan = planPipeline(sc.capture, sc.display, caps, settings);

    FramePacer pacer;
    PacerConfig pc;
    pc.multiplier = r.plan.multiplier;
    pc.mode = sc.pacing;
    pacer.configure(pc);
    LatencyMonitor monitor;
    monitor.configure({sc.cardMs, 0, sc.display.refreshHz});
    monitor.setFrameGeneration(r.plan.multiplier);

    const double inScale = double(sc.capture.width) * sc.capture.height / (3840.0 * 2160.0);
    const double outScale = double(r.plan.outputWidth) * r.plan.outputHeight / (3840.0 * 2160.0);
    const double bytesScale = sc.capture.hdr ? 2.0 : 1.0;   // P010 vs NV12
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(1.0, 0.06);
    std::normal_distribution<double> arrivalJitter(0.0, 0.15);

    const int frames = int(sc.capture.fps * seconds);
    Micros lastPresent = 0;
    for (int i = 0; i < frames; ++i) {
        const Micros arrival = fromMs(i * 1000.0 / sc.capture.fps + arrivalJitter(rng));
        const double copy = kCost.captureCopyPer4kNv12 * inScale * bytesScale * noise(rng);
        monitor.onSourceFrame(arrival, copy);

        std::array<double, size_t(GpuStage::Count)> st;
        st.fill(-1);
        st[size_t(GpuStage::Upload)] = kCost.uploadPer4k * inScale * noise(rng);
        st[size_t(GpuStage::Color)] = kCost.color * inScale * noise(rng);
        if (r.plan.superRes == SrPath::VideoSr) st[size_t(GpuStage::SuperRes)] = kCost.videoSr * outScale * noise(rng);
        if (r.plan.superRes == SrPath::Dlss) st[size_t(GpuStage::SuperRes)] = kCost.dlssSr * outScale * noise(rng);
        if (r.plan.hdr == HdrPath::RtxHdr) st[size_t(GpuStage::Hdr)] = kCost.rtxHdr * outScale * noise(rng);
        if (r.plan.hdr == HdrPath::ToneMap) st[size_t(GpuStage::Hdr)] = kCost.toneMap * outScale * noise(rng);
        if (r.plan.multiplier > 1) {
            st[size_t(GpuStage::Flow)] = kCost.flow * noise(rng);
            st[size_t(GpuStage::FrameGen)] = (kCost.fgFirst + kCost.fgNext * (r.plan.multiplier - 2)) * outScale * noise(rng);
        }
        st[size_t(GpuStage::Output)] = kCost.outputPerFrame * r.plan.multiplier;
        double gpu = 0;
        for (const double v : st) gpu += v > 0 ? v : 0;
        monitor.onGpuBatch(arrival, st, gpu);

        const Micros submitted = arrival + fromMs(copy + kCost.cpuSubmit);
        const Micros ready = submitted + fromMs(gpu);
        const auto batch = pacer.plan(arrival, submitted);
        pacer.observeProcessing(arrival, ready);
        monitor.onGeneratedDropped(batch.droppedGenerated);
        for (const auto& f : batch.frames) {
            // The present thread waits for the target time and the GPU fence.
            Micros t = std::max({f.target, ready, lastPresent + 100});
            t += fromMs(kCost.presentCall);
            monitor.onPresent(t, arrival, !f.real);
            lastPresent = t;
        }
    }
    r.snap = monitor.snapshot(lastPresent);
    return r;
}

const char* pacingName(PacingMode m) { return m == PacingMode::Even ? "均匀" : "低延迟"; }

void printScenario(const Scenario& sc) {
    const auto r = run(sc);
    const auto& s = r.snap;
    std::printf("\n== %s\n", sc.name);
    std::printf("   采集 %ux%u@%.0f %s  ->  输出 %ux%u  超分=%s  HDR=%s  补帧=%ux (%s)\n",
        sc.capture.width, sc.capture.height, sc.capture.fps, sc.capture.hdr ? "HDR10" : "SDR",
        r.plan.outputWidth, r.plan.outputHeight, srPathKey(r.plan.superRes), hdrPathKey(r.plan.hdr),
        r.plan.multiplier, pacingName(sc.pacing));
    for (const auto& note : r.plan.notes) std::printf("   提示: %s\n", note.c_str());
    std::printf("   帧率          源 %.1f fps  ->  输出 %.1f fps   (显示器 %.0f Hz)\n", s.sourceFps, s.outputFps, sc.display.refreshHz);
    std::printf("   GPU 批次      P50 %.2f ms  P95 %.2f ms\n", s.gpuTotal.p50, s.gpuTotal.p95);
    std::printf("   处理管线      回调 -> 真实帧 Present  P50 %.1f ms  P95 %.1f ms\n", s.pipeline.p50, s.pipeline.p95);
    std::printf("   补帧额外延迟  ≈ %.1f ms\n", s.fgAddedMs);
    std::printf("   端到端估算    ≈ %.1f ms  (采集卡 %.0f + 管线 %.1f + 扫描 %.1f)\n",
        s.endToEndMs, sc.cardMs, s.pipeline.p50, s.scanoutMs);
    std::printf("   帧间隔        P50 %.2f ms  P99 %.2f ms   丢弃生成帧 %llu\n",
        s.presentInterval.p50, s.presentInterval.p99, static_cast<unsigned long long>(s.droppedGenerated));
    std::printf("   显存估算      ≈ %.0f MB\n", r.plan.vramEstimateMB);
}

void colorTrace() {
    AutoColor ac;
    AutoColorConfig cfg;
    cfg.enabled = true;
    cfg.strength = 0.6f;
    ac.configure(cfg);
    auto frame = [](float lo, float hi, float r, float g, float b) {
        ColorStats s;
        for (int i = 0; i < ColorStats::kBins; ++i) {
            const float c = (i + 0.5f) / ColorStats::kBins;
            if (c >= lo && c <= hi) s.luma[size_t(i)] = 1000;
        }
        s.midR = r; s.midG = g; s.midB = b;
        s.highR = r * 4; s.highG = g * 4; s.highB = b * 4;
        s.meanChroma = 0.12f;
        return s;
    };
    std::printf("\n自动调色收敛（强度 0.6，速度 正常）：前 2 秒为偏蓝发灰画面，之后切到正常画面\n");
    std::printf("  时间    黑位    白位    伽马    R增益   B增益   饱和度  切换\n");
    for (int i = 0; i <= 240; ++i) {
        const bool second = i >= 120;
        const auto s = second ? frame(0.0f, 1.0f, 0.2f, 0.2f, 0.2f) : frame(0.14f, 0.82f, 0.19f, 0.2f, 0.225f);
        const auto& p = ac.update(s, 1.0 / 60);
        if (i % 15 == 0 || ac.lastWasSceneCut())
            std::printf("  %4.2fs  %.3f  %.3f  %.3f  %.3f  %.3f  %.3f  %s\n", i / 60.0, p.blackPoint, p.whitePoint,
                p.gamma, p.gainR, p.gainB, p.saturation, ac.lastWasSceneCut() ? "<- 场景切换" : "");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--color") == 0) {
        colorTrace();
        return 0;
    }
    const std::vector<Scenario> scenarios = {
        {"PS5 4K60 HDR  ->  4K 144Hz HDR 显示器", {3840, 2160, 60, true}, {3840, 2160, 144, true, true}, 2, PacingMode::Even, 0},
        {"PS5 4K60 HDR  ->  4K 240Hz HDR 显示器, 4x", {3840, 2160, 60, true}, {3840, 2160, 240, true, true}, 4, PacingMode::Even, 0},
        {"PS5 4K60 HDR  ->  4K 240Hz, 4x 低延迟节奏", {3840, 2160, 60, true}, {3840, 2160, 240, true, true}, 4, PacingMode::LowLatency, 0},
        {"PS5 1440p120 SDR  ->  4K 240Hz (超分 + 2x)", {2560, 1440, 120, false}, {3840, 2160, 240, false, true}, 2, PacingMode::Even, 0},
        {"PS5 4K 画质模式: 60Hz 信号中 30fps 内容, 4x -> 120fps", {3840, 2160, 30, true}, {3840, 2160, 144, true, true}, 4, PacingMode::Even, 0},
        {"NS2 1080p60 SDR  ->  4K 240Hz HDR (超分 + RTX HDR + 4x)", {1920, 1080, 60, false}, {3840, 2160, 240, true, true}, 4, PacingMode::Even, 0},
        {"NS2 4K60 HDR  ->  4K 144Hz HDR, 补帧关", {3840, 2160, 60, true}, {3840, 2160, 144, true, true}, 1, PacingMode::Even, 0},
    };
    std::printf("Vela 管线模拟 —— 真实节奏/延迟代码 + 估算 GPU 开销（RTX 5070 Ti）\n");
    std::printf("采集卡硬件延迟未计入（0 = 未校准）；生成帧不计入“真实帧延迟”。\n");
    for (const auto& sc : scenarios) printScenario(sc);
    return 0;
}
