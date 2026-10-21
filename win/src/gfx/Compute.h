#pragma once
#include "gfx/Device.h"

#include <vector>

namespace vela::gfx {

// One compute shader with a fixed root layout:
//   [0] root constants b0 (constantDwords)
//   [1] descriptor table t0..t(srvCount-1)
//   [2] descriptor table u0..u(uavCount-1)
//   s0  static linear-clamp sampler
// Descriptor tables are contiguous blocks built once per pipeline build, so
// recording a pass is three root calls and a Dispatch.
class ComputePass {
public:
    bool create(const Device& device, const wchar_t* shaderName, uint32_t srvCount, uint32_t uavCount, uint32_t constantDwords);
    void record(ID3D12GraphicsCommandList* list, const Device& device, uint32_t srvTable, uint32_t uavTable,
                const void* constants, uint32_t groupsX, uint32_t groupsY) const;
    explicit operator bool() const { return pso_ != nullptr; }
    uint32_t srvCount() const { return srvCount_; }
    uint32_t uavCount() const { return uavCount_; }

private:
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_;
    uint32_t srvCount_ = 0, uavCount_ = 0, constantDwords_ = 0;
};

inline uint32_t groups(uint32_t extent, uint32_t size = 16) { return (extent + size - 1) / size; }

} // namespace vela::gfx
