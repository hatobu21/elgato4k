#include "audio/AudioPassthrough.h"

#include "base/Log.h"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <format>

namespace vela::audio {
namespace {
std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return s;
}
} // namespace

std::vector<Endpoint> Passthrough::captureEndpoints() {
    std::vector<Endpoint> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return out;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return out;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        LPWSTR id = nullptr;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(devices->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
            SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR)
            out.push_back({id, name.pwszVal});
        PropVariantClear(&name);
        CoTaskMemFree(id);
    }
    return out;
}

std::wstring Passthrough::matchEndpoint(const std::wstring& videoName) {
    const auto endpoints = captureEndpoints();
    const auto video = lower(videoName);
    for (const auto& e : endpoints) {
        const auto n = lower(e.name);
        if (n.find(L"elgato") != std::wstring::npos || n.find(L"game capture") != std::wstring::npos ||
            (!video.empty() && n.find(video) != std::wstring::npos))
            return e.id;
    }
    return {};
}

bool Passthrough::start(const std::wstring& captureId) {
    stop();
    if (captureId.empty()) return false;
    stop_ = false;
    thread_ = std::thread([this, captureId] { run(captureId); });
    return true;
}

void Passthrough::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Passthrough::run(std::wstring captureId) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> input, output;
    ComPtr<IAudioClient> capture;
    ComPtr<IAudioClient> render;
    ComPtr<IAudioCaptureClient> captureClient;
    ComPtr<IAudioRenderClient> renderClient;
    WAVEFORMATEX* format = nullptr;
    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 renderFrames = 0;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
              SUCCEEDED(enumerator->GetDevice(captureId.c_str(), &input)) &&
              SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output)) &&
              SUCCEEDED(input->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capture)) &&
              SUCCEEDED(output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render)) &&
              SUCCEEDED(capture->GetMixFormat(&format));
    if (ok) {
        // Shared-mode mix formats are 32-bit float. The render side converts
        // rate and channel layout if the two devices differ.
        ok = SUCCEEDED(capture->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, format, nullptr)) &&
             SUCCEEDED(capture->SetEventHandle(captureEvent)) &&
             SUCCEEDED(capture->GetService(IID_PPV_ARGS(&captureClient)));
        bool lowLatency = false;
        ComPtr<IAudioClient3> render3;
        WAVEFORMATEX* renderMix = nullptr;
        if (ok && SUCCEEDED(render.As(&render3)) && SUCCEEDED(render->GetMixFormat(&renderMix)) &&
            renderMix->nSamplesPerSec == format->nSamplesPerSec && renderMix->nChannels == format->nChannels) {
            UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
            lowLatency = SUCCEEDED(render3->GetSharedModeEnginePeriod(format, &def, &fundamental, &minimum, &maximum)) &&
                         SUCCEEDED(render3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimum, format, nullptr));
        }
        CoTaskMemFree(renderMix);
        if (ok && !lowLatency)
            ok = SUCCEEDED(render->Initialize(AUDCLNT_SHAREMODE_SHARED,
                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                     100000, 0, format, nullptr));
        ok = ok && SUCCEEDED(render->SetEventHandle(renderEvent)) && SUCCEEDED(render->GetService(IID_PPV_ARGS(&renderClient))) &&
             SUCCEEDED(render->GetBufferSize(&renderFrames));
        log::info("audio", std::format("passthrough {} rate={} ch={} lowLatency={}", ok ? "ready" : "failed",
            format->nSamplesPerSec, format->nChannels, lowLatency));
    }

    if (ok && SUCCEEDED(capture->Start()) && SUCCEEDED(render->Start())) {
        const UINT32 channels = format->nChannels;
        const double framesPerMs = format->nSamplesPerSec / 1000.0;
        std::deque<float> ring;   // interleaved samples
        bool primed = false;
        HANDLE events[] = {captureEvent, renderEvent};
        while (!stop_) {
            WaitForMultipleObjects(2, events, FALSE, 50);
            // Drain every captured packet.
            UINT32 packet = 0;
            while (SUCCEEDED(captureClient->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                const float* samples = reinterpret_cast<const float*>(data);
                const float gain = volume_.load();
                for (UINT32 i = 0; i < frames * channels; ++i)
                    ring.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : samples[i] * gain);
                captureClient->ReleaseBuffer(frames);
            }
            // Sound waits `target` frames in total (our ring + the device's
            // queue), the picture's latency. Prime up to it, then pass audio
            // through at the device's pace. Drift between the card's clock and
            // the output's: drop the oldest audio 30 ms over the target, prime
            // again 8 ms under it (a short gap, only after a delay change).
            UINT32 padding = 0;
            if (FAILED(render->GetCurrentPadding(&padding))) continue;
            const size_t target = size_t(delayMs_.load() * framesPerMs);
            size_t total = ring.size() / channels + padding;
            if (total > target + size_t(30 * framesPerMs)) {
                const size_t drop = std::min(total - target, ring.size() / channels);
                ring.erase(ring.begin(), ring.begin() + std::ptrdiff_t(drop * channels));
                total -= drop;
            }
            if (primed && total + size_t(8 * framesPerMs) < target) primed = false;
            if (!primed && total >= target) primed = true;
            const UINT32 space = renderFrames - padding;
            const UINT32 frames = primed ? std::min<UINT32>(space, UINT32(ring.size() / channels)) : 0;
            if (frames > 0) {
                BYTE* out = nullptr;
                if (SUCCEEDED(renderClient->GetBuffer(frames, &out))) {
                    float* dst = reinterpret_cast<float*>(out);
                    std::copy_n(ring.begin(), frames * channels, dst);
                    ring.erase(ring.begin(), ring.begin() + std::ptrdiff_t(frames * channels));
                    renderClient->ReleaseBuffer(frames, 0);
                }
            }
            bufferedMs_ = (ring.size() / channels + padding) / framesPerMs;
        }
        capture->Stop();
        render->Stop();
    }
    CoTaskMemFree(format);
    CloseHandle(captureEvent);
    CloseHandle(renderEvent);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

} // namespace vela::audio
