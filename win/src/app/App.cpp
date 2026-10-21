#include "app/App.h"

#include "app/Protocol.h"
#include "base/Log.h"

#include <windowsx.h>

namespace vela::app {
namespace {
constexpr wchar_t kClass[] = L"VelaVideo";
constexpr UINT kStateChanged = WM_APP + 1;
constexpr UINT_PTR kTelemetryTimer = 1, kSaveTimer = 2, kHideTimer = 3;
} // namespace

int App::run(core::Settings settings) {
    settings_ = settings;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    const int w = 1600, h = 900;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    video_ = CreateWindowExW(0, kClass, L"Vela", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, wc.hInstance, this);
    if (!video_) return 1;
    ShowWindow(video_, SW_SHOW);
    if (settings_.display.fullscreen) setFullscreen(true);

    engine_.onStateChanged = [this] { PostMessageW(video_, kStateChanged, 0, 0); };
    engine_.start(video_, settings_);

    overlay_.onMessage = [this](const std::string& json) { onUiMessage(json); };
    if (!overlay_.create(video_, (exeDirectory() / L"ui").wstring()))
        log::error("app", "interface could not start (WebView2 runtime missing?); video keeps running");
    SetTimer(video_, kTelemetryTimer, 100, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

void App::setFullscreen(bool on) {
    if (on == fullscreen_) return;
    fullscreen_ = on;
    if (on) {
        GetWindowRect(video_, &windowed_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(video_, MONITOR_DEFAULTTONEAREST), &mi);
        // Borderless window covering the monitor: Windows 11 promotes it to
        // independent flip whenever nothing else is drawn over it.
        SetWindowLongPtrW(video_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(video_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(video_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(video_, nullptr, windowed_.left, windowed_.top, windowed_.right - windowed_.left,
                     windowed_.bottom - windowed_.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    overlay_.follow();
}

void App::showUi(bool visible) {
    KillTimer(video_, kHideTimer);
    if (visible) {
        overlay_.show(true);
        overlay_.post(R"({"type":"ui","visible":true})");
        overlay_.post(stateMessage(engine_.state()));
    } else {
        // Let the page's fade finish, then take the window off screen.
        SetTimer(video_, kHideTimer, 300, nullptr);
    }
}

void App::change(const core::Settings& next) {
    settings_ = next;
    engine_.apply(settings_);
    SetTimer(video_, kSaveTimer, 1000, nullptr);   // save once the user stops changing things
}

void App::onUiMessage(const std::string& text) {
    const auto msg = nlohmann::json::parse(text, nullptr, false);
    if (msg.is_discarded() || !msg.contains("type")) return;
    const auto type = msg["type"].get<std::string>();
    if (type == "hello") {
        overlay_.post(stateMessage(engine_.state()));
    } else if (type == "set" && msg.contains("path") && msg.contains("value")) {
        auto next = settings_;
        if (applySetting(next, msg["path"].get<std::string>(), msg["value"])) change(next);
    } else if (type == "command" && msg.contains("name")) {
        const auto name = msg["name"].get<std::string>();
        if (name == "toggleFullscreen") setFullscreen(!fullscreen_);
        else if (name == "resetStats") engine_.resetStats();
        else if (name == "hideUi") showUi(false);
    } else if (type == "ui" && msg.contains("visible")) {
        if (!msg["visible"].get<bool>()) showUi(false);
    }
}

LRESULT CALLBACK App::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && !self->video_) self->video_ = hwnd;
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case kStateChanged:
        overlay_.post(stateMessage(engine_.state()));
        return 0;
    case WM_TIMER:
        if (wp == kTelemetryTimer && overlay_.visible()) overlay_.post(telemetryMessage(engine_.telemetry()));
        else if (wp == kSaveTimer) { KillTimer(video_, kSaveTimer); saveSettings(settings_); }
        else if (wp == kHideTimer) { KillTimer(video_, kHideTimer); overlay_.show(false); }
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            engine_.resize(LOWORD(lp), HIWORD(lp));
            overlay_.follow();
        }
        return 0;
    case WM_MOVE:
        overlay_.follow();
        return 0;
    case WM_MOUSEMOVE:
        // Only reaches the video window while the interface is hidden.
        if (!overlay_.visible()) showUi(true);
        return 0;
    case WM_LBUTTONDBLCLK:
        setFullscreen(!fullscreen_);
        return 0;
    case WM_KEYDOWN: {
        auto next = settings_;
        switch (wp) {
        case VK_HOME: showUi(!overlay_.visible()); return 0;
        case 'F': next.frameGen.enabled = !next.frameGen.enabled; change(next); return 0;
        case 'L': next.latency.hud = !next.latency.hud; change(next); return 0;
        case VK_ESCAPE: if (fullscreen_) setFullscreen(false); return 0;
        default: break;
        }
        if (wp >= '2' && wp <= '6') { next.frameGen.multiplier = unsigned(wp - '0'); change(next); return 0; }
        break;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN)) { setFullscreen(!fullscreen_); return 0; }
        break;
    case WM_CLOSE:
        KillTimer(video_, kTelemetryTimer);
        saveSettings(settings_);
        overlay_.destroy();
        engine_.stop();
        DestroyWindow(video_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(video_, msg, wp, lp);
}

} // namespace vela::app
