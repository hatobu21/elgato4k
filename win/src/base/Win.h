#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "vela/core/Time.h"

namespace vela {

using Microsoft::WRL::ComPtr;

// ---- clock: QueryPerformanceCounter in microseconds, the core's time base
inline int64_t qpcFrequency() {
    static const int64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return frequency;
}
inline core::Micros qpcToMicros(int64_t qpc) {
    const int64_t f = qpcFrequency();
    return (qpc / f) * 1000000 + (qpc % f) * 1000000 / f;
}
inline core::Micros nowMicros() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return qpcToMicros(c.QuadPart);
}

// ---- strings
inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

// ---- paths
inline std::filesystem::path exeDirectory() {
    wchar_t buffer[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    return std::filesystem::path(buffer).parent_path();
}
inline std::filesystem::path appDataDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::filesystem::path base = n > 0 && n < MAX_PATH ? std::filesystem::path(buffer) : exeDirectory();
    return base / L"Vela";
}

// ---- SEH around proprietary runtimes (NGX, NVOF, NvAPI). A crash inside a
// vendor DLL disables that feature instead of taking the process down.
#if defined(_MSC_VER)
#define VELA_SEH_TRY __try
#define VELA_SEH_EXCEPT(codeVar) __except ((codeVar) = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
#else
// Non-MSVC builds are used only for syntax checks of this tree.
#define VELA_SEH_TRY if (true)
#define VELA_SEH_EXCEPT(codeVar) else
#endif

} // namespace vela
