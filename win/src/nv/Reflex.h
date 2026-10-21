#pragma once
// NVIDIA Reflex low-latency mode and latency markers through nvapi64.dll.
// ABI subset checked against NVIDIA/nvapi (MIT); adapted from Veyra (GPL-3.0)
// include/veyra/gfx/ReflexSession.h.
//
// A capture pipeline cannot use Reflex's sleep: the frame arrives when the
// console sends it, and sleeping before processing only adds latency. Vela
// enables the driver low-latency mode and sends markers (so the driver and
// tools such as FrameView see the frame boundaries) but never calls Sleep.
#include "base/Win.h"
#include "base/Log.h"

#include <d3d12.h>

#include <format>

namespace vela::nv {

enum class ReflexMarker : uint32_t {
    SimulationStart = 0, SimulationEnd = 1, RenderSubmitStart = 2, RenderSubmitEnd = 3, PresentStart = 4, PresentEnd = 5,
};

class Reflex {
    struct SleepParams { uint32_t version; uint8_t low, boost; uint32_t interval; uint8_t markers, minQueue, reserved[30]; };
    struct MarkerParams { uint32_t version; uint64_t frame; uint32_t type; uint64_t reserved0; uint8_t reserved[56]; };
    static_assert(sizeof(SleepParams) == 44 && sizeof(MarkerParams) == 88);
    using Query = void*(__cdecl*)(uint32_t);
    using Init = int(__cdecl*)();
    using SetMode = int(__cdecl*)(IUnknown*, SleepParams*);
    using Marker = int(__cdecl*)(IUnknown*, MarkerParams*);

public:
    // nvapi64.dll stays loaded for the life of the process: the driver keeps
    // per-device state that is called back when the device is released.
    bool enable(ID3D12Device* device, bool on) {
        if (!load()) return false;
        device_ = device;
        SleepParams p{};
        p.version = sizeof(p) | (1u << 16);
        p.low = on ? 1 : 0;
        p.markers = on ? 1 : 0;
        const int r = setMode_(device_, &p);
        active_ = on && r == 0;
        log::info("reflex", std::format("low latency mode={} status={}", on, r));
        return r == 0;
    }
    void mark(uint64_t frame, ReflexMarker type) {
        if (!active_ || !frame) return;
        MarkerParams p{};
        p.version = sizeof(p) | (1u << 16);
        p.frame = frame;
        p.type = static_cast<uint32_t>(type);
        if (marker_(device_, &p) != 0) active_ = false;
    }
    bool active() const { return active_; }

private:
    bool load() {
        if (setMode_) return true;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        HMODULE m = LoadLibraryExW((std::wstring(system) + L"\\nvapi64.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE pinned = nullptr;
        if (m) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(m), &pinned);
        auto query = m ? reinterpret_cast<Query>(GetProcAddress(m, "nvapi_QueryInterface")) : nullptr;
        if (!query) return false;
        auto init = reinterpret_cast<Init>(query(0x0150E828));
        setMode_ = reinterpret_cast<SetMode>(query(0xac1ca9e0));
        marker_ = reinterpret_cast<Marker>(query(0xd9984c05));
        if (!init || !setMode_ || !marker_ || init() != 0) { setMode_ = nullptr; return false; }
        return true;
    }

    ID3D12Device* device_ = nullptr;
    SetMode setMode_ = nullptr;
    Marker marker_ = nullptr;
    bool active_ = false;
};

} // namespace vela::nv
