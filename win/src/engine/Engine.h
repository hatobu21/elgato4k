#pragma once
// The engine thread: owns the device, capture, pipeline and presenter, turns
// every captured frame into one GPU batch, asks the pacer when each output
// frame should be shown, and keeps the latency statistics. The UI talks to it
// only through apply()/state()/telemetry().
#include "audio/AudioPassthrough.h"
#include "capture/Capture.h"
#include "engine/Pipeline.h"
#include "engine/Presenter.h"
#include "gfx/Swapchain.h"
#include "nv/NgxCore.h"
#include "nv/Reflex.h"
#include "vela/core/AutoColor.h"
#include "vela/core/ContentCadence.h"
#include "vela/core/FramePacer.h"
#include "vela/core/LatencyMonitor.h"
#include "vela/core/ModePlanner.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace vela::engine {

struct SystemCheck {
    std::string name;
    int state = 0;          // 0 ok, 1 warning, 2 problem
    std::string detail;
};

struct SourceState {
    bool signal = false;
    uint32_t width = 0, height = 0;
    double fps = 0;
    bool hdr = false;
    std::string format;     // "NV12" / "P010"
    float maxCll = 0, maxFall = 0;
};

struct EngineState {
    core::Settings settings;
    core::GpuCaps caps;
    core::Plan plan;
    core::DisplayInfo display;
    float displayPeakNits = 0;
    SourceState source;
    std::vector<capture::Device> devices;
    std::vector<capture::Mode> modes;
    std::vector<SystemCheck> system;
    std::string gpuName;
};

struct EngineTelemetry {
    core::LatencySnapshot latency;
    std::vector<double> intervals;      // recent present intervals, ms, oldest first
    core::ColorParams color;
    double contentFps = 0, detectedFps = 0;     // game frame rate inside the signal
    uint64_t repeats = 0;                       // repeated pictures skipped
};

class Engine {
public:
    ~Engine() { stop(); }
    bool start(HWND videoWindow, const core::Settings& settings);
    void stop();

    void apply(const core::Settings& settings);    // any thread
    void resize(uint32_t width, uint32_t height);   // any thread
    void setViewport(const RECT& rect);             // any thread; picture pane in the window
    void resetStats();

    EngineState state() const;
    EngineTelemetry telemetry() const;
    std::function<void()> onStateChanged;           // called on the engine thread

private:
    // Capture slots: written by the MF thread, read by the GPU via the pipeline.
    struct UploadRing {
        enum class State : uint8_t { Free, Writing, Ready, InFlight };
        std::array<State, Pipeline::kUploadSlots> state{};
        std::array<uint64_t, Pipeline::kUploadSlots> fence{};
        std::array<uint64_t, Pipeline::kUploadSlots> sequence{};
        std::array<capture::Frame, Pipeline::kUploadSlots> layout{};
        std::array<double, Pipeline::kUploadSlots> copyMs{};
        std::array<bool, Pipeline::kUploadSlots> historyReset{};
        uint64_t next = 0, dropped = 0;
        std::mutex mutex;
        HANDLE event = nullptr;
        void reset();
    };
    struct PendingBatch {
        uint32_t slot;
        uint64_t fence;
        core::Micros arrival, submitted;
    };

    void run();
    bool initialize();
    void shutdown();
    bool configure(bool reopenDevice);
    void onCaptureFrame(const capture::Frame& frame);
    void processFrame();
    void collectFinished();
    void pollSignal();
    void publishState();
    capture::Mode chooseMode(const core::Settings& s) const;
    core::Plan planFor(const core::Settings& s, const capture::Mode& mode);
    bool applyPlanLive(const core::Plan& plan);
    void refreshCaps();
    std::vector<SystemCheck> systemChecks() const;

    HWND window_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    gfx::Device device_;
    gfx::Swapchain swapchain_;
    nv::NgxCore ngx_;
    nv::Reflex reflex_;
    capture::Source source_;
    audio::Passthrough audio_;
    Pipeline pipeline_;
    Presenter presenter_;
    UploadRing ring_;

    // Only the engine thread touches these.
    core::FramePacer pacer_;
    core::AutoColor autoColor_;
    core::GpuCaps caps_;
    nv::DlssFg::Caps fgCaps_;
    capture::SignalInfo signal_;
    core::DisplayInfo display_;
    float displayPeak_ = 0;
    std::deque<PendingBatch> pending_;
    std::array<bool, Pipeline::kBatchSlots> batchBusy_{};
    std::array<uint64_t, Pipeline::kBatchSlots> batchFence_{};
    uint32_t nextBatch_ = 0;
    bool resetHistory_ = true;
    uint64_t reflexFrame_ = 0;
    core::Micros lastPoll_ = 0, lastStats_ = 0;
    uint64_t pollCount_ = 0;
    Presenter::Callbacks presenterCallbacks_;
    std::atomic<bool> capturing_{false};
    std::atomic<bool> dedup_{false};
    double lastContentFps_ = 0;

    // Capture thread + engine thread: repeat detection and content rate.
    mutable std::mutex cadenceMutex_;
    core::ContentCadence cadence_;
    core::PictureCompare compare_;

    // Shared with other threads.
    mutable std::mutex mutex_;
    core::Settings settings_;
    std::optional<core::Settings> pendingSettings_;
    std::optional<std::pair<uint32_t, uint32_t>> pendingResize_;
    bool pendingReset_ = false;
    core::LatencyMonitor monitor_;
    std::deque<double> intervals_;
    core::Micros lastPresented_ = 0;
    EngineState state_;
    core::ColorParams color_;
    std::mutex batchMutex_;
    std::condition_variable batchCv_;
};

} // namespace vela::engine
