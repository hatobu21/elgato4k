#pragma once
// Adapted from Veyra (GPL-3.0) src/ngx/NgxCoreHost: one NGX core per D3D12
// device; every feature allocates its parameters through it, and every call
// into the NVIDIA runtime is SEH-guarded.
#include "base/Win.h"

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <string>

namespace vela::nv {

std::string ngxResult(NVSDK_NGX_Result result);

class NgxCore {
public:
    ~NgxCore() { shutdown(); }

    // runtimeDirectory: absolute folder holding nvngx_dlss.dll, nvngx_dlssg.dll,
    // nvngx_vsr.dll and nvngx_truehdr.dll (see docs: runtime layout).
    bool initialize(ID3D12Device* device, const std::wstring& runtimeDirectory);
    void shutdown();
    bool ready() const { return initialized_; }

    NVSDK_NGX_Parameter* allocate();
    void destroy(NVSDK_NGX_Parameter* parameters);

    // Capability block (owned by NGX; release with releaseCapabilities).
    NVSDK_NGX_Parameter* capabilities();
    void releaseCapabilities(NVSDK_NGX_Parameter* caps);

    const std::wstring& runtimeDirectory() const { return runtimeDirectory_; }

private:
    ID3D12Device* device_ = nullptr;
    bool initialized_ = false;
    std::wstring runtimeDirectory_;
};

} // namespace vela::nv
