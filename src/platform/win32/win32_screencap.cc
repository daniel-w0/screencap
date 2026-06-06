#include "pch.h"
#include "platform/platform_screencap.h"
#include "screencap.h"
#include <dwmapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <commctrl.h>
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Comctl32.lib")

#define WM_TRAYICON (WM_USER + 1)
constexpr int TRAY_MENU_SETTINGS = 1001;
constexpr int TRAY_MENU_EXIT = 1002;

#pragma region OCR Decl
using DataWriter = winrt::Windows::Storage::Streams::DataWriter;
using IBuffer = winrt::Windows::Storage::Streams::IBuffer;
using SoftwareBitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using OcrEngine = winrt::Windows::Media::Ocr::OcrEngine;
using BitmapPixelFormat = winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using BitmapAlphaMode = winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using OcrResult = winrt::Windows::Media::Ocr::OcrResult;

struct sc_ocr_line {
    sc_rect rect;
    std::vector<sc_rect> chars;
};

struct Win32Toggle {
    RECT rect;
    const char* text;
    bool* targetValue;
    bool isHovered;
};

sc_internal OcrResult _ocr_get_bitmap_result(const unsigned char* data, int w, int h);
sc_internal void _ocr_run_async();
sc_internal bool _sc_rects_intersect(const sc_rect& a, const sc_rect& b);
sc_internal bool _ocr_snap_to_text(const sc_rect& drag, sc_rect& out);
sc_internal bool _ocr_text_at_point(POINT pt, sc_rect& out);
sc_internal std::wstring _ocr_text(OcrResult const& result);
#pragma endregion

#pragma region Windows Decl
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TrayUtilityWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam);
void UpdateHoverRect(POINT pt);
void RegisterTrayIcon(HWND hwnd);
#pragma endregion

#pragma region Structures
struct {
    std::vector<sc_monitor_info> monitors;
    sc_capture_options options  = { 0 };
    HWND overlayHwnd            = nullptr;
    sc_rect currentRect         = { 0 };
    sc_rect finalRect           = { 0 };
    POINT dragStart             = { 0 };
    HWND hoveredHwnd            = nullptr;
    HWND finalHwnd              = nullptr;
    HWND backgroundHwnd          = nullptr;
    HDC frozenDC                = nullptr;
    HBITMAP frozenBitmap        = nullptr;

    std::vector<sc_ocr_line> ocrLines;
    std::mutex               ocrMutex;

    bool capturing    = false;
    bool captureReady = false;
    bool mouseDown    = false;
    bool dragging     = false;
    bool snapDrag     = false;
    bool wasDragging  = false;
} g_state;

struct FindWindowData {
    POINT pt;
    HWND result;
    HWND overlayHwnd;
};
#pragma endregion

#pragma region Utils
std::string _sc_plat_get_default_save_path() {
    static fs::path pictures = fs::path(getenv("USERPROFILE")) / "Pictures";
    if (!fs::exists(pictures)) {
        pictures = fs::current_path();
    }

    fs::path base_savepath = pictures / "Screencap";
    if (!fs::exists(base_savepath)) {
        std::error_code ec;
        fs::create_directories(base_savepath, ec);
    }

    if (!fs::exists(base_savepath)) {
        return fs::current_path().string();
    }

    return base_savepath.string();
}

sc_internal bool _sc_rects_intersect(const sc_rect& a, const sc_rect& b) {
    return a.x < b.x + b.width && a.x + a.width > b.x &&
        a.y < b.y + b.height && a.y + a.height > b.y;
}

sc_internal void _sc_get_display_metrics(int& vx, int& vy, int& vw, int& vh) {
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

sc_internal void GetRealWindowRect(HWND hwnd, RECT* rect) {
    if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, rect, sizeof(RECT)) != S_OK) {
        GetWindowRect(hwnd, rect);
    }
}

sc_internal const sc_monitor_info* _sc_monitor_at_point(POINT pt) {
    for (const auto& m : g_state.monitors) {
        if (pt.x >= m.rect.x && pt.x < m.rect.x + m.rect.width &&
            pt.y >= m.rect.y && pt.y < m.rect.y + m.rect.height) {
            return &m;
        }
    }
    return nullptr;
}

sc_internal void _sc_get_monitors(std::vector<sc_monitor_info>& monitors) {
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

sc_internal std::string _sc_get_key_display_string(uint32_t modifiers, uint32_t key) {
    std::string res;
    if (modifiers & MOD_CONTROL) res += "Ctrl + ";
    if (modifiers & MOD_SHIFT)   res += "Shift + ";
    if (modifiers & MOD_ALT)     res += "Alt + ";

    if (key == VK_SNAPSHOT) {
        res += "PrintScreen";
    } else {
        char keyName[32] = { 0 };
        UINT scanCode = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
        LONG lParamValue = (scanCode & 0xFF) << 16;
        if (key >= VK_PRIOR && key <= VK_HELP) lParamValue |= (1 << 24);
        if (GetKeyNameTextA(lParamValue, keyName, sizeof(keyName)) > 0) {
            res += keyName;
        }
        else {
            res += (char)key;
        }
    }
    return res;
}

#pragma endregion

#pragma region Screencap
sc_internal bool _sc_write_to_clipboard(UINT format, const void* data, size_t size, const void* secondaryData = nullptr, size_t secondarySize = 0) {
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size + secondarySize);
    if (!hMem) {
        return false;
    }
    
    void* pMem = GlobalLock(hMem);
    memcpy(pMem, data, size);
    if (secondaryData && secondarySize) {
        memcpy((char*)pMem + size, secondaryData, secondarySize);
    }
    GlobalUnlock(hMem);
    
    if (!SetClipboardData(format, hMem)) {
        GlobalFree(hMem);
        return false;
    }
    return true;
}

void _sc_init_impl() {
    { // Register hotkeys
        for (auto& hk : sc_get_app().hotkeys) {
            hk.registered = RegisterHotKey(nullptr, hk.id, hk.modifiers | MOD_NOREPEAT, hk.key);
            if (!hk.registered) {
                fprintf(stderr, "Failed to register hotkey: %s\n", sc_hotkey_id_strings[hk.id]);
            }
        }

        HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
        DWORD prev_mode;
        if (GetConsoleMode(hInput, &prev_mode)) {
            SetConsoleMode(hInput, ENABLE_EXTENDED_FLAGS | (prev_mode & ~ENABLE_QUICK_EDIT_MODE));
        }
    }

    winrt::init_apartment();
    SetProcessDPIAware();
    _sc_get_monitors(g_state.monitors);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = "ScOverlayWindow";
    RegisterClassA(&wc);

    WNDCLASSA wcUtility = {};
    wcUtility.lpfnWndProc = TrayUtilityWndProc;
    wcUtility.hInstance = GetModuleHandle(nullptr);
    wcUtility.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcUtility.lpszClassName = "ScTrayUtilityWindow";
    RegisterClassA(&wcUtility);

    g_state.backgroundHwnd = CreateWindowExA(
        0, "ScTrayUtilityWindow", nullptr, 0,
        0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (g_state.backgroundHwnd) {
        RegisterTrayIcon(g_state.backgroundHwnd);
    }
}

void _sc_shutdown_impl() {
    for (const auto& hk : sc_get_app().hotkeys) {
        if (hk.registered) {
            UnregisterHotKey(nullptr, hk.id);
        }
    }
}

bool sc_update(sc_capture_options& active_options) {
    MSG msg = {};
    if (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_DESTROY || (msg.message == WM_COMMAND && msg.wParam == TRAY_MENU_EXIT)) {
            sc_shutdown();
            return false;
        } if (msg.message == WM_HOTKEY) {
            active_options.extract_text = (msg.wParam == sc_hotkey_ocr);

            if (msg.wParam == sc_hotkey_active_window) {
                active_options.mode = sc_capture_mode::window_under_cursor;
            } else if (msg.wParam == sc_hotkey_current_monitor) {
                active_options.mode = sc_capture_mode::monitor_under_cursor;
            } else {
                active_options.mode = sc_capture_mode::interactive;
            }

            sc_begin_capture(active_options);
        }

        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return true;
}

void sc_begin_capture(sc_capture_options options) {
    if (g_state.capturing) {
        return;
    }
    g_state.capturing = true;
    g_state.options = options;
    g_state.mouseDown = false;
    g_state.dragging = false;
    g_state.wasDragging = false;
    g_state.snapDrag = false;
    g_state.captureReady = false;
    g_state.hoveredHwnd = nullptr;
    g_state.finalHwnd = nullptr;

    int vx, vy, vw, vh;
    _sc_get_display_metrics(vx, vy, vw, vh);
    
    HDC hScreenDC = GetDC(nullptr);
    if (g_state.frozenDC) {
        DeleteDC(g_state.frozenDC);
        DeleteObject(g_state.frozenBitmap);
    }
    g_state.frozenDC = CreateCompatibleDC(hScreenDC);
    g_state.frozenBitmap = CreateCompatibleBitmap(hScreenDC, vw, vh);
    SelectObject(g_state.frozenDC, g_state.frozenBitmap);
    BitBlt(g_state.frozenDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY);
    ReleaseDC(nullptr, hScreenDC);

    POINT pt;
    GetCursorPos(&pt);
    
    if (options.mode == sc_capture_mode::window_under_cursor) {
        if (HWND hwnd = GetAncestor(WindowFromPoint(pt), GA_ROOT)) {
            RECT rect;
            GetRealWindowRect(hwnd, &rect);
            g_state.finalRect = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
            g_state.finalHwnd = hwnd;
            g_state.captureReady = true;
        } else {
            g_state.capturing = false;
        }
        return;
    } else if (options.mode == sc_capture_mode::monitor_under_cursor) {
        if (const auto* m = _sc_monitor_at_point(pt)) {
            g_state.finalRect = m->rect;
            g_state.captureReady = true;
        } else {
            g_state.capturing = false;
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_state.ocrMutex);
        g_state.ocrLines.clear();
    }

    if (options.extract_text) {
        _ocr_run_async();
    }

    if (!g_state.overlayHwnd) {        
        g_state.overlayHwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 
            "ScOverlayWindow", "ScreenshotOverlay", 
            WS_POPUP, 
            vx, vy, vw, vh, 
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr
        );
    } else {
        SetWindowPos(g_state.overlayHwnd, HWND_TOPMOST, vx, vy, vw, vh, SWP_NOACTIVATE);
    }

    UpdateHoverRect(pt);
    ShowWindow(g_state.overlayHwnd, SW_SHOW);
    SetForegroundWindow(g_state.overlayHwnd);
    SetFocus(g_state.overlayHwnd);
}

bool sc_capture_update(sc_capture_info& ci) {
    if (!g_state.captureReady) {
        return false;
    }
    g_state.captureReady = false;
    g_state.capturing = false;
    
    if (g_state.finalRect.width <= 0 || g_state.finalRect.height <= 0 || !g_state.frozenDC) {
        return false;
    }

    int vx, vy, vw, vh;
    _sc_get_display_metrics(vx, vy, vw, vh);
    
    int captureWidth = g_state.finalRect.width;
    int captureHeight = g_state.finalRect.height;
    ci.width = captureWidth;
    ci.height = captureHeight;

    if (g_state.options.extract_text) {
        ci.width *= 2;
        ci.height *= 2;
        while (ci.width < 40 || ci.height < 40) {
            ci.width *= 2;
            ci.height *= 2;
        }
    }
    ci.channels = 4;

    HDC hMemoryDC = CreateCompatibleDC(g_state.frozenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(g_state.frozenDC, ci.width, ci.height);
    SelectObject(hMemoryDC, hBitmap);
    SetStretchBltMode(hMemoryDC, HALFTONE);

    bool windowCaptured = false;
    if (g_state.finalHwnd && !g_state.wasDragging && IsWindow(g_state.finalHwnd)) {
        RECT rWin;
        GetWindowRect(g_state.finalHwnd, &rWin);
        HDC hTempDC = CreateCompatibleDC(g_state.frozenDC);
        HBITMAP hTempBmp = CreateCompatibleBitmap(g_state.frozenDC, rWin.right - rWin.left, rWin.bottom - rWin.top);
        SelectObject(hTempDC, hTempBmp);

        if (PrintWindow(g_state.finalHwnd, hTempDC, PW_RENDERFULLCONTENT)) {
            RECT rDwm;
            GetRealWindowRect(g_state.finalHwnd, &rDwm);
            StretchBlt(
                hMemoryDC, 0, 0, ci.width, ci.height, 
                hTempDC, rDwm.left - rWin.left, rDwm.top - rWin.top, 
                captureWidth, captureHeight, SRCCOPY
            );
            windowCaptured = true;
        }
        DeleteObject(hTempBmp);
        DeleteDC(hTempDC);
    }
    
    if (!windowCaptured) {
        StretchBlt(
            hMemoryDC, 0, 0, ci.width, ci.height, 
            g_state.frozenDC, g_state.finalRect.x - vx, g_state.finalRect.y - vy, 
            captureWidth, captureHeight, SRCCOPY
        );
    }

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (int)ci.width;
    bih.biHeight = -static_cast<int>(ci.height);
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    ci.data = new unsigned char[ci.width * ci.height * 4];
    GetDIBits(hMemoryDC, hBitmap, 0, ci.height, ci.data, (BITMAPINFO*)&bih, DIB_RGB_COLORS);

    if (g_state.options.extract_text) {
        try {
            int pad = 24;
            int pw = (int)ci.width + pad * 2;
            int ph = (int)ci.height + pad * 2;
            std::vector<unsigned char> padded(pw * ph * 4);
            
            for (int i = 0; i < pw * ph; ++i) {
                padded[i * 4 + 0] = ci.data[0];
                padded[i * 4 + 1] = ci.data[1];
                padded[i * 4 + 2] = ci.data[2];
                padded[i * 4 + 3] = 255;
            }
            for (int y = 0; y < (int)ci.height; ++y) {
                memcpy(&padded[((y + pad) * pw + pad) * 4], &ci.data[y * ci.width * 4], ci.width * 4);
            }

            if (OcrResult result = _ocr_get_bitmap_result(padded.data(), pw, ph)) {
                std::wstring text = _ocr_text(result);
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    _sc_write_to_clipboard(CF_UNICODETEXT, text.c_str(), (text.length() + 1) * sizeof(wchar_t));
                    CloseClipboard();
                }
            }
        } catch (std::exception& e) {
            fprintf(stderr, "OCR failed: %s\n", e.what());
        }
    } else if (sc_get_app().opt_copy_to_clipboard && OpenClipboard(nullptr)) {
        EmptyClipboard();
        _sc_write_to_clipboard(CF_DIB, &bih, sizeof(BITMAPINFOHEADER), ci.data, ci.width * ci.height * 4);

        if (!ci.channels_swapped) {
            _sc_swap_channels((uint32_t*)ci.data, ci.width * ci.height);
            ci.channels_swapped = true;
        }
        
        std::vector<unsigned char> pngData;
        stbi_write_png_to_func([](void* ctx, void* d, int s) {
            auto* vec = static_cast<std::vector<unsigned char>*>(ctx);
            auto* bytes = static_cast<unsigned char*>(d);
            vec->insert(vec->end(), bytes, bytes + s);
        }, &pngData, ci.width, ci.height, 4, ci.data, ci.width * 4);
        
        _sc_write_to_clipboard(RegisterClipboardFormatA("PNG"), pngData.data(), pngData.size());
        CloseClipboard();
    }

    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    return true;
}

void _sc_cleanup_impl(sc_capture_info& ci) {
    if (g_state.overlayHwnd) {
        DestroyWindow(g_state.overlayHwnd);
        g_state.overlayHwnd = nullptr;
    }
    if (g_state.frozenDC) {
        DeleteDC(g_state.frozenDC);
        g_state.frozenDC = nullptr;
    }
    if (g_state.frozenBitmap) {
        DeleteObject(g_state.frozenBitmap);
        g_state.frozenBitmap = nullptr;
    }

    g_state.capturing = false;
    g_state.captureReady = false;
    g_state.mouseDown = false;
    g_state.dragging = false;
    g_state.snapDrag = false;
    g_state.wasDragging = false;

    g_state.hoveredHwnd = nullptr;
    g_state.finalHwnd = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_state.ocrMutex);
        g_state.ocrLines.clear();
    }
}
#pragma endregion

#pragma region Windows Impl
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, g_state.options.extract_text ? IDC_IBEAM : IDC_CROSS));
            return TRUE;
            
        case WM_MOUSEMOVE: {
            if (!g_state.capturing) {
                break;
            }
            POINT pt;
            GetCursorPos(&pt);
            
            if (g_state.mouseDown) {
                if (!g_state.dragging && (std::abs(pt.x - g_state.dragStart.x) > 3 || std::abs(pt.y - g_state.dragStart.y) > 3)) {
                    g_state.dragging = true;
                }
                if (g_state.dragging) {
                    sc_rect dragRect = { 
                        std::min(g_state.dragStart.x, pt.x), 
                        std::min(g_state.dragStart.y, pt.y), 
                        std::abs(pt.x - g_state.dragStart.x), 
                        std::abs(pt.y - g_state.dragStart.y) 
                    };
                    sc_rect snapped;
                    if (g_state.snapDrag && _ocr_snap_to_text(dragRect, snapped)) {
                        g_state.currentRect = snapped;
                    } else {
                        g_state.currentRect = dragRect;
                    }
                }
            } else {
                sc_rect textRect;
                if (g_state.options.extract_text && _ocr_text_at_point(pt, textRect)) {
                    g_state.hoveredHwnd = nullptr;
                    g_state.currentRect = textRect;
                } else {
                    UpdateHoverRect(pt);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!g_state.capturing) {
                break;
            }
            POINT pt;
            GetCursorPos(&pt);
            g_state.mouseDown = true;
            g_state.dragStart = pt;
            sc_rect textRect;
            g_state.snapDrag = g_state.options.extract_text && _ocr_text_at_point(pt, textRect);
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP:
            if (!g_state.capturing) {
                break;
            }
            if (g_state.mouseDown) {
                g_state.finalRect = g_state.currentRect;
                g_state.finalHwnd = g_state.hoveredHwnd;
                g_state.wasDragging = g_state.dragging;
                ReleaseCapture();
                ShowWindow(hwnd, SW_HIDE);
                g_state.captureReady = true;
                g_state.mouseDown = false;
                g_state.dragging = false;
            }
            return 0;
            
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (g_state.mouseDown) {
                    ReleaseCapture();
                    g_state.mouseDown = false;
                    g_state.dragging = false;
                    POINT pt;
                    GetCursorPos(&pt);
                    UpdateHoverRect(pt);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    g_state.capturing = false;
                    ShowWindow(hwnd, SW_HIDE);
                }
            }
            return 0;
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr;
            GetClientRect(hwnd, &cr);
            
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            HBITMAP hOldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
            
            if (g_state.frozenDC) {
                BitBlt(memDC, 0, 0, cr.right, cr.bottom, g_state.frozenDC, 0, 0, SRCCOPY);
            }

            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            
            RECT r = { 
                g_state.currentRect.x - vx, 
                g_state.currentRect.y - vy, 
                g_state.currentRect.x - vx + g_state.currentRect.width, 
                g_state.currentRect.y - vy + g_state.currentRect.height 
            };

            HDC hAlphaDC = CreateCompatibleDC(memDC);
            HBITMAP hAlphaBmp = CreateCompatibleBitmap(memDC, 1, 1);
            HBITMAP hOldAlphaBmp = (HBITMAP)SelectObject(hAlphaDC, hAlphaBmp);
            
            SetPixel(hAlphaDC, 0, 0, RGB(155, 155, 215));
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 32, 0 };
            GdiAlphaBlend(memDC, r.left, r.top, r.right - r.left, r.bottom - r.top, hAlphaDC, 0, 0, 1, 1, bf);
            
            SelectObject(hAlphaDC, hOldAlphaBmp);
            DeleteObject(hAlphaBmp);
            DeleteDC(hAlphaDC);

            // Dotted rectangle
            HPEN hPen = CreatePen(PS_DOT, 1, RGB(156, 215, 228));
            HPEN hOldPen = (HPEN)SelectObject(memDC, hPen);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
            
            SetBkMode(memDC, TRANSPARENT);
            Rectangle(memDC, r.left, r.top, r.right, r.bottom);
            
            SelectObject(memDC, hOldBrush);
            SelectObject(memDC, hOldPen);
            DeleteObject(hPen);

            // Size text
            if (g_state.currentRect.width > 0 && g_state.currentRect.height > 0) {
                HFONT hOldFont = (HFONT)SelectObject(memDC, GetStockObject(DEFAULT_GUI_FONT));
                
                char sizeText[64];
                snprintf(sizeText, sizeof(sizeText), "%d x %d", g_state.currentRect.width, g_state.currentRect.height);
                SIZE textSize;
                GetTextExtentPoint32A(memDC, sizeText, (int)strlen(sizeText), &textSize);
                
                int textX = r.left;
                int textY = r.top - textSize.cy - 4;
                if (textY < 0) {
                    textY = r.top + 4;
                }
                
                RECT textBg = { textX, textY, textX + textSize.cx + 6, textY + textSize.cy + 4 };
                HBRUSH bgBrushText = (HBRUSH)GetStockObject(BLACK_BRUSH);
                
                FillRect(memDC, &textBg, bgBrushText);
                DeleteObject(bgBrushText);
                
                SetTextColor(memDC, RGB(255, 255, 255));
                TextOutA(memDC, textX + 3, textY + 2, sizeText, (int)strlen(sizeText));
                
                SelectObject(memDC, hOldFont);
            }

            // Magnifier
            POINT pt;
            GetCursorPos(&pt);
            int magSize = 120;
            int zoom = 4;
            int srcSize = magSize / zoom;
            
            int destX = pt.x - vx + 20;
            if (destX + magSize > vw) {
                destX = pt.x - vx - magSize - 20;
            }
            int destY = pt.y - vy + 20;
            if (destY + magSize > vh) {
                destY = pt.y - vy - magSize - 20;
            }
            
            StretchBlt(memDC, destX, destY, magSize, magSize, g_state.frozenDC, pt.x - srcSize / 2 - vx, pt.y - srcSize / 2 - vy, srcSize, srcSize, SRCCOPY);

            HPEN magPen = CreatePen(PS_SOLID, 1, RGB(156, 215, 228));
            HPEN oldMagPen = (HPEN)SelectObject(memDC, magPen);
            
            MoveToEx(memDC, destX + magSize / 2, destY, nullptr);
            LineTo(memDC, destX + magSize / 2, destY + magSize);
            MoveToEx(memDC, destX, destY + magSize / 2, nullptr);
            LineTo(memDC, destX + magSize, destY + magSize / 2);
            
            HBRUSH hOldMagBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, destX, destY, destX + magSize, destY + magSize);
            
            SelectObject(memDC, hOldMagBrush);
            SelectObject(memDC, oldMagPen);
            DeleteObject(magPen);

            BitBlt(hdc, 0, 0, cr.right, cr.bottom, memDC, 0, 0, SRCCOPY);
            
            SelectObject(memDC, hOldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

enum SettingsTab {
    TAB_GENERAL,
    TAB_INPUT
};

void OpenFolderPickerDialog(HWND hwnd, HWND hEditPath) {
    std::thread([](HWND hwndParent, HWND hEdit) {
        winrt::init_apartment();
        IFileOpenDialog* pFolderDlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFolderDlg)))) {
            DWORD options;
            pFolderDlg->GetOptions(&options);
            pFolderDlg->SetOptions(options | FOS_PICKFOLDERS);

            if (SUCCEEDED(pFolderDlg->Show(hwndParent))) {
                IShellItem* pItem = nullptr;
                if (SUCCEEDED(pFolderDlg->GetResult(&pItem))) {
                    PWSTR pszPath = nullptr;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                        SetWindowTextW(hEdit, pszPath);
                        sc_get_app().save_path = winrt::to_string(pszPath);
                        CoTaskMemFree(pszPath);
                    }
                    pItem->Release();
                }
            }
            pFolderDlg->Release();
        }
    }, hwnd, hEditPath).detach();
}

LRESULT CALLBACK BrowseButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    static bool isHovered = false;

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!isHovered) {
                isHovered = true;
                InvalidateRect(hwnd, nullptr, FALSE);

                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT };
                TrackMouseEvent(&tme);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            isHovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static std::array<Win32Toggle, 1> toggles;
    static SettingsTab activeTab = TAB_GENERAL;
    
    static RECT tabGeneralRect = { 10, 20, 140, 50 };
    static RECT tabInputRect   = { 10, 55, 140, 85 };
    static bool hoverGeneral   = false;
    static bool hoverInput     = false;

    static HWND hEditPath = nullptr;
    static HWND hBtnBrowse = nullptr;
    static RECT pathLabelRect = { 160, 20, 500, 40 };

    static HFONT hFont = nullptr;
    static HFONT hBoldFont = nullptr;
    static HBRUSH hBgBrush = nullptr;
    static HBRUSH hSidebarBrush = nullptr;
    static HBRUSH hRowNormalBrush = nullptr;
    static HBRUSH hRowHoverBrush = nullptr;
    static HBRUSH hPillOnBrush = nullptr;
    static HBRUSH hPillOffBrush = nullptr;
    static HBRUSH hThumbBrush = nullptr;
    static HBRUSH hGreenBrush = nullptr;
    static HBRUSH hRedBrush = nullptr;

    switch (msg) {
        case WM_CREATE: {
            toggles[0] = { { 160, 110, 0, 150 }, "Copy screenshot directly to Clipboard", &sc_get_app().opt_copy_to_clipboard, false };

            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

            hFont = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            hBoldFont = CreateFontA(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            
            hBgBrush = CreateSolidBrush(RGB(28, 28, 28));
            hSidebarBrush = CreateSolidBrush(RGB(32, 32, 32));
            hRowNormalBrush = CreateSolidBrush(RGB(38, 38, 38));
            hRowHoverBrush = CreateSolidBrush(RGB(45, 45, 45));
            hPillOnBrush = CreateSolidBrush(RGB(0, 120, 215));
            hPillOffBrush = CreateSolidBrush(RGB(100, 100, 100));
            hThumbBrush = CreateSolidBrush(RGB(255, 255, 255));
            hGreenBrush = CreateSolidBrush(RGB(34, 139, 34));
            hRedBrush = CreateSolidBrush(RGB(178, 34, 34));

            hEditPath = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 160, 45, 260, 24, hwnd, (HMENU)3001, GetModuleHandle(nullptr), nullptr);
            SendMessageW(hEditPath, WM_SETFONT, (WPARAM)hFont, TRUE);

            std::wstring widePath = winrt::to_hstring(sc_get_app().save_path).c_str();
            SetWindowTextW(hEditPath, widePath.c_str());

            hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 430, 45, 80, 24, hwnd, (HMENU)3002, GetModuleHandle(nullptr), nullptr);
            SendMessageW(hBtnBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

            SetWindowTheme(hEditPath, L"Explorer", nullptr);
            
            SetWindowSubclass(hBtnBrowse, BrowseButtonSubclassProc, 0, 0);

            return 0;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
            if (pDIS->CtlID == 3002) {
                bool isPressed = (pDIS->itemState & ODS_SELECTED);
                bool isFocused = (pDIS->itemState & ODS_FOCUS);
                
                POINT mousePt;
                GetCursorPos(&mousePt);
                ScreenToClient(hBtnBrowse, &mousePt);
                bool isHovered = PtInRect(&pDIS->rcItem, mousePt);

                HBRUSH btnBrush = nullptr;
                if (isPressed) {
                    btnBrush = CreateSolidBrush(RGB(55, 55, 55));
                } else if (isHovered) {
                    btnBrush = CreateSolidBrush(RGB(48, 48, 48));
                } else {
                    btnBrush = CreateSolidBrush(RGB(38, 38, 38));
                }

                FillRect(pDIS->hDC, &pDIS->rcItem, btnBrush);
                DeleteObject(btnBrush);

                HPEN borderPen = CreatePen(PS_SOLID, 1, (isFocused || isHovered) ? RGB(0, 120, 215) : RGB(55, 55, 55));
                HPEN oldPen = (HPEN)SelectObject(pDIS->hDC, borderPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(pDIS->hDC, GetStockObject(NULL_BRUSH));
                
                Rectangle(pDIS->hDC, pDIS->rcItem.left, pDIS->rcItem.top, pDIS->rcItem.right, pDIS->rcItem.bottom);
                
                SelectObject(pDIS->hDC, oldBrush);
                SelectObject(pDIS->hDC, oldPen);
                DeleteObject(borderPen);

                SetBkMode(pDIS->hDC, TRANSPARENT);
                SetTextColor(pDIS->hDC, RGB(240, 240, 240));
                HFONT oldFont = (HFONT)SelectObject(pDIS->hDC, hFont);

                DrawTextW(pDIS->hDC, L"Browse...", -1, &pDIS->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(pDIS->hDC, oldFont);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hwndChild = (HWND)lParam;
            if (hwndChild == hEditPath) {
                SetTextColor(hdcStatic, RGB(240, 240, 240));
                SetBkColor(hdcStatic, RGB(45, 45, 45));
                static HBRUSH hEditBg = CreateSolidBrush(RGB(45, 45, 45));
                return (INT_PTR)hEditBg;
            }
            break;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            for (int i = 0; i < toggles.size(); ++i) {
                toggles[i].rect.right = width - 20; 
            }
            if (hEditPath && hBtnBrowse) {
                MoveWindow(hEditPath, 160, 45, width - 260, 24, TRUE);
                MoveWindow(hBtnBrowse, width - 90, 45, 70, 24, TRUE);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == 3001) {
                int len = GetWindowTextLengthW(hEditPath);
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(hEditPath, buf.data(), len + 1);
                sc_get_app().save_path = winrt::to_string(buf.data());
            }
            
            if (LOWORD(wParam) == 3002) {
                OpenFolderPickerDialog(hwnd, hEditPath);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr;
            GetClientRect(hwnd, &cr);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            FillRect(memDC, &cr, hBgBrush);

            RECT sidebarRect = { 0, 0, 150, cr.bottom };
            FillRect(memDC, &sidebarRect, hSidebarBrush);

            SetBkMode(memDC, TRANSPARENT);

            bool drawGenHover = hoverGeneral || (activeTab == TAB_GENERAL);
            HBRUSH hGenTabBrush = CreateSolidBrush(drawGenHover ? RGB(45, 45, 45) : RGB(32, 32, 32));
            FillRect(memDC, &tabGeneralRect, hGenTabBrush);
            DeleteObject(hGenTabBrush);
            SelectObject(memDC, activeTab == TAB_GENERAL ? hBoldFont : hFont);
            SetTextColor(memDC, activeTab == TAB_GENERAL ? RGB(255, 255, 255) : RGB(200, 200, 200));
            TextOutA(memDC, tabGeneralRect.left + 15, tabGeneralRect.top + 7, "General", 7);

            bool drawInputHover = hoverInput || (activeTab == TAB_INPUT);
            HBRUSH hInputTabBrush = CreateSolidBrush(drawInputHover ? RGB(45, 45, 45) : RGB(32, 32, 32));
            FillRect(memDC, &tabInputRect, hInputTabBrush);
            DeleteObject(hInputTabBrush);
            SelectObject(memDC, activeTab == TAB_INPUT ? hBoldFont : hFont);
            SetTextColor(memDC, activeTab == TAB_INPUT ? RGB(255, 255, 255) : RGB(200, 200, 200));
            TextOutA(memDC, tabInputRect.left + 15, tabInputRect.top + 7, "Input", 5);

            SelectObject(memDC, hFont);
            SetTextColor(memDC, RGB(240, 240, 240));

            if (activeTab == TAB_GENERAL) {
                ShowWindow(hEditPath, SW_SHOW);
                ShowWindow(hBtnBrowse, SW_SHOW);

                TextOutA(memDC, pathLabelRect.left, pathLabelRect.top, "Screenshot Destination Folder Path:", 35);

                for (int i = 0; i < toggles.size(); ++i) {
                    FillRect(memDC, &toggles[i].rect, toggles[i].isHovered ? hRowHoverBrush : hRowNormalBrush);
                    TextOutA(memDC, toggles[i].rect.left + 15, toggles[i].rect.top + 10, toggles[i].text, (int)strlen(toggles[i].text));

                    bool isOn = *toggles[i].targetValue;
                    int pillLeft = toggles[i].rect.right - 55;
                    int pillTop = toggles[i].rect.top + 9;
                    RECT pillRect = { pillLeft, pillTop, pillLeft + 40, pillTop + 20 };

                    FillRect(memDC, &pillRect, isOn ? hPillOnBrush : hPillOffBrush);

                    RECT thumbRect = { 
                        isOn ? pillLeft + 22 : pillLeft + 3, 
                        pillTop + 3, 
                        isOn ? pillLeft + 37 : pillLeft + 18, 
                        pillTop + 17 
                    };
                    FillRect(memDC, &thumbRect, hThumbBrush);
                }
            } else if (activeTab == TAB_INPUT) {
                ShowWindow(hEditPath, SW_HIDE);
                ShowWindow(hBtnBrowse, SW_HIDE);

                int startY = 20;
                for (size_t i = 0; i < sc_get_app().hotkeys.size(); ++i) {
                    const auto& hk = sc_get_app().hotkeys[i];
                    RECT rowRect = { 160, startY + (int)i * 35, cr.right - 20, startY + (int)i * 35 + 30 };
                    
                    FillRect(memDC, &rowRect, hRowNormalBrush);

                    RECT indicatorRect = { rowRect.left + 10, rowRect.top + 10, rowRect.left + 20, rowRect.top + 20 };
                    FillRect(memDC, &indicatorRect, hk.registered ? hGreenBrush : hRedBrush);

                    const char* idName = sc_hotkey_id_strings[hk.id];
                    TextOutA(memDC, rowRect.left + 30, rowRect.top + 6, idName, (int)strlen(idName));

                    std::string bindStr = _sc_get_key_display_string(hk.modifiers, hk.key);
                    int textWidth = 0;
                    SIZE textSize;
                    if (GetTextExtentPoint32A(memDC, bindStr.c_str(), (int)bindStr.length(), &textSize)) {
                        textWidth = textSize.cx;
                    }
                    TextOutA(memDC, rowRect.right - textWidth - 15, rowRect.top + 6, bindStr.c_str(), (int)bindStr.length());
                }
            }

            BitBlt(hdc, 0, 0, cr.right, cr.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            bool trackingChanged = false;

            bool insideGen = PtInRect(&tabGeneralRect, pt);
            if (insideGen != hoverGeneral) {
                hoverGeneral = insideGen;
                trackingChanged = true;
            }

            bool insideInput = PtInRect(&tabInputRect, pt);
            if (insideInput != hoverInput) {
                hoverInput = insideInput;
                trackingChanged = true;
            }

            if (activeTab == TAB_GENERAL) {
                for (int i = 0; i < toggles.size(); ++i) {
                    bool currentlyInside = PtInRect(&toggles[i].rect, pt);
                    if (currentlyInside != toggles[i].isHovered) {
                        toggles[i].isHovered = currentlyInside;
                        trackingChanged = true;
                    }
                }
            }

            if (trackingChanged) {
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT };
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            hoverGeneral = false;
            hoverInput = false;
            for (int i = 0; i < toggles.size(); ++i) toggles[i].isHovered = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };

            if (PtInRect(&tabGeneralRect, pt)) {
                if (activeTab != TAB_GENERAL) {
                    activeTab = TAB_GENERAL;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }

            if (PtInRect(&tabInputRect, pt)) {
                if (activeTab != TAB_INPUT) {
                    activeTab = TAB_INPUT;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }

            if (activeTab == TAB_GENERAL) {
                for (int i = 0; i < toggles.size(); ++i) {
                    if (PtInRect(&toggles[i].rect, pt)) {
                        *toggles[i].targetValue = !(*toggles[i].targetValue);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_DESTROY: {
            DeleteObject(hFont);
            DeleteObject(hBoldFont);
            DeleteObject(hBgBrush);
            DeleteObject(hSidebarBrush);
            DeleteObject(hRowNormalBrush);
            DeleteObject(hRowHoverBrush);
            DeleteObject(hPillOnBrush);
            DeleteObject(hPillOffBrush);
            DeleteObject(hThumbBrush);
            DeleteObject(hGreenBrush);
            DeleteObject(hRedBrush);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TrayUtilityWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON: {
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);

                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, TRAY_MENU_SETTINGS, "Settings...");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuA(hMenu, MF_STRING, TRAY_MENU_EXIT, "Exit");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
            }
            return 0;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == TRAY_MENU_SETTINGS) {
                static bool classRegistered = false;
                if (!classRegistered) {
                    WNDCLASSW wcc = {};
                    wcc.lpfnWndProc = SettingsWndProc;
                    wcc.hInstance = GetModuleHandle(nullptr);
                    wcc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                    wcc.lpszClassName = L"ScCustomSettingsWindow";
                    RegisterClassW(&wcc);
                    classRegistered = true;
                }

                HWND hSettings = CreateWindowExW(
                    0, L"ScCustomSettingsWindow", L"Screencap Options",
                    WS_OVERLAPPEDWINDOW | WS_THICKFRAME,
                    CW_USEDEFAULT, CW_USEDEFAULT, 550, 260,
                    nullptr, nullptr, GetModuleHandle(nullptr), nullptr
                );

                ShowWindow(hSettings, SW_SHOW);
                UpdateWindow(hSettings);
            } 
            else if (wmId == TRAY_MENU_EXIT) {
                NOTIFYICONDATAA nid = {};
                nid.cbSize = sizeof(NOTIFYICONDATAA);
                nid.hWnd = hwnd;
                nid.uID = 1;
                Shell_NotifyIconA(NIM_DELETE, &nid);
                PostQuitMessage(0);
            }
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam) {
    FindWindowData* data = reinterpret_cast<FindWindowData*>(lParam);
    if (hwnd == data->overlayHwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    RECT r;
    GetRealWindowRect(hwnd, &r);
    if (PtInRect(&r, data->pt)) {
        data->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

void UpdateHoverRect(POINT pt) {
    FindWindowData data = { pt, nullptr, g_state.overlayHwnd };
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&data));
    
    char className[256] = { 0 };
    if (data.result) {
        GetClassNameA(data.result, className, 256);
    }

    if (!data.result || strcmp(className, "Progman") == 0 || strcmp(className, "WorkerW") == 0) {
        g_state.hoveredHwnd = nullptr;
        if (const auto* m = _sc_monitor_at_point(pt)) {
            g_state.currentRect = m->rect;
        }
    } else {
        g_state.hoveredHwnd = data.result;
        RECT r;
        GetRealWindowRect(data.result, &r);
        g_state.currentRect = { r.left, r.top, r.right - r.left, r.bottom - r.top };
    }
}

void RegisterTrayIcon(HWND hwnd) {
    NOTIFYICONDATAA nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    strcpy_s(nid.szTip, "Screencap Utility");

    Shell_NotifyIconA(NIM_ADD, &nid);
}
#pragma endregion

#pragma region OCR Impl
using DataWriter = winrt::Windows::Storage::Streams::DataWriter;
using IBuffer = winrt::Windows::Storage::Streams::IBuffer;
using SoftwareBitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using OcrEngine = winrt::Windows::Media::Ocr::OcrEngine;
using BitmapPixelFormat = winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using BitmapAlphaMode = winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using OcrResult = winrt::Windows::Media::Ocr::OcrResult;

sc_internal OcrResult _ocr_get_bitmap_result(const unsigned char* data, int w, int h) {
    DataWriter writer;
    writer.WriteBytes(winrt::array_view<const uint8_t>(data, w * h * 4));
    
    SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
        writer.DetachBuffer(), 
        BitmapPixelFormat::Bgra8, 
        w, h, 
        BitmapAlphaMode::Ignore
    );
    
    OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
    return engine ? engine.RecognizeAsync(bitmap).get() : nullptr;
}

sc_internal void _ocr_run_async() {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int scale = 2;

    if (OcrEngine probe = OcrEngine::TryCreateFromUserProfileLanguages()) {
        uint32_t maxDim = probe.MaxImageDimension();
        while (scale > 1 && ((uint32_t)(vw * scale) > maxDim || (uint32_t)(vh * scale) > maxDim)) {
            --scale;
        }
    }

    int sw = vw * scale;
    int sh = vh * scale;
    
    HDC hScaledDC = CreateCompatibleDC(g_state.frozenDC);
    HBITMAP hScaledBmp = CreateCompatibleBitmap(g_state.frozenDC, sw, sh);
    SelectObject(hScaledDC, hScaledBmp);
    SetStretchBltMode(hScaledDC, HALFTONE);
    StretchBlt(hScaledDC, 0, 0, sw, sh, g_state.frozenDC, 0, 0, vw, vh, SRCCOPY);

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = sw;
    bih.biHeight = -sh;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    auto bits = std::make_shared<std::vector<unsigned char>>(sw * sh * 4);
    GetDIBits(hScaledDC, hScaledBmp, 0, sh, bits->data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);
    
    DeleteObject(hScaledBmp);
    DeleteDC(hScaledDC);

    std::thread([bits, sw, sh, scale, vx, vy]() {
        winrt::init_apartment();
        try {
            if (OcrResult result = _ocr_get_bitmap_result(bits->data(), sw, sh)) {
                std::vector<sc_ocr_line> lines;
                
                for (auto const& line : result.Lines()) {
                    sc_ocr_line ol = {};
                    bool first = true;
                    int minx = 0, miny = 0, maxx = 0, maxy = 0;
                    
                    for (auto const& word : line.Words()) {
                        auto wr = word.BoundingRect();
                        sc_rect box = { 
                            (int)(wr.X / scale) + vx, 
                            (int)(wr.Y / scale) + vy, 
                            (int)(wr.Width / scale), 
                            (int)(wr.Height / scale) 
                        };
                        
                        int n = std::max(1, (int)word.Text().size());
                        for (int c = 0; c < n; ++c) {
                            int x0 = box.x + box.width * c / n;
                            int width_val = (box.x + box.width * (c + 1) / n) - x0;
                            ol.chars.push_back({ x0, box.y, width_val, box.height });
                        }
                        
                        if (first) {
                            minx = box.x;
                            miny = box.y;
                            maxx = box.x + box.width;
                            maxy = box.y + box.height;
                            first = false;
                        } else {
                            minx = std::min(minx, box.x);
                            miny = std::min(miny, box.y);
                            maxx = std::max(maxx, box.x + box.width);
                            maxy = std::max(maxy, box.y + box.height);
                        }
                    }
                    
                    if (!first) {
                        ol.rect = { minx, miny, maxx - minx, maxy - miny };
                        lines.push_back(ol);
                    }
                }
                
                std::lock_guard<std::mutex> lock(g_state.ocrMutex);
                g_state.ocrLines = lines;
            }
        } catch (std::exception& e) {
            fprintf(stderr, "OCR failed: %s\n", e.what());
        }
    }).detach();
}

sc_internal bool _ocr_snap_to_text(const sc_rect& drag, sc_rect& out) {
    std::lock_guard<std::mutex> lock(g_state.ocrMutex);
    bool found = false;
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    
    for (const auto& line : g_state.ocrLines) {
        for (const auto& w : line.chars) {
            if (!_sc_rects_intersect(drag, w)) {
                continue;
            }
            if (!found) {
                minx = w.x;
                miny = w.y;
                maxx = w.x + w.width;
                maxy = w.y + w.height;
                found = true;
            } else {
                minx = std::min(minx, w.x);
                miny = std::min(miny, w.y);
                maxx = std::max(maxx, w.x + w.width);
                maxy = std::max(maxy, w.y + w.height);
            }
        }
    }
    
    if (found) {
        out = { minx, miny, maxx - minx, maxy - miny };
    }
    return found;
}

sc_internal bool _ocr_text_at_point(POINT pt, sc_rect& out) {
    std::lock_guard<std::mutex> lock(g_state.ocrMutex);
    for (const auto& line : g_state.ocrLines) {
        if (pt.x >= line.rect.x && pt.x < line.rect.x + line.rect.width && 
            pt.y >= line.rect.y && pt.y < line.rect.y + line.rect.height) {
            out = line.rect;
            return true;
        }
    }
    return false;
}

sc_internal std::wstring _ocr_text(OcrResult const& result) {
    struct Frag { 
        int cy;
        int height;
        int left;
        std::wstring text;
    };
    
    std::vector<Frag> frags;
    for (auto const& line : result.Lines()) {
        int top = 0, bottom = 0, left = 0;
        bool first = true;
        
        for (auto const& word : line.Words()) {
            auto r = word.BoundingRect();
            int t = (int)r.Y;
            int b = (int)(r.Y + r.Height);
            int l = (int)r.X;
            
            if (first) {
                top = t;
                bottom = b;
                left = l;
                first = false;
            } else {
                top = std::min(top, t);
                bottom = std::max(bottom, b);
                left = std::min(left, l);
            }
        }
        if (!first) {
            frags.push_back({ (top + bottom) / 2, bottom - top, left, std::wstring(line.Text().c_str()) });
        }
    }
    
    std::sort(frags.begin(), frags.end(), [](const Frag& a, const Frag& b) {
        return a.cy < b.cy;
    });

    std::wstring text;
    size_t i = 0;
    while (i < frags.size()) {
        size_t j = i + 1;
        while (j < frags.size() && std::abs(frags[j].cy - frags[i].cy) < frags[i].height / 2) {
            ++j;
        }
        std::sort(frags.begin() + i, frags.begin() + j, [](const Frag& a, const Frag& b) {
            return a.left < b.left;
        });
        if (!text.empty()) {
            text += L"\r\n";
        }
        for (size_t k = i; k < j; ++k) {
            if (k > i) {
                text += L" ";
            }
            text += frags[k].text;
        }
        i = j;
    }
    return text;
}
#pragma endregion