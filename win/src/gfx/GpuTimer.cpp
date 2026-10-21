#include "gfx/GpuTimer.h"

#include <algorithm>
#include <limits>

namespace vela::gfx {

bool GpuTimer::create(const Device& device) {
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = UINT(kSlots * kStages * 2);
    if (FAILED(device.d3d()->CreateQueryHeap(&q, IID_PPV_ARGS(&heap_)))) return false;
    readback_ = device.buffer(sizeof(uint64_t) * q.Count, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false, L"vela.timestamps");
    ticksToMs_ = device.timestampFrequency() ? 1000.0 / double(device.timestampFrequency()) : 0;
    return readback_ != nullptr;
}

void GpuTimer::begin(uint32_t slot) {
    marked_[slot].fill(0);
    resolved_[slot] = false;
}

void GpuTimer::mark(ID3D12GraphicsCommandList* list, uint32_t slot, core::GpuStage stage, bool end) {
    list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index(slot, stage, end));
    marked_[slot][static_cast<size_t>(stage)] |= end ? 2 : 1;
}

void GpuTimer::resolve(ID3D12GraphicsCommandList* list, uint32_t slot) {
    const UINT first = index(slot, core::GpuStage::Upload, false);
    list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, UINT(kStages * 2), readback_.Get(), first * sizeof(uint64_t));
    resolved_[slot] = true;
}

bool GpuTimer::read(uint32_t slot, StageArray& stagesMs, double& totalMs) {
    stagesMs.fill(-1);
    totalMs = -1;
    if (!resolved_[slot] || ticksToMs_ == 0) return false;
    const size_t first = index(slot, core::GpuStage::Upload, false);
    D3D12_RANGE range{first * sizeof(uint64_t), (first + kStages * 2) * sizeof(uint64_t)};
    uint64_t* data = nullptr;
    if (FAILED(readback_->Map(0, &range, reinterpret_cast<void**>(&data)))) return false;
    const uint64_t* t = data + first;
    uint64_t lo = std::numeric_limits<uint64_t>::max(), hi = 0;
    for (size_t s = 0; s < kStages; ++s) {
        if (marked_[slot][s] != 3) continue;
        const uint64_t b = t[s * 2], e = t[s * 2 + 1];
        if (e < b) continue;
        stagesMs[s] = double(e - b) * ticksToMs_;
        lo = std::min(lo, b);
        hi = std::max(hi, e);
    }
    D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    if (hi > lo) totalMs = double(hi - lo) * ticksToMs_;
    resolved_[slot] = false;
    return totalMs >= 0;
}

} // namespace vela::gfx
