#include "gfx/Swapchain.h"

#include "base/Log.h"

#include <format>

namespace vela::gfx {

bool Swapchain::create(Device& device, HWND window, uint32_t width, uint32_t height, bool hdr) {
    device_ = &device;
    hdr_ = hdr;
    BOOL allowTearing = FALSE;
    device.factory()->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    tearing_ = allowTearing != FALSE;

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width;
    d.Height = height;
    d.Format = format();
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kBuffers;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(device.factory()->CreateSwapChainForHwnd(device.queue(), window, &d, nullptr, nullptr, &sc1)) ||
        FAILED(sc1.As(&swapchain_))) {
        log::error("swapchain", "CreateSwapChainForHwnd failed");
        return false;
    }
    // Alt+Enter exclusive mode is not used: borderless fullscreen gets
    // independent flip on Windows 11 when nothing covers the window.
    device.factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    swapchain_->SetMaximumFrameLatency(1);
    waitable_ = swapchain_->GetFrameLatencyWaitableObject();
    width_ = width;
    height_ = height;
    applyColorSpace();
    log::info("swapchain", std::format("{}x{} hdr={} tearing={}", width, height, hdr_, tearing_));
    return acquireBuffers();
}

bool Swapchain::resize(uint32_t width, uint32_t height, bool hdr) {
    for (auto& b : buffers_) b = {};
    hdr_ = hdr;
    const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(swapchain_->ResizeBuffers(kBuffers, width, height, format(), flags))) {
        log::error("swapchain", "ResizeBuffers failed");
        return false;
    }
    width_ = width;
    height_ = height;
    applyColorSpace();
    return acquireBuffers();
}

void Swapchain::release() {
    for (auto& b : buffers_) b = {};
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    swapchain_.Reset();
}

bool Swapchain::acquireBuffers() {
    for (uint32_t i = 0; i < kBuffers; ++i) {
        auto& b = buffers_[i];
        if (FAILED(swapchain_->GetBuffer(i, IID_PPV_ARGS(&b.resource)))) return false;
        b.format = format();
        b.width = width_;
        b.height = height_;
        b.state = D3D12_RESOURCE_STATE_PRESENT;
    }
    return true;
}

void Swapchain::applyColorSpace() {
    const auto space = hdr_ ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT support = 0;
    if (SUCCEEDED(swapchain_->CheckColorSpaceSupport(space, &support)) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        swapchain_->SetColorSpace1(space);
    else if (hdr_)
        log::warn("swapchain", "HDR10 colour space not supported on this output");
}

void Swapchain::setHdrMetadata(float maxCll, float maxFall, float masteringPeak) {
    if (!hdr_ || !swapchain_) return;
    DXGI_HDR_METADATA_HDR10 m{};
    // BT.2020 primaries and D65 white in units of 0.00002.
    m.RedPrimary[0] = 35400; m.RedPrimary[1] = 14600;
    m.GreenPrimary[0] = 8500; m.GreenPrimary[1] = 39850;
    m.BluePrimary[0] = 6550; m.BluePrimary[1] = 2300;
    m.WhitePoint[0] = 15635; m.WhitePoint[1] = 16450;
    m.MaxMasteringLuminance = UINT(masteringPeak > 0 ? masteringPeak : 1000);
    m.MinMasteringLuminance = 1;   // 0.0001 nits
    m.MaxContentLightLevel = UINT16(maxCll > 0 ? maxCll : 1000);
    m.MaxFrameAverageLightLevel = UINT16(maxFall > 0 ? maxFall : 400);
    swapchain_->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(m), &m);
}

HRESULT Swapchain::present(bool vsync) {
    // VRR and tearing both present immediately; the pacer already decided when.
    const UINT flags = !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    return swapchain_->Present(vsync ? 1 : 0, flags);
}

core::DisplayInfo Swapchain::queryDisplay(IDXGIFactory6* factory, HWND window, float* peakNits) {
    core::DisplayInfo info;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            info.width = dm.dmPelsWidth;
            info.height = dm.dmPelsHeight;
            info.refreshHz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
        }
    }
    // Windows HDR state comes from the output's current colour space.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&d)) && d.Monitor == monitor) {
                info.hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                if (peakNits) *peakNits = d.MaxLuminance;
            }
            output.Reset();
        }
        adapter.Reset();
    }
    // VRR capability is not exposed by DXGI; tearing support + a G-SYNC /
    // FreeSync monitor is the practical signal, reported by the user setting.
    info.vrr = true;
    return info;
}

} // namespace vela::gfx
