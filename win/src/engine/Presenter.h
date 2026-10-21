#pragma once
// Present thread. Takes finished frames with the host time the pacer chose,
// waits until that time (high resolution timer + short spin), copies the
// frame into the back buffer (or composes it when the window size differs or
// the HUD is on) and presents. Late generated frames are dropped here too.
#include "engine/Pipeline.h"
#include "gfx/Compute.h"
#include "gfx/Swapchain.h"
#include "nv/Reflex.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace vela::engine {

struct PresentItem {
    gfx::Texture* texture = nullptr;
    uint32_t srv = 0;
    core::Micros target = 0;        // pacer's present time
    core::Micros arrival = 0;       // capture callback of the real frame of the batch
    core::Micros spacing = 0;       // planned distance to the next frame
    bool real = false;
    bool lastOfBatch = false;
    uint32_t batchSlot = 0;
    uint64_t reflexFrame = 0;
};

struct HudText {
    bool on = false;
    unsigned corner = 1;
    std::array<uint8_t, 16> glyphs{};
    unsigned count = 0;
    static HudText format(double e2eMs, double outputFps, double sourceFps, bool on, unsigned corner);
};

class Presenter {
public:
    struct Callbacks {
        std::function<void(core::Micros presented, core::Micros arrival, bool generated)> presented;
        std::function<void(unsigned dropped)> dropped;
        std::function<void(uint32_t batchSlot, uint64_t fence)> released;
    };

    ~Presenter() { stop(); }
    bool start(gfx::Device& device, gfx::Swapchain& swapchain, nv::Reflex* reflex, Callbacks callbacks);
    void stop();
    void push(std::vector<PresentItem> items);
    // Drops everything queued and reports those batches released.
    void discard();
    void setHud(const HudText& hud);
    // Where the picture goes in the window (device pixels). Windowed layout
    // puts it in a pane beside the scope column; empty = the whole window.
    void setViewport(const RECT& rect);
    void setVsync(bool vsync) { vsync_ = vsync; }

private:
    void run();
    bool presentOne(const PresentItem& item);
    void sleepUntil(core::Micros target);

    gfx::Device* device_ = nullptr;
    gfx::Swapchain* swapchain_ = nullptr;
    nv::Reflex* reflex_ = nullptr;
    Callbacks callbacks_;
    gfx::ComputePass compose_;
    gfx::Texture composite_;
    uint32_t compositeUav_ = 1;     // reserved descriptor range of the device
    std::array<ComPtr<ID3D12CommandAllocator>, 3> allocators_;
    std::array<uint64_t, 3> allocatorFence_{};
    uint32_t allocatorIndex_ = 0;
    ComPtr<ID3D12GraphicsCommandList> list_;
    HANDLE timer_ = nullptr;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PresentItem> queue_;
    HudText hud_;
    RECT viewport_{};
    std::atomic<bool> vsync_{false};
    bool stop_ = false;
};

} // namespace vela::engine
