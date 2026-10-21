#include "gfx/Device.h"

#include "base/Log.h"

#include <d3d12sdklayers.h>

#include <format>

namespace vela::gfx {

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

void transition(ID3D12GraphicsCommandList* list, Texture& texture, D3D12_RESOURCE_STATES after) {
    transition(list, texture.get(), texture.state, after);
    texture.state = after;
}

void uavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    list->ResourceBarrier(1, &b);
}

bool Device::create(bool debugLayer) {
    if (debugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)))) {
        log::error("gfx", "CreateDXGIFactory2 failed");
        return false;
    }
    // Prefer the high-performance NVIDIA adapter; the whole product depends on it.
    ComPtr<IDXGIAdapter4> fallback;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter4> candidate;
        if (factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC3 desc{};
        candidate->GetDesc3(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
        if (!fallback) fallback = candidate;
        if (desc.VendorId == 0x10DE) { adapter_ = candidate; break; }
    }
    if (!adapter_) adapter_ = fallback;
    if (!adapter_) { log::error("gfx", "no hardware adapter"); return false; }

    DXGI_ADAPTER_DESC3 desc{};
    adapter_->GetDesc3(&desc);
    adapterName_ = desc.Description;
    vendorId_ = desc.VendorId;
    vramBytes_ = desc.DedicatedVideoMemory;
    luid_ = desc.AdapterLuid;
    if (vendorId_ != 0x10DE) log::warn("gfx", "adapter is not NVIDIA: DLSS, RTX Video and NVOF will be unavailable");

    if (FAILED(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)))) {
        log::error("gfx", "D3D12CreateDevice failed");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    // High priority: the queue competes with nothing of ours, but a game
    // overlay or browser on the same GPU should not delay presents.
    q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(device_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_)))) return false;
    queue_->SetName(L"vela.direct");
    queue_->GetTimestampFrequency(&timestampFrequency_);
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC h{};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    h.NumDescriptors = kHeapSize;
    h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap_)))) return false;
    descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    nullSrv(0);   // slot 0: the handle bound for tables a pass does not use

    log::info("gfx", std::format("adapter={} vendor=0x{:04X} vram={}MB", narrow(adapterName_), vendorId_, vramBytes_ >> 20));
    return true;
}

uint64_t Device::signal() {
    std::lock_guard lock(signalMutex_);
    ++fenceValue_;
    queue_->Signal(fence_.Get(), fenceValue_);
    return fenceValue_;
}

bool Device::waitCpu(uint64_t value, DWORD timeoutMs) {
    if (fence_->GetCompletedValue() >= value) return true;
    std::lock_guard lock(waitMutex_);
    fence_->SetEventOnCompletion(value, fenceEvent_);
    return WaitForSingleObject(fenceEvent_, timeoutMs) == WAIT_OBJECT_0 || fence_->GetCompletedValue() >= value;
}

void Device::waitIdle() {
    if (queue_) waitCpu(signal(), 5000);
}

uint32_t Device::allocate(uint32_t count) {
    if (next_ + count > kHeapSize) {
        log::error("gfx", "descriptor heap exhausted");
        return UINT32_MAX;
    }
    const uint32_t first = next_;
    next_ += count;
    return first;
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::cpu(uint32_t index) const {
    auto h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * descriptorSize_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Device::gpu(uint32_t index) const {
    auto h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * descriptorSize_;
    return h;
}

void Device::srv(uint32_t index, const Texture& t) const {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = t.format;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(t.get(), &d, cpu(index));
}

void Device::uav(uint32_t index, const Texture& t) const {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = t.format;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(t.get(), nullptr, &d, cpu(index));
}

void Device::rawSrv(uint32_t index, ID3D12Resource* buffer, uint64_t bytes) const {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = UINT(bytes / 4);
    d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(buffer, &d, cpu(index));
}

void Device::rawUav(uint32_t index, ID3D12Resource* buffer, uint64_t bytes) const {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = UINT(bytes / 4);
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(buffer, nullptr, &d, cpu(index));
}

void Device::nullSrv(uint32_t index) const {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(nullptr, &d, cpu(index));
}

void Device::nullUav(uint32_t index) const {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(nullptr, nullptr, &d, cpu(index));
}

Texture Device::texture(uint32_t width, uint32_t height, DXGI_FORMAT format, bool unordered, const wchar_t* name) const {
    Texture t;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&t.resource)))) {
        log::error("gfx", std::format("texture {}x{} format={} failed", width, height, int(format)));
        return {};
    }
    if (name) t.resource->SetName(name);
    t.format = format;
    t.width = width;
    t.height = height;
    return t;
}

ComPtr<ID3D12Resource> Device::buffer(uint64_t bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                                      bool unordered, const wchar_t* name) const {
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (bytes + 255) & ~uint64_t(255);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) {
        log::error("gfx", std::format("buffer {} bytes failed", bytes));
        return nullptr;
    }
    if (name) r->SetName(name);
    return r;
}

} // namespace vela::gfx
