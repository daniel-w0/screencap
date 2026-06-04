#include "platform/platform_screencap.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <WinUser.h>
#include <wingdi.h>
#include <cstdlib>
#include <cstdio>
#include <cstdint>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <algorithm>
#include <vector>

static std::vector<sc_monitor_info> g_monitors;

void _sc_swap_channels(uint32_t* pixels, int totalPixels) {
    for (int i = 0; i < totalPixels; ++i) {
        uint32_t p = pixels[i];
        pixels[i] = (p & 0xFF00FF00) | ((p & 0x00FF0000) >> 16) | ((p & 0x000000FF) << 16);
    }
}

void _sc_get_monitors(std::vector<sc_monitor_info>& monitors) {
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
        auto& monitors = *reinterpret_cast<std::vector<sc_monitor_info>*>(dwData);
        MONITORINFOEXA mi = {};
        mi.cbSize = sizeof(MONITORINFOEXA);

        if (GetMonitorInfoA(hMonitor, (LPMONITORINFO)&mi)) {
            sc_monitor_info info = {};

            info.id = static_cast<uint8_t>(monitors.size());

            strncpy(info.name, mi.szDevice, sizeof(info.name) - 1);
            info.name[sizeof(info.name) - 1] = '\0';

            info.rect.x = mi.rcMonitor.left;
            info.rect.y = mi.rcMonitor.top;
            info.rect.width = mi.rcMonitor.right - mi.rcMonitor.left;
            info.rect.height = mi.rcMonitor.bottom - mi.rcMonitor.top;

            monitors.push_back(info);
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
}

void sc_initialize() {
    SetProcessDPIAware();
    _sc_get_monitors(g_monitors);
}

bool sc_capture_region(sc_rect rect, sc_capture_info& ci) {
    HDC hScreenDC = GetDC(nullptr);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    const int channels = 4;

    if (!hMemoryDC) {
        fprintf(stderr, "Failed to create compatible DC\n");
        return false;
    }

    ci.width = rect.width;
    ci.height = rect.height;

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, ci.width, ci.height);
    if (!hBitmap) {
        fprintf(stderr, "Failed to create compatible bitmap\n");
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }

    SelectObject(hMemoryDC, hBitmap);
    if (!BitBlt(hMemoryDC, 0, 0, ci.width, ci.height, hScreenDC, rect.x, rect.y, SRCCOPY)) {
        fprintf(stderr, "Failed to copy bitmap data\n");
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = ci.width;
    bih.biHeight = -ci.height;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    size_t imageSize = ci.width * ci.height * channels;

    ci.data = new unsigned char[imageSize];

    if (!GetDIBits(hMemoryDC, hBitmap, 0, ci.height, ci.data, (BITMAPINFO*)&bih, DIB_RGB_COLORS)) {
        fprintf(stderr, "Failed to get bitmap data\n");
        delete[] ci.data;
        DeleteObject(hBitmap);
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }

    //OpenClipboard(nullptr);
    //SetClipboardData(CF_BITMAP, hBitmap);
    //CloseClipboard();

    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(nullptr, hScreenDC);

    ci.channels = channels;

    return true;
}

bool sc_capture_window(int pid, sc_capture_info& ci) {
    struct EnumWindowsData {
        int targetPid;
        sc_capture_info& ci;
    } data = { pid, ci };

    return EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        EnumWindowsData* data = reinterpret_cast<EnumWindowsData*>(lParam);
        DWORD windowPid;
        GetWindowThreadProcessId(hwnd, &windowPid);
        if (windowPid == static_cast<DWORD>(data->targetPid)) {
            RECT rect;
            if (GetWindowRect(hwnd, &rect)) {
                sc_rect captureRect = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
                if (sc_capture_region(captureRect, data->ci)) {
                    return TRUE;
                }
            }
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
}

bool sc_capture_desktop(uint8_t desktop, sc_capture_info& ci) {
    if (desktop >= g_monitors.size()) {
        fprintf(stderr, "Invalid desktop ID\n");
        return false;
    }

    if (!sc_capture_region(g_monitors[desktop].rect, ci)) {
        fprintf(stderr, "read_screen_pixels_into failed\n");
        return false;
    }
    return true;
}

bool sc_save_capture(const char* filename, const sc_capture_info& ci) {
    _sc_swap_channels((uint32_t*)ci.data, ci.width * ci.height);
    stbi_write_png(filename, ci.width, ci.height, ci.channels, ci.data, ci.width * ci.channels);
    delete[] ci.data;
    return true;
}

bool sc_capture_auto(sc_capture_info& ci) {
    POINT cursorPos;
    GetCursorPos(&cursorPos);
    for (const auto& monitor : g_monitors) {
        if (cursorPos.x >= monitor.rect.x && cursorPos.x < monitor.rect.x + monitor.rect.width &&
            cursorPos.y >= monitor.rect.y && cursorPos.y < monitor.rect.y + monitor.rect.height) {
            return sc_capture_region(monitor.rect, ci);
        }
    }
    fprintf(stderr, "Cursor is not on any monitor\n");
    return false;
}

void sc_begin_capture() {

}

bool sc_capture_update(sc_capture_info& ci) {
    return false;
}