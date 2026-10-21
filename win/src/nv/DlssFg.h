#pragma once
// DLSS Frame Generation on video frames. Adapted from Veyra (GPL-3.0)
// src/ngx/DlssFgBackend, without the RTX 30/40 unlock paths: this build
// targets RTX 50 (Blackwell), where multi-frame generation is native.
#include "nv/NgxCore.h"

namespace vela::nv {

class DlssFg {
public:
    struct Caps {
        bool available = false;
        unsigned maxMultiplier = 0;     // MultiFrameCountMax + 1
        bool needsDriver = false;
        bool hags = false;              // HwSchMode == 2
    };
    static Caps query(NgxCore& core);

    struct CreateDesc {
        uint32_t width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;   // RGB10A2 for HDR10
        bool performanceMode = false;   // DLSSG.LiteCandidateBlendingEnabled, runtimes that know it
    };
    struct EvalDesc {
        ID3D12Resource* backbuffer = nullptr;   // current real frame (NON_PIXEL_SHADER_RESOURCE)
        ID3D12Resource* depth = nullptr;        // constant zero R32_FLOAT
        ID3D12Resource* motion = nullptr;       // RG16F, output pixels, current -> previous
        ID3D12Resource* output = nullptr;       // generated frame (UNORDERED_ACCESS)
        bool reset = false;
        bool hdr = false;
        uint64_t frameId = 0;                   // increments per real frame
        unsigned count = 1, index = 1;          // generated frames per pair, 1-based index
    };

    ~DlssFg() { release(); }
    bool create(NgxCore& core, ID3D12GraphicsCommandList* list, const CreateDesc& desc);
    bool evaluate(ID3D12GraphicsCommandList* list, const EvalDesc& desc);
    void release();
    bool ready() const { return handle_ && !fatal_; }

private:
    NgxCore* core_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    bool fatal_ = false;
};

} // namespace vela::nv
