#pragma once
// Window management and the glue between the page and the engine. Everything
// here runs on the UI thread (STA); the engine runs on its own threads.
#include "app/UiOverlay.h"
#include "engine/Engine.h"

namespace vela::app {

class App {
public:
    int run(core::Settings settings);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void setFullscreen(bool on);
    void onUiMessage(const std::string& json);
    void change(const core::Settings& next);
    void showUi(bool visible);
    void flash(const std::string& text);
    void postMode();

    HWND video_ = nullptr;
    UiOverlay overlay_;
    engine::Engine engine_;
    core::Settings settings_;
    RECT windowed_{};
    bool fullscreen_ = false;
};

} // namespace vela::app
