#pragma once
// NVIDIA Optical Flow (NVOF) D3D12 session. Ported from Veyra (GPL-3.0)
// src/ngx/NvOfSession with the reverse-flow and fault-injection paths removed.
//
// Contract:
//  - inputA = previous frame, inputB = current frame, both B8G8R8A8_UNORM at
//    width x height; vectors point current -> previous, S10.5 SHORT2 at grid
//    extent in flowOut (R16G16_SINT), cost in costOut (R8_UINT, same extent).
//  - The caller signals inFence at inValue once both inputs are written; NVOF
//    signals outFence at nextOutValue(); the caller's queue waits on that.
//  - Teardown order: unregisterAll() -> release textures -> shutdown().
#include "base/Win.h"

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace vela::nv {

class NvOf {
public:
    struct Desc {
        uint32_t width = 0, height = 0;
        ID3D12Fence* inFence = nullptr;
        ID3D12Fence* outFence = nullptr;
        uint32_t quality = 1;       // 0 fast, 1 medium, 2 slow
        uint32_t gridSize = 4;
    };

    ~NvOf() { shutdown(); }
    bool initialize(ID3D12Device* device, ID3D12Resource* inputA, ID3D12Resource* inputB,
                    ID3D12Resource* flowOut, ID3D12Resource* costOut, const Desc& desc);
    bool execute(uint64_t inValue);
    bool unregisterAll();
    void shutdown();

    bool ready() const { return initialized_; }
    uint64_t nextOutValue() const { return nextOutValue_; }
    uint32_t gridWidth() const { return gridW_; }
    uint32_t gridHeight() const { return gridH_; }
    static bool runtimePresent();

private:
    struct FnList;
    using Handle = void*;
    void unregister(Handle& h, const char* name);

    HMODULE dll_ = nullptr;
    FnList* fn_ = nullptr;
    Handle of_ = nullptr, inputA_ = nullptr, inputB_ = nullptr, flow_ = nullptr, cost_ = nullptr;
    ID3D12Fence* inFence_ = nullptr;
    ID3D12Fence* outFence_ = nullptr;
    uint64_t nextOutValue_ = 1;
    uint32_t gridW_ = 0, gridH_ = 0;
    bool initialized_ = false;
};

} // namespace vela::nv
