#include "engine/Engine.h"

#include "base/Log.h"
#include "capture/Elgato.h"

#include <avrt.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

namespace vela::engine {
namespace {
std::filesystem::path runtimeDirectory() { return exeDirectory() / L"runtime"; }
bool runtimeFile(const wchar_t* name) {
    std::error_code ec;
    return std::filesystem::is_regular_file(runtimeDirectory() / name, ec);
}
bool samePlan(const core::Plan& a, const core::Plan& b) {
    return a.outputWidth == b.outputWidth && a.outputHeight == b.outputHeight && a.superRes == b.superRes &&
           a.hdr == b.hdr && a.multiplier == b.multiplier;
}
bool sameMode(const capture::Mode& a, const capture::Mode& b) {
    return a.width == b.width && a.height == b.height && a.rateNum * b.rateDen == b.rateNum * a.rateDen && a.format == b.format;
}
} // namespace

void Engine::UploadRing::reset() {
    std::lock_guard lock(mutex);
    state.fill(State::Free);
    fence.fill(0);
    sequence.fill(0);
}

// ------------------------------------------------------------------ lifecycle
bool Engine::start(HWND videoWindow, const core::Settings& settings) {
    window_ = videoWindow;
    {
        std::lock_guard lock(mutex_);
        settings_ = settings;
        settings_.clamp();
    }
    ring_.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Engine::stop() {
    stop_ = true;
    if (ring_.event) SetEvent(ring_.event);
    if (thread_.joinable()) thread_.join();
    if (ring_.event) CloseHandle(ring_.event);
    ring_.event = nullptr;
}

void Engine::apply(const core::Settings& settings) {
    std::lock_guard lock(mutex_);
    pendingSettings_ = settings;
    pendingSettings_->clamp();
    SetEvent(ring_.event);
}

void Engine::resize(uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);
    pendingResize_ = {width, height};
    SetEvent(ring_.event);
}

void Engine::resetStats() {
    std::lock_guard lock(mutex_);
    pendingReset_ = true;
}

EngineState Engine::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

EngineTelemetry Engine::telemetry() const {
    std::lock_guard lock(mutex_);
    EngineTelemetry t;
    t.latency = monitor_.snapshot(nowMicros());
    t.intervals.assign(intervals_.begin(), intervals_.end());
    t.color = color_;
    std::lock_guard cadenceLock(cadenceMutex_);
    t.contentFps = cadence_.contentFps();
    t.detectedFps = cadence_.detectedFps();
    t.repeats = cadence_.repeats();
    return t;
}

void Engine::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task);
    if (initialize()) {
        while (!stop_) {
            WaitForSingleObject(ring_.event, 20);
            if (stop_) break;

            std::optional<core::Settings> next;
            std::optional<std::pair<uint32_t, uint32_t>> size;
            {
                std::lock_guard lock(mutex_);
                next.swap(pendingSettings_);
                size.swap(pendingResize_);
                if (pendingReset_) {
                    monitor_.reset();
                    intervals_.clear();
                    pendingReset_ = false;
                }
            }
            if (size) {
                // The pipeline keeps running; only the swap chain and the
                // presenter's composition target follow the window.
                presenter_.stop();
                device_.waitIdle();
                swapchain_.resize(std::max(size->first, 64u), std::max(size->second, 64u), swapchain_.hdr());
                presenter_.start(device_, swapchain_, &reflex_, presenterCallbacks_);
            }
            if (next) {
                core::Settings previous;
                {
                    std::lock_guard lock(mutex_);
                    previous = settings_;
                    settings_ = *next;
                }
                const bool deviceChanged = previous.capture.devicePath != next->capture.devicePath;
                const auto& p = pipeline_.config();
                {
                    std::lock_guard lock(cadenceMutex_);
                    cadence_.configure(next->frameGen.contentRate, p.mode.fps());
                }
                // Recompute what the new settings would run; rebuild only when
                // routes or sizes change. A new multiplier switches live.
                const auto mode = chooseMode(*next);
                const auto plan = planFor(*next, mode);
                const bool rebuild = deviceChanged || !sameMode(mode, p.mode) ||
                    previous.frameGen.performanceMode != next->frameGen.performanceMode ||
                    previous.capture.audio != next->capture.audio || !applyPlanLive(plan);
                if (rebuild) {
                    configure(deviceChanged);
                } else {
                    // Live values: pacing, latency calibration, colour, SR/HDR tuning, volume.
                    core::PacerConfig pc = pacer_.config();
                    pc.mode = next->frameGen.pacing;
                    pc.compression = next->frameGen.compression;
                    pacer_.configure(pc);
                    autoColor_.configure(next->color);
                    pipeline_.updateSettings(*next);
                    reflex_.enable(device_.d3d(), next->frameGen.reflex);
                    presenter_.setVsync(next->display.sync == core::SyncMode::VSync);
                    audio_.setVolume(next->capture.muted ? 0.0f : next->capture.volume);
                    {
                        std::lock_guard lock(mutex_);
                        monitor_.configure({next->latency.captureHardwareMs, next->latency.displayProcessingMs, display_.refreshHz});
                    }
                    publishState();
                }
            }
            collectFinished();
            processFrame();
            pollSignal();
        }
    }
    shutdown();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

bool Engine::initialize() {
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (!device_.create(false)) return false;
    if (!ngx_.initialize(device_.d3d(), runtimeDirectory().wstring()))
        log::warn("engine", "NGX unavailable: DLSS, RTX Video and frame generation are off");
    refreshCaps();
    RECT rc{};
    GetClientRect(window_, &rc);
    display_ = gfx::Swapchain::queryDisplay(device_.factory(), window_, &displayPeak_);
    if (!swapchain_.create(device_, window_, std::max<LONG>(rc.right, 64), std::max<LONG>(rc.bottom, 64), false)) return false;
    configure(true);   // without a device or signal the loop still runs and retries
    return true;
}

void Engine::shutdown() {
    capturing_ = false;
    source_.stop();
    audio_.stop();
    presenter_.stop();
    pipeline_.release();
    source_.close();
    device_.waitIdle();
    swapchain_.release();
    ngx_.shutdown();
    MFShutdown();
}

void Engine::refreshCaps() {
    caps_ = {};
    if (ngx_.ready()) {
        fgCaps_ = nv::DlssFg::query(ngx_);
        caps_.fgMaxMultiplier = fgCaps_.maxMultiplier;
        caps_.hags = fgCaps_.hags;
        caps_.dlssSr = runtimeFile(L"nvngx_dlss.dll");
        caps_.videoSr = runtimeFile(L"nvngx_vsr.dll");
        caps_.trueHdr = runtimeFile(L"nvngx_truehdr.dll");
    }
    caps_.opticalFlow = nv::NvOf::runtimePresent();
    caps_.vramMB = device_.vramBytes() >> 20;
}

// ------------------------------------------------------------------ configure
bool Engine::configure(bool reopenDevice) {
    core::Settings s;
    {
        std::lock_guard lock(mutex_);
        s = settings_;
    }
    capturing_ = false;
    source_.stop();
    audio_.stop();
    presenter_.stop();
    collectFinished();
    device_.waitIdle();
    pending_.clear();
    ring_.reset();

    if (reopenDevice || source_.modes().empty()) {
        const auto devices = capture::Source::enumerate();
        {
            std::lock_guard lock(mutex_);
            state_.devices = devices;
        }
        auto it = std::find_if(devices.begin(), devices.end(), [&](const capture::Device& d) { return d.link == s.capture.devicePath; });
        if (it == devices.end() && !devices.empty()) it = devices.begin();
        if (it == devices.end() || !source_.open(*it)) {
            log::warn("engine", "no capture device");
            publishState();
            return false;
        }
        if (s.capture.devicePath.empty()) s.capture.devicePath = it->link;
    }

    display_ = gfx::Swapchain::queryDisplay(device_.factory(), window_, &displayPeak_);
    signal_ = source_.signal();
    const auto mode = chooseMode(s);
    const bool srcHdr = mode.format == capture::PixelFormat::P010 && signal_.hdr;
    {
        std::lock_guard lock(cadenceMutex_);
        cadence_.configure(s.frameGen.contentRate, mode.fps());
        compare_.reset();
        lastContentFps_ = cadence_.contentFps();
    }

    // Build; if a vendor feature cannot be created, drop it and plan again.
    PipelineConfig config;
    config.mode = mode;
    config.settings = s;
    config.maxMultiplier = caps_.fgMaxMultiplier;
    bool built = false;
    for (int attempt = 0; attempt < 4 && !built; ++attempt) {
        config.plan = planFor(s, mode);
        config.signal = signal_;
        config.signal.hdr = srcHdr;
        built = pipeline_.build(device_, ngx_.ready() ? &ngx_ : nullptr, config);
        if (built) break;
        if (config.plan.multiplier > 1) { caps_.fgMaxMultiplier = 0; config.plan.notes.push_back("帧生成创建失败，已关闭"); }
        else if (config.plan.superRes != core::SrPath::None) { caps_.videoSr = caps_.dlssSr = false; }
        else if (config.plan.hdr == core::HdrPath::RtxHdr) { caps_.trueHdr = false; }
        else break;
    }
    if (!built) {
        log::error("engine", "pipeline build failed");
        publishState();
        return false;
    }

    const bool hdrOut = pipeline_.outputFormat() == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (swapchain_.hdr() != hdrOut) swapchain_.resize(swapchain_.width(), swapchain_.height(), hdrOut);
    swapchain_.setHdrMetadata(signal_.maxCll, signal_.maxFall, signal_.masteringPeak);

    core::PacerConfig pc;
    pc.multiplier = config.plan.multiplier;
    pc.mode = s.frameGen.pacing;
    pc.compression = s.frameGen.compression;
    pacer_.configure(pc);
    pacer_.reset();
    autoColor_.configure(s.color);
    reflex_.enable(device_.d3d(), s.frameGen.reflex);
    {
        std::lock_guard lock(mutex_);
        monitor_.configure({s.latency.captureHardwareMs, s.latency.displayProcessingMs, display_.refreshHz});
        monitor_.setFrameGeneration(config.plan.multiplier);
        settings_.capture.devicePath = s.capture.devicePath;
    }
    {
        std::lock_guard lock(batchMutex_);
        batchBusy_.fill(false);
        batchFence_.fill(0);
    }
    nextBatch_ = 0;
    resetHistory_ = true;
    dedup_ = config.plan.multiplier > 1;

    Presenter::Callbacks& callbacks = presenterCallbacks_;
    callbacks.presented = [this](core::Micros t, core::Micros arrival, bool generated) {
        std::lock_guard lock(mutex_);
        monitor_.onPresent(t, arrival, generated);
        if (lastPresented_ > 0) intervals_.push_back(core::toMs(t - lastPresented_));
        lastPresented_ = t;
        while (intervals_.size() > 960) intervals_.pop_front();
    };
    callbacks.dropped = [this](unsigned n) {
        std::lock_guard lock(mutex_);
        monitor_.onGeneratedDropped(n);
    };
    callbacks.released = [this](uint32_t slot, uint64_t fence) {
        {
            std::lock_guard lock(batchMutex_);
            batchBusy_[slot] = false;
            batchFence_[slot] = fence;
        }
        batchCv_.notify_all();
    };
    presenter_.setVsync(s.display.sync == core::SyncMode::VSync);
    presenter_.start(device_, swapchain_, &reflex_, callbacks);

    capturing_ = true;
    source_.start(mode, [this](const capture::Frame& f) { onCaptureFrame(f); });
    if (s.capture.audio) {
        const auto& devices = state_.devices;
        auto it = std::find_if(devices.begin(), devices.end(), [&](const capture::Device& d) { return d.link == s.capture.devicePath; });
        audio_.start(audio::Passthrough::matchEndpoint(it != devices.end() ? it->name : L""));
        audio_.setVolume(s.capture.muted ? 0.0f : s.capture.volume);
    }
    publishState();
    return true;
}

// ------------------------------------------------------------------ frames
void Engine::onCaptureFrame(const capture::Frame& frame) {
    if (!capturing_) return;
    const core::Micros t0 = nowMicros();
    {
        std::lock_guard lock(mutex_);
        monitor_.onSignalFrame(frame.arrival);
    }
    // Repeated pictures (30 fps game in a 60 Hz signal) never reach the GPU
    // while frame generation runs: DLSS-G must see two different pictures.
    core::CadenceDecision decision;
    {
        std::lock_guard lock(cadenceMutex_);
        const bool changed = compare_.changed({frame.data, frame.bytes, frame.pitch, frame.chromaOffset,
                                               frame.width, frame.height, frame.format == capture::PixelFormat::P010});
        decision = cadence_.onFrame(frame.arrival, changed);
    }
    if (dedup_ && !decision.process) return;
    int slot = -1;
    {
        std::lock_guard lock(ring_.mutex);
        for (uint32_t i = 0; i < Pipeline::kUploadSlots && slot < 0; ++i)
            if (ring_.state[i] == UploadRing::State::Free) slot = int(i);
        if (slot < 0) {
            // Engine is behind: overwrite the oldest unprocessed frame.
            uint64_t oldest = UINT64_MAX;
            for (uint32_t i = 0; i < Pipeline::kUploadSlots; ++i)
                if (ring_.state[i] == UploadRing::State::Ready && ring_.sequence[i] < oldest) { oldest = ring_.sequence[i]; slot = int(i); }
        }
        if (slot < 0 || frame.bytes > pipeline_.uploadCapacity()) {
            ++ring_.dropped;
            return;
        }
        if (ring_.state[size_t(slot)] == UploadRing::State::Ready) ++ring_.dropped;
        ring_.state[size_t(slot)] = UploadRing::State::Writing;
    }
    std::memcpy(pipeline_.uploadPointer(uint32_t(slot)), frame.data, frame.bytes);
    {
        std::lock_guard lock(ring_.mutex);
        ring_.layout[size_t(slot)] = frame;
        ring_.layout[size_t(slot)].data = nullptr;
        ring_.historyReset[size_t(slot)] = decision.reset;
        ring_.copyMs[size_t(slot)] = core::toMs(nowMicros() - t0);
        ring_.sequence[size_t(slot)] = ++ring_.next;
        ring_.state[size_t(slot)] = UploadRing::State::Ready;
    }
    SetEvent(ring_.event);
}

void Engine::processFrame() {
    if (!pipeline_.built()) return;
    const uint64_t completed = device_.completed();
    int slot = -1;
    capture::Frame layout;
    double copyMs = 0;
    bool cadenceReset = false;
    uint64_t droppedNow = 0;
    {
        std::lock_guard lock(ring_.mutex);
        uint64_t newest = 0;
        for (uint32_t i = 0; i < Pipeline::kUploadSlots; ++i) {
            if (ring_.state[i] == UploadRing::State::InFlight && ring_.fence[i] <= completed) ring_.state[i] = UploadRing::State::Free;
            if (ring_.state[i] == UploadRing::State::Ready && ring_.sequence[i] > newest) { newest = ring_.sequence[i]; slot = int(i); }
        }
        if (slot < 0) return;
        // Older ready frames are stale once a newer one exists.
        for (uint32_t i = 0; i < Pipeline::kUploadSlots; ++i)
            if (int(i) != slot && ring_.state[i] == UploadRing::State::Ready) { ring_.state[i] = UploadRing::State::Free; ++ring_.dropped; }
        ring_.state[size_t(slot)] = UploadRing::State::InFlight;
        ring_.fence[size_t(slot)] = UINT64_MAX;
        layout = ring_.layout[size_t(slot)];
        copyMs = ring_.copyMs[size_t(slot)];
        cadenceReset = ring_.historyReset[size_t(slot)];
        droppedNow = ring_.dropped;
        ring_.dropped = 0;
    }

    // Wait for the batch slot: the presenter must be done with its frames.
    const uint32_t b = nextBatch_;
    {
        std::unique_lock lock(batchMutex_);
        if (!batchCv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !batchBusy_[b]; })) {
            lock.unlock();
            presenter_.discard();
            lock.lock();
            batchBusy_[b] = false;
        }
    }
    uint64_t presentedFence = 0;
    {
        std::lock_guard lock(batchMutex_);
        presentedFence = batchFence_[b];
    }
    device_.waitCpu(presentedFence, 100);
    // The slot's timing and statistics must be read before they are overwritten.
    auto usesSlot = [&](const PendingBatch& p) { return p.slot == b; };
    for (int i = 0; i < 8 && std::any_of(pending_.begin(), pending_.end(), usesSlot); ++i) {
        device_.waitCpu(pending_.front().fence, 100);
        collectFinished();
    }
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), usesSlot), pending_.end());

    ++reflexFrame_;
    reflex_.mark(reflexFrame_, nv::ReflexMarker::SimulationStart);
    reflex_.mark(reflexFrame_, nv::ReflexMarker::SimulationEnd);
    reflex_.mark(reflexFrame_, nv::ReflexMarker::RenderSubmitStart);
    BatchOut out;
    const bool ok = pipeline_.process(uint32_t(slot), layout, b, autoColor_.current(), resetHistory_ || cadenceReset, out);
    reflex_.mark(reflexFrame_, nv::ReflexMarker::RenderSubmitEnd);
    const core::Micros submitted = nowMicros();
    {
        std::lock_guard lock(ring_.mutex);
        ring_.fence[size_t(slot)] = ok ? out.fence : device_.signal();
    }
    if (!ok) {
        log::error("engine", "batch failed; rebuilding without the failing feature");
        if (pipeline_.degraded()) {
            const auto& plan = pipeline_.config().plan;
            if (plan.multiplier > 1) caps_.fgMaxMultiplier = 0;
            else if (plan.superRes != core::SrPath::None) caps_.videoSr = caps_.dlssSr = false;
            else if (plan.hdr == core::HdrPath::RtxHdr) caps_.trueHdr = false;
        }
        configure(false);
        return;
    }
    resetHistory_ = false;
    nextBatch_ = (b + 1) % Pipeline::kBatchSlots;
    {
        std::lock_guard lock(batchMutex_);
        batchBusy_[b] = true;
    }

    // Ask the pacer when each frame goes on screen, then hand them over.
    const auto plan = pacer_.plan(layout.arrival, submitted);
    std::vector<PresentItem> items;
    for (const auto& pf : plan.frames) {
        const auto it = std::find_if(out.frames.begin(), out.frames.end(), [&](const FrameOut& f) {
            return pf.real ? f.real : (!f.real && f.subframe == pf.subframe);
        });
        if (it == out.frames.end()) continue;
        PresentItem item;
        item.texture = it->texture;
        item.srv = it->srv;
        item.target = pf.target;
        item.arrival = layout.arrival;
        item.real = it->real;
        item.batchSlot = b;
        item.reflexFrame = reflexFrame_;
        items.push_back(item);
    }
    for (size_t i = 0; i < items.size(); ++i)
        items[i].spacing = i + 1 < items.size() ? items[i + 1].target - items[i].target : plan.interval / std::max(1u, pipeline_.multiplier());
    if (!items.empty()) items.back().lastOfBatch = true;
    presenter_.push(std::move(items));

    pending_.push_back({b, out.fence, layout.arrival, submitted});
    std::lock_guard lock(mutex_);
    monitor_.onSourceFrame(layout.arrival, copyMs);
    monitor_.onSourceDropped(droppedNow);
    monitor_.onGeneratedDropped(plan.droppedGenerated);
}

void Engine::collectFinished() {
    const uint64_t completed = device_.completed();
    while (!pending_.empty() && pending_.front().fence <= completed) {
        const auto p = pending_.front();
        pending_.pop_front();
        gfx::GpuTimer::StageArray stages;
        double total = -1;
        core::ColorStats stats;
        const bool haveStats = pipeline_.collect(p.slot, stages, total, stats);
        if (total > 0) pacer_.observeProcessing(p.arrival, p.submitted + core::fromMs(total));
        const core::Micros now = nowMicros();
        if (haveStats) {
            const double dt = lastStats_ ? core::toMs(now - lastStats_) / 1000.0 : 1.0 / 60;
            lastStats_ = now;
            autoColor_.update(stats, std::min(dt, 0.25));
        }
        std::lock_guard lock(mutex_);
        monitor_.onGpuBatch(now, stages, total);
        color_ = autoColor_.current();
    }
}

void Engine::pollSignal() {
    const core::Micros now = nowMicros();
    if (now - lastPoll_ < 250000) return;
    lastPoll_ = now;
    // HUD text at 4 Hz.
    {
        core::Settings s;
        core::LatencySnapshot snap;
        {
            std::lock_guard lock(mutex_);
            s = settings_;
            snap = monitor_.snapshot(now);
        }
        presenter_.setHud(HudText::format(snap.endToEndMs, snap.outputFps, snap.sourceFps, s.latency.hud, s.latency.hudCorner));
        // Lip sync: sound waits as long as the picture does.
        if (snap.pipeline.count > 30) audio_.setDelayMs(snap.pipeline.p50);
    }
    // Every 0.5 s: console switched HDR on/off, monitor HDR toggled or refresh
    // changed -> rebuild. Every 2 s without a device: look for one again.
    ++pollCount_;
    if (source_.modes().empty()) {
        if (pollCount_ % 8 == 0) configure(true);
        return;
    }
    if (pollCount_ % 2) return;
    // The game changed its frame rate (auto detection): re-plan; usually only
    // the multiplier's cap moves, which switches without a rebuild.
    double content = 0;
    {
        std::lock_guard lock(cadenceMutex_);
        content = cadence_.contentFps();
    }
    if (pipeline_.built() && std::fabs(content - lastContentFps_) > 0.5) {
        lastContentFps_ = content;
        core::Settings s;
        {
            std::lock_guard lock(mutex_);
            s = settings_;
        }
        log::info("engine", std::format("content rate {:.0f} fps", content));
        if (!applyPlanLive(planFor(s, pipeline_.config().mode))) configure(false);
        return;
    }
    float peak = 0;
    const auto display = gfx::Swapchain::queryDisplay(device_.factory(), window_, &peak);
    const auto signal = source_.signal();
    if (signal.hdr != signal_.hdr || display.hdr != display_.hdr || display.refreshHz != display_.refreshHz || !pipeline_.built()) {
        log::info("engine", std::format("signal/display change: hdr {}->{} displayHdr {}->{} refresh {}->{}",
            signal_.hdr, signal.hdr, display_.hdr, display.hdr, display_.refreshHz, display.refreshHz));
        configure(false);
    }
}

capture::Mode Engine::chooseMode(const core::Settings& s) const {
    const bool wantP010 = signal_.hdr && s.capture.preferHdr && display_.hdr && s.hdr.mode == core::HdrMode::Auto;
    return capture::Source::choose(source_.modes(), s.capture.width, s.capture.height, s.capture.fps, wantP010);
}

core::Plan Engine::planFor(const core::Settings& s, const capture::Mode& mode) {
    const bool srcHdr = mode.format == capture::PixelFormat::P010 && signal_.hdr;
    double content = mode.fps();
    {
        std::lock_guard lock(cadenceMutex_);
        content = std::min(cadence_.contentFps(), mode.fps());
    }
    // Frame generation multiplies the content rate, not the HDMI rate.
    return core::planPipeline({mode.width, mode.height, content, srcHdr}, display_, caps_, s);
}

bool Engine::applyPlanLive(const core::Plan& plan) {
    if (samePlan(plan, pipeline_.config().plan) && plan.outputFps == pipeline_.config().plan.outputFps) return true;
    if (!pipeline_.updatePlan(plan)) return false;
    core::PacerConfig pc = pacer_.config();
    pc.multiplier = plan.multiplier;
    pacer_.configure(pc);
    dedup_ = plan.multiplier > 1;
    {
        std::lock_guard lock(mutex_);
        monitor_.setFrameGeneration(plan.multiplier);
    }
    publishState();
    return true;
}

// ------------------------------------------------------------------ state
std::vector<SystemCheck> Engine::systemChecks() const {
    std::vector<SystemCheck> c;
    const bool nvidia = device_.vendorId() == 0x10DE;
    c.push_back({"GPU", nvidia ? 0 : 2, std::format("{} · {} GB", narrow(device_.adapterName()), (device_.vramBytes() + (512ull << 20)) >> 30)});
    c.push_back({"硬件加速 GPU 计划", fgCaps_.hags ? 0 : 2, fgCaps_.hags ? "已开启" : "未开启（设置 → 显示 → 图形）"});
    c.push_back({"DLSS 帧生成运行库", caps_.fgMaxMultiplier >= 2 ? 0 : 2,
        caps_.fgMaxMultiplier >= 2 ? std::format("最多 {}×", caps_.fgMaxMultiplier) : (fgCaps_.needsDriver ? "需要更新驱动" : "未找到 nvngx_dlssg.dll")});
    c.push_back({"DLSS 超分运行库", caps_.dlssSr ? 0 : 1, caps_.dlssSr ? "nvngx_dlss.dll" : "未找到（HDR 超分不可用）"});
    c.push_back({"RTX Video SDK", caps_.videoSr && caps_.trueHdr ? 0 : 1,
        std::string(caps_.videoSr ? "VSR" : "无 VSR") + " · " + (caps_.trueHdr ? "TrueHDR" : "无 TrueHDR")});
    c.push_back({"NVIDIA 光流", caps_.opticalFlow ? 0 : 2, caps_.opticalFlow ? "nvofapi64.dll" : "驱动未提供"});
    c.push_back({"Windows HDR", display_.hdr ? 0 : 1, display_.hdr ? "已开启" : "未开启"});
    const bool device = !source_.modes().empty();
    c.push_back({"采集卡", device ? 0 : 2, device ? "已连接" : "未找到"});
    return c;
}

void Engine::publishState() {
    {
        std::lock_guard lock(mutex_);
        state_.settings = settings_;
        state_.caps = caps_;
        state_.plan = pipeline_.built() ? pipeline_.config().plan : core::Plan{};
        state_.display = display_;
        state_.displayPeakNits = displayPeak_;
        state_.modes = source_.modes();
        state_.gpuName = narrow(device_.adapterName());
        const auto& mode = pipeline_.config().mode;
        state_.source.signal = pipeline_.built() && capturing_;
        state_.source.width = mode.width;
        state_.source.height = mode.height;
        state_.source.fps = mode.fps();
        state_.source.hdr = pipeline_.config().signal.hdr;
        state_.source.format = mode.format == capture::PixelFormat::P010 ? "P010" : "NV12";
        state_.source.maxCll = signal_.maxCll;
        state_.source.maxFall = signal_.maxFall;
        state_.system = systemChecks();
    }
    if (onStateChanged) onStateChanged();
}

} // namespace vela::engine
