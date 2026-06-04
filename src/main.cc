#if defined(SC_PLATFORM_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <Windows.h>
#  include <cstdlib>
#endif // SC_PLATFORM_WINDOWS
#include <cstdio>
#include <cstdint>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>

#include "platform/platform_screencap.h"

constexpr int HOTKEY_ID_SCREENSHOT = 1;

int entry(int argc, char** argv) {
    sc_initialize();

    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT)) {
        fprintf(stderr, "Failed to register hotkey\n");
        return 1;
    }

    MSG msg = { 0 };
    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_ID_SCREENSHOT) {
                printf("Screenshot hotkey pressed\n");
                sc_begin_capture();
            }
        }

        sc_capture_info ci;
        if (sc_capture_update(ci)) {
            std::string filename = "screenshot_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".png";
            if (sc_save_capture(filename.c_str(), ci)) {
                fprintf(stdout, "Saved screenshot to %s\n", filename.c_str());
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT);

    return 0;
}

#if defined(SC_PLATFORM_WINDOWS)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
#if defined(SC_DEBUG)
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
    }
#endif

    return entry(__argc, __argv);
}
#endif // SC_PLATFORM_WINDOWS