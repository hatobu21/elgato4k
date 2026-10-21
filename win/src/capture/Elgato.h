#pragma once
// Elgato private KS property set: HDMI HDR InfoFrame and the card's own HDR
// tone mapping switch. Adapted from Veyra (GPL-3.0) ElgatoHdrControl, which
// follows elgatosf/capture-device-support (MIT) and Nitlink (MIT); here the
// same properties are reached through IKsControl on the Media Foundation
// source instead of IKsPropertySet on a DirectShow filter.
#include "capture/Capture.h"

#include <array>

namespace vela::capture {

struct InfoFrame {
    bool valid = false;
    unsigned eotf = 0;              // 0 SDR, 2 PQ, 3 HLG
    float peak = 0, maxCll = 0, maxFall = 0;
};
InfoFrame parseInfoFrame(const std::array<unsigned char, 32>& bytes);
bool isElgato(std::wstring_view name);
bool isElgatoMk2(std::wstring_view name);

class ElgatoControl {
public:
    explicit ElgatoControl(IUnknown* source, std::wstring_view name);
    ~ElgatoControl();   // restores the tone mapping switch it changed
    bool available() const { return control_ != nullptr; }

    InfoFrame readInfoFrame();
    // 1: the card tone maps HDR to SDR itself (used for 8-bit capture on an
    // SDR display); 0: HDR passes through in P010.
    bool setHardwareToneMap(bool on);
    SignalInfo signal(PixelFormat format);

private:
    bool get(unsigned long id, void* data, unsigned long size);
    bool set(unsigned long id, const void* data, unsigned long size);

    ComPtr<IUnknown> control_;      // IKsControl
    bool mk2_ = false;
    bool restore_ = false;
    unsigned long original_ = 0;
};

} // namespace vela::capture
