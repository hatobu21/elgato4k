#include "nv/DlssFg.h"

#include "base/Log.h"

#include <algorithm>
#include <format>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace vela::nv {
namespace {
constexpr const char* kLiteParameter = "DLSSG.LiteCandidateBlendingEnabled";

__declspec(noinline) NVSDK_NGX_Result callCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle** handle,
                                                 NVSDK_NGX_Parameter* params, NVSDK_NGX_DLSSG_Create_Params* create, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NGX_D3D12_CREATE_DLSSG(list, 1, 1, handle, params, create); }
    VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callEvaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle* handle,
                                                   NVSDK_NGX_Parameter* params, NVSDK_NGX_D3D12_DLSSG_Eval_Params* eval,
                                                   NVSDK_NGX_DLSSG_Opt_Eval_Params* opt, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NGX_D3D12_EVALUATE_DLSSG(list, handle, params, eval, opt); }
    VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callRelease(NVSDK_NGX_Handle* handle, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NVSDK_NGX_D3D12_ReleaseFeature(handle); } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

bool hagsEnabled() {
    HKEY key = nullptr;
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const bool ok = RegQueryValueExW(key, L"HwSchMode", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && value == 2;
    RegCloseKey(key);
    return ok;
}
} // namespace

DlssFg::Caps DlssFg::query(NgxCore& core) {
    Caps caps;
    caps.hags = hagsEnabled();
    NVSDK_NGX_Parameter* p = core.capabilities();
    if (!p) return caps;
    int available = 0, needsDriver = 0;
    unsigned int multiMax = 0;
    p->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    p->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needsDriver);
    p->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &multiMax);
    core.releaseCapabilities(p);
    caps.available = available != 0;
    caps.needsDriver = needsDriver != 0;
    caps.maxMultiplier = caps.available ? std::max(multiMax, 1u) + 1 : 0;
    log::info("dlssg", std::format("available={} needsDriver={} multiFrameCountMax={} hags={}",
        caps.available, caps.needsDriver, multiMax, caps.hags));
    return caps;
}

bool DlssFg::create(NgxCore& core, ID3D12GraphicsCommandList* list, const CreateDesc& desc) {
    release();
    core_ = &core;
    params_ = core.allocate();
    if (!params_) return false;
    width_ = desc.width;
    height_ = desc.height;
    fatal_ = false;

    // Backbuffer, motion and depth are always tagged; HUD-less, UI and the
    // distortion field never exist for video and are declared absent.
    params_->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags,
        unsigned(NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth));
    params_->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,
        unsigned(NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI |
                 NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField));
    params_->Set(kLiteParameter, desc.performanceMode ? 1u : 0u);

    NVSDK_NGX_DLSSG_Create_Params create{};
    create.Width = desc.width;
    create.Height = desc.height;
    create.NativeBackbufferFormat = unsigned(desc.format);
    create.RenderWidth = desc.width;
    create.RenderHeight = desc.height;
    create.DynamicResolutionScaling = false;
    DWORD seh = 0;
    const auto r = callCreate(list, &handle_, params_, &create, seh);
    log::info("dlssg", std::format("create {}x{} format={} perfMode={} result={} seh=0x{:X}",
        desc.width, desc.height, int(desc.format), desc.performanceMode, ngxResult(r), seh));
    if (NVSDK_NGX_FAILED(r) || seh || !handle_) {
        handle_ = nullptr;
        release();
        return false;
    }
    return true;
}

bool DlssFg::evaluate(ID3D12GraphicsCommandList* list, const EvalDesc& d) {
    if (!handle_ || fatal_ || d.count < 1 || d.count > 5 || d.index < 1 || d.index > d.count) return false;

    NVSDK_NGX_D3D12_DLSSG_Eval_Params eval{};
    eval.pBackbuffer = d.backbuffer;
    eval.pDepth = d.depth;
    eval.pMVecs = d.motion;
    eval.pOutputInterpFrame = d.output;

    // A still camera: all motion lives in the motion vector texture, but the
    // projection and its inverse must still describe the same camera.
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};
    opt.multiFrameCount = d.count;
    opt.multiFrameIndex = d.index;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float v = r == c ? 1.0f : 0.0f;
            opt.cameraViewToClip[r][c] = opt.clipToCameraView[r][c] = opt.clipToLensClip[r][c] =
                opt.clipToPrevClip[r][c] = opt.prevClipToClip[r][c] = v;
        }
    opt.mvecScale[0] = -1.0f / float(width_);   // pixels, current -> previous, to normalised
    opt.mvecScale[1] = -1.0f / float(height_);
    opt.cameraNear = 0.1f;
    opt.cameraFar = 1000.0f;
    opt.cameraFOV = 1.5707963f;
    opt.cameraAspectRatio = float(width_) / float(height_);
    opt.cameraUp[1] = 1.0f;
    opt.cameraRight[0] = 1.0f;
    opt.cameraFwd[2] = 1.0f;
    const float px = 1.0f / opt.cameraAspectRatio;
    const float pz = opt.cameraFar / (opt.cameraFar - opt.cameraNear);
    const float pw = -opt.cameraNear * pz;
    opt.cameraViewToClip[0][0] = px;
    opt.cameraViewToClip[2][2] = pz;
    opt.cameraViewToClip[2][3] = 1.0f;
    opt.cameraViewToClip[3][2] = pw;
    opt.cameraViewToClip[3][3] = 0.0f;
    opt.clipToCameraView[0][0] = 1.0f / px;
    opt.clipToCameraView[2][2] = 0.0f;
    opt.clipToCameraView[2][3] = 1.0f / pw;
    opt.clipToCameraView[3][2] = 1.0f;
    opt.clipToCameraView[3][3] = -pz / pw;
    opt.colorBuffersHDR = d.hdr;
    opt.depthInverted = false;
    opt.cameraMotionIncluded = true;
    opt.reset = d.reset;
    opt.notRenderingGameFrames = false;
    opt.motionVectorsInvalidValue = 3.402823466e38f;   // zero is a valid "no motion"
    opt.motionVectorsDilated = false;
    opt.menuDetectionEnabled = false;
    opt.backbufferSubrectSize = {width_, height_};
    opt.outputInterpSubrectSize = {width_, height_};
    opt.mvecsSubrectSize = {width_, height_};
    opt.depthSubrectSize = {width_, height_};

    params_->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(d.frameId));
    DWORD seh = 0;
    const auto r = callEvaluate(list, handle_, params_, &eval, &opt, seh);
    if (NVSDK_NGX_FAILED(r) || seh) {
        // Never call a handle again after a runtime fault; the engine drops
        // frame generation and re-creates it on the next pipeline build.
        log::error("dlssg", std::format("evaluate frame={} {}/{} result={} seh=0x{:X}", d.frameId, d.index, d.count, ngxResult(r), seh));
        fatal_ = true;
        return false;
    }
    return true;
}

void DlssFg::release() {
    if (handle_) {
        DWORD seh = 0;
        const auto r = callRelease(handle_, seh);
        log::info("dlssg", std::format("release result={} seh=0x{:X}", ngxResult(r), seh));
        handle_ = nullptr;
    }
    if (params_ && core_) core_->destroy(params_);
    params_ = nullptr;
}

} // namespace vela::nv
