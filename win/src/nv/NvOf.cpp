#include "nv/NvOf.h"

#include "base/Log.h"

// NVIDIA Optical Flow SDK headers (NvOFInterface), see docs for the SDK path.
#include <nvOpticalFlowCommon.h>
#include <nvOpticalFlowD3D12.h>

#include <format>
#include <memory>

namespace vela::nv {

using PFN_CreateInstance = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);
using PFN_MaxVersion = NV_OF_STATUS(NVOFAPI*)(uint32_t*);

struct NvOf::FnList {
    NV_OF_D3D12_API_FUNCTION_LIST list{};
};

bool NvOf::runtimePresent() {
    HMODULE m = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return false;
    FreeLibrary(m);
    return true;
}

bool NvOf::initialize(ID3D12Device* device, ID3D12Resource* inputA, ID3D12Resource* inputB,
                      ID3D12Resource* flowOut, ID3D12Resource* costOut, const Desc& desc) {
    if (!device || !inputA || !inputB || !flowOut || !costOut || !desc.inFence || !desc.outFence) return false;
    shutdown();
    inFence_ = desc.inFence;
    outFence_ = desc.outFence;

    // Absolute System32 load: the driver ships nvofapi64.dll.
    dll_ = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll_) { log::error("nvof", std::format("nvofapi64.dll load failed err={}", GetLastError())); return false; }
    auto maxVersion = reinterpret_cast<PFN_MaxVersion>(GetProcAddress(dll_, "NvOFGetMaxSupportedApiVersion"));
    auto createInstance = reinterpret_cast<PFN_CreateInstance>(GetProcAddress(dll_, "NvOFAPICreateInstanceD3D12"));
    uint32_t version = 0;
    if (!maxVersion || !createInstance || maxVersion(&version) != NV_OF_SUCCESS) { log::error("nvof", "exports missing"); return false; }
    fn_ = new FnList();
    if (createInstance(version, &fn_->list) != NV_OF_SUCCESS || !fn_->list.nvCreateOpticalFlowD3D12 || !fn_->list.nvOFInit ||
        !fn_->list.nvOFExecuteD3D12 || !fn_->list.nvOFRegisterResourceD3D12 || !fn_->list.nvOFDestroy || !fn_->list.nvOFGetCaps) {
        log::error("nvof", "function table incomplete");
        return false;
    }
    NvOFHandle handle = nullptr;
    if (fn_->list.nvCreateOpticalFlowD3D12(device, &handle) != NV_OF_SUCCESS || !handle) { log::error("nvof", "create failed"); return false; }
    of_ = handle;

    // Grid capability: two-call protocol, element counts (not bytes).
    uint32_t count = 0;
    if (fn_->list.nvOFGetCaps(handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &count) != NV_OF_SUCCESS || count == 0 || count > 64) return false;
    std::vector<uint32_t> grids(count);
    if (fn_->list.nvOFGetCaps(handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &count) != NV_OF_SUCCESS) return false;
    bool gridOk = false;
    for (uint32_t i = 0; i < count; ++i) gridOk |= grids[i] == desc.gridSize;
    if (!gridOk) { log::error("nvof", std::format("grid {} unsupported", desc.gridSize)); return false; }
    gridW_ = (desc.width + desc.gridSize - 1) / desc.gridSize;
    gridH_ = (desc.height + desc.gridSize - 1) / desc.gridSize;

    NV_OF_INIT_PARAMS init{};
    init.width = desc.width;
    init.height = desc.height;
    init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(desc.gridSize);
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = desc.quality == 0 ? NV_OF_PERF_LEVEL_FAST : desc.quality == 1 ? NV_OF_PERF_LEVEL_MEDIUM : NV_OF_PERF_LEVEL_SLOW;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_TRUE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    const auto initStatus = fn_->list.nvOFInit(handle, &init);
    log::info("nvof", std::format("init {}x{} grid={} perf={} status={}", desc.width, desc.height, desc.gridSize, int(init.perfLevel), int(initStatus)));
    if (initStatus != NV_OF_SUCCESS) return false;

    // Registration runs GPU work; it completes on its own fence, waited once.
    ComPtr<ID3D12Fence> regFence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&regFence)))) return false;
    std::unique_ptr<void, decltype(&CloseHandle)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), &CloseHandle);
    const uint64_t inputReady = inFence_->GetCompletedValue();
    uint64_t registered = 0;
    auto reg = [&](ID3D12Resource* resource, Handle& out, const char* name) {
        NvOFGPUBufferHandle h = nullptr;
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
        p.resource = resource;
        p.hOFGpuBuffer = &h;
        p.inputFencePoint = registered ? NV_OF_FENCE_POINT{regFence.Get(), registered} : NV_OF_FENCE_POINT{inFence_, inputReady};
        p.outputFencePoint = {regFence.Get(), registered + 1};
        const auto st = fn_->list.nvOFRegisterResourceD3D12(handle, &p);
        if (st != NV_OF_SUCCESS) { log::error("nvof", std::format("register {} status={}", name, int(st))); return false; }
        out = h;
        ++registered;
        return true;
    };
    const bool ok = reg(inputA, inputA_, "inputA") && reg(inputB, inputB_, "inputB") &&
                    reg(flowOut, flow_, "flow") && reg(costOut, cost_, "cost");
    if (registered && regFence->GetCompletedValue() < registered) {
        regFence->SetEventOnCompletion(registered, event.get());
        WaitForSingleObject(event.get(), 5000);
    }
    if (!ok || regFence->GetCompletedValue() < registered) {
        unregisterAll();
        return false;
    }
    initialized_ = true;
    return true;
}

bool NvOf::execute(uint64_t inValue) {
    if (!initialized_) return false;
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    NV_OF_FENCE_POINT inPoint{inFence_, inValue};
    NV_OF_FENCE_POINT outPoint{outFence_, nextOutValue_};
    in.inputFrame = static_cast<NvOFGPUBufferHandle>(inputB_);       // current
    in.referenceFrame = static_cast<NvOFGPUBufferHandle>(inputA_);   // previous
    in.disableTemporalHints = NV_OF_TRUE;
    in.numFencePoints = 1;
    in.fencePoint = &inPoint;
    out.outputBuffer = static_cast<NvOFGPUBufferHandle>(flow_);
    out.outputCostBuffer = static_cast<NvOFGPUBufferHandle>(cost_);
    out.fencePoint = &outPoint;
    const auto st = fn_->list.nvOFExecuteD3D12(static_cast<NvOFHandle>(of_), &in, &out);
    if (st != NV_OF_SUCCESS) {
        log::error("nvof", std::format("execute status={}", int(st)));
        return false;
    }
    ++nextOutValue_;
    return true;
}

void NvOf::unregister(Handle& h, const char* name) {
    if (!h || !fn_ || !fn_->list.nvOFUnregisterResourceD3D12) return;
    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{};
    p.hOFGpuBuffer = static_cast<NvOFGPUBufferHandle>(h);
    const auto st = fn_->list.nvOFUnregisterResourceD3D12(&p);
    if (st != NV_OF_SUCCESS) log::error("nvof", std::format("unregister {} status={}", name, int(st)));
    else h = nullptr;
}

bool NvOf::unregisterAll() {
    unregister(cost_, "cost");
    unregister(flow_, "flow");
    unregister(inputB_, "inputB");
    unregister(inputA_, "inputA");
    return !cost_ && !flow_ && !inputA_ && !inputB_;
}

void NvOf::shutdown() {
    unregisterAll();
    if (of_ && fn_ && fn_->list.nvOFDestroy) fn_->list.nvOFDestroy(static_cast<NvOFHandle>(of_));
    of_ = nullptr;
    delete fn_;
    fn_ = nullptr;
    if (dll_) FreeLibrary(dll_);
    dll_ = nullptr;
    initialized_ = false;
    inFence_ = outFence_ = nullptr;
    nextOutValue_ = 1;
}

} // namespace vela::nv
