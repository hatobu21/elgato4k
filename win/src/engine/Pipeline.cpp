#include "engine/Pipeline.h"

#include "base/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace vela::engine {
namespace {
const D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
// Finished frames rest in a combined read state, so the presenter can copy
// or sample them without a transition of its own.
const D3D12_RESOURCE_STATES kFrameRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr uint32_t kStatsBytes = 512;
constexpr uint32_t kFlowGrid = 4;
constexpr uint32_t kCostThreshold = 200;

size_t align(size_t v, size_t a) { return (v + a - 1) / a * a; }

struct CaptureConstants { uint32_t width, height, pitch, chromaOffset, p010, bt2020, fullRangeChroma, flowStep, writeFlow; };
struct AnalysisConstants { uint32_t width, height, hdr; };
struct ColorConstants {
    uint32_t width, height, mode;
    float blackPoint, whitePoint, gamma, gainR, gainG, gainB, saturation, peakNits;
};
struct MotionConstants { uint32_t gridW, gridH, gridSize, flowW, flowH, outW, outH, costThreshold; };
struct EncodeConstants { uint32_t width, height, inputKind, outputKind; };
} // namespace

void Pipeline::release() {
    if (device_ && built_) device_->waitIdle();
    // NVOF: unregister while the textures are alive, release them, then shut down.
    nvof_.unregisterAll();
    fg_.release();
    dlssSr_.release();
    videoSr_.release();
    trueHdr_.release();
    for (auto& r : real_) r = {};
    for (auto& g : generated_) for (auto& t : g) t = {};
    working_ = stageA_ = srOut_ = scrgb_ = motion_ = depthOut_ = depthIn_ = {};
    flowA_ = flowB_ = flowRaw_ = flowCost_ = {};
    nvof_.shutdown();
    for (uint32_t i = 0; i < kUploadSlots; ++i) {
        if (upload_[i]) upload_[i]->Unmap(0, nullptr);
        upload_[i].Reset();
        uploadPtr_[i] = nullptr;
    }
    stats_.Reset();
    statsZero_.Reset();
    for (auto& r : statsReadback_) r.Reset();
    for (auto& a : allocators_) a.Reset();
    list_.Reset();
    ofIn_.Reset();
    ofOut_.Reset();
    built_ = false;
    historyValid_ = false;
}

bool Pipeline::build(gfx::Device& device, nv::NgxCore* ngx, const PipelineConfig& config) {
    release();
    device_ = &device;
    ngx_ = ngx;
    config_ = config;
    degraded_ = false;
    const auto& plan = config.plan;
    inW_ = config.mode.width;
    inH_ = config.mode.height;
    outW_ = plan.outputWidth;
    outH_ = plan.outputHeight;
    srcHdr_ = config.mode.format == capture::PixelFormat::P010 && config.signal.hdr;
    outFormat_ = plan.hdr == core::HdrPath::Passthrough || plan.hdr == core::HdrPath::RtxHdr
        ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    multiplier_ = plan.multiplier;
    maxMultiplier_ = multiplier_ > 1 ? std::clamp(config.maxMultiplier, multiplier_, kMaxGenerated + 1) : 1;
    flow_ = multiplier_ > 1 || plan.superRes == core::SrPath::Dlss;
    flowStep_ = inW_ > 1920 ? 2 : 1;   // 4K flows at 1080p: NVOF cost /4, quality holds
    flowW_ = inW_ / flowStep_;
    flowH_ = inH_ / flowStep_;
    colorMode_ = !srcHdr_ ? 0 : plan.hdr == core::HdrPath::ToneMap ? 4 : plan.superRes == core::SrPath::Dlss ? 3 : 2;

    device_->resetDescriptors();
    if (!createResources() || !createDescriptors()) return false;
    if (!capturePass_.create(device, L"CaptureToWorking", 1, 2, 9) ||
        !analysisPass_.create(device, L"Analysis", 1, 1, 3) ||
        !colorPass_.create(device, L"ColorApply", 1, 1, 11) ||
        !motionPass_.create(device, L"FlowToMotion", 2, 1, 8) ||
        !encodePass_.create(device, L"Encode", 1, 1, 4))
        return false;
    for (auto& a : allocators_)
        if (FAILED(device.d3d()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(device.d3d()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_))))
        return false;
    list_->Close();
    if (!timer_.create(device)) return false;
    if (!createFeatures()) return false;
    built_ = true;
    log::info("pipeline", std::format("built {}x{} {} -> {}x{} sr={} hdr={} fg={}x colorMode={} flow={}x{}",
        inW_, inH_, srcHdr_ ? "HDR" : "SDR", outW_, outH_, core::srPathKey(plan.superRes), core::hdrPathKey(plan.hdr),
        multiplier_, colorMode_, flow_ ? flowW_ : 0, flow_ ? flowH_ : 0));
    return true;
}

bool Pipeline::createResources() {
    auto& d = *device_;
    const auto& plan = config_.plan;
    const bool p010 = config_.mode.format == capture::PixelFormat::P010;
    // Upload slots sized for driver padding: pitch rounded up, rows to 32.
    const size_t pitch = align(size_t(inW_) * (p010 ? 2 : 1), 512) + 512;
    uploadBytes_ = pitch * align(inH_, 32) * 3 / 2;
    for (uint32_t i = 0; i < kUploadSlots; ++i) {
        upload_[i] = d.buffer(uploadBytes_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, L"vela.upload");
        D3D12_RANGE none{0, 0};
        if (!upload_[i] || FAILED(upload_[i]->Map(0, &none, reinterpret_cast<void**>(&uploadPtr_[i])))) return false;
    }

    working_ = d.texture(inW_, inH_, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"vela.working");
    const bool needStage = plan.superRes != core::SrPath::None || plan.hdr == core::HdrPath::RtxHdr;
    if (needStage)
        stageA_ = d.texture(inW_, inH_, colorMode_ == 3 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, true, L"vela.stageA");
    if (plan.superRes == core::SrPath::Dlss)
        srOut_ = d.texture(outW_, outH_, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"vela.srOut");
    else if (plan.superRes == core::SrPath::VideoSr && plan.hdr == core::HdrPath::RtxHdr)
        srOut_ = d.texture(outW_, outH_, DXGI_FORMAT_R8G8B8A8_UNORM, true, L"vela.srOut");
    if (plan.hdr == core::HdrPath::RtxHdr)
        scrgb_ = d.texture(outW_, outH_, DXGI_FORMAT_R16G16B16A16_FLOAT, true, L"vela.scrgb");
    for (uint32_t s = 0; s < kBatchSlots; ++s) {
        real_[s] = d.texture(outW_, outH_, outFormat_, true, L"vela.real");
        for (unsigned k = 0; k + 1 < maxMultiplier_; ++k)
            generated_[s][k] = d.texture(outW_, outH_, outFormat_, true, L"vela.generated");
    }
    if (flow_) {
        flowA_ = d.texture(flowW_, flowH_, DXGI_FORMAT_B8G8R8A8_UNORM, false, L"vela.flowA");
        flowB_ = d.texture(flowW_, flowH_, DXGI_FORMAT_B8G8R8A8_UNORM, true, L"vela.flowB");
        const uint32_t gw = (flowW_ + kFlowGrid - 1) / kFlowGrid, gh = (flowH_ + kFlowGrid - 1) / kFlowGrid;
        flowRaw_ = d.texture(gw, gh, DXGI_FORMAT_R16G16_SINT, false, L"vela.flowRaw");
        flowCost_ = d.texture(gw, gh, DXGI_FORMAT_R8_UINT, false, L"vela.flowCost");
        motion_ = d.texture(outW_, outH_, DXGI_FORMAT_R16G16_FLOAT, true, L"vela.motion");
    }
    if (multiplier_ > 1) depthOut_ = d.texture(outW_, outH_, DXGI_FORMAT_R32_FLOAT, false, L"vela.depthOut");
    if (plan.superRes == core::SrPath::Dlss) depthIn_ = d.texture(inW_, inH_, DXGI_FORMAT_R32_FLOAT, false, L"vela.depthIn");

    stats_ = d.buffer(kStatsBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, L"vela.stats");
    statsZero_ = d.buffer(kStatsBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, L"vela.statsZero");
    void* zero = nullptr;
    if (!stats_ || !statsZero_ || FAILED(statsZero_->Map(0, nullptr, &zero))) return false;
    std::memset(zero, 0, kStatsBytes);
    statsZero_->Unmap(0, nullptr);
    for (auto& r : statsReadback_) {
        r = d.buffer(kStatsBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false, L"vela.statsReadback");
        if (!r) return false;
    }
    if (!working_ || (needStage && !stageA_) || !real_[0] || (flow_ && (!flowB_ || !motion_))) return false;
    if (FAILED(d.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ofIn_))) ||
        FAILED(d.d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ofOut_))))
        return false;
    ofInValue_ = 0;
    return true;
}

bool Pipeline::createDescriptors() {
    auto& d = *device_;
    const auto& plan = config_.plan;
    captureSrv_ = d.allocate(kUploadSlots);
    for (uint32_t u = 0; u < kUploadSlots; ++u) d.rawSrv(captureSrv_ + u, upload_[u].Get(), uploadBytes_);
    captureUav_ = d.allocate(2);
    d.uav(captureUav_, working_);
    if (flow_) d.uav(captureUav_ + 1, flowB_); else d.nullUav(captureUav_ + 1);

    analysisSrv_ = d.allocate(1);
    d.srv(analysisSrv_, working_);
    analysisUav_ = d.allocate(1);
    d.rawUav(analysisUav_, stats_.Get(), kStatsBytes);

    colorSrv_ = d.allocate(1);
    d.srv(colorSrv_, working_);
    if (stageA_) { colorUavStage_ = d.allocate(1); d.uav(colorUavStage_, stageA_); }

    if (flow_) {
        motionSrv_ = d.allocate(2);
        d.srv(motionSrv_, flowRaw_);
        d.srv(motionSrv_ + 1, flowCost_);
        motionUav_ = d.allocate(1);
        d.uav(motionUav_, motion_);
    }
    const gfx::Texture* encodeSource = plan.superRes == core::SrPath::Dlss ? &srOut_ : plan.hdr == core::HdrPath::RtxHdr ? &scrgb_ : nullptr;
    if (encodeSource) { encodeSrv_ = d.allocate(1); d.srv(encodeSrv_, *encodeSource); }

    for (uint32_t s = 0; s < kBatchSlots; ++s) {
        colorUavReal_[s] = d.allocate(1);
        d.uav(colorUavReal_[s], real_[s]);
        encodeUavReal_[s] = colorUavReal_[s];   // same view, same texture
        realSrv_[s] = d.allocate(1);
        d.srv(realSrv_[s], real_[s]);
        for (unsigned k = 0; k + 1 < maxMultiplier_; ++k) {
            generatedSrv_[s][k] = d.allocate(1);
            d.srv(generatedSrv_[s][k], generated_[s][k]);
        }
    }
    return true;
}

void Pipeline::zero(ID3D12GraphicsCommandList* list, gfx::Texture& t, ComPtr<ID3D12Resource>& staging) {
    if (!t) return;
    const auto desc = t.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device_->d3d()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    staging = device_->buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, L"vela.zero");
    void* p = nullptr;
    if (!staging || FAILED(staging->Map(0, nullptr, &p))) return;
    std::memset(p, 0, size_t(bytes));
    staging->Unmap(0, nullptr);
    gfx::transition(list, t, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{t.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    gfx::transition(list, t, kSrv);
}

bool Pipeline::createFeatures() {
    const auto& plan = config_.plan;
    auto* list = list_.Get();
    allocators_[0]->Reset();
    list->Reset(allocators_[0].Get(), nullptr);
    ComPtr<ID3D12Resource> zeroA, zeroB;
    zero(list, depthOut_, zeroA);
    zero(list, depthIn_, zeroB);
    bool ok = true;
    if (plan.superRes == core::SrPath::VideoSr) ok = ngx_ && videoSr_.create(*ngx_, list);
    if (ok && plan.superRes == core::SrPath::Dlss) ok = ngx_ && dlssSr_.create(*ngx_, list, {inW_, inH_, outW_, outH_});
    if (ok && plan.hdr == core::HdrPath::RtxHdr) ok = ngx_ && trueHdr_.create(*ngx_, list);
    if (ok && multiplier_ > 1) {
        nv::DlssFg::CreateDesc fd;
        fd.width = outW_;
        fd.height = outH_;
        fd.format = outFormat_;
        fd.performanceMode = config_.settings.frameGen.performanceMode;
        ok = ngx_ && fg_.create(*ngx_, list, fd);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list};
    device_->queue()->ExecuteCommandLists(1, lists);
    device_->waitIdle();
    if (!ok) {
        log::error("pipeline", "vendor feature creation failed; the engine re-plans without it");
        return false;
    }
    if (flow_) {
        nv::NvOf::Desc od;
        od.width = flowW_;
        od.height = flowH_;
        od.inFence = ofIn_.Get();
        od.outFence = ofOut_.Get();
        od.quality = 1;
        od.gridSize = kFlowGrid;
        if (!nvof_.initialize(device_->d3d(), flowA_.get(), flowB_.get(), flowRaw_.get(), flowCost_.get(), od)) {
            log::error("pipeline", "NVOF initialisation failed");
            return false;
        }
    }
    return true;
}

bool Pipeline::updatePlan(const core::Plan& plan) {
    const auto& p = config_.plan;
    if (!built_ || plan.outputWidth != p.outputWidth || plan.outputHeight != p.outputHeight ||
        plan.superRes != p.superRes || plan.hdr != p.hdr || (plan.multiplier > 1) != (multiplier_ > 1) ||
        plan.multiplier > maxMultiplier_)
        return false;
    if (plan.multiplier != multiplier_) historyValid_ = false;   // DLSS-G restarts its history
    multiplier_ = plan.multiplier;
    config_.plan = plan;
    return true;
}

bool Pipeline::process(uint32_t uploadSlot, const capture::Frame& layout, uint32_t slot,
                       const core::ColorParams& color, bool reset, BatchOut& out) {
    if (!built_) return false;
    auto& d = *device_;
    const auto& plan = config_.plan;
    const auto& settings = config_.settings;
    auto* list = list_.Get();
    auto* queue = d.queue();
    reset = reset || !historyValid_;

    allocators_[slot]->Reset();
    list->Reset(allocators_[slot].Get(), nullptr);
    timer_.begin(slot);
    auto submit = [&] {
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        list->Reset(allocators_[slot].Get(), nullptr);
    };
    using core::GpuStage;

    // ---- upload: bytes -> working (+ flow input, + statistics)
    timer_.mark(list, slot, GpuStage::Upload, false);
    if (flow_) {
        gfx::transition(list, flowA_, D3D12_RESOURCE_STATE_COPY_DEST);
        gfx::transition(list, flowB_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(flowA_.get(), flowB_.get());   // previous <- current
        gfx::transition(list, flowA_, D3D12_RESOURCE_STATE_COMMON);
        gfx::transition(list, flowB_, kUav);
    }
    gfx::transition(list, working_, kUav);
    const CaptureConstants cc{inW_, inH_, layout.pitch, layout.chromaOffset,
        layout.format == capture::PixelFormat::P010 ? 1u : 0u, srcHdr_ ? 1u : 0u,
        config_.signal.fullRangeChroma && layout.format == capture::PixelFormat::P010 ? 1u : 0u, flowStep_, flow_ ? 1u : 0u};
    capturePass_.record(list, d, captureSrv_ + uploadSlot, captureUav_, &cc, gfx::groups(inW_), gfx::groups(inH_));
    gfx::uavBarrier(list, working_.get());
    gfx::transition(list, working_, kSrv);
    if (flow_) gfx::transition(list, flowB_, D3D12_RESOURCE_STATE_COMMON);
    const bool analyse = settings.color.enabled;
    if (analyse) {
        gfx::transition(list, stats_.Get(), kUav, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(stats_.Get(), 0, statsZero_.Get(), 0, kStatsBytes);
        gfx::transition(list, stats_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kUav);
        const AnalysisConstants ac{inW_, inH_, srcHdr_ ? 1u : 0u};
        analysisPass_.record(list, d, analysisSrv_, analysisUav_, &ac, gfx::groups((inW_ + 3) / 4), gfx::groups((inH_ + 3) / 4));
        gfx::uavBarrier(list, stats_.Get());
        gfx::transition(list, stats_.Get(), kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(statsReadback_[slot].Get(), 0, stats_.Get(), 0, kStatsBytes);
        gfx::transition(list, stats_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
    }
    timer_.mark(list, slot, GpuStage::Upload, true);

    // ---- optical flow runs on NVOF's own engine while colour/SR/HDR record
    uint64_t flowReady = 0;
    if (flow_) {
        submit();
        queue->Signal(ofIn_.Get(), ++ofInValue_);
        if (!nvof_.execute(ofInValue_)) { degraded_ = true; list->Close(); return false; }
        flowReady = nvof_.nextOutValue() - 1;
    }
    bool motionReady = false;
    auto buildMotion = [&] {
        submit();
        queue->Wait(ofOut_.Get(), flowReady);
        timer_.mark(list, slot, GpuStage::Flow, false);
        gfx::transition(list, flowRaw_, kSrv);
        gfx::transition(list, flowCost_, kSrv);
        gfx::transition(list, motion_, kUav);
        const uint32_t gw = (flowW_ + kFlowGrid - 1) / kFlowGrid, gh = (flowH_ + kFlowGrid - 1) / kFlowGrid;
        const MotionConstants mc{gw, gh, kFlowGrid, flowW_, flowH_, outW_, outH_, kCostThreshold};
        motionPass_.record(list, d, motionSrv_, motionUav_, &mc, gfx::groups(outW_), gfx::groups(outH_));
        gfx::uavBarrier(list, motion_.get());
        gfx::transition(list, motion_, kSrv);
        gfx::transition(list, flowRaw_, D3D12_RESOURCE_STATE_COMMON);
        gfx::transition(list, flowCost_, D3D12_RESOURCE_STATE_COMMON);
        timer_.mark(list, slot, GpuStage::Flow, true);
        motionReady = true;
    };

    // ---- colour: auto correction + conversion into the route's format
    gfx::Texture& real = real_[slot];
    const bool direct = plan.superRes == core::SrPath::None && plan.hdr != core::HdrPath::RtxHdr;
    gfx::Texture& colorTarget = direct ? real : stageA_;
    timer_.mark(list, slot, GpuStage::Color, false);
    gfx::transition(list, colorTarget, kUav);
    const ColorConstants col{inW_, inH_, colorMode_, color.blackPoint, color.whitePoint, color.gamma,
        color.gainR, color.gainG, color.gainB, color.saturation,
        config_.signal.masteringPeak > 0 ? config_.signal.masteringPeak : 1000.0f};
    colorPass_.record(list, d, colorSrv_, direct ? colorUavReal_[slot] : colorUavStage_, &col, gfx::groups(inW_), gfx::groups(inH_));
    gfx::uavBarrier(list, colorTarget.get());
    timer_.mark(list, slot, GpuStage::Color, true);

    auto encode = [&](uint32_t inputKind, uint32_t outputKind) {
        gfx::transition(list, real, kUav);
        const EncodeConstants ec{outW_, outH_, inputKind, outputKind};
        encodePass_.record(list, d, encodeSrv_, encodeUavReal_[slot], &ec, gfx::groups(outW_), gfx::groups(outH_));
        gfx::uavBarrier(list, real.get());
    };

    // ---- super resolution
    if (plan.superRes == core::SrPath::VideoSr) {
        gfx::Texture& dst = plan.hdr == core::HdrPath::RtxHdr ? srOut_ : real;
        timer_.mark(list, slot, GpuStage::SuperRes, false);
        gfx::transition(list, stageA_, D3D12_RESOURCE_STATE_COMMON);
        gfx::transition(list, dst, D3D12_RESOURCE_STATE_COMMON);   // RTX Video SDK contract
        if (!videoSr_.evaluate(list, stageA_.get(), dst.get(), settings.superRes.quality)) { degraded_ = true; list->Close(); return false; }
        gfx::uavBarrier(list, dst.get());
        timer_.mark(list, slot, GpuStage::SuperRes, true);
    } else if (plan.superRes == core::SrPath::Dlss) {
        buildMotion();
        timer_.mark(list, slot, GpuStage::SuperRes, false);
        gfx::transition(list, stageA_, kSrv);
        gfx::transition(list, srOut_, kUav);
        if (!dlssSr_.evaluate(list, stageA_.get(), srOut_.get(), depthIn_.get(), motion_.get(), reset, settings.superRes.sharpness)) {
            degraded_ = true; list->Close(); return false;
        }
        gfx::uavBarrier(list, srOut_.get());
        gfx::transition(list, srOut_, kSrv);
        encode(2, 1);   // linear BT.2020 -> PQ
        timer_.mark(list, slot, GpuStage::SuperRes, true);
    }

    // ---- HDR: SDR -> RTX Video HDR -> HDR10
    if (plan.hdr == core::HdrPath::RtxHdr) {
        gfx::Texture& src = plan.superRes == core::SrPath::VideoSr ? srOut_ : stageA_;
        timer_.mark(list, slot, GpuStage::Hdr, false);
        gfx::transition(list, src, kSrv);
        gfx::transition(list, scrgb_, kUav);
        if (!trueHdr_.evaluate(list, src.get(), scrgb_.get(), settings.hdr)) { degraded_ = true; list->Close(); return false; }
        gfx::uavBarrier(list, scrgb_.get());
        gfx::transition(list, scrgb_, kSrv);
        encode(0, 1);   // scRGB -> PQ
        timer_.mark(list, slot, GpuStage::Hdr, true);
    }

    // ---- frame generation between the previous real frame and this one
    out = {};
    out.slot = slot;
    ++frameId_;
    if (multiplier_ > 1) {
        if (!motionReady) buildMotion();
        timer_.mark(list, slot, GpuStage::FrameGen, false);
        gfx::transition(list, real, kSrv);
        const unsigned count = multiplier_ - 1;
        // A reset frame only primes DLSS-G's history; nothing it outputs is shown.
        const unsigned evaluations = reset ? 1 : count;
        for (unsigned k = 1; k <= evaluations; ++k) {
            gfx::Texture& gen = generated_[slot][k - 1];
            gfx::transition(list, gen, kUav);
            nv::DlssFg::EvalDesc e;
            e.backbuffer = real.get();
            e.depth = depthOut_.get();
            e.motion = motion_.get();
            e.output = gen.get();
            e.reset = reset;
            e.hdr = outFormat_ == DXGI_FORMAT_R10G10B10A2_UNORM;
            e.frameId = frameId_;
            e.count = reset ? 1 : count;
            e.index = k;
            if (!fg_.evaluate(list, e)) { degraded_ = true; list->Close(); return false; }
            gfx::uavBarrier(list, gen.get());
            gfx::transition(list, gen, kFrameRead);
            if (!reset) out.frames.push_back({&gen, generatedSrv_[slot][k - 1], k, false});
        }
        timer_.mark(list, slot, GpuStage::FrameGen, true);
    }
    gfx::transition(list, real, kFrameRead);
    out.frames.push_back({&real, realSrv_[slot], multiplier_, true});

    timer_.resolve(list, slot);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    out.fence = d.signal();
    historyValid_ = true;
    statsValid_[slot] = analyse;
    return true;
}

bool Pipeline::collect(uint32_t slot, gfx::GpuTimer::StageArray& stages, double& totalMs, core::ColorStats& stats) {
    timer_.read(slot, stages, totalMs);
    if (!statsValid_[slot]) return false;
    statsValid_[slot] = false;
    uint32_t* w = nullptr;
    D3D12_RANGE range{0, 74 * sizeof(uint32_t)};
    if (FAILED(statsReadback_[slot]->Map(0, &range, reinterpret_cast<void**>(&w)))) return false;
    stats = {};
    for (int i = 0; i < core::ColorStats::kBins; ++i) stats.luma[size_t(i)] = w[i];
    const float mid = w[67] ? 1.0f / (1024.0f * float(w[67])) : 0;
    const float high = w[71] ? 1.0f / (1024.0f * float(w[71])) : 0;
    stats.midR = float(w[64]) * mid;
    stats.midG = float(w[65]) * mid;
    stats.midB = float(w[66]) * mid;
    stats.highR = float(w[68]) * high;
    stats.highG = float(w[69]) * high;
    stats.highB = float(w[70]) * high;
    stats.meanChroma = w[73] ? float(w[72]) / (1024.0f * float(w[73])) : 0;
    stats.hdr = srcHdr_;
    D3D12_RANGE none{0, 0};
    statsReadback_[slot]->Unmap(0, &none);
    return true;
}

} // namespace vela::engine
