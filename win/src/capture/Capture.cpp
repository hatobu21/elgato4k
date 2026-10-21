#include "capture/Capture.h"

#include "base/Log.h"
#include "capture/Elgato.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace vela::capture {

std::string Mode::key() const {
    return std::format("{}x{}@{}:{}", width, height, int(std::lround(fps())), format == PixelFormat::P010 ? "p010" : "nv12");
}

// IMFSourceReaderCallback forwarding samples to the owning Source.
class Source::Callback final : public IMFSourceReaderCallback {
public:
    explicit Callback(Source* owner) : owner_(owner) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *out = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) override {
        if (SUCCEEDED(status) && !(flags & MF_SOURCE_READERF_ERROR)) {
            if (sample) owner_->onSample(sample, timestamp);
            std::lock_guard lock(owner_->mutex_);
            if (owner_->running_ && owner_->reader_)
                owner_->reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr);
        } else {
            log::error("capture", std::format("reader error hr=0x{:08X} flags=0x{:X}", uint32_t(status), flags));
        }
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    Source* owner_;
    volatile LONG refs_ = 1;
};

std::vector<Device> Source::enumerate() {
    std::vector<Device> out;
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1))) return out;
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &devices, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* name = nullptr;
        wchar_t* link = nullptr;
        UINT32 length = 0;
        Device d;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length))) d.name = name;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &length))) d.link = link;
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devices[i]->Release();
        out.push_back(std::move(d));
    }
    CoTaskMemFree(devices);
    // Elgato devices first: they are what this build is for.
    std::stable_partition(out.begin(), out.end(), [](const Device& d) { return isElgato(d.name); });
    return out;
}

Source::Source() = default;
Source::~Source() { close(); }

bool Source::open(const Device& device) {
    close();
    device_ = device;
    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 2);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, device.link.c_str());
    if (FAILED(MFCreateDeviceSource(attributes.Get(), &source_))) {
        log::error("capture", std::format("cannot open {}", narrow(device.name)));
        return false;
    }
    elgato_ = std::make_unique<ElgatoControl>(source_.Get(), device.name);

    callback_.Attach(new Callback(this));
    ComPtr<IMFAttributes> readerAttributes;
    MFCreateAttributes(&readerAttributes, 3);
    readerAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
    readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);   // native NV12/P010 only
    readerAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (FAILED(MFCreateSourceReaderFromMediaSource(source_.Get(), readerAttributes.Get(), &reader_))) {
        log::error("capture", "source reader creation failed");
        return false;
    }

    modes_.clear();
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader_->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type))) break;
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        Mode m;
        m.nativeIndex = i;
        if (subtype == MFVideoFormat_NV12) m.format = PixelFormat::Nv12;
        else if (subtype == MFVideoFormat_P010) m.format = PixelFormat::P010;
        else continue;   // YUY2 / MJPEG / RGB are not used by this build
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &m.width, &m.height);
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &m.rateNum, &m.rateDen);
        modes_.push_back(m);
    }
    log::info("capture", std::format("opened {} with {} NV12/P010 modes", narrow(device.name), modes_.size()));
    return !modes_.empty();
}

Mode Source::choose(const std::vector<Mode>& modes, uint32_t width, uint32_t height, double fps, bool preferHdr) {
    Mode best;
    double bestScore = -1;
    for (const auto& m : modes) {
        double score = 0;
        if (width && height) score += (m.width == width && m.height == height) ? 1000 : 0;
        else score += double(m.width) * m.height / 1e4;   // biggest picture
        if (fps > 0) score += 500 - std::min(500.0, std::fabs(m.fps() - fps) * 10);
        else score += std::min(m.fps(), 60.0);            // 60 Hz at 4K beats 120 Hz at 1440p
        if ((m.format == PixelFormat::P010) == preferHdr) score += 50;
        if (score > bestScore) { bestScore = score; best = m; }
    }
    return best;
}

bool Source::start(const Mode& mode, Sink sink) {
    if (!reader_) return false;
    stop();
    ComPtr<IMFMediaType> type;
    if (FAILED(reader_->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), mode.nativeIndex, &type)) ||
        FAILED(reader_->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) {
        log::error("capture", std::format("card refused {}", mode.key()));
        return false;
    }
    // An 8-bit format on an HDR signal: let the card tone map (OBS behaviour);
    // P010 keeps HDR intact.
    if (elgato_ && elgato_->available()) elgato_->setHardwareToneMap(mode.format == PixelFormat::Nv12);
    {
        std::lock_guard lock(mutex_);
        current_ = mode;
        sink_ = std::move(sink);
        running_ = true;
    }
    log::info("capture", std::format("start {}", mode.key()));
    return SUCCEEDED(reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr));
}

void Source::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    if (reader_) reader_->Flush(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
}

void Source::close() {
    stop();
    {
        std::lock_guard lock(mutex_);
        sink_ = nullptr;
    }
    elgato_.reset();
    reader_.Reset();
    if (source_) source_->Shutdown();
    source_.Reset();
    callback_.Reset();
    modes_.clear();
}

SignalInfo Source::signal() const {
    std::lock_guard lock(mutex_);
    return elgato_ && elgato_->available() ? elgato_->signal(current_.format) : SignalInfo{};
}

void Source::onSample(IMFSample* sample, long long) {
    const core::Micros arrival = nowMicros();
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return;
    Frame f;
    f.arrival = arrival;
    {
        std::lock_guard lock(mutex_);
        if (!running_ || !sink_) return;
        f.width = current_.width;
        f.height = current_.height;
        f.format = current_.format;
    }
    ComPtr<IMF2DBuffer2> buffer2d;
    BYTE* scanline0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    bool locked2d = false;
    if (SUCCEEDED(buffer.As(&buffer2d)) &&
        SUCCEEDED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0, &pitch, &start, &length)) && pitch > 0) {
        locked2d = true;
        f.data = scanline0;
        f.pitch = uint32_t(pitch);
        f.bytes = length - size_t(scanline0 - start);
    } else {
        DWORD max = 0;
        if (FAILED(buffer->Lock(&start, &max, &length))) return;
        f.data = start;
        f.bytes = length;
        f.pitch = f.width * (f.format == PixelFormat::P010 ? 2 : 1);
    }
    // Planes are contiguous; the luma plane may be padded below the picture,
    // so the chroma offset follows from the buffer size (Y rows + Y/2 rows).
    const size_t rows = f.pitch ? f.bytes / f.pitch : 0;
    f.chromaOffset = uint32_t((rows * 2 / 3) * f.pitch);
    if (f.chromaOffset < size_t(f.pitch) * f.height) f.chromaOffset = f.pitch * f.height;
    {
        std::lock_guard lock(mutex_);
        if (sink_ && f.bytes >= size_t(f.chromaOffset) + size_t(f.pitch) * f.height / 2) sink_(f);
    }
    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();
}

} // namespace vela::capture
