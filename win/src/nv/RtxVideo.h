#pragma once
// RTX Video SDK features through the shared NGX core: Video Super Resolution
// (SDR RGBA8 -> RGBA8) and TrueHDR (SDR RGBA8 -> scRGB FP16, 1.0 = 80 nits).
// Adapted from Veyra (GPL-3.0) src/ngx/VideoSrBackend and TrueHdrBackend.
#include "nv/NgxCore.h"
#include "vela/core/Settings.h"

namespace vela::nv {

class VideoSr {
public:
    ~VideoSr() { release(); }
    bool create(NgxCore& core, ID3D12GraphicsCommandList* list);
    // input/output in COMMON state, both R8G8B8A8_UNORM. quality 1..4.
    bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output, unsigned quality);
    void release();
    bool ready() const { return handle_ != nullptr; }

private:
    NgxCore* core_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
};

class TrueHdr {
public:
    ~TrueHdr() { release(); }
    bool create(NgxCore& core, ID3D12GraphicsCommandList* list);
    // input NON_PIXEL_SHADER_RESOURCE RGBA8; output UNORDERED_ACCESS RGBA16F, same size.
    bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output, const core::HdrSettings& s);
    void release();
    bool ready() const { return handle_ != nullptr; }

private:
    NgxCore* core_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    HMODULE module_ = nullptr;
};

} // namespace vela::nv
