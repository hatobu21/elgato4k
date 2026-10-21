#include "gfx/Compute.h"

#include "base/Log.h"

#include <algorithm>
#include <format>
#include <fstream>

namespace vela::gfx {
namespace {
std::vector<char> readShader(const wchar_t* name) {
    const auto path = exeDirectory() / L"shaders" / (std::wstring(name) + L".cso");
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
} // namespace

bool ComputePass::create(const Device& device, const wchar_t* shaderName, uint32_t srvCount, uint32_t uavCount, uint32_t constantDwords) {
    srvCount_ = srvCount;
    uavCount_ = uavCount;
    constantDwords_ = constantDwords;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = srvCount;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = uavCount;

    // Parameter order is fixed (constants, SRV table, UAV table); an empty
    // table is replaced by a one-slot table so indices never shift.
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = std::max(constantDwords, 1u);
    for (int i = 0; i < 2; ++i) {
        if (ranges[i].NumDescriptors == 0) ranges[i].NumDescriptors = 1;
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device.d3d()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
        log::error("gfx", std::format("root signature for {} failed", narrow(shaderName)));
        return false;
    }

    const auto code = readShader(shaderName);
    if (code.empty()) {
        log::error("gfx", std::format("shader {}.cso missing next to the executable", narrow(shaderName)));
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_.Get();
    pso.CS = {code.data(), code.size()};
    if (FAILED(device.d3d()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)))) {
        log::error("gfx", std::format("pipeline state for {} failed", narrow(shaderName)));
        return false;
    }
    return true;
}

void ComputePass::record(ID3D12GraphicsCommandList* list, const Device& device, uint32_t srvTable, uint32_t uavTable,
                         const void* constants, uint32_t groupsX, uint32_t groupsY) const {
    // NGX evaluations change heaps and root state, so every pass rebinds.
    ID3D12DescriptorHeap* heaps[] = {device.heap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso_.Get());
    if (constants && constantDwords_) list->SetComputeRoot32BitConstants(0, constantDwords_, constants, 0);
    // Unused tables still get a valid (never read) handle.
    list->SetComputeRootDescriptorTable(1, device.gpu(srvCount_ ? srvTable : 0));
    list->SetComputeRootDescriptorTable(2, device.gpu(uavCount_ ? uavTable : 0));
    list->Dispatch(groupsX, groupsY, 1);
}

} // namespace vela::gfx
