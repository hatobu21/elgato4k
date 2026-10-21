#include "app/UiOverlay.h"

#include "app/ComHandler.h"
#include "base/Log.h"

#include <WebView2.h>
#include <dcomp.h>
#include <windowsx.h>

#include <format>

namespace vela::app {
namespace {
constexpr wchar_t kClass[] = L"VelaOverlay";

using EnvironmentDone = Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
    IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>;
using ControllerDone = Handler<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler,
    IID_ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler, HRESULT, ICoreWebView2CompositionController*>;
using MessageReceived = Handler<ICoreWebView2WebMessageReceivedEventHandler,
    IID_ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;
using CursorChanged = Handler<ICoreWebView2CursorChangedEventHandler,
    IID_ICoreWebView2CursorChangedEventHandler, ICoreWebView2CompositionController*, IUnknown*>;

template <typename T>
bool query(IUnknown* from, const IID& iid, ComPtr<T>& out) {
    return from && SUCCEEDED(from->QueryInterface(iid, reinterpret_cast<void**>(out.ReleaseAndGetAddressOf())));
}
} // namespace

UiOverlay::UiOverlay() = default;
UiOverlay::~UiOverlay() { destroy(); }

bool UiOverlay::create(HWND owner, const std::wstring& uiFolder) {
    owner_ = owner;
    folder_ = uiFolder;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kClass, L"Vela", WS_POPUP,
                            0, 0, 100, 100, owner, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    follow();
    ShowWindow(hwnd_, SW_SHOWNA);

    if (FAILED(DCompositionCreateDevice2(nullptr, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(dcomp_.GetAddressOf()))) ||
        FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_)) || FAILED(dcomp_->CreateVisual(&visual_)) ||
        FAILED(target_->SetRoot(visual_.Get())) || FAILED(dcomp_->Commit())) {
        log::error("ui", "DirectComposition setup failed");
        return false;
    }
    const auto userData = (appDataDirectory() / L"webview2").wstring();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        EnvironmentDone::make([this](HRESULT result, ICoreWebView2Environment* env) {
            if (SUCCEEDED(result) && env) onEnvironment(env);
            else log::error("ui", std::format("WebView2 environment failed 0x{:08X}; is the Evergreen runtime installed?", uint32_t(result)));
            return S_OK;
        }));
    return SUCCEEDED(hr);
}

void UiOverlay::onEnvironment(ICoreWebView2Environment* env) {
    ComPtr<ICoreWebView2Environment3> env3;
    if (!query(env, IID_ICoreWebView2Environment3, env3)) return;
    env3->CreateCoreWebView2CompositionController(hwnd_, ControllerDone::make([this](HRESULT result, ICoreWebView2CompositionController* c) {
        if (SUCCEEDED(result) && c) onController(c);
        return S_OK;
    }));
}

void UiOverlay::onController(ICoreWebView2CompositionController* controller) {
    composition_ = controller;
    if (!query(controller, IID_ICoreWebView2Controller, controller_)) return;
    composition_->put_RootVisualTarget(visual_.Get());
    dcomp_->Commit();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    controller_->put_Bounds(rc);
    controller_->put_IsVisible(visible_ ? TRUE : FALSE);
    ComPtr<ICoreWebView2Controller2> controller2;
    if (query(controller_.Get(), IID_ICoreWebView2Controller2, controller2))
        controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});   // transparent page

    controller_->get_CoreWebView2(&webview_);
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings))) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
#ifdef NDEBUG
        settings->put_AreDevToolsEnabled(FALSE);
#endif
    }
    // The page is served from the folder next to the executable; no network.
    ComPtr<ICoreWebView2_3> webview3;
    if (query(webview_.Get(), IID_ICoreWebView2_3, webview3))
        webview3->SetVirtualHostNameToFolderMapping(L"vela.ui", folder_.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
    EventRegistrationToken token{};
    webview_->add_WebMessageReceived(MessageReceived::make([this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
        LPWSTR json = nullptr;
        if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json && onMessage) onMessage(narrow(json));
        CoTaskMemFree(json);
        return S_OK;
    }), &token);
    composition_->add_CursorChanged(CursorChanged::make([this](ICoreWebView2CompositionController*, IUnknown*) {
        UINT32 id = 0;
        if (SUCCEEDED(composition_->get_SystemCursorId(&id)) && id) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(id)));
        return S_OK;
    }), &token);
    webview_->Navigate(L"https://vela.ui/index.html");
}

void UiOverlay::destroy() {
    if (controller_) controller_->Close();
    webview_.Reset();
    controller_.Reset();
    composition_.Reset();
    visual_.Reset();
    target_.Reset();
    dcomp_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void UiOverlay::follow() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(owner_, &rc);
    POINT origin{0, 0};
    ClientToScreen(owner_, &origin);
    SetWindowPos(hwnd_, nullptr, origin.x, origin.y, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

void UiOverlay::show(bool visible) {
    if (!hwnd_ || visible == visible_) return;
    visible_ = visible;
    if (visible) follow();
    ShowWindow(hwnd_, visible ? SW_SHOWNA : SW_HIDE);
    if (controller_) controller_->put_IsVisible(visible ? TRUE : FALSE);
    if (!visible) SetFocus(owner_);
}

void UiOverlay::post(const std::string& json) {
    if (webview_) webview_->PostWebMessageAsJson(widen(json).c_str());
}

LRESULT CALLBACK UiOverlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<UiOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT UiOverlay::handle(UINT msg, WPARAM wp, LPARAM lp) {
    // Visual hosting: the host forwards mouse input to WebView2 itself.
    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    bool mouse = true;
    switch (msg) {
    case WM_MOUSEMOVE: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE; break;
    case WM_LBUTTONDOWN: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN; break;
    case WM_LBUTTONUP: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP; break;
    case WM_LBUTTONDBLCLK: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK; break;
    case WM_RBUTTONDOWN: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN; break;
    case WM_RBUTTONUP: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP; break;
    case WM_MOUSEWHEEL: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL; break;
    case WM_MOUSELEAVE: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE; break;
    default: mouse = false;
    }
    if (mouse) {
        if (composition_) {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (msg == WM_MOUSEWHEEL) ScreenToClient(hwnd_, &p);
            if (msg == WM_MOUSELEAVE) p = {0, 0};
            const UINT32 data = msg == WM_MOUSEWHEEL ? UINT32(GET_WHEEL_DELTA_WPARAM(wp)) : 0;
            composition_->SendMouseInput(kind, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS(GET_KEYSTATE_WPARAM(wp)), data, p);
        }
        if (msg == WM_MOUSEMOVE && !tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (msg == WM_MOUSELEAVE) tracking_ = false;
        if (msg == WM_LBUTTONDOWN && controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    }
    switch (msg) {
    case WM_SIZE:
        if (controller_) {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            controller_->put_Bounds(rc);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && composition_) {
            UINT32 id = 0;
            if (SUCCEEDED(composition_->get_SystemCursorId(&id)) && id) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(id)));
                return TRUE;
            }
        }
        break;
    case WM_SETFOCUS:
        if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace vela::app
