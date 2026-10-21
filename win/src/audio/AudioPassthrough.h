#pragma once
// Capture card sound to the default output: WASAPI shared mode, event
// driven, smallest engine period (IAudioClient3). The ring buffer is held at
// the video pipeline's latency so sound and picture stay in sync (frame
// generation delays the picture by 15-25 ms). Not a feature of its own;
// without it the player would be silent.
#include "base/Win.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace vela::audio {

struct Endpoint {
    std::wstring id, name;
};

class Passthrough {
public:
    static std::vector<Endpoint> captureEndpoints();
    // Picks the endpoint whose name matches the video device (Elgato first).
    static std::wstring matchEndpoint(const std::wstring& videoName);

    ~Passthrough() { stop(); }
    bool start(const std::wstring& captureId);
    void stop();
    void setVolume(float volume) { volume_.store(volume); }
    // Delay sound to match the picture: the engine passes its measured
    // capture-to-present latency (P50). Clamped to 5..120 ms.
    void setDelayMs(double ms) { delayMs_.store(ms < 5 ? 5 : ms > 120 ? 120 : ms); }
    bool running() const { return thread_.joinable(); }
    double bufferedMs() const { return bufferedMs_.load(); }

private:
    void run(std::wstring captureId);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<double> delayMs_{10};
    std::atomic<double> bufferedMs_{0};
};

} // namespace vela::audio
