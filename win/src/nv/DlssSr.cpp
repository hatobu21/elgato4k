#include "nv/DlssSr.h"

#include "base/Log.h"

#include <format>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nvsdk_ngx_helpers.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vela::nv {
namespace {
__declspec(noinline) NVSDK_NGX_Result callCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle** handle,
                                                 NVSDK_NGX_Parameter* params, NVSDK_NGX_DLSS_Create_Params* create, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, handle, params, create); }
    VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callEvaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle* handle,
                                                   NVSDK_NGX_Parameter* params, NVSDK_NGX_D3D12_DLSS_Eval_Params* eval, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NGX_D3D12_EVALUATE_DLSS_EXT(list, handle, params, eval); }
    VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callRelease(NVSDK_NGX_Handle* handle, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NVSDK_NGX_D3D12_ReleaseFeature(handle); } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}
} // namespace

bool DlssSr::create(NgxCore& core, ID3D12GraphicsCommandList* list, const CreateDesc& desc) {
    release();
    core_ = &core;
    params_ = core.allocate();
    if (!params_) return false;
    inWidth_ = desc.inWidth;
    inHeight_ = desc.inHeight;

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = desc.inWidth;
    create.Feature.InHeight = desc.inHeight;
    create.Feature.InTargetWidth = desc.outWidth;
    create.Feature.InTargetHeight = desc.outHeight;
    // The ratio is fixed by the capture mode, not by a quality preset; DLAA
    // style "quality" is the closest hint for 1.5x-2x video upscaling.
    create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    // Linear FP16 input needs IsHDR; video has no exposure texture.
    // Motion vectors are full (output) resolution, so MVLowRes stays clear.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    // DLSS 4 transformer model for every mode.
    for (const char* mode : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(params_, mode, unsigned(NVSDK_NGX_DLSS_Hint_Render_Preset_K));
    DWORD seh = 0;
    const auto r = callCreate(list, &handle_, params_, &create, seh);
    log::info("dlss-sr", std::format("create {}x{} -> {}x{} result={} seh=0x{:X}", desc.inWidth, desc.inHeight,
        desc.outWidth, desc.outHeight, ngxResult(r), seh));
    if (NVSDK_NGX_FAILED(r) || seh || !handle_) {
        handle_ = nullptr;
        release();
        return false;
    }
    return true;
}

bool DlssSr::evaluate(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* output,
                      ID3D12Resource* depth, ID3D12Resource* motion, bool reset, float sharpness) {
    if (!handle_) return false;
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = color;
    eval.Feature.pInOutput = output;
    eval.Feature.InSharpness = sharpness;
    eval.pInDepth = depth;
    eval.pInMotionVectors = motion;
    eval.InReset = reset ? 1 : 0;
    // Motion is in output pixels, current -> previous (same texture DLSS-G uses).
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    eval.InRenderSubrectDimensions = {inWidth_, inHeight_};
    DWORD seh = 0;
    const auto r = callEvaluate(list, handle_, params_, &eval, seh);
    if (NVSDK_NGX_FAILED(r) || seh) {
        log::error("dlss-sr", std::format("evaluate result={} seh=0x{:X}", ngxResult(r), seh));
        return false;
    }
    return true;
}

void DlssSr::release() {
    if (handle_) {
        DWORD seh = 0;
        callRelease(handle_, seh);
        handle_ = nullptr;
    }
    if (params_ && core_) core_->destroy(params_);
    params_ = nullptr;
}

} // namespace vela::nv
