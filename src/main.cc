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
constexpr int HOTKEY_ID_SCREENSHOT_ACTIVE_WINDOW = 4;
constexpr int HOTKEY_ID_SCREENSHOT_CURRENT_MONITOR = 5;

// Controls:
// PrintScreen - Begin screenshot capture (select region or window)
// Ctrl + Shift + PrintScreen - Same as above but capture to clipboard
// Ctrl + Alt + PrintScreen - Extract text from region and copy to clipboard
// Alt + PrintScreen - Capture active window
// Ctrl + PrintScreen - Capture current monitor

std::string get_filename_timestamp() {
    // dd-mm-yyyy_hh-mm-ss
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&now_tm, &now_time_t);
#else
    localtime_r(&now_time_t, &now_tm);
#endif
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%d-%m-%Y_%H-%M-%S", &now_tm);
    return std::string(buffer);
}

int entry(int argc, char** argv) {
    sc_initialize();

    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT, MOD_NOREPEAT, VK_SNAPSHOT)) {
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
    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_ACTIVE_WINDOW, MOD_ALT | MOD_NOREPEAT, VK_SNAPSHOT)) {
        fprintf(stderr, "Failed to register current monitor hotkey\n");
        return 1;
    }
    if (!RegisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_CURRENT_MONITOR, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT)) {
        fprintf(stderr, "Failed to register current monitor hotkey\n");
        return 1;
    }

    sc_capture_options active_options = {0};
    MSG msg = { 0 };

    while (true) {
        sc_capture_info ci = {};

        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOTKEY_ID_SCREENSHOT || msg.wParam == HOTKEY_ID_SCREENSHOT_CLIPBOARD || msg.wParam == HOTKEY_ID_SCREENSHOT_OCR) {
                    active_options.include_cursor = false;
                    active_options.copy_to_clipboard = (msg.wParam == HOTKEY_ID_SCREENSHOT_CLIPBOARD);
                    active_options.extract_text = (msg.wParam == HOTKEY_ID_SCREENSHOT_OCR);
                    sc_begin_capture(active_options);
                } else {
                    bool success = false;
                    if (msg.wParam == HOTKEY_ID_SCREENSHOT_ACTIVE_WINDOW) {
                        success = sc_capture_window(-1, ci);
                    } else if (msg.wParam == HOTKEY_ID_SCREENSHOT_CURRENT_MONITOR) {
                        success = sc_capture_desktop(-1, ci);
                    }

                    if (success) {
                        std::string filename = get_filename_timestamp() + ".png";
                        if (sc_save_capture(filename.c_str(), ci)) {
                            fprintf(stdout, "Saved screenshot to %s\n", filename.c_str());
                        }
                        delete[] ci.data;
                    } else {
                        fprintf(stderr, "Failed to capture current monitor\n");
                    }
                }
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (sc_capture_update(ci)) {
            if (active_options.extract_text) {
                fprintf(stdout, "Extracted text to clipboard\n");
                delete[] ci.data;
            } else if (active_options.copy_to_clipboard) {
                fprintf(stdout, "Copied screenshot to clipboard\n");
                delete[] ci.data;
            } else {
                std::string filename = get_filename_timestamp() + ".png";
                if (sc_save_capture(filename.c_str(), ci)) {
                    fprintf(stdout, "Saved screenshot to %s\n", filename.c_str());
                }
                delete[] ci.data;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT);
    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_CLIPBOARD);
    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_OCR);
    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_ACTIVE_WINDOW);
    UnregisterHotKey(nullptr, HOTKEY_ID_SCREENSHOT_CURRENT_MONITOR);

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