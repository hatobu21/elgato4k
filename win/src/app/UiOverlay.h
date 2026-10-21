#pragma once
// The interface layer: an owned, borderless popup over the video window,
// WS_EX_NOREDIRECTIONBITMAP, with WebView2 in visual hosting mode composed
// through DirectComposition. The page background is transparent, so the
// dock and panels float on the video. When the page hides itself the window
// is hidden too, and the video window is alone on screen again (independent
// flip, no compositor in the present path).
#include "base/Win.h"

#include <functional>
#include <string>

struct ICoreWebView2;
struct ICoreWebView2Environment;
struct ICoreWebView2Controller;
struct ICoreWebView2CompositionController;
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;

namespace vela::app {

class UiOverlay {
public:
    UiOverlay();
    ~UiOverlay();   // defined where the COM types are complete
    std::function<void(const std::string& json)> onMessage;   // page -> host, UI thread

    bool create(HWND owner, const std::wstring& uiFolder);
    void destroy();
    void follow();                      // match the owner's client area
    void show(bool visible);
    bool visible() const { return visible_; }
    bool ready() const { return webview_ != nullptr; }
    void post(const std::string& json); // host -> page

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void onEnvironment(ICoreWebView2Environment* env);
    void onController(ICoreWebView2CompositionController* controller);

    HWND owner_ = nullptr, hwnd_ = nullptr;
    std::wstring folder_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<ICoreWebView2CompositionController> composition_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    bool visible_ = true;
    bool tracking_ = false;
};

} // namespace vela::app
