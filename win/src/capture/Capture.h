#pragma once
// Capture card input through Media Foundation (source reader, async, no
// converters). The Elgato 4K Pro delivers NV12 for SDR and P010 for HDR10;
// both are copied as-is and converted on the GPU.
#include "base/Win.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct IMFMediaSource;
struct IMFSourceReader;
struct IMFSample;

namespace vela::capture {

enum class PixelFormat : uint8_t { Unknown = 0, Nv12, P010 };

struct Device {
    std::wstring name;
    std::wstring link;          // symbolic link, stable across enumerations
};

struct Mode {
    uint32_t width = 0, height = 0;
    uint32_t rateNum = 0, rateDen = 1;
    PixelFormat format = PixelFormat::Unknown;
    uint32_t nativeIndex = 0;   // index into the reader's native media types
    double fps() const { return rateDen ? double(rateNum) / rateDen : 0; }
    std::string key() const;    // "3840x2160@60:p010"
};

// HDR metadata the card reports from the HDMI InfoFrame.
struct SignalInfo {
    bool hdr = false;           // PQ (or HLG) transfer declared by the source
    bool hlg = false;
    float maxCll = 0, maxFall = 0, masteringPeak = 0;
    bool fullRangeChroma = false;   // Elgato non-MK.2 quirk, applies to P010 only (see Elgato.cpp)
};

struct Frame {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint32_t pitch = 0;             // bytes per luma row
    uint32_t chromaOffset = 0;      // byte offset of the interleaved chroma plane
    uint32_t width = 0, height = 0;
    PixelFormat format = PixelFormat::Unknown;
    core::Micros arrival = 0;       // QPC at the reader callback
};

class Source {
public:
    using Sink = std::function<void(const Frame&)>;   // called on the MF worker thread

    static std::vector<Device> enumerate();
    Source();
    ~Source();

    bool open(const Device& device);
    std::vector<Mode> modes() const { return modes_; }
    // Picks the mode, starts reading. Returns false if the card refuses it.
    bool start(const Mode& mode, Sink sink);
    void stop();
    void close();

    // Best mode for a request: exact match, else the highest rate at the
    // requested size, else 4K60 P010 / NV12 by preference.
    static Mode choose(const std::vector<Mode>& modes, uint32_t width, uint32_t height, double fps, bool preferHdr);

    SignalInfo signal() const;
    const Mode& current() const { return current_; }

private:
    class Callback;
    void onSample(IMFSample* sample, long long timestamp);

    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<Callback> callback_;
    Device device_;
    std::vector<Mode> modes_;
    Mode current_;
    Sink sink_;
    std::unique_ptr<class ElgatoControl> elgato_;
    mutable std::mutex mutex_;
    bool running_ = false;
    uint64_t streamTicks_ = 0;
};

} // namespace vela::capture
