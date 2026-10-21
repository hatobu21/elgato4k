// Vela: capture card player for frame generation (Elgato 4K Pro, RTX 50).
#include "app/App.h"
#include "app/Protocol.h"
#include "base/Log.h"

#include <objbase.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The video path is latency bound: keep the process above normal desktop
    // work. Individual threads also join MMCSS ("Games" / "Pro Audio").
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    // Do not let Windows throttle the process when the window loses focus.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;   // WebView2 needs STA
    vela::log::open();
    vela::log::info("app", "start");
    vela::app::App app;
    const int code = app.run(vela::app::loadSettings());
    vela::log::info("app", "exit");
    vela::log::close();
    CoUninitialize();
    return code;
}
