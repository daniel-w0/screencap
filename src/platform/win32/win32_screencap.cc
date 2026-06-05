#include "pch.h"
#include "platform/platform_screencap.h"
#include "screencap.h"

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

sc_internal OcrResult _ocr_get_bitmap_result(const unsigned char* data, int w, int h);
sc_internal void _ocr_run_async();
sc_internal bool _sc_rects_intersect(const sc_rect& a, const sc_rect& b);
sc_internal bool _ocr_snap_to_text(const sc_rect& drag, sc_rect& out);
sc_internal bool _ocr_text_at_point(POINT pt, sc_rect& out);
sc_internal std::wstring _ocr_text(OcrResult const& result);
#pragma endregion

#pragma region Windows Decl
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam);
void UpdateHoverRect(POINT pt);
#pragma endregion

#pragma region Structures
struct {
    std::vector<sc_monitor_info> monitors;
    sc_capture_options options = { 0 };
    HWND overlayHwnd = nullptr;
    bool capturing = false;
    bool mouseDown = false;
    bool dragging = false;
    bool snapDrag = false;
    bool wasDragging = false;
    POINT dragStart = { 0 };
    sc_rect currentRect = { 0 };
    bool captureReady = false;
    sc_rect finalRect = { 0 };
    HWND hoveredHwnd = nullptr;
    HWND finalHwnd = nullptr;
    HDC frozenDC = nullptr;
    HBITMAP frozenBitmap = nullptr;
    std::vector<sc_ocr_line> ocrLines;
    std::mutex ocrMutex;
} state;

struct FindWindowData {
    POINT pt;
    HWND result;
    HWND overlayHwnd;
};
#pragma endregion

#pragma region Utils
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

sc_internal void _sc_swap_channels(uint32_t* pixels, int totalPixels) {
    for (int i = 0; i < totalPixels; ++i) {
        uint32_t p = pixels[i];
        pixels[i] = 0xFF000000 | (p & 0x0000FF00) | ((p & 0x00FF0000) >> 16) | ((p & 0x000000FF) << 16);
    }
}


sc_internal const sc_monitor_info* _sc_monitor_at_point(POINT pt) {
    for (const auto& m : state.monitors) {
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

void sc_initialize() {
    winrt::init_apartment();
    SetProcessDPIAware();
    _sc_get_monitors(state.monitors);
}

void sc_begin_capture(sc_capture_options options) {
    if (state.capturing) {
        return;
    }
    state.capturing = true;
    state.options = options;
    state.mouseDown = false;
    state.dragging = false;
    state.wasDragging = false;
    state.snapDrag = false;
    state.captureReady = false;
    state.hoveredHwnd = nullptr;
    state.finalHwnd = nullptr;

    int vx, vy, vw, vh;
    _sc_get_display_metrics(vx, vy, vw, vh);
    
    HDC hScreenDC = GetDC(nullptr);
    if (state.frozenDC) {
        DeleteDC(state.frozenDC);
        DeleteObject(state.frozenBitmap);
    }
    state.frozenDC = CreateCompatibleDC(hScreenDC);
    state.frozenBitmap = CreateCompatibleBitmap(hScreenDC, vw, vh);
    SelectObject(state.frozenDC, state.frozenBitmap);
    BitBlt(state.frozenDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY);
    ReleaseDC(nullptr, hScreenDC);

    POINT pt;
    GetCursorPos(&pt);
    
    if (options.mode == sc_capture_mode::window_under_cursor) {
        if (HWND hwnd = GetAncestor(WindowFromPoint(pt), GA_ROOT)) {
            RECT rect;
            GetRealWindowRect(hwnd, &rect);
            state.finalRect = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
            state.finalHwnd = hwnd;
            state.captureReady = true;
        } else {
            state.capturing = false;
        }
        return;
    } else if (options.mode == sc_capture_mode::monitor_under_cursor) {
        if (const auto* m = _sc_monitor_at_point(pt)) {
            state.finalRect = m->rect;
            state.captureReady = true;
        } else {
            state.capturing = false;
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state.ocrMutex);
        state.ocrLines.clear();
    }

    if (options.extract_text) {
        _ocr_run_async();
    }

    if (!state.overlayHwnd) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wc.lpszClassName = "ScOverlayWindow";
        RegisterClassA(&wc);
        
        state.overlayHwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 
            "ScOverlayWindow", "ScreenshotOverlay", 
            WS_POPUP, 
            vx, vy, vw, vh, 
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr
        );
    } else {
        SetWindowPos(state.overlayHwnd, HWND_TOPMOST, vx, vy, vw, vh, SWP_NOACTIVATE);
    }

    UpdateHoverRect(pt);
    ShowWindow(state.overlayHwnd, SW_SHOW);
    SetForegroundWindow(state.overlayHwnd);
    SetFocus(state.overlayHwnd);
}

bool sc_capture_update(sc_capture_info& ci) {
    if (!state.captureReady) {
        return false;
    }
    state.captureReady = false;
    state.capturing = false;
    
    if (state.finalRect.width <= 0 || state.finalRect.height <= 0 || !state.frozenDC) {
        return false;
    }

    int vx, vy, vw, vh;
    _sc_get_display_metrics(vx, vy, vw, vh);
    
    int captureWidth = state.finalRect.width;
    int captureHeight = state.finalRect.height;
    ci.width = captureWidth;
    ci.height = captureHeight;

    if (state.options.extract_text) {
        ci.width *= 2;
        ci.height *= 2;
        while (ci.width < 40 || ci.height < 40) {
            ci.width *= 2;
            ci.height *= 2;
        }
    }
    ci.channels = 4;

    HDC hMemoryDC = CreateCompatibleDC(state.frozenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(state.frozenDC, ci.width, ci.height);
    SelectObject(hMemoryDC, hBitmap);
    SetStretchBltMode(hMemoryDC, HALFTONE);

    bool windowCaptured = false;
    if (state.finalHwnd && !state.wasDragging && IsWindow(state.finalHwnd)) {
        RECT rWin;
        GetWindowRect(state.finalHwnd, &rWin);
        HDC hTempDC = CreateCompatibleDC(state.frozenDC);
        HBITMAP hTempBmp = CreateCompatibleBitmap(state.frozenDC, rWin.right - rWin.left, rWin.bottom - rWin.top);
        SelectObject(hTempDC, hTempBmp);

        if (PrintWindow(state.finalHwnd, hTempDC, PW_RENDERFULLCONTENT)) {
            RECT rDwm;
            GetRealWindowRect(state.finalHwnd, &rDwm);
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
            state.frozenDC, state.finalRect.x - vx, state.finalRect.y - vy, 
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

    if (state.options.extract_text) {
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
    } else if (state.options.copy_to_clipboard && OpenClipboard(nullptr)) {
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

bool sc_capture_region(sc_rect rect, sc_capture_info& ci) {
    HDC hScreenDC = GetDC(nullptr);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    if (!hMemoryDC) {
        ReleaseDC(nullptr, hScreenDC);
        return false;
    }
    
    ci.width = rect.width;
    ci.height = rect.height;
    ci.channels = 4;

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, ci.width, ci.height);
    SelectObject(hMemoryDC, hBitmap);
    BitBlt(hMemoryDC, 0, 0, ci.width, ci.height, hScreenDC, rect.x, rect.y, SRCCOPY);

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = (int)ci.width;
    bih.biHeight = -static_cast<int>(ci.height);
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    ci.data = new unsigned char[ci.width * ci.height * 4];
    GetDIBits(hMemoryDC, hBitmap, 0, ci.height, ci.data, (BITMAPINFO*)&bih, DIB_RGB_COLORS);

    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(nullptr, hScreenDC);
    return true;
}

bool sc_capture_window(int pid, sc_capture_info& ci) {
    if (pid == -1) {
        POINT pt;
        GetCursorPos(&pt);
        if (HWND hwnd = WindowFromPoint(pt)) {
            RECT rect;
            GetRealWindowRect(hwnd, &rect);
            sc_rect cRect = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
            return sc_capture_region(cRect, ci);
        }
    }
    
    struct EnumData { 
        int pid; 
        sc_capture_info& ci; 
    } data = { pid, ci };
    
    return EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* d = reinterpret_cast<EnumData*>(lp);
        DWORD wPid;
        GetWindowThreadProcessId(hwnd, &wPid);
        
        if (wPid == static_cast<DWORD>(d->pid)) {
            RECT r;
            GetRealWindowRect(hwnd, &r);
            sc_rect cRect = { r.left, r.top, r.right - r.left, r.bottom - r.top };
            return !sc_capture_region(cRect, d->ci);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
}

bool sc_capture_desktop(int8_t desktop, sc_capture_info& ci) {
    if (desktop == -1) {
        POINT pt;
        GetCursorPos(&pt);
        if (const auto* m = _sc_monitor_at_point(pt)) {
            desktop = m->id;
        }
    }
    if (desktop >= state.monitors.size()) {
        return false;
    }
    return sc_capture_region(state.monitors[desktop].rect, ci);
}

bool sc_save_capture(const char* filename, sc_capture_info& ci) {
    if (!ci.channels_swapped) {
        _sc_swap_channels((uint32_t*)ci.data, ci.width * ci.height);
        ci.channels_swapped = true;
    }
    stbi_write_png(filename, ci.width, ci.height, ci.channels, ci.data, ci.width * ci.channels);
    return true;
}
#pragma endregion

#pragma region Windows Impl
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, state.options.extract_text ? IDC_IBEAM : IDC_CROSS));
            return TRUE;
            
        case WM_MOUSEMOVE: {
            if (!state.capturing) {
                break;
            }
            POINT pt;
            GetCursorPos(&pt);
            
            if (state.mouseDown) {
                if (!state.dragging && (std::abs(pt.x - state.dragStart.x) > 3 || std::abs(pt.y - state.dragStart.y) > 3)) {
                    state.dragging = true;
                }
                if (state.dragging) {
                    sc_rect dragRect = { 
                        std::min(state.dragStart.x, pt.x), 
                        std::min(state.dragStart.y, pt.y), 
                        std::abs(pt.x - state.dragStart.x), 
                        std::abs(pt.y - state.dragStart.y) 
                    };
                    sc_rect snapped;
                    if (state.snapDrag && _ocr_snap_to_text(dragRect, snapped)) {
                        state.currentRect = snapped;
                    } else {
                        state.currentRect = dragRect;
                    }
                }
            } else {
                sc_rect textRect;
                if (state.options.extract_text && _ocr_text_at_point(pt, textRect)) {
                    state.hoveredHwnd = nullptr;
                    state.currentRect = textRect;
                } else {
                    UpdateHoverRect(pt);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!state.capturing) {
                break;
            }
            POINT pt;
            GetCursorPos(&pt);
            state.mouseDown = true;
            state.dragStart = pt;
            sc_rect textRect;
            state.snapDrag = state.options.extract_text && _ocr_text_at_point(pt, textRect);
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP:
            if (!state.capturing) {
                break;
            }
            if (state.mouseDown) {
                state.finalRect = state.currentRect;
                state.finalHwnd = state.hoveredHwnd;
                state.wasDragging = state.dragging;
                ReleaseCapture();
                ShowWindow(hwnd, SW_HIDE);
                state.captureReady = true;
                state.mouseDown = false;
                state.dragging = false;
            }
            return 0;
            
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (state.mouseDown) {
                    ReleaseCapture();
                    state.mouseDown = false;
                    state.dragging = false;
                    POINT pt;
                    GetCursorPos(&pt);
                    UpdateHoverRect(pt);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    state.capturing = false;
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
            SelectObject(memDC, memBitmap);
            
            if (state.frozenDC) {
                BitBlt(memDC, 0, 0, cr.right, cr.bottom, state.frozenDC, 0, 0, SRCCOPY);
            }

            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            
            RECT r = { 
                state.currentRect.x - vx, 
                state.currentRect.y - vy, 
                state.currentRect.x - vx + state.currentRect.width, 
                state.currentRect.y - vy + state.currentRect.height 
            };

            HDC hAlphaDC = CreateCompatibleDC(memDC);
            HBITMAP hAlphaBmp = CreateCompatibleBitmap(memDC, 1, 1);
            SelectObject(hAlphaDC, hAlphaBmp);
            SetPixel(hAlphaDC, 0, 0, RGB(0, 120, 215));
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 64, 0 };
            GdiAlphaBlend(memDC, r.left, r.top, r.right - r.left, r.bottom - r.top, hAlphaDC, 0, 0, 1, 1, bf);
            DeleteObject(hAlphaBmp);
            DeleteDC(hAlphaDC);

            HPEN hPen = CreatePen(PS_DOT, 1, RGB(255, 0, 0));
            HPEN hOldPen = (HPEN)SelectObject(memDC, hPen);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
            SetBkMode(memDC, TRANSPARENT);
            Rectangle(memDC, r.left, r.top, r.right, r.bottom);
            SelectObject(memDC, hOldBrush);
            SelectObject(memDC, hOldPen);
            DeleteObject(hPen);

            if (state.currentRect.width > 0 && state.currentRect.height > 0) {
                HFONT hOldFont = (HFONT)SelectObject(memDC, GetStockObject(DEFAULT_GUI_FONT));
                char sizeText[64];
                snprintf(sizeText, sizeof(sizeText), "%d x %d", state.currentRect.width, state.currentRect.height);
                SIZE textSize;
                GetTextExtentPoint32A(memDC, sizeText, (int)strlen(sizeText), &textSize);
                
                int textX = r.left;
                int textY = r.top - textSize.cy - 4;
                if (textY < 0) {
                    textY = r.top + 4;
                }
                
                RECT textBg = { textX, textY, textX + textSize.cx + 6, textY + textSize.cy + 4 };
                HBRUSH bgBrushText = CreateSolidBrush(RGB(0, 0, 0));
                FillRect(memDC, &textBg, bgBrushText);
                DeleteObject(bgBrushText);
                
                SetTextColor(memDC, RGB(255, 255, 255));
                TextOutA(memDC, textX + 3, textY + 2, sizeText, (int)strlen(sizeText));
                SelectObject(memDC, hOldFont);
            }

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
            
            StretchBlt(memDC, destX, destY, magSize, magSize, state.frozenDC, pt.x - srcSize / 2 - vx, pt.y - srcSize / 2 - vy, srcSize, srcSize, SRCCOPY);

            HPEN magPen = CreatePen(PS_SOLID, 1, RGB(255, 50, 50));
            HPEN oldMagPen = (HPEN)SelectObject(memDC, magPen);
            MoveToEx(memDC, destX + magSize / 2, destY, nullptr);
            LineTo(memDC, destX + magSize / 2, destY + magSize);
            MoveToEx(memDC, destX, destY + magSize / 2, nullptr);
            LineTo(memDC, destX + magSize, destY + magSize / 2);
            
            HBRUSH oldBorderBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, destX, destY, destX + magSize, destY + magSize);
            SelectObject(memDC, oldBorderBrush);
            SelectObject(memDC, oldMagPen);
            DeleteObject(magPen);

            BitBlt(hdc, 0, 0, cr.right, cr.bottom, memDC, 0, 0, SRCCOPY);
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
    FindWindowData data = { pt, nullptr, state.overlayHwnd };
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&data));
    
    char className[256] = { 0 };
    if (data.result) {
        GetClassNameA(data.result, className, 256);
    }

    if (!data.result || strcmp(className, "Progman") == 0 || strcmp(className, "WorkerW") == 0) {
        state.hoveredHwnd = nullptr;
        if (const auto* m = _sc_monitor_at_point(pt)) {
            state.currentRect = m->rect;
        }
    } else {
        state.hoveredHwnd = data.result;
        RECT r;
        GetRealWindowRect(data.result, &r);
        state.currentRect = { r.left, r.top, r.right - r.left, r.bottom - r.top };
    }
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
    
    HDC hScaledDC = CreateCompatibleDC(state.frozenDC);
    HBITMAP hScaledBmp = CreateCompatibleBitmap(state.frozenDC, sw, sh);
    SelectObject(hScaledDC, hScaledBmp);
    SetStretchBltMode(hScaledDC, HALFTONE);
    StretchBlt(hScaledDC, 0, 0, sw, sh, state.frozenDC, 0, 0, vw, vh, SRCCOPY);

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
                
                std::lock_guard<std::mutex> lock(state.ocrMutex);
                state.ocrLines = lines;
            }
        } catch (std::exception& e) {
            fprintf(stderr, "OCR failed: %s\n", e.what());
        }
    }).detach();
}

sc_internal bool _ocr_snap_to_text(const sc_rect& drag, sc_rect& out) {
    std::lock_guard<std::mutex> lock(state.ocrMutex);
    bool found = false;
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    
    for (const auto& line : state.ocrLines) {
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
    std::lock_guard<std::mutex> lock(state.ocrMutex);
    for (const auto& line : state.ocrLines) {
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