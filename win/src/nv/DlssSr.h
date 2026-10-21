#pragma once
// DLSS Super Resolution for HDR video frames (linear FP16 in and out).
// Adapted from Veyra (GPL-3.0) src/ngx/DlssSrBackend.
#include "nv/NgxCore.h"

namespace vela::nv {

class DlssSr {
public:
    struct CreateDesc {
        uint32_t inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
    };
    ~DlssSr() { release(); }
    bool create(NgxCore& core, ID3D12GraphicsCommandList* list, const CreateDesc& desc);
    // color: NON_PIXEL_SHADER_RESOURCE, output: UNORDERED_ACCESS. Motion at
    // output resolution in output pixels, current -> previous.
    bool evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output,
                  ID3D12Resource* depth, ID3D12Resource* motion, bool reset, float sharpness);
    void release();
    bool ready() const { return handle_ != nullptr; }

private:
    NgxCore* core_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* handle_ = nullptr;
    uint32_t inWidth_ = 0, inHeight_ = 0;
};

} // namespace vela::nv
