#pragma once
// The GPU graph for one capture mode and one plan. Built once (GPU idle),
// then process() records and submits one batch per captured frame:
//
//   upload bytes -> CaptureToWorking -> Analysis (auto colour stats)
//   -> ColorApply -> [RTX Video SR | DLSS SR -> Encode] -> [RTX HDR -> Encode]
//   -> real frame;  NVOF -> FlowToMotion -> DLSS-G x (n-1) -> generated frames
//
// Six routes cover every plan (see Pipeline.cpp). Every intermediate exists
// only if the plan needs it, and the last stage of each route writes straight
// into the real frame, so no route pays for a copy.
#include "capture/Capture.h"
#include "gfx/Compute.h"
#include "gfx/Device.h"
#include "gfx/GpuTimer.h"
#include "nv/DlssFg.h"
#include "nv/DlssSr.h"
#include "nv/NvOf.h"
#include "nv/RtxVideo.h"
#include "vela/core/AutoColor.h"
#include "vela/core/ModePlanner.h"

#include <array>
#include <vector>

namespace vela::engine {

struct PipelineConfig {
    core::Plan plan;
    capture::Mode mode;
    capture::SignalInfo signal;
    core::Settings settings;
    // Generated frame textures are allocated for this many, so a change of
    // the content rate or of the multiplier switches without a rebuild.
    unsigned maxMultiplier = 2;
};

struct FrameOut {
    gfx::Texture* texture = nullptr;
    uint32_t srv = 0;           // descriptor for Compose
    unsigned subframe = 0;      // 1..n-1 generated, n real
    bool real = false;
};

struct BatchOut {
    uint32_t slot = 0;
    uint64_t fence = 0;         // whole batch done
    std::vector<FrameOut> frames;   // generated in order, then the real frame
};

class Pipeline {
public:
    static constexpr uint32_t kUploadSlots = 4;
    static constexpr uint32_t kBatchSlots = 3;
    static constexpr uint32_t kMaxGenerated = 5;

    ~Pipeline() { release(); }
    bool build(gfx::Device& device, nv::NgxCore* ngx, const PipelineConfig& config);
    void release();
    bool built() const { return built_; }
    const PipelineConfig& config() const { return config_; }
    // Values read per frame (SR quality, sharpness, RTX HDR tuning); no rebuild.
    void updateSettings(const core::Settings& settings) { config_.settings = settings; }
    // Same routes and sizes, different multiplier (content rate or user
    // change). False if the new plan needs a rebuild instead.
    bool updatePlan(const core::Plan& plan);

    // CPU-visible upload slot the capture thread copies a frame into.
    uint8_t* uploadPointer(uint32_t slot) const { return uploadPtr_[slot]; }
    size_t uploadCapacity() const { return uploadBytes_; }

    // Records and submits one batch. `reset` drops temporal history (first
    // frame, signal change). The batch slot must be free (see Engine).
    bool process(uint32_t uploadSlot, const capture::Frame& layout, uint32_t batchSlot,
                 const core::ColorParams& color, bool reset, BatchOut& out);

    // After the batch fence: GPU stage times and auto colour statistics.
    bool collect(uint32_t batchSlot, gfx::GpuTimer::StageArray& stages, double& totalMs, core::ColorStats& stats);

    uint32_t outputWidth() const { return outW_; }
    uint32_t outputHeight() const { return outH_; }
    DXGI_FORMAT outputFormat() const { return outFormat_; }
    bool frameGeneration() const { return fg_.ready(); }
    unsigned multiplier() const { return multiplier_; }
    bool degraded() const { return degraded_; }   // a vendor feature failed at runtime

private:
    bool createResources();
    bool createDescriptors();
    bool createFeatures();
    void zero(ID3D12GraphicsCommandList* list, gfx::Texture& texture, ComPtr<ID3D12Resource>& staging);

    gfx::Device* device_ = nullptr;
    nv::NgxCore* ngx_ = nullptr;
    PipelineConfig config_;
    bool built_ = false, degraded_ = false;

    uint32_t inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0, flowW_ = 0, flowH_ = 0, flowStep_ = 1;
    DXGI_FORMAT outFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    unsigned multiplier_ = 1, maxMultiplier_ = 1;
    bool srcHdr_ = false, flow_ = false;
    uint32_t colorMode_ = 0;

    // upload ring
    std::array<ComPtr<ID3D12Resource>, kUploadSlots> upload_;
    std::array<uint8_t*, kUploadSlots> uploadPtr_{};
    size_t uploadBytes_ = 0;

    // textures
    gfx::Texture working_, stageA_, srOut_, scrgb_, motion_, depthOut_, depthIn_;
    gfx::Texture flowA_, flowB_, flowRaw_, flowCost_;
    std::array<gfx::Texture, kBatchSlots> real_;
    std::array<std::array<gfx::Texture, kMaxGenerated>, kBatchSlots> generated_;
    ComPtr<ID3D12Resource> stats_, statsZero_;
    std::array<ComPtr<ID3D12Resource>, kBatchSlots> statsReadback_;
    std::array<bool, kBatchSlots> statsValid_{};

    // passes and their descriptor tables
    gfx::ComputePass capturePass_, analysisPass_, colorPass_, motionPass_, encodePass_;
    uint32_t captureSrv_ = 0, captureUav_ = 0, analysisSrv_ = 0, analysisUav_ = 0;
    uint32_t colorSrv_ = 0, colorUavStage_ = 0, motionSrv_ = 0, motionUav_ = 0, encodeSrv_ = 0;
    std::array<uint32_t, kBatchSlots> colorUavReal_{}, encodeUavReal_{}, realSrv_{};
    std::array<std::array<uint32_t, kMaxGenerated>, kBatchSlots> generatedSrv_{};

    // command recording
    std::array<ComPtr<ID3D12CommandAllocator>, kBatchSlots> allocators_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    gfx::GpuTimer timer_;

    // vendor features
    nv::VideoSr videoSr_;
    nv::DlssSr dlssSr_;
    nv::TrueHdr trueHdr_;
    nv::DlssFg fg_;
    nv::NvOf nvof_;
    ComPtr<ID3D12Fence> ofIn_, ofOut_;
    uint64_t ofInValue_ = 0;
    uint64_t frameId_ = 0;
    bool historyValid_ = false;
};

} // namespace vela::engine
