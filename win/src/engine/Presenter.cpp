#include "engine/Presenter.h"

#include "base/Log.h"

#include <avrt.h>

#include <cmath>
#include <format>

namespace vela::engine {
namespace {
struct ComposeConstants {
    uint32_t dstW, dstH;
    int32_t rectX, rectY;
    uint32_t rectW, rectH;
    uint32_t hudOn;
    int32_t hudX, hudY;
    uint32_t hudScale, hudChars;
    float hudWhite;
    uint32_t text[4];
};

uint8_t glyph(char c) {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    switch (c) {
    case '.': return 10;
    case 'm': return 11;
    case 's': return 12;
    case 'f': return 13;
    case 'p': return 14;
    case '/': return 15;
    default: return 16;
    }
}
} // namespace

HudText HudText::format(double e2eMs, double outputFps, double sourceFps, bool on, unsigned corner) {
    HudText h;
    h.on = on;
    h.corner = corner;
    // "18.2ms 120/60fps" fits the 16 glyph slots.
    const auto text = std::format("{:.1f}ms {}/{}fps", e2eMs, int(std::lround(outputFps)), int(std::lround(sourceFps)));
    for (char c : text) {
        if (h.count == h.glyphs.size()) break;
        h.glyphs[h.count++] = glyph(c);
    }
    return h;
}

bool Presenter::start(gfx::Device& device, gfx::Swapchain& swapchain, nv::Reflex* reflex, Callbacks callbacks) {
    stop();
    device_ = &device;
    swapchain_ = &swapchain;
    reflex_ = reflex;
    callbacks_ = std::move(callbacks);
    if (!compose_ && !compose_.create(device, L"Compose", 1, 1, 16)) return false;
    composite_ = device.texture(swapchain.width(), swapchain.height(), swapchain.format(), true, L"vela.composite");
    if (!composite_) return false;
    device.uav(compositeUav_, composite_);
    for (auto& a : allocators_)
        if (!a && FAILED(device.d3d()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (!list_) {
        if (FAILED(device.d3d()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_))))
            return false;
        list_->Close();
    }
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Presenter::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    discard();
    if (device_) device_->waitIdle();
    composite_ = {};
}

void Presenter::push(std::vector<PresentItem> items) {
    {
        std::lock_guard lock(mutex_);
        for (auto& i : items) queue_.push_back(i);
    }
    cv_.notify_one();
}

void Presenter::discard() {
    std::deque<PresentItem> dropped;
    {
        std::lock_guard lock(mutex_);
        dropped.swap(queue_);
    }
    for (const auto& i : dropped)
        if (i.lastOfBatch && callbacks_.released) callbacks_.released(i.batchSlot, device_ ? device_->completed() : 0);
}

void Presenter::setHud(const HudText& hud) {
    std::lock_guard lock(mutex_);
    hud_ = hud;
}

void Presenter::sleepUntil(core::Micros target) {
    const core::Micros now = nowMicros();
    if (target - now > 1200 && timer_) {
        // Wake ~0.7 ms early, then spin: the timer alone is ~0.5 ms accurate.
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(target - now - 700) * 10;
        if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer_, 100);
    }
    while (nowMicros() < target) YieldProcessor();
}

void Presenter::run() {
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    for (;;) {
        PresentItem item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            item = queue_.front();
            queue_.pop_front();
        }
        // A generated frame more than half a slot late would only shorten the
        // next interval; skip it. The real frame is always shown.
        if (!item.real && nowMicros() > item.target + item.spacing / 2) {
            if (callbacks_.dropped) callbacks_.dropped(1);
            continue;
        }
        sleepUntil(item.target);
        if (!presentOne(item)) log::warn("present", "present failed");
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

bool Presenter::presentOne(const PresentItem& item) {
    auto& sc = *swapchain_;
    WaitForSingleObjectEx(sc.waitable(), 100, FALSE);

    const uint32_t a = allocatorIndex_;
    allocatorIndex_ = (allocatorIndex_ + 1) % uint32_t(allocators_.size());
    device_->waitCpu(allocatorFence_[a], 1000);
    allocators_[a]->Reset();
    auto* list = list_.Get();
    list->Reset(allocators_[a].Get(), nullptr);

    HudText hud;
    {
        std::lock_guard lock(mutex_);
        hud = hud_;
    }
    auto& bb = sc.buffer(sc.current());
    auto& frame = *item.texture;
    const bool direct = !hud.on && frame.width == bb.width && frame.height == bb.height && frame.format == bb.format;
    if (direct) {
        gfx::transition(list, bb, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(bb.get(), frame.get());
    } else {
        // Letterbox the frame into the window, keep its aspect.
        const double scale = std::min(double(bb.width) / frame.width, double(bb.height) / frame.height);
        const uint32_t w = uint32_t(frame.width * scale), h = uint32_t(frame.height * scale);
        ComposeConstants c{};
        c.dstW = bb.width;
        c.dstH = bb.height;
        c.rectX = int32_t(bb.width - w) / 2;
        c.rectY = int32_t(bb.height - h) / 2;
        c.rectW = w;
        c.rectH = h;
        c.hudOn = hud.on ? 1 : 0;
        c.hudScale = std::max(2u, bb.height / 360);
        c.hudChars = hud.count;
        const int32_t boxW = int32_t(hud.count * c.hudScale * 4 + c.hudScale * 3), boxH = int32_t(c.hudScale * 9);
        const int32_t margin = int32_t(c.hudScale * 8);
        c.hudX = (hud.corner % 2 == 0) ? margin : int32_t(bb.width) - boxW - margin;
        c.hudY = (hud.corner < 2) ? margin : int32_t(bb.height) - boxH - margin;
        c.hudWhite = sc.hdr() ? 0.58f : 1.0f;   // PQ code of ~203 nits
        for (unsigned i = 0; i < 16; ++i) c.text[i / 4] |= uint32_t(hud.glyphs[i]) << ((i % 4) * 8);
        gfx::transition(list, composite_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compose_.record(list, *device_, item.srv, compositeUav_, &c, gfx::groups(bb.width), gfx::groups(bb.height));
        gfx::uavBarrier(list, composite_.get());
        gfx::transition(list, composite_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gfx::transition(list, bb, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(bb.get(), composite_.get());
    }
    gfx::transition(list, bb, D3D12_RESOURCE_STATE_PRESENT);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    device_->queue()->ExecuteCommandLists(1, lists);

    if (reflex_ && item.real) reflex_->mark(item.reflexFrame, nv::ReflexMarker::PresentStart);
    const HRESULT hr = sc.present(vsync_.load());
    const core::Micros presented = nowMicros();
    if (reflex_ && item.real) reflex_->mark(item.reflexFrame, nv::ReflexMarker::PresentEnd);
    const uint64_t fence = device_->signal();
    allocatorFence_[a] = fence;
    if (callbacks_.presented && SUCCEEDED(hr)) callbacks_.presented(presented, item.arrival, !item.real);
    if (item.lastOfBatch && callbacks_.released) callbacks_.released(item.batchSlot, fence);
    return SUCCEEDED(hr);
}

} // namespace vela::engine
