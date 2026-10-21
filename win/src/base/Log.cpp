#include "base/Log.h"

#include "base/Win.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>

namespace vela::log {
namespace {
std::mutex gMutex;
FILE* gFile = nullptr;
std::string gBuffer;
std::chrono::steady_clock::time_point gLastFlush;

void write(const char* level, std::string_view tag, std::string_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const auto line = std::format("{:02}:{:02}:{:02}.{:03} {} [{}] {}\n", t.wHour, t.wMinute, t.wSecond,
        t.wMilliseconds, level, tag, message);
    OutputDebugStringA(line.c_str());
    std::lock_guard lock(gMutex);
    if (!gFile) return;
    gBuffer += line;
    const auto now = std::chrono::steady_clock::now();
    // Errors flush at once; everything else at most every 250 ms or 64 KiB.
    if (level[0] == 'E' || gBuffer.size() > 65536 || now - gLastFlush > std::chrono::milliseconds(250)) {
        std::fwrite(gBuffer.data(), 1, gBuffer.size(), gFile);
        std::fflush(gFile);
        gBuffer.clear();
        gLastFlush = now;
    }
}
} // namespace

void open() {
    std::lock_guard lock(gMutex);
    if (gFile) return;
    std::error_code ec;
    const auto dir = appDataDirectory() / L"logs";
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / L"vela.log";
    // Keep one previous log for crash reports.
    std::filesystem::rename(path, dir / L"vela.previous.log", ec);
    gFile = _wfopen(path.c_str(), L"wb");
    gLastFlush = std::chrono::steady_clock::now();
}

void flush() {
    std::lock_guard lock(gMutex);
    if (gFile && !gBuffer.empty()) {
        std::fwrite(gBuffer.data(), 1, gBuffer.size(), gFile);
        std::fflush(gFile);
        gBuffer.clear();
    }
}

void close() {
    flush();
    std::lock_guard lock(gMutex);
    if (gFile) std::fclose(gFile);
    gFile = nullptr;
}

void info(std::string_view tag, std::string_view message) { write("I", tag, message); }
void warn(std::string_view tag, std::string_view message) { write("W", tag, message); }
void error(std::string_view tag, std::string_view message) { write("E", tag, message); }

} // namespace vela::log
