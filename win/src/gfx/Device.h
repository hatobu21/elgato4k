#pragma once
#include "base/Win.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <mutex>
#include <string>

namespace vela::gfx {

// A GPU texture with the state this code last put it in. One owner thread
// records barriers for a given texture at a time (engine or presenter).
struct Texture {
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* get() const { return resource.Get(); }
    explicit operator bool() const { return resource != nullptr; }
};

void transition(ID3D12GraphicsCommandList* list, Texture& texture, D3D12_RESOURCE_STATES after);
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
void uavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource);

// D3D12 device on the NVIDIA GPU, one direct queue shared by the engine and
// present threads, one fence, and one shader-visible descriptor heap.
class Device {
public:
    bool create(bool debugLayer);
    void waitIdle();

    ID3D12Device* d3d() const { return device_.Get(); }
    IDXGIFactory6* factory() const { return factory_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    ID3D12Fence* fence() const { return fence_.Get(); }

    // Fence: signal() is safe from any thread and keeps queue order.
    uint64_t signal();
    uint64_t completed() const { return fence_->GetCompletedValue(); }
    bool waitCpu(uint64_t value, DWORD timeoutMs);

    // Descriptors: [0, kReserved) belong to the swap chain, the rest is a
    // linear allocator reset whenever the pipeline is rebuilt (GPU idle).
    static constexpr uint32_t kReserved = 16;
    static constexpr uint32_t kHeapSize = 4096;
    uint32_t allocate(uint32_t count);
    void resetDescriptors() { next_ = kReserved; }
    ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t index) const;

    void srv(uint32_t index, const Texture& texture) const;
    void uav(uint32_t index, const Texture& texture) const;
    void rawSrv(uint32_t index, ID3D12Resource* buffer, uint64_t bytes) const;
    void rawUav(uint32_t index, ID3D12Resource* buffer, uint64_t bytes) const;
    void nullSrv(uint32_t index) const;
    void nullUav(uint32_t index) const;

    Texture texture(uint32_t width, uint32_t height, DXGI_FORMAT format, bool unordered, const wchar_t* name) const;
    ComPtr<ID3D12Resource> buffer(uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  bool unordered, const wchar_t* name) const;

    const std::wstring& adapterName() const { return adapterName_; }
    uint32_t vendorId() const { return vendorId_; }
    uint64_t vramBytes() const { return vramBytes_; }
    LUID luid() const { return luid_; }
    uint64_t timestampFrequency() const { return timestampFrequency_; }

private:
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter4> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    HANDLE fenceEvent_ = nullptr;
    std::mutex signalMutex_, waitMutex_;
    uint64_t fenceValue_ = 0;
    uint32_t descriptorSize_ = 0, next_ = kReserved;
    std::wstring adapterName_;
    uint32_t vendorId_ = 0;
    uint64_t vramBytes_ = 0;
    LUID luid_{};
    uint64_t timestampFrequency_ = 0;
};

} // namespace vela::gfx
