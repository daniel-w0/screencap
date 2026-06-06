#include "pch.h"
#include "win32_ui.h"
#include "screencap.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <commctrl.h>

static bool g_class_registered = false;

#pragma region Utils
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
#pragma endregion
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

void sc_open_settings_window() {
    if (!g_class_registered) {
        WNDCLASSEXW wcc = {};
        wcc.cbSize = sizeof(WNDCLASSEXW);
        wcc.lpfnWndProc = SettingsWndProc;
        wcc.hInstance = GetModuleHandle(nullptr);
        wcc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcc.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
        wcc.hIconSm = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON));
        wcc.lpszClassName = L"ScCustomSettingsWindow";
        RegisterClassExW(&wcc);
        g_class_registered = true;
    }

    HWND hSettings = CreateWindowExW(
        0, L"ScCustomSettingsWindow", L"Screencap",
        WS_OVERLAPPEDWINDOW | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 550, 260,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    ShowWindow(hSettings, SW_SHOW);
    UpdateWindow(hSettings);
}

enum SettingsTab {
    TAB_GENERAL,
    TAB_INPUT
};

struct Win32Toggle {
    const char* name;
    RECT rect;
    const wchar_t* text;
    bool* targetValue;
    bool isHovered;
};

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

sc_internal void _sc_on_option_changed(Win32Toggle& toggle) {
    if (toggle.name == "run_on_startup") {
        printf("Toggled on_startup to %d\n", *toggle.targetValue);
    }
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static std::array<Win32Toggle, 2> toggles;
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
            toggles[0] = { "copy_to_clipboard", { 160, 110, 0, 150 }, sc_get_localized_string("Copy screenshot to Clipboard").c_str(), &sc_get_app().opt_copy_to_clipboard, false};
            toggles[1] = { "run_on_startup", { 160, 150, 500, 190 }, sc_get_localized_string("Run on Startup").c_str(), &sc_get_app().opt_on_startup_enabled, false};

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

            hBtnBrowse = CreateWindowExW(0, L"BUTTON", sc_get_localized_string("Browse...").c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 430, 45, 100, 24, hwnd, (HMENU)3002, GetModuleHandle(nullptr), nullptr);
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

                std::wstring& browseText = sc_get_localized_string("Browse...");
                DrawTextW(pDIS->hDC, browseText.c_str(), -1, &pDIS->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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
                MoveWindow(hEditPath, 160, 45, width - 290, 24, TRUE);
                MoveWindow(hBtnBrowse, width - 120, 45, 100, 24, TRUE);
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

            std::wstring& generalText = sc_get_localized_string("General");
            TextOutW(memDC, tabGeneralRect.left + 15, tabGeneralRect.top + 7, generalText.c_str(), (int)generalText.length());

            bool drawInputHover = hoverInput || (activeTab == TAB_INPUT);
            HBRUSH hInputTabBrush = CreateSolidBrush(drawInputHover ? RGB(45, 45, 45) : RGB(32, 32, 32));
            FillRect(memDC, &tabInputRect, hInputTabBrush);
            DeleteObject(hInputTabBrush);
            SelectObject(memDC, activeTab == TAB_INPUT ? hBoldFont : hFont);
            SetTextColor(memDC, activeTab == TAB_INPUT ? RGB(255, 255, 255) : RGB(200, 200, 200));

            std::wstring& inputText = sc_get_localized_string("Input");
            TextOutW(memDC, tabInputRect.left + 15, tabInputRect.top + 7, inputText.c_str(), (int)inputText.length());

            SelectObject(memDC, hFont);
            SetTextColor(memDC, RGB(240, 240, 240));

            if (activeTab == TAB_GENERAL) {
                ShowWindow(hEditPath, SW_SHOW);
                ShowWindow(hBtnBrowse, SW_SHOW);

                std::wstring& pathLabel = sc_get_localized_string("Screenshot Destination");
                TextOutW(memDC, pathLabelRect.left, pathLabelRect.top, pathLabel.c_str(), (int)pathLabel.length());

                for (int i = 0; i < toggles.size(); ++i) {
                    FillRect(memDC, &toggles[i].rect, toggles[i].isHovered ? hRowHoverBrush : hRowNormalBrush);
                    TextOutW(memDC, toggles[i].rect.left + 15, toggles[i].rect.top + 10, toggles[i].text, (int)wcslen(toggles[i].text));

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
                        _sc_on_option_changed(toggles[i]);
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