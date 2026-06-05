#include "platform/platform_screencap.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
#include <WinUser.h>
#include <wingdi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 2
#endif
#include <iostream>

static std::vector<sc_monitor_info> g_monitors;
static sc_capture_options g_options = { 0 };

static HWND g_overlayHwnd = nullptr;
static bool g_capturing = false;
static bool g_mouseDown = false;
static bool g_dragging = false;
static bool g_wasDragging = false;
static POINT g_dragStart = { 0 };
static sc_rect g_currentRect = { 0 };
static bool g_captureReady = false;
static sc_rect g_finalRect = { 0 };
static HWND g_hoveredHwnd = nullptr;
static HWND g_finalHwnd = nullptr;

static HDC g_frozenDC = nullptr;
static HBITMAP g_frozenBitmap = nullptr;

struct FindWindowData {
    POINT pt;
    HWND result;
    HWND overlayHwnd;
};

void GetRealWindowRect(HWND hwnd, RECT* rect) {
    if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, rect, sizeof(RECT)) != S_OK) {
        GetWindowRect(hwnd, rect);
    }
}

void _sc_swap_channels(uint32_t* pixels, int totalPixels) {
    for (int i = 0; i < totalPixels; ++i) {
        uint32_t p = pixels[i];
        pixels[i] = 0xFF000000 | (p & 0x0000FF00) | ((p & 0x00FF0000) >> 16) | ((p & 0x000000FF) << 16);
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

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam) {
    FindWindowData* data = reinterpret_cast<FindWindowData*>(lParam);
    if (hwnd == data->overlayHwnd) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsIconic(hwnd)) return TRUE;

    RECT r;
    GetRealWindowRect(hwnd, &r);
    if (PtInRect(&r, data->pt)) {
        data->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

void UpdateHoverRect(POINT pt) {
    FindWindowData data = { pt, nullptr, g_overlayHwnd };
    EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&data));

    char className[256] = { 0 };
    if (data.result) {
        GetClassNameA(data.result, className, 256);
    }

    if (!data.result || strcmp(className, "Progman") == 0 || strcmp(className, "WorkerW") == 0) {
        g_hoveredHwnd = nullptr;
        for (const auto& m : g_monitors) {
            if (pt.x >= m.rect.x && pt.x < m.rect.x + m.rect.width &&
                pt.y >= m.rect.y && pt.y < m.rect.y + m.rect.height) {
                g_currentRect = m.rect;
                break;
            }
        }
    } else {
        g_hoveredHwnd = data.result;
        RECT r;
        GetRealWindowRect(data.result, &r);
        g_currentRect = { r.left, r.top, r.right - r.left, r.bottom - r.top };
    }
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            return TRUE;
        case WM_MOUSEMOVE: {
            if (!g_capturing) break;
            POINT pt;
            GetCursorPos(&pt);
            if (g_mouseDown) {
                if (!g_dragging) {
                    if (std::abs(pt.x - g_dragStart.x) > 3 || std::abs(pt.y - g_dragStart.y) > 3) {
                        g_dragging = true;
                    }
                }
                if (g_dragging) {
                    g_currentRect.x = std::min(g_dragStart.x, pt.x);
                    g_currentRect.y = std::min(g_dragStart.y, pt.y);
                    g_currentRect.width = std::abs(pt.x - g_dragStart.x);
                    g_currentRect.height = std::abs(pt.y - g_dragStart.y);
                }
            } else {
                UpdateHoverRect(pt);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!g_capturing) break;
            POINT pt;
            GetCursorPos(&pt);
            g_mouseDown = true;
            g_dragStart = pt;
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!g_capturing) break;
            if (g_mouseDown) {
                g_finalRect = g_currentRect;
                g_finalHwnd = g_hoveredHwnd;
                g_wasDragging = g_dragging;
                ReleaseCapture();
                ShowWindow(hwnd, SW_HIDE);
                g_captureReady = true;
                g_mouseDown = false;
                g_dragging = false;
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                if (g_mouseDown) {
                    g_mouseDown = false;
                    g_dragging = false;
                    ReleaseCapture();
                    POINT pt;
                    GetCursorPos(&pt);
                    UpdateHoverRect(pt);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    g_capturing = false;
                    ShowWindow(hwnd, SW_HIDE);
                }
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr;
            GetClientRect(hwnd, &cr);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            SelectObject(memDC, memBitmap);

            if (g_frozenDC) {
                BitBlt(memDC, 0, 0, cr.right, cr.bottom, g_frozenDC, 0, 0, SRCCOPY);
            }

            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            RECT r = { g_currentRect.x - vx, g_currentRect.y - vy,
                       g_currentRect.x - vx + g_currentRect.width,
                       g_currentRect.y - vy + g_currentRect.height };

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

            if (g_currentRect.width > 0 && g_currentRect.height > 0) {
                HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                HFONT hOldFont = (HFONT)SelectObject(memDC, hFont);
                char sizeText[64];
                snprintf(sizeText, sizeof(sizeText), "%d x %d", g_currentRect.width, g_currentRect.height);
                
                SIZE textSize;
                GetTextExtentPoint32A(memDC, sizeText, (int)strlen(sizeText), &textSize);

                int textX = r.left;
                int textY = r.top - textSize.cy - 4;
                if (textY < 0) textY = r.top + 4; 

                RECT textBg = { textX, textY, textX + textSize.cx + 6, textY + textSize.cy + 4 };
                HBRUSH bgBrushText = CreateSolidBrush(RGB(0, 0, 0));
                FillRect(memDC, &textBg, bgBrushText);
                DeleteObject(bgBrushText);

                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, RGB(255, 255, 255));
                TextOutA(memDC, textX + 3, textY + 2, sizeText, (int)strlen(sizeText));
                SelectObject(memDC, hOldFont);
            }

            POINT pt;
            GetCursorPos(&pt);
            int magSize = 120;
            int zoom = 4;
            int srcSize = magSize / zoom;
            int srcX = pt.x - srcSize / 2;
            int srcY = pt.y - srcSize / 2;
            int destX = pt.x - vx + 20;
            int destY = pt.y - vy + 20;

            if (destX + magSize > vw) destX = pt.x - vx - magSize - 20;
            if (destY + magSize > vh) destY = pt.y - vy - magSize - 20;

            StretchBlt(memDC, destX, destY, magSize, magSize, g_frozenDC, srcX - vx, srcY - vy, srcSize, srcSize, SRCCOPY);

            HPEN magPen = CreatePen(PS_SOLID, 1, RGB(255, 50, 50));
            HPEN oldMagPen = (HPEN)SelectObject(memDC, magPen);
            MoveToEx(memDC, destX + magSize / 2, destY, nullptr);
            LineTo(memDC, destX + magSize / 2, destY + magSize);
            MoveToEx(memDC, destX, destY + magSize / 2, nullptr);
            LineTo(memDC, destX + magSize, destY + magSize / 2);

            HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldBorderBrush = (HBRUSH)SelectObject(memDC, nullBrush);
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

void sc_initialize() {
    winrt::init_apartment();
    SetProcessDPIAware();
    _sc_get_monitors(g_monitors);
}

void sc_begin_capture(sc_capture_options options) {
    if (g_capturing) return;
    g_capturing = true;
    g_options = options;
    g_mouseDown = false;
    g_dragging = false;
    g_wasDragging = false;
    g_captureReady = false;
    g_hoveredHwnd = nullptr;
    g_finalHwnd = nullptr;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hScreenDC = GetDC(nullptr);
    if (g_frozenDC) {
        DeleteDC(g_frozenDC);
        DeleteObject(g_frozenBitmap);
    }
    g_frozenDC = CreateCompatibleDC(hScreenDC);
    g_frozenBitmap = CreateCompatibleBitmap(hScreenDC, vw, vh);
    SelectObject(g_frozenDC, g_frozenBitmap);
    BitBlt(g_frozenDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY);
    ReleaseDC(nullptr, hScreenDC);

    if (!g_overlayHwnd) {
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wc.lpszClassName = "ScOverlayWindow";
        RegisterClassA(&wc);

        g_overlayHwnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            "ScOverlayWindow", "ScreenshotOverlay",
            WS_POPUP,
            vx, vy, vw, vh,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr
        );
    } else {
        SetWindowPos(g_overlayHwnd, HWND_TOPMOST, vx, vy, vw, vh, SWP_NOACTIVATE);
    }

    POINT pt;
    GetCursorPos(&pt);
    UpdateHoverRect(pt);

    ShowWindow(g_overlayHwnd, SW_SHOW);
    SetForegroundWindow(g_overlayHwnd);
    SetFocus(g_overlayHwnd);
}

bool sc_capture_update(sc_capture_info& ci) {
    if (g_captureReady) {
        g_captureReady = false;
        g_capturing = false;

        if (g_finalRect.width > 0 && g_finalRect.height > 0 && g_frozenDC) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);

            int captureWidth = g_finalRect.width;
            int captureHeight = g_finalRect.height;

            ci.width = captureWidth;
            ci.height = captureHeight;

            if (g_options.extract_text) {
                while (ci.width < 40 || ci.height < 40) {
                    ci.width *= 2;
                    ci.height *= 2;
                }
            }

            ci.channels = 4;

            HDC hMemoryDC = CreateCompatibleDC(g_frozenDC);
            HBITMAP hBitmap = CreateCompatibleBitmap(g_frozenDC, ci.width, ci.height);
            SelectObject(hMemoryDC, hBitmap);

            bool windowCaptured = false;
            if (g_finalHwnd && !g_wasDragging && IsWindow(g_finalHwnd)) {
                RECT rWin;
                GetWindowRect(g_finalHwnd, &rWin);
                
                int winW = rWin.right - rWin.left;
                int winH = rWin.bottom - rWin.top;

                HDC hTempDC = CreateCompatibleDC(g_frozenDC);
                HBITMAP hTempBmp = CreateCompatibleBitmap(g_frozenDC, winW, winH);
                SelectObject(hTempDC, hTempBmp);

                if (PrintWindow(g_finalHwnd, hTempDC, PW_RENDERFULLCONTENT)) {
                    RECT rDwm;
                    GetRealWindowRect(g_finalHwnd, &rDwm);
                    int dx = rDwm.left - rWin.left;
                    int dy = rDwm.top - rWin.top;
                    
                    StretchBlt(hMemoryDC, 0, 0, ci.width, ci.height, hTempDC, dx, dy, captureWidth, captureHeight, SRCCOPY);
                    windowCaptured = true;
                }

                DeleteObject(hTempBmp);
                DeleteDC(hTempDC);
            }

            if (!windowCaptured) {
                StretchBlt(hMemoryDC, 0, 0, ci.width, ci.height, g_frozenDC, g_finalRect.x - vx, g_finalRect.y - vy, captureWidth, captureHeight, SRCCOPY);
            }

            BITMAPINFOHEADER bih = {};
            bih.biSize = sizeof(BITMAPINFOHEADER);
            bih.biWidth = ci.width;
            bih.biHeight = -static_cast<int>(ci.height);
            bih.biPlanes = 1;
            bih.biBitCount = 32;
            bih.biCompression = BI_RGB;

            ci.data = new unsigned char[ci.width * ci.height * 4];
            GetDIBits(hMemoryDC, hBitmap, 0, ci.height, ci.data, (BITMAPINFO*)&bih, DIB_RGB_COLORS);

            if (g_options.extract_text) {
                try {
                    winrt::Windows::Storage::Streams::DataWriter writer;
                    writer.WriteBytes(winrt::array_view<const uint8_t>(ci.data, ci.width * ci.height * 4));
                    winrt::Windows::Storage::Streams::IBuffer buffer = writer.DetachBuffer();
                    
                    winrt::Windows::Graphics::Imaging::SoftwareBitmap bitmap = 
                        winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
                            buffer, 
                            winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, 
                            ci.width, ci.height, 
                            winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied);

                    winrt::Windows::Media::Ocr::OcrEngine engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
                    if (engine) {
                        winrt::Windows::Media::Ocr::OcrResult result = engine.RecognizeAsync(bitmap).get();
                        
                        std::wstring text;
                        for (auto const& line : result.Lines()) {
                            text += line.Text().c_str();
                            text += L"\r\n";
                        }
                        
                        if (OpenClipboard(nullptr)) {
                            EmptyClipboard();
                            size_t memSize = (text.length() + 1) * sizeof(wchar_t);
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, memSize);
                            if (hMem) {
                                memcpy(GlobalLock(hMem), text.c_str(), memSize);
                                GlobalUnlock(hMem);
                                SetClipboardData(CF_UNICODETEXT, hMem);
                            }
                            CloseClipboard();
                        }
                    }
                } catch (winrt::hresult_error const& ex) {
                    std::wcerr << L"OCR Failed: " << ex.message().c_str() << std::endl;
                } catch (...) {
                    fprintf(stderr, "OCR Failed\n");
                }
            } else if (g_options.copy_to_clipboard) {
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    
                    size_t dibSize = sizeof(BITMAPINFOHEADER) + (ci.width * ci.height * 4);
                    HGLOBAL hGlobalDIB = GlobalAlloc(GMEM_MOVEABLE, dibSize);
                    if (hGlobalDIB) {
                        void* pGlobal = GlobalLock(hGlobalDIB);
                        memcpy(pGlobal, &bih, sizeof(BITMAPINFOHEADER));
                        memcpy((char*)pGlobal + sizeof(BITMAPINFOHEADER), ci.data, ci.width * ci.height * 4);
                        GlobalUnlock(hGlobalDIB);
                        if (!SetClipboardData(CF_DIB, hGlobalDIB)) GlobalFree(hGlobalDIB);
                    }

                    _sc_swap_channels((uint32_t*)ci.data, ci.width * ci.height);
                    
                    std::vector<unsigned char> pngData;
                    stbi_write_png_to_func([](void* context, void* data, int size) {
                        auto* vec = static_cast<std::vector<unsigned char>*>(context);
                        auto* bytes = static_cast<unsigned char*>(data);
                        vec->insert(vec->end(), bytes, bytes + size);
                    }, &pngData, ci.width, ci.height, 4, ci.data, ci.width * 4);

                    UINT formatPNG = RegisterClipboardFormatA("PNG");
                    HGLOBAL hGlobalPNG = GlobalAlloc(GMEM_MOVEABLE, pngData.size());
                    if (hGlobalPNG) {
                        void* pGlobal = GlobalLock(hGlobalPNG);
                        memcpy(pGlobal, pngData.data(), pngData.size());
                        GlobalUnlock(hGlobalPNG);
                        if (!SetClipboardData(formatPNG, hGlobalPNG)) GlobalFree(hGlobalPNG);
                    }

                    CloseClipboard();
                }
            }

            DeleteObject(hBitmap);
            DeleteDC(hMemoryDC);

            return true;
        }
    }
    return false;
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
            GetRealWindowRect(hwnd, &rect);
            sc_rect captureRect = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
            if (sc_capture_region(captureRect, data->ci)) {
                return TRUE;
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