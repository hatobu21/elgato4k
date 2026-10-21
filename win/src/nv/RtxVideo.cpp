#include "nv/RtxVideo.h"

#include "base/Log.h"

#include <filesystem>
#include <format>

namespace vela::nv {
namespace {
// RTX Video SDK 1.1 feature ids on the NGX core ABI.
constexpr NVSDK_NGX_Feature kVideoSr = NVSDK_NGX_Feature_Reserved16;
constexpr NVSDK_NGX_Feature kTrueHdr = NVSDK_NGX_Feature_Reserved14;

enum class Op { Create, Evaluate, Release };

__declspec(noinline) NVSDK_NGX_Result invoke(Op op, NVSDK_NGX_Feature feature, ID3D12GraphicsCommandList* list,
                                             NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY {
        if (op == Op::Create) r = NVSDK_NGX_D3D12_CreateFeature(list, feature, p, h);
        else if (op == Op::Evaluate) r = NVSDK_NGX_D3D12_EvaluateFeature(list, *h, p);
        else r = NVSDK_NGX_D3D12_ReleaseFeature(*h);
    } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

bool check(const char* tag, const char* what, NVSDK_NGX_Result r, DWORD seh) {
    const bool ok = NVSDK_NGX_SUCCEED(r) && !seh;
    if (!ok) log::error(tag, std::format("{} result={} seh=0x{:X}", what, ngxResult(r), seh));
    return ok;
}
} // namespace

// ---------------------------------------------------------------- VideoSr
bool VideoSr::create(NgxCore& core, ID3D12GraphicsCommandList* list) {
    release();
    core_ = &core;
    params_ = core.allocate();
    if (!params_) return false;
    params_->Set("CreationNodeMask", 1u);
    params_->Set("VisibilityNodeMask", 1u);
    DWORD seh = 0;
    const auto r = invoke(Op::Create, kVideoSr, list, params_, &handle_, seh);
    log::info("video-sr", std::format("create result={} seh=0x{:X}", ngxResult(r), seh));
    if (!check("video-sr", "create", r, seh) || !handle_) {
        handle_ = nullptr;
        release();
        return false;
    }
    return true;
}

bool VideoSr::evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output, unsigned quality) {
    if (!handle_ || !input || !output || quality < 1 || quality > 4) return false;
    const auto a = input->GetDesc(), b = output->GetDesc();
    params_->Set("Input1", input);
    params_->Set("Output", output);
    params_->Set("Rect.X", 0u);
    params_->Set("Rect.Y", 0u);
    params_->Set("Rect.W", unsigned(a.Width));
    params_->Set("Rect.H", unsigned(a.Height));
    params_->Set("OutRect.X", 0u);
    params_->Set("OutRect.Y", 0u);
    params_->Set("OutRect.W", unsigned(b.Width));
    params_->Set("OutRect.H", unsigned(b.Height));
    params_->Set("VSR.QualityLevel", quality);
    DWORD seh = 0;
    return check("video-sr", "evaluate", invoke(Op::Evaluate, kVideoSr, list, params_, &handle_, seh), seh);
}

void VideoSr::release() {
    if (handle_) {
        DWORD seh = 0;
        invoke(Op::Release, kVideoSr, nullptr, params_, &handle_, seh);
        handle_ = nullptr;
    }
    if (params_ && core_) core_->destroy(params_);
    params_ = nullptr;
}

// ---------------------------------------------------------------- TrueHdr
bool TrueHdr::create(NgxCore& core, ID3D12GraphicsCommandList* list) {
    release();
    // Pin the runtime from the explicit runtime folder before NGX looks for it.
    const auto path = std::filesystem::path(core.runtimeDirectory()) / L"nvngx_truehdr.dll";
    module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) {
        log::error("true-hdr", std::format("nvngx_truehdr.dll not loadable (win32={})", GetLastError()));
        return false;
    }
    core_ = &core;
    params_ = core.allocate();
    if (!params_) return false;
    params_->Set("CreationNodeMask", 1u);
    params_->Set("VisibilityNodeMask", 1u);
    DWORD seh = 0;
    const auto r = invoke(Op::Create, kTrueHdr, list, params_, &handle_, seh);
    log::info("true-hdr", std::format("create result={} seh=0x{:X}", ngxResult(r), seh));
    if (!check("true-hdr", "create", r, seh) || !handle_) {
        handle_ = nullptr;
        release();
        return false;
    }
    return true;
}

bool TrueHdr::evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output, const core::HdrSettings& s) {
    if (!handle_ || !input || !output) return false;
    const auto a = input->GetDesc(), b = output->GetDesc();
    params_->Set("Input1", input);
    params_->Set("Output", output);
    params_->Set("TrueHDR.InLeft", 0u);
    params_->Set("TrueHDR.InTop", 0u);
    params_->Set("TrueHDR.InRight", unsigned(a.Width));
    params_->Set("TrueHDR.InBottom", unsigned(a.Height));
    params_->Set("TrueHDR.OutLeft", 0u);
    params_->Set("TrueHDR.OutTop", 0u);
    params_->Set("TrueHDR.OutRight", unsigned(b.Width));
    params_->Set("TrueHDR.OutBottom", unsigned(b.Height));
    // The SDK's own scale: contrast/saturation 0..200 (100 neutral), middle grey 10..100, peak in nits.
    params_->Set("TrueHDR.Contrast", s.contrast);
    params_->Set("TrueHDR.Saturation", s.saturation);
    params_->Set("TrueHDR.MiddleGray", s.middleGray);
    params_->Set("TrueHDR.MaxLuminance", s.peakNits);
    DWORD seh = 0;
    return check("true-hdr", "evaluate", invoke(Op::Evaluate, kTrueHdr, list, params_, &handle_, seh), seh);
}

void TrueHdr::release() {
    if (handle_) {
        DWORD seh = 0;
        invoke(Op::Release, kTrueHdr, nullptr, params_, &handle_, seh);
        handle_ = nullptr;
    }
    if (params_ && core_) core_->destroy(params_);
    params_ = nullptr;
    core_ = nullptr;
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
}

} // namespace vela::nv
