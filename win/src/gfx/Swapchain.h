#pragma once
#include "gfx/Device.h"
#include "vela/core/ModePlanner.h"

namespace vela::gfx {

// Flip-model swap chain for the video window. SDR: RGBA8 / sRGB. HDR: RGB10A2
// with the PQ BT.2020 colour space (HDR10), which is also the DLSS-G HDR
// format, so frames go to the back buffer with a plain copy.
class Swapchain {
public:
    static constexpr uint32_t kBuffers = 3;

    bool create(Device& device, HWND window, uint32_t width, uint32_t height, bool hdr);
    bool resize(uint32_t width, uint32_t height, bool hdr);   // caller drained the queue
    void release();

    HANDLE waitable() const { return waitable_; }
    uint32_t current() const { return swapchain_->GetCurrentBackBufferIndex(); }
    gfx::Texture& buffer(uint32_t i) { return buffers_[i]; }
    HRESULT present(bool vsync);
    void setHdrMetadata(float maxCll, float maxFall, float masteringPeak);

    bool hdr() const { return hdr_; }
    DXGI_FORMAT format() const { return hdr_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool tearingSupported() const { return tearing_; }

    // Monitor the window is on: size, refresh, whether Windows HDR is on.
    static core::DisplayInfo queryDisplay(IDXGIFactory6* factory, HWND window, float* peakNits);

private:
    bool acquireBuffers();
    void applyColorSpace();

    Device* device_ = nullptr;
    ComPtr<IDXGISwapChain4> swapchain_;
    gfx::Texture buffers_[kBuffers];
    HANDLE waitable_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    bool hdr_ = false, tearing_ = false;
};

} // namespace vela::gfx
