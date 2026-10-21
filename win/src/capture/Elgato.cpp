#include "capture/Elgato.h"

#include "base/Log.h"

#include <dshow.h>
#include <ks.h>
#include <ksproxy.h>

#include <algorithm>
#include <format>

namespace vela::capture {
namespace {
// Elgato vendor property set and ids (elgatosf/capture-device-support).
constexpr GUID kPropertySet{0xD1E5209F, 0x68FD, 0x4529, {0xBE, 0xE0, 0x5E, 0x7A, 0x1F, 0x47, 0x92, 0x26}};
constexpr unsigned long kInfoFrameLow = 720, kInfoFrameHigh = 721, kToneMap = 722;
// IID_IKsControl, spelled out so no extra import library is needed.
constexpr GUID kIKsControl{0x28F54685, 0x06FD, 0x11D2, {0xB2, 0x7A, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
} // namespace

bool isElgatoMk2(std::wstring_view name) {
    for (const auto* alias : {L"Game Capture 4K60 Pro MK.2", L"Elgato Game Capture 4K60 Pro MK.2"})
        if (CompareStringOrdinal(name.data(), int(name.size()), alias, -1, TRUE) == CSTR_EQUAL) return true;
    return false;
}

bool isElgato(std::wstring_view name) {
    std::wstring lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    return lower.find(L"elgato") != std::wstring::npos || lower.rfind(L"game capture ", 0) == 0 ||
           lower.find(L"4k pro") != std::wstring::npos;
}

InfoFrame parseInfoFrame(const std::array<unsigned char, 32>& b) {
    // An all-zero packet is the driver's way of saying "SDR"; a failed read
    // is reported separately by the caller.
    if (std::all_of(b.begin(), b.end(), [](unsigned char v) { return v == 0; })) return {true};
    if ((b[0] & 0x7f) != 7 || b[1] != 1 || b[2] != 26 || b[5] != 0) return {};
    unsigned sum = 0;
    for (unsigned i = 0; i < 30; ++i) sum += b[i];
    if ((sum & 255) != 0 || (b[4] != 0 && b[4] != 2 && b[4] != 3)) return {};
    const auto u16 = [&](unsigned i) { return float(unsigned(b[i]) | (unsigned(b[i + 1]) << 8)); };
    return {true, b[4], u16(22), u16(26), u16(28)};
}

ElgatoControl::ElgatoControl(IUnknown* source, std::wstring_view name) : mk2_(isElgatoMk2(name)) {
    if (!source || !isElgato(name)) return;
    ComPtr<IUnknown> control;
    if (SUCCEEDED(source->QueryInterface(kIKsControl, reinterpret_cast<void**>(control.GetAddressOf())))) control_ = control;
    log::info("elgato", std::format("vendor control {} mk2={}", control_ ? "available" : "unavailable", mk2_));
}

ElgatoControl::~ElgatoControl() {
    if (restore_) set(kToneMap, &original_, sizeof(original_));
}

bool ElgatoControl::get(unsigned long id, void* data, unsigned long size) {
    if (!control_) return false;
    KSPROPERTY p{};
    p.Set = kPropertySet;
    p.Id = id;
    p.Flags = KSPROPERTY_TYPE_GET;
    ULONG returned = 0;
    auto* ks = static_cast<IKsControl*>(control_.Get());
    const HRESULT hr = ks->KsProperty(&p, sizeof(p), data, size, &returned);
    return SUCCEEDED(hr) && returned == size;
}

bool ElgatoControl::set(unsigned long id, const void* data, unsigned long size) {
    if (!control_) return false;
    KSPROPERTY p{};
    p.Set = kPropertySet;
    p.Id = id;
    p.Flags = KSPROPERTY_TYPE_SET;
    ULONG returned = 0;
    auto* ks = static_cast<IKsControl*>(control_.Get());
    return SUCCEEDED(ks->KsProperty(&p, sizeof(p), const_cast<void*>(data), size, &returned));
}

InfoFrame ElgatoControl::readInfoFrame() {
    std::array<unsigned char, 32> bytes{};
    if (!get(kInfoFrameLow, bytes.data(), 16) || !get(kInfoFrameHigh, bytes.data() + 16, 16)) return {};
    return parseInfoFrame(bytes);
}

bool ElgatoControl::setHardwareToneMap(bool on) {
    unsigned long current = 0;
    const bool known = get(kToneMap, &current, sizeof(current)) && current <= 1;
    const unsigned long wanted = on ? 1 : 0;
    if (known && current == wanted) return true;
    if (!set(kToneMap, &wanted, sizeof(wanted))) {
        // 4K X / 4K S class cards answer "unsupported" and keep their state.
        log::info("elgato", "hardware tone map switch not available on this card");
        return false;
    }
    if (known && !restore_) {
        original_ = current;
        restore_ = true;
    }
    log::info("elgato", std::format("hardware tone map={}", wanted));
    return true;
}

SignalInfo ElgatoControl::signal(PixelFormat format) {
    SignalInfo s;
    const auto frame = readInfoFrame();
    s.hdr = frame.valid && (frame.eotf == 2 || frame.eotf == 3);
    s.hlg = frame.valid && frame.eotf == 3;
    s.maxCll = frame.maxCll;
    s.maxFall = frame.maxFall;
    s.masteringPeak = frame.peak;
    // Elgato cards other than the 4K60 Pro MK.2 send HDR P010 with limited
    // luma and full-range chroma (Nitlink, measured against the console).
    s.fullRangeChroma = !mk2_;
    (void)format;
    return s;
}

} // namespace vela::capture
