#if defined(SC_PLATFORM_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <Windows.h>
#  include <cstdlib>
#endif

#include <cstdio>
#include <cstdint>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
#include "platform/platform_screencap.h"

constexpr int HOTKEY_ID_SCREENSHOT = 1;
constexpr int HOTKEY_ID_SCREENSHOT_CLIPBOARD = 2;
constexpr int HOTKEY_ID_SCREENSHOT_OCR = 3;

int entry(int argc, char** argv) {
    sc_initialize();

    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT)) {
        fprintf(stderr, "Failed to register hotkey\n");
        return 1;
    }
    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_CLIPBOARD, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_SNAPSHOT)) {
        fprintf(stderr, "Failed to register clipboard hotkey\n");
        return 1;
    }
    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_OCR, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SNAPSHOT)) {
        fprintf(stderr, "Failed to register OCR hotkey\n");
        return 1;
    }

    sc_capture_options active_options = {0};
    MSG msg = { 0 };

    while (true) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOTKEY_ID_SCREENSHOT || msg.wParam == HOTKEY_ID_SCREENSHOT_CLIPBOARD || msg.wParam == HOTKEY_ID_SCREENSHOT_OCR) {
                    active_options.include_cursor = false;
                    active_options.copy_to_clipboard = (msg.wParam == HOTKEY_ID_SCREENSHOT_CLIPBOARD);
                    active_options.extract_text = (msg.wParam == HOTKEY_ID_SCREENSHOT_OCR);
                    sc_begin_capture(active_options);
                }
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        sc_capture_info ci;
        if (sc_capture_update(ci)) {
            if (active_options.extract_text) {
                fprintf(stdout, "Extracted text to clipboard\n");
                delete[] ci.data;
            } else if (active_options.copy_to_clipboard) {
                fprintf(stdout, "Copied screenshot to clipboard\n");
                delete[] ci.data;
            } else {
                std::string filename = "screenshot_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".png";
                if (sc_save_capture(filename.c_str(), ci)) {
                    fprintf(stdout, "Saved screenshot to %s\n", filename.c_str());
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT);
    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_CLIPBOARD);
    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_OCR);

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
#endif