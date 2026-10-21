#pragma once
#include "gfx/Device.h"
#include "vela/core/LatencyMonitor.h"

#include <array>

namespace vela::gfx {

// D3D12 timestamp queries for the stages of one batch. Each batch slot has
// its own queries and readback region; read() is called only after the
// batch's fence completed, so the CPU never waits for timing data.
class GpuTimer {
public:
    static constexpr uint32_t kSlots = 4;
    static constexpr size_t kStages = static_cast<size_t>(core::GpuStage::Count);
    using StageArray = std::array<double, kStages>;

    bool create(const Device& device);
    void begin(uint32_t slot);
    void mark(ID3D12GraphicsCommandList* list, uint32_t slot, core::GpuStage stage, bool end);
    void resolve(ID3D12GraphicsCommandList* list, uint32_t slot);
    bool read(uint32_t slot, StageArray& stagesMs, double& totalMs);

private:
    static uint32_t index(uint32_t slot, core::GpuStage stage, bool end) {
        return slot * kStages * 2 + static_cast<uint32_t>(stage) * 2 + (end ? 1 : 0);
    }
    ComPtr<ID3D12QueryHeap> heap_;
    ComPtr<ID3D12Resource> readback_;
    std::array<std::array<uint8_t, kStages>, kSlots> marked_{};   // bit0 begin, bit1 end
    std::array<bool, kSlots> resolved_{};
    double ticksToMs_ = 0;
};

} // namespace vela::gfx
