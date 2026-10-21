#include "nv/NgxCore.h"

#include "base/Log.h"

#include <format>

namespace vela::nv {
namespace {
// Project identity for a custom engine. NVIDIA expects a GUID-like string.
constexpr const char* kProjectId = "8f1e2c4a-6b7d-4e39-9a15-0c3d5e7f9a21";
constexpr const char* kEngineVersion = "0.1.0";

__declspec(noinline) NVSDK_NGX_Result callInit(ID3D12Device* device, const wchar_t* dataPath,
                                               const NVSDK_NGX_FeatureCommonInfo* info, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                dataPath, device, info, NVSDK_NGX_Version_API);
    } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callShutdown(ID3D12Device* device, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NVSDK_NGX_D3D12_Shutdown1(device); } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callAllocate(NVSDK_NGX_Parameter** out, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NVSDK_NGX_D3D12_AllocateParameters(out); } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callDestroy(NVSDK_NGX_Parameter* p, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NVSDK_NGX_D3D12_DestroyParameters(p); } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}

__declspec(noinline) NVSDK_NGX_Result callCapabilities(NVSDK_NGX_Parameter** out, DWORD& seh) {
    seh = 0;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
    VELA_SEH_TRY { r = NVSDK_NGX_D3D12_GetCapabilityParameters(out); } VELA_SEH_EXCEPT(seh) { r = NVSDK_NGX_Result_FAIL_PlatformError; }
    return r;
}
} // namespace

std::string ngxResult(NVSDK_NGX_Result result) {
    return std::format("0x{:08X}", static_cast<uint32_t>(result));
}

bool NgxCore::initialize(ID3D12Device* device, const std::wstring& runtimeDirectory) {
    if (initialized_) return true;
    runtimeDirectory_ = runtimeDirectory;
    const wchar_t* paths[] = {runtimeDirectory_.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    const auto dataPath = (appDataDirectory() / L"ngx").wstring();
    std::error_code ec;
    std::filesystem::create_directories(dataPath, ec);
    DWORD seh = 0;
    const auto r = callInit(device, dataPath.c_str(), &info, seh);
    log::info("ngx", std::format("Init_with_ProjectID result={} seh=0x{:X}", ngxResult(r), seh));
    if (NVSDK_NGX_FAILED(r) || seh) return false;
    device_ = device;
    initialized_ = true;
    return true;
}

void NgxCore::shutdown() {
    if (!initialized_) return;
    DWORD seh = 0;
    const auto r = callShutdown(device_, seh);
    log::info("ngx", std::format("Shutdown1 result={} seh=0x{:X}", ngxResult(r), seh));
    initialized_ = false;
    device_ = nullptr;
}

NVSDK_NGX_Parameter* NgxCore::allocate() {
    if (!initialized_) return nullptr;
    NVSDK_NGX_Parameter* p = nullptr;
    DWORD seh = 0;
    const auto r = callAllocate(&p, seh);
    if (NVSDK_NGX_FAILED(r) || seh) {
        log::error("ngx", std::format("AllocateParameters result={} seh=0x{:X}", ngxResult(r), seh));
        return nullptr;
    }
    return p;
}

void NgxCore::destroy(NVSDK_NGX_Parameter* parameters) {
    if (!parameters) return;
    DWORD seh = 0;
    callDestroy(parameters, seh);
}

NVSDK_NGX_Parameter* NgxCore::capabilities() {
    if (!initialized_) return nullptr;
    NVSDK_NGX_Parameter* p = nullptr;
    DWORD seh = 0;
    const auto r = callCapabilities(&p, seh);
    return NVSDK_NGX_SUCCEED(r) && !seh ? p : nullptr;
}

void NgxCore::releaseCapabilities(NVSDK_NGX_Parameter* caps) {
    // GetCapabilityParameters hands out a block the core did not allocate;
    // it is destroyed directly, like the SDK samples do.
    destroy(caps);
}

} // namespace vela::nv
