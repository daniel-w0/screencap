#include "pch.h"
#include "win32_ui.h"
#include "screencap.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <unordered_map>
#include "stb_image.h"

static bool g_class_registered = false;
static HWND g_settings_window = nullptr;
static float g_scale = 1.0f;

constexpr int MIN_WINDOW_WIDTH = 550;
constexpr int MIN_WINDOW_HEIGHT = 328;

#pragma region Utils
static float get_dpi_scale(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    return dpi / 96.0f;
}

int scale_i(int val) { return (int)(val * g_scale); }
int unscale_i(int val) { return (int)(val / g_scale); }

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
                        sc_save_config();
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

sc_internal std::string _sc_get_key_display_string(uint32_t modifiers, uint32_t key) {
    if (key == 0) return "None";

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
        } else {
            res += (char)key;
        }
    }
    return res;
}

sc_internal void _sc_on_option_changed(const char* name, bool value) {
    if (strcmp(name, "run_on_startup") == 0) {
        _sc_set_run_on_startup_impl(value);
    }
    sc_save_config();
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

    if (!g_settings_window) {
        g_scale = get_dpi_scale(nullptr);
        g_settings_window = CreateWindowExW(
            0, L"ScCustomSettingsWindow", L"Screencap",
            WS_OVERLAPPEDWINDOW | WS_THICKFRAME | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, scale_i(MIN_WINDOW_WIDTH), scale_i(MIN_WINDOW_HEIGHT),
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr
        );
    } else {
        BringWindowToTop(g_settings_window);
        SetForegroundWindow(g_settings_window);
    }
    ShowWindow(g_settings_window, SW_SHOW);
    UpdateWindow(g_settings_window);
}

#pragma region UI
struct sc_ui_theme {
    HFONT  font      = nullptr;
    HFONT  bold      = nullptr;
    HBRUSH bg        = nullptr;
    HBRUSH sidebar   = nullptr;
    HBRUSH row       = nullptr;
    HBRUSH row_hover = nullptr;
    HBRUSH pill_on   = nullptr;
    HBRUSH pill_off  = nullptr;
    HBRUSH thumb     = nullptr;
    HBRUSH ok        = nullptr;
    HBRUSH err       = nullptr;
    HBRUSH recording = nullptr;
};

enum sc_widget_kind {
    SC_W_LABEL,
    SC_W_TOGGLE,
    SC_W_HOTKEY_ROW,
    SC_W_GALLERY_ITEM,
    SC_W_DROPDOWN
};

struct sc_widget {
    sc_widget_kind kind;
    RECT rect;
    std::wstring label;
    bool* value = nullptr;
    const char* opt_name = nullptr;
    int hotkey = -1;
    std::string full_path;
    
    std::string string_val;
    std::vector<std::string> options;
};

struct sc_settings_ui {
    sc_ui_theme theme;
    std::vector<sc_widget> widgets;
    int active_tab = 0;
    int hovered_tab = -1;
    int hovered_widget = -1;
    HWND edit_path = nullptr;
    HWND btn_browse = nullptr;
    int scroll_y = 0;
    int max_scroll_y = 0;
    std::unordered_map<std::string, HBITMAP> image_cache;

    std::vector<std::string> gallery_files;
    bool gallery_needs_refresh = true;
    bool needs_layout = true;
    RECT last_content_rect = { 0, 0, 0, 0 };

    int expanded_dropdown = -1;
    int dropdown_hovered_index = -1;
    int dropdown_scroll_y = 0;

    int editing_hotkey = -1;
};

struct sc_tab {
    const char* loc_key;
    void (*build)(sc_settings_ui& ui, RECT content);
    void (*on_activate)(sc_settings_ui& ui, bool active);
};

static sc_settings_ui g_ui;

#define WM_HOTKEY_RECORDED (WM_APP + 100)

static HHOOK g_keyboard_hook = nullptr;

static void _sc_stop_hotkey_recording() {
    if (g_keyboard_hook) {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
}

static LRESULT CALLBACK _sc_ll_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_ui.editing_hotkey != -1 && g_settings_window) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
            UINT vk = kb->vkCode;
            if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU &&
                vk != VK_LCONTROL && vk != VK_RCONTROL &&
                vk != VK_LSHIFT   && vk != VK_RSHIFT   &&
                vk != VK_LMENU    && vk != VK_RMENU) {
                uint32_t mods = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
                if (kb->flags & LLKHF_ALTDOWN)        mods |= MOD_ALT;
                PostMessage(g_settings_window, WM_HOTKEY_RECORDED, (WPARAM)vk, (LPARAM)mods);
                return 1; // swallow
            }
        }
    }
    return CallNextHookEx(g_keyboard_hook, nCode, wParam, lParam);
}

static void theme_create(sc_ui_theme& t) {
    t.font      = CreateFontA(scale_i(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    t.bold      = CreateFontA(scale_i(15), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    t.bg        = CreateSolidBrush(RGB(28, 28, 28));
    t.sidebar   = CreateSolidBrush(RGB(32, 32, 32));
    t.row       = CreateSolidBrush(RGB(38, 38, 38));
    t.row_hover = CreateSolidBrush(RGB(45, 45, 45));
    t.pill_on   = CreateSolidBrush(RGB(0, 120, 215));
    t.pill_off  = CreateSolidBrush(RGB(100, 100, 100));
    t.thumb     = CreateSolidBrush(RGB(255, 255, 255));
    t.ok        = CreateSolidBrush(RGB(34, 139, 34));
    t.err       = CreateSolidBrush(RGB(178, 34, 34));
    t.recording = CreateSolidBrush(RGB(0, 60, 120));
}

static void theme_destroy(sc_ui_theme& t) {
    for (HGDIOBJ o : { (HGDIOBJ)t.font, (HGDIOBJ)t.bold, (HGDIOBJ)t.bg, (HGDIOBJ)t.sidebar, (HGDIOBJ)t.row, (HGDIOBJ)t.row_hover, (HGDIOBJ)t.pill_on, (HGDIOBJ)t.pill_off, (HGDIOBJ)t.thumb, (HGDIOBJ)t.ok, (HGDIOBJ)t.err, (HGDIOBJ)t.recording }) {
        if (o) DeleteObject(o);
    }
    t = sc_ui_theme{};
}

static sc_widget make_label(RECT rect, std::wstring label) {
    return { .kind = SC_W_LABEL, .rect = rect, .label = label };
}
static sc_widget make_toggle(RECT rect, std::wstring label, bool* value, const char* name) {
    return { .kind = SC_W_TOGGLE, .rect = rect, .label = label, .value = value, .opt_name = name };
}
static sc_widget make_hotkey(RECT rect, int index) {
    return { .kind = SC_W_HOTKEY_ROW, .rect = rect, .hotkey = index };
}
static sc_widget make_gallery_item(RECT rect, std::wstring filename, std::string full_path) {
    return sc_widget{ .kind = SC_W_GALLERY_ITEM, .rect = rect, .label = filename, .full_path = full_path };
}
static sc_widget make_dropdown(RECT rect, std::wstring label, std::string current_val, std::vector<std::string> options, const char* name) {
    sc_widget w = {};
    w.kind = SC_W_DROPDOWN;
    w.rect = rect;
    w.label = label;
    w.string_val = current_val;
    w.options = options;
    w.opt_name = name;
    return w;
}

static void draw_label(HDC dc, const sc_ui_theme& t, const sc_widget& w) {
    SelectObject(dc, t.font);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutW(dc, scale_i(w.rect.left), scale_i(w.rect.top), w.label.c_str(), (int)w.label.length());
}

static void draw_toggle(HDC dc, const sc_ui_theme& t, const sc_widget& w, bool hovered) {
    RECT r = { scale_i(w.rect.left), scale_i(w.rect.top), scale_i(w.rect.right), scale_i(w.rect.bottom) };
    FillRect(dc, &r, hovered ? t.row_hover : t.row);
    SelectObject(dc, t.font);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutW(dc, scale_i(w.rect.left + 15), scale_i(w.rect.top + 10), w.label.c_str(), (int)w.label.length());

    bool isOn = *w.value;
    int pillLeft = w.rect.right - 55;
    int pillTop = w.rect.top + 9;
    RECT pillRect = { scale_i(pillLeft), scale_i(pillTop), scale_i(pillLeft + 40), scale_i(pillTop + 20) };
    FillRect(dc, &pillRect, isOn ? t.pill_on : t.pill_off);

    RECT thumbRect = {
        scale_i(isOn ? pillLeft + 22 : pillLeft + 3),
        scale_i(pillTop + 3),
        scale_i(isOn ? pillLeft + 37 : pillLeft + 18),
        scale_i(pillTop + 17)
    };
    FillRect(dc, &thumbRect, t.thumb);
}

static void draw_dropdown(HDC dc, const sc_ui_theme& t, const sc_widget& w, bool hovered) {
    RECT r = { scale_i(w.rect.left), scale_i(w.rect.top), scale_i(w.rect.right), scale_i(w.rect.bottom) };
    FillRect(dc, &r, hovered ? t.row_hover : t.row);
    SelectObject(dc, t.font);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutW(dc, scale_i(w.rect.left + 15), scale_i(w.rect.top + 10), w.label.c_str(), (int)w.label.length());

    RECT boxRect = { scale_i(w.rect.right - 165), scale_i(w.rect.top + 6), scale_i(w.rect.right - 15), scale_i(w.rect.bottom - 6) };
    FillRect(dc, &boxRect, t.sidebar);
    
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(65, 65, 65));
    HPEN oldPen = (HPEN)SelectObject(dc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen); 
    DeleteObject(borderPen);

    std::wstring valW = _sc_utf8_to_wstring(w.string_val);
    TextOutW(dc, boxRect.left + scale_i(10), boxRect.top + scale_i(5), valW.c_str(), (int)valW.length());
    TextOutW(dc, boxRect.right - scale_i(20), boxRect.top + scale_i(5), L"\x25BC", 1);
}

static void draw_hotkey(HDC dc, const sc_ui_theme& t, const sc_widget& w, bool hovered) {
    const auto& hk = sc_get_app().hotkeys[w.hotkey];
    bool editing = (g_ui.editing_hotkey == w.hotkey);

    RECT r = { scale_i(w.rect.left), scale_i(w.rect.top), scale_i(w.rect.right), scale_i(w.rect.bottom) };
    FillRect(dc, &r, editing ? t.recording : (hovered ? t.row_hover : t.row));

    RECT indicator = { scale_i(w.rect.left + 10), scale_i(w.rect.top + 10), scale_i(w.rect.left + 20), scale_i(w.rect.top + 20) };
    HBRUSH indicatorBrush = (hk.key == 0) ? t.pill_off : (hk.registered ? t.ok : t.err);
    FillRect(dc, &indicator, indicatorBrush);

    SelectObject(dc, t.font);
    SetTextColor(dc, RGB(240, 240, 240));
    TextOutA(dc, scale_i(w.rect.left + 30), scale_i(w.rect.top + 6), sc_hotkey_id_strings[hk.id], (int)strlen(sc_hotkey_id_strings[hk.id]));

    if (editing) {
        const char* prompt = "Press a key... (Esc to cancel, Del to remove)";
        SIZE textSize;
        int textWidth = GetTextExtentPoint32A(dc, prompt, (int)strlen(prompt), &textSize) ? textSize.cx : 0;
        SetTextColor(dc, RGB(180, 210, 255));
        TextOutA(dc, scale_i(w.rect.right) - textWidth - scale_i(15), scale_i(w.rect.top + 6), prompt, (int)strlen(prompt));
    } else {
        std::string bindStr = _sc_get_key_display_string(hk.modifiers, hk.key);
        SIZE textSize;
        int textWidth = GetTextExtentPoint32A(dc, bindStr.c_str(), (int)bindStr.length(), &textSize) ? textSize.cx : 0;
        TextOutA(dc, scale_i(w.rect.right) - textWidth - scale_i(15), scale_i(w.rect.top + 6), bindStr.c_str(), (int)bindStr.length());
    }
}

static void draw_gallery_item(HDC dc, const sc_ui_theme& t, const sc_widget& w, bool hovered) {
    RECT r = { scale_i(w.rect.left), scale_i(w.rect.top), scale_i(w.rect.right), scale_i(w.rect.bottom) };
    FillRect(dc, &r, hovered ? t.row_hover : t.row);

    HPEN borderPen = CreatePen(PS_SOLID, 1, hovered ? RGB(0, 120, 215) : RGB(55, 55, 55));
    HPEN oldPen = (HPEN)SelectObject(dc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(borderPen);

    SelectObject(dc, t.font);
    SetTextColor(dc, RGB(220, 220, 220));
    SetBkMode(dc, TRANSPARENT);

    HBITMAP hBmp = nullptr;
    auto it = g_ui.image_cache.find(w.full_path);
    if (it != g_ui.image_cache.end()) {
        hBmp = it->second;
    } else {
        int img_w, img_h, channels;
        unsigned char* data = stbi_load(w.full_path.c_str(), &img_w, &img_h, &channels, 4);
        if (data) {
            int target_w = img_w;
            int target_h = img_h;
            int max_dim = 640;
            if (img_w > max_dim || img_h > max_dim) {
                float scale = (float)max_dim / std::max(img_w, img_h);
                target_w = (int)(img_w * scale);
                target_h = (int)(img_h * scale);
            }
            if (target_w < 1) target_w = 1;
            if (target_h < 1) target_h = 1;

            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = target_w;
            bmi.bmiHeader.biHeight = -target_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits;
            HDC hdc = GetDC(NULL);
            hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            ReleaseDC(NULL, hdc);

            if (hBmp && bits) {
                unsigned char* dest = (unsigned char*)bits;
                for (int y = 0; y < target_h; ++y) {
                    for (int x = 0; x < target_w; ++x) {
                        int src_x = x * img_w / target_w;
                        int src_y = y * img_h / target_h;
                        int src_idx = (src_y * img_w + src_x) * 4;
                        int dst_idx = (y * target_w + x) * 4;
                        dest[dst_idx + 0] = data[src_idx + 2];
                        dest[dst_idx + 1] = data[src_idx + 1];
                        dest[dst_idx + 2] = data[src_idx + 0];
                        dest[dst_idx + 3] = data[src_idx + 3];
                    }
                }
            }
            stbi_image_free(data);
        }
        g_ui.image_cache[w.full_path] = hBmp;
    }

    if (hBmp) {
        HDC memDC = CreateCompatibleDC(dc);
        HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

        BITMAP bmp;
        GetObject(hBmp, sizeof(BITMAP), &bmp);

        RECT imgRect = r;
        imgRect.bottom -= scale_i(30);
        imgRect.left += scale_i(2);
        imgRect.right -= scale_i(2);
        imgRect.top += scale_i(2);

        float srcAspect = (float)bmp.bmWidth / (float)bmp.bmHeight;
        float dstAspect = (float)(imgRect.right - imgRect.left) / (float)(imgRect.bottom - imgRect.top);

        int drawW = imgRect.right - imgRect.left;
        int drawH = imgRect.bottom - imgRect.top;
        int drawX = imgRect.left;
        int drawY = imgRect.top;

        if (srcAspect > dstAspect) {
            drawH = (int)(drawW / srcAspect);
            drawY += ((imgRect.bottom - imgRect.top) - drawH) / 2;
        } else {
            drawW = (int)(drawH * srcAspect);
            drawX += ((imgRect.right - imgRect.left) - drawW) / 2;
        }

        int oldMode = SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, drawX, drawY, drawW, drawH, memDC, 0, 0, bmp.bmWidth, bmp.bmHeight, SRCCOPY);
        SetStretchBltMode(dc, oldMode);

        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
    } else {
        RECT iconRect = r;
        iconRect.bottom -= scale_i(30);
        DrawTextW(dc, L"PNG", -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT textRect = r;
    textRect.top = textRect.bottom - scale_i(30);
    textRect.left += scale_i(5);
    textRect.right -= scale_i(5);
    DrawTextW(dc, w.label.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_widget(HDC dc, const sc_ui_theme& t, const sc_widget& w, bool hovered) {
    switch (w.kind) {
        case SC_W_LABEL:       draw_label(dc, t, w);       break;
        case SC_W_TOGGLE:      draw_toggle(dc, t, w, hovered); break;
        case SC_W_DROPDOWN:    draw_dropdown(dc, t, w, hovered); break;
        case SC_W_HOTKEY_ROW:  draw_hotkey(dc, t, w, hovered); break;
        case SC_W_GALLERY_ITEM: draw_gallery_item(dc, t, w, hovered); break;
    }
}

static int widget_at(const std::vector<sc_widget>& widgets, POINT pt) {
    for (int i = 0; i < (int)widgets.size(); ++i) {
        RECT r = { scale_i(widgets[i].rect.left), scale_i(widgets[i].rect.top), scale_i(widgets[i].rect.right), scale_i(widgets[i].rect.bottom) };
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static RECT sidebar_tab_rect(int i) {
    int top = 20 + i * 35;
    return RECT{ scale_i(10), scale_i(top), scale_i(140), scale_i(top + 30) };
}

static void build_general_tab(sc_settings_ui& ui, RECT content) {
    ui.widgets.clear();
    ui.widgets.push_back(make_label({ 160, 20, 500, 40 }, sc_get_localized_string("Screenshot Destination").c_str()));

    int y = 90;
    
    ui.widgets.push_back(make_dropdown({ 160, y, content.right - 20, y + 40 }, 
        sc_get_localized_string("Language").c_str(), 
        sc_get_app().language_code, 
        sc_get_app().available_languages, 
        "language"));
    y += 45;
        
    ui.widgets.push_back(make_toggle({ 160, y, content.right - 20, y + 40 }, sc_get_localized_string("Copy screenshot to Clipboard").c_str(), &sc_get_app().opt_copy_to_clipboard, "copy_to_clipboard"));
    y += 45;
    ui.widgets.push_back(make_toggle({ 160, y, content.right - 20, y + 40 }, sc_get_localized_string("Run on Startup").c_str(), &sc_get_app().opt_on_startup_enabled, "run_on_startup"));
    y += 45;
    ui.widgets.push_back(make_toggle({ 160, y, content.right - 20, y + 40 }, sc_get_localized_string("Play sound on capture").c_str(), &sc_get_app().opt_play_sound, "play_sound"));
}

static void activate_general_tab(sc_settings_ui& ui, bool active) {
    int cmd = active ? SW_SHOW : SW_HIDE;
    ShowWindow(ui.edit_path, cmd);
    ShowWindow(ui.btn_browse, cmd);
    if (active) ui.needs_layout = true;
}

static void build_input_tab(sc_settings_ui& ui, RECT content) {
    ui.widgets.clear();
    const auto& hotkeys = sc_get_app().hotkeys;
    int startY = 20;
    for (size_t i = 0; i < hotkeys.size(); ++i) {
        int top = startY + (int)i * 35;
        ui.widgets.push_back(make_hotkey({ 160, top, content.right - 20, top + 30 }, (int)i));
    }
}

bool list_files_in_directory(const std::string& directory, std::vector<std::string>& out_files) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (strstr(filename.c_str(), ".png") || strstr(filename.c_str(), ".jpg") || strstr(filename.c_str(), ".jpeg") || strstr(filename.c_str(), ".bmp") || strstr(filename.c_str(), ".gif")) {
                out_files.push_back(filename);
            }
        }
    }

    std::sort(out_files.begin(), out_files.end(), [&](const std::string& a, const std::string& b) {
        auto a_time = fs::last_write_time(fs::path(directory) / a, ec);
        auto b_time = fs::last_write_time(fs::path(directory) / b, ec);
        return a_time > b_time;
    });

    return true;
}

static void activate_input_tab(sc_settings_ui& ui, bool active) {
    if (active) ui.needs_layout = true;
}

static void build_gallery_tab(sc_settings_ui& ui, RECT content) {
    ui.widgets.clear();

    std::string dir = sc_get_save_path().string();

    if (ui.gallery_needs_refresh) {
        ui.gallery_files.clear();
        list_files_in_directory(dir, ui.gallery_files);
        ui.gallery_needs_refresh = false;
    }

    if (ui.gallery_files.empty()) {
        return;
    }

    const int item_w = 320;
    const int item_h = 260;
    const int gap = 20;
    
    int start_x = 170;
    int start_y = 20 - ui.scroll_y;
    int available_w = content.right - start_x - 10; 

    int cols = std::max(1, (available_w + gap) / (item_w + gap));
    int rows = ((int)ui.gallery_files.size() + cols - 1) / cols;

    int total_content_height = (rows * (item_h + gap)) + 20;
    ui.max_scroll_y = std::max(0, total_content_height - (int)content.bottom);

    for (size_t i = 0; i < ui.gallery_files.size(); ++i) {
        int col = i % cols;
        int row = i / cols;

        int x = start_x + col * (item_w + gap);
        int y = start_y + row * (item_h + gap);

        if (y + item_h > 0 && y < content.bottom) {
            std::string full_path = dir + "\\" + ui.gallery_files[i];
            ui.widgets.push_back(make_gallery_item({ x, y, x + item_w, y + item_h }, _sc_utf8_to_wstring(ui.gallery_files[i]), full_path));
        }
    }
}

static void activate_gallery_tab(sc_settings_ui& ui, bool active) {
    if (active) {
        ui.scroll_y = 0;
        ui.gallery_needs_refresh = true;
        ui.needs_layout = true;
    }
}

static sc_tab g_tabs[] = {
    { "General", build_general_tab, activate_general_tab },
    { "Input",   build_input_tab,   activate_input_tab   },
    { "Recents", build_gallery_tab, activate_gallery_tab }
};

static const int g_tab_count = (int)(sizeof(g_tabs) / sizeof(g_tabs[0]));
#pragma endregion

void sc_settings_on_capture_saved(const std::string& path) {
    g_ui.gallery_needs_refresh = true;
    if (g_ui.active_tab == 2) {
        g_ui.needs_layout = true;
        InvalidateRect(GetActiveWindow(), nullptr, FALSE);
    }
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    sc_settings_ui& ui = g_ui;

    auto ensure_layout = [&]() {
        RECT cr;
        GetClientRect(hwnd, &cr);
        if (ui.needs_layout || cr.right != ui.last_content_rect.right || cr.bottom != ui.last_content_rect.bottom) {
            RECT logical_cr = { 0, 0, unscale_i(cr.right), unscale_i(cr.bottom) };
            g_tabs[ui.active_tab].build(ui, logical_cr);
            ui.needs_layout = false;
            ui.last_content_rect = cr;
        }
        return cr;
    };

    switch (msg) {
        case WM_CREATE: {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

            ui = sc_settings_ui{};
            theme_create(ui.theme);

            ui.edit_path = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, scale_i(160), scale_i(45), scale_i(260), scale_i(24), hwnd, (HMENU)3001, GetModuleHandle(nullptr), nullptr);
            SendMessageW(ui.edit_path, WM_SETFONT, (WPARAM)ui.theme.font, TRUE);

            std::wstring widePath = winrt::to_hstring(sc_get_app().save_path).c_str();
            SetWindowTextW(ui.edit_path, widePath.c_str());

            ui.btn_browse = CreateWindowExW(0, L"BUTTON", sc_get_localized_string("Browse...").c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, scale_i(430), scale_i(45), scale_i(100), scale_i(24), hwnd, (HMENU)3002, GetModuleHandle(nullptr), nullptr);
            SendMessageW(ui.btn_browse, WM_SETFONT, (WPARAM)ui.theme.font, TRUE);

            SetWindowTheme(ui.edit_path, L"Explorer", nullptr);

            SetWindowSubclass(ui.btn_browse, BrowseButtonSubclassProc, 0, 0);

            g_tabs[ui.active_tab].on_activate(ui, true);
            return 0;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;

            if (pDIS->CtlType == ODT_MENU) {
                bool isSelected = (pDIS->itemState & ODS_SELECTED);
                HBRUSH bgBrush = isSelected ? ui.theme.row_hover : ui.theme.row;
                FillRect(pDIS->hDC, &pDIS->rcItem, bgBrush);

                if (pDIS->itemData) {
                    SetBkMode(pDIS->hDC, TRANSPARENT);
                    SetTextColor(pDIS->hDC, RGB(240, 240, 240));
                    HFONT oldFont = (HFONT)SelectObject(pDIS->hDC, ui.theme.font);

                    const wchar_t* text = (const wchar_t*)pDIS->itemData;
                    RECT textRect = pDIS->rcItem;
                    textRect.left += scale_i(10);

                    DrawTextW(pDIS->hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(pDIS->hDC, oldFont);
                }
                return TRUE;
            }

            if (pDIS->CtlID == 3002) {
                bool isPressed = (pDIS->itemState & ODS_SELECTED);
                bool isFocused = (pDIS->itemState & ODS_FOCUS);

                POINT mousePt;
                GetCursorPos(&mousePt);
                ScreenToClient(ui.btn_browse, &mousePt);
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
                HFONT oldFont = (HFONT)SelectObject(pDIS->hDC, ui.theme.font);

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
            if (hwndChild == ui.edit_path) {
                SetTextColor(hdcStatic, RGB(240, 240, 240));
                SetBkColor(hdcStatic, RGB(45, 45, 45));
                return (INT_PTR)ui.theme.row_hover;
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = scale_i(MIN_WINDOW_WIDTH);
            mmi->ptMinTrackSize.y = scale_i(MIN_WINDOW_HEIGHT);
            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(lParam);
            MoveWindow(ui.edit_path, scale_i(160), scale_i(45), width - scale_i(290), scale_i(24), TRUE);
            MoveWindow(ui.btn_browse, width - scale_i(120), scale_i(45), scale_i(100), scale_i(24), TRUE);
            ui.expanded_dropdown = -1;
            ui.needs_layout = true;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (ui.expanded_dropdown != -1) {
                const sc_widget& dw = ui.widgets[ui.expanded_dropdown];
                RECT cr; GetClientRect(hwnd, &cr);
                int item_height = scale_i(28);
                int space_below = cr.bottom - scale_i(dw.rect.bottom);
                int max_visible_items = std::max(1, (space_below - scale_i(20)) / item_height);

                int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                ui.dropdown_scroll_y -= (zDelta * 28) / WHEEL_DELTA;

                int max_scroll = std::max(0, (int)dw.options.size() * 28 - max_visible_items * 28);

                if (ui.dropdown_scroll_y < 0) ui.dropdown_scroll_y = 0;
                if (ui.dropdown_scroll_y > max_scroll) ui.dropdown_scroll_y = max_scroll;

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (ui.active_tab == 2) {
                int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                ui.scroll_y -= (zDelta / WHEEL_DELTA) * 40; 
                
                if (ui.scroll_y < 0) ui.scroll_y = 0;
                if (ui.scroll_y > ui.max_scroll_y) ui.scroll_y = ui.max_scroll_y;
                
                ui.needs_layout = true;
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_COMMAND: {
            if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == 3001) {
                int len = GetWindowTextLengthW(ui.edit_path);
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(ui.edit_path, buf.data(), len + 1);
                sc_get_app().save_path = winrt::to_string(buf.data());
                sc_save_config();
            }

            if (LOWORD(wParam) == 3002) {
                OpenFolderPickerDialog(hwnd, ui.edit_path);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT cr = ensure_layout();

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            FillRect(memDC, &cr, ui.theme.bg);

            RECT sidebarRect = { 0, 0, scale_i(150), cr.bottom };
            FillRect(memDC, &sidebarRect, ui.theme.sidebar);

            SetBkMode(memDC, TRANSPARENT);

            for (int i = 0; i < g_tab_count; ++i) {
                RECT tabRect = sidebar_tab_rect(i);
                bool active = (i == ui.active_tab);
                bool hot = active || (i == ui.hovered_tab);

                FillRect(memDC, &tabRect, hot ? ui.theme.row_hover : ui.theme.sidebar);
                SelectObject(memDC, active ? ui.theme.bold : ui.theme.font);
                SetTextColor(memDC, active ? RGB(255, 255, 255) : RGB(200, 200, 200));

                std::wstring& label = sc_get_localized_string(g_tabs[i].loc_key);
                TextOutW(memDC, tabRect.left + scale_i(15), tabRect.top + scale_i(7), label.c_str(), (int)label.length());
            }

            for (int i = 0; i < (int)ui.widgets.size(); ++i) {
                draw_widget(memDC, ui.theme, ui.widgets[i], i == ui.hovered_widget);
            }

            if (ui.expanded_dropdown >= 0 && ui.expanded_dropdown < (int)ui.widgets.size()) {
                const sc_widget& dw = ui.widgets[ui.expanded_dropdown];
                int item_height = scale_i(28);

                int space_below = cr.bottom - scale_i(dw.rect.bottom);
                int max_visible_items = std::max(1, (space_below - scale_i(20)) / item_height);
                int visible_items = std::min((int)dw.options.size(), max_visible_items);

                RECT boxRect = { scale_i(dw.rect.right - 165), scale_i(dw.rect.bottom - 6), scale_i(dw.rect.right - 15), scale_i(dw.rect.bottom - 6) + visible_items * item_height };

                FillRect(memDC, &boxRect, ui.theme.sidebar);

                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
                HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, boxRect.left, boxRect.top, boxRect.right, boxRect.bottom);
                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(borderPen);

                HRGN hRgn = CreateRectRgn(boxRect.left + 1, boxRect.top + 1, boxRect.right - 1, boxRect.bottom - 1);
                SelectClipRgn(memDC, hRgn);

                SelectObject(memDC, ui.theme.font);
                for (size_t i = 0; i < dw.options.size(); ++i) {
                    int item_top = boxRect.top + 1 + (int)i * item_height - scale_i(ui.dropdown_scroll_y);
                    int item_bottom = item_top + item_height;

                    if (item_bottom > boxRect.top && item_top < boxRect.bottom) {
                        RECT itemRect = { boxRect.left + 1, item_top, boxRect.right - 1, item_bottom };
                        if (ui.dropdown_hovered_index == (int)i) {
                            FillRect(memDC, &itemRect, ui.theme.row_hover);
                        }
                        SetTextColor(memDC, RGB(240, 240, 240));
                        std::wstring optW = _sc_utf8_to_wstring(dw.options[i]);
                        TextOutW(memDC, itemRect.left + scale_i(10), itemRect.top + scale_i(5), optW.c_str(), (int)optW.length());
                    }
                }

                SelectClipRgn(memDC, nullptr);
                DeleteObject(hRgn);
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
            ensure_layout();

            if (ui.expanded_dropdown != -1) {
                const sc_widget& dw = ui.widgets[ui.expanded_dropdown];
                RECT cr; GetClientRect(hwnd, &cr);
                int item_height = scale_i(28);
                int space_below = cr.bottom - scale_i(dw.rect.bottom);
                int max_visible_items = std::max(1, (space_below - scale_i(20)) / item_height);
                int visible_items = std::min((int)dw.options.size(), max_visible_items);
                RECT expRect = { scale_i(dw.rect.right - 165), scale_i(dw.rect.bottom - 6), scale_i(dw.rect.right - 15), scale_i(dw.rect.bottom - 6) + visible_items * item_height };
                
                if (PtInRect(&expRect, pt)) {
                    int idx = (pt.y - expRect.top + scale_i(ui.dropdown_scroll_y)) / item_height;
                    ui.dropdown_hovered_index = (idx >= 0 && idx < (int)dw.options.size()) ? idx : -1;
                } else {
                    ui.dropdown_hovered_index = -1;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            int prevTab = ui.hovered_tab;
            int prevWidget = ui.hovered_widget;

            ui.hovered_tab = -1;
            for (int i = 0; i < g_tab_count; ++i) {
                RECT tabRect = sidebar_tab_rect(i);
                if (PtInRect(&tabRect, pt)) {
                    ui.hovered_tab = i;
                    break;
                }
            }

            int hit = widget_at(ui.widgets, pt);
            ui.hovered_widget = (hit >= 0 && (ui.widgets[hit].kind == SC_W_TOGGLE || ui.widgets[hit].kind == SC_W_DROPDOWN || ui.widgets[hit].kind == SC_W_GALLERY_ITEM || ui.widgets[hit].kind == SC_W_HOTKEY_ROW)) ? hit : -1;

            if (ui.hovered_tab != prevTab || ui.hovered_widget != prevWidget) {
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT };
                TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            ui.hovered_tab = -1;
            ui.hovered_widget = -1;
            ui.dropdown_hovered_index = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            ensure_layout();

            if (ui.expanded_dropdown != -1) {
                const sc_widget& dw = ui.widgets[ui.expanded_dropdown];
                RECT cr; GetClientRect(hwnd, &cr);
                int item_height = scale_i(28);
                int space_below = cr.bottom - scale_i(dw.rect.bottom);
                int max_visible_items = std::max(1, (space_below - scale_i(20)) / item_height);
                int visible_items = std::min((int)dw.options.size(), max_visible_items);
                RECT expRect = { scale_i(dw.rect.right - 165), scale_i(dw.rect.bottom - 6), scale_i(dw.rect.right - 15), scale_i(dw.rect.bottom - 6) + visible_items * item_height };

                if (PtInRect(&expRect, pt)) {
                    int idx = (pt.y - expRect.top + scale_i(ui.dropdown_scroll_y)) / item_height;
                    if (idx >= 0 && idx < (int)dw.options.size()) {
                        if (dw.opt_name && strcmp(dw.opt_name, "language") == 0) {
                            sc_set_language(dw.options[idx]);
                            ui.needs_layout = true; 
                        }
                    }
                }
                
                ui.expanded_dropdown = -1;
                ui.dropdown_hovered_index = -1;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }

            for (int i = 0; i < g_tab_count; ++i) {
                RECT tabRect = sidebar_tab_rect(i);
                if (PtInRect(&tabRect, pt)) {
                    if (i != ui.active_tab) {
                        g_tabs[ui.active_tab].on_activate(ui, false);
                        ui.active_tab = i;
                        g_tabs[ui.active_tab].on_activate(ui, true);
                        InvalidateRect(hwnd, nullptr, TRUE);
                    }
                    return 0;
                }
            }

            int hit = widget_at(ui.widgets, pt);
            if (hit >= 0) {
                if (ui.widgets[hit].kind == SC_W_TOGGLE) {
                    sc_widget& w = ui.widgets[hit];
                    *w.value = !(*w.value);
                    _sc_on_option_changed(w.opt_name, *w.value);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (ui.widgets[hit].kind == SC_W_DROPDOWN) {
                    RECT boxRect = { scale_i(ui.widgets[hit].rect.right - 165), scale_i(ui.widgets[hit].rect.top + 6), scale_i(ui.widgets[hit].rect.right - 15), scale_i(ui.widgets[hit].rect.bottom - 6) };
                    if (PtInRect(&boxRect, pt)) {
                        ui.expanded_dropdown = hit;
                        ui.dropdown_scroll_y = 0;
                        InvalidateRect(hwnd, nullptr, TRUE);
                    }
                } else if (ui.widgets[hit].kind == SC_W_GALLERY_ITEM) {
                    ShellExecuteA(nullptr, "open", ui.widgets[hit].full_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else if (ui.widgets[hit].kind == SC_W_HOTKEY_ROW) {
                    if (ui.editing_hotkey != -1) {
                        _sc_stop_hotkey_recording();
                        ui.editing_hotkey = -1;
                        sc_reregister_hotkeys();
                    }
                    for (auto& hk : sc_get_app().hotkeys) {
                        if (hk.registered) {
                            UnregisterHotKey(nullptr, hk.id);
                            hk.registered = false;
                        }
                    }
                    ui.editing_hotkey = ui.widgets[hit].hotkey;
                    SetFocus(hwnd);
                    g_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, _sc_ll_keyboard_proc, nullptr, 0);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            ensure_layout();

            if (ui.active_tab == 2 && ui.expanded_dropdown == -1) {
                int hit = widget_at(ui.widgets, pt);
                if (hit >= 0 && ui.widgets[hit].kind == SC_W_GALLERY_ITEM) {
                    sc_widget& w = ui.widgets[hit];
                    
                    HMENU hMenu = CreatePopupMenu();

                    MENUINFO mi = { sizeof(MENUINFO) };
                    mi.fMask = MIM_BACKGROUND | MIM_STYLE;
                    mi.dwStyle = MNS_NOCHECK;
                    mi.hbrBack = ui.theme.bg;
                    SetMenuInfo(hMenu, &mi);

                    AppendMenuW(hMenu, MF_OWNERDRAW, 1, sc_get_localized_string("Copy to Clipboard").c_str());
                    AppendMenuW(hMenu, MF_OWNERDRAW, 2, sc_get_localized_string("Open Containing Folder").c_str());
                    AppendMenuW(hMenu, MF_OWNERDRAW, 3, sc_get_localized_string("Open").c_str());
                    AppendMenuW(hMenu, MF_OWNERDRAW, 4, sc_get_localized_string("Delete").c_str());

                    POINT screenPt = pt;
                    ClientToScreen(hwnd, &screenPt);
                    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, hwnd, nullptr);
                    DestroyMenu(hMenu);

                    if (cmd == 1) {
                        int cw, ch, cc;
                        unsigned char* cdata = stbi_load(w.full_path.c_str(), &cw, &ch, &cc, 4);
                        if (cdata) {
                            if (OpenClipboard(hwnd)) {
                                EmptyClipboard();
                                
                                BITMAPINFOHEADER bih = {};
                                bih.biSize = sizeof(BITMAPINFOHEADER);
                                bih.biWidth = cw;
                                bih.biHeight = ch;
                                bih.biPlanes = 1;
                                bih.biBitCount = 32;
                                bih.biCompression = BI_RGB;

                                std::vector<unsigned char> dibData(cw * ch * 4);
                                for (int y = 0; y < ch; ++y) {
                                    for (int x = 0; x < cw; ++x) {
                                        int src_idx = ((ch - 1 - y) * cw + x) * 4;
                                        int dst_idx = (y * cw + x) * 4;
                                        dibData[dst_idx + 0] = cdata[src_idx + 2];
                                        dibData[dst_idx + 1] = cdata[src_idx + 1];
                                        dibData[dst_idx + 2] = cdata[src_idx + 0];
                                        dibData[dst_idx + 3] = cdata[src_idx + 3];
                                    }
                                }
                                _sc_write_to_clipboard(CF_DIB, &bih, sizeof(BITMAPINFOHEADER), dibData.data(), dibData.size());

                                FILE* f = fopen(w.full_path.c_str(), "rb");
                                if (f) {
                                    fseek(f, 0, SEEK_END);
                                    size_t fsize = ftell(f);
                                    fseek(f, 0, SEEK_SET);
                                    std::vector<unsigned char> fileData(fsize);
                                    fread(fileData.data(), 1, fsize, f);
                                    fclose(f);
                                    _sc_write_to_clipboard(RegisterClipboardFormatA("PNG"), fileData.data(), fileData.size());
                                }

                                CloseClipboard();
                            }
                            stbi_image_free(cdata);
                        }
                    } else if (cmd == 2) {
                        std::string arg = "/select,\"" + w.full_path + "\"";
                        ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                    } else if (cmd == 3) {
                        ShellExecuteA(nullptr, "open", w.full_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    } else if (cmd == 4) {
                        if (MessageBoxW(hwnd, sc_get_localized_string("Are you sure you want to delete this screenshot?").c_str(), sc_get_localized_string("Confirm Delete").c_str(), MB_ICONWARNING | MB_YESNO) == IDYES) {
                            DeleteFileA(w.full_path.c_str());
                            ui.gallery_needs_refresh = true;
                            if (ui.active_tab == 2) {
                                ui.needs_layout = true;
                                InvalidateRect(hwnd, nullptr, TRUE);
                            }
                        }
                    }
                }
            }
            return 0;
        }

        case WM_MEASUREITEM: {
            LPMEASUREITEMSTRUCT pMIS = (LPMEASUREITEMSTRUCT)lParam;
            if (pMIS->CtlType == ODT_MENU) {
                UINT textWidth = 0;
                if (pMIS->itemData) {
                    const wchar_t* text = (const wchar_t*)pMIS->itemData;
                    HDC hdc = GetDC(hwnd);
                    HFONT oldFont = (HFONT)SelectObject(hdc, ui.theme.font);
                    SIZE sz;
                    if (GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz)) {
                        textWidth = (UINT)sz.cx;
                    }
                    SelectObject(hdc, oldFont);
                    ReleaseDC(hwnd, hdc);
                }
                pMIS->itemWidth = std::max<UINT>(scale_i(150), textWidth + scale_i(30));
                pMIS->itemHeight = scale_i(24);
                return TRUE;
            }
            break;
        }

        case WM_HOTKEY_RECORDED: {
            if (ui.editing_hotkey == -1) return 0;

            UINT     vk   = (UINT)wParam;
            uint32_t mods = (uint32_t)lParam;

            _sc_stop_hotkey_recording();

            if (vk == VK_ESCAPE) {
                ui.editing_hotkey = -1;
                sc_reregister_hotkeys();
            } else if (vk == VK_DELETE) {
                sc_get_app().hotkeys[ui.editing_hotkey].key       = 0;
                sc_get_app().hotkeys[ui.editing_hotkey].modifiers = 0;
                ui.editing_hotkey = -1;
                sc_reregister_hotkeys();
                sc_save_config();
            } else {
                sc_get_app().hotkeys[ui.editing_hotkey].key       = vk;
                sc_get_app().hotkeys[ui.editing_hotkey].modifiers = mods;
                ui.editing_hotkey = -1;
                sc_reregister_hotkeys();
                sc_save_config();
            }

            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE && ui.editing_hotkey != -1) {
                _sc_stop_hotkey_recording();
                ui.editing_hotkey = -1;
                sc_reregister_hotkeys();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }

        case WM_DESTROY: {
            if (ui.editing_hotkey != -1) {
                _sc_stop_hotkey_recording();
                ui.editing_hotkey = -1;
                sc_reregister_hotkeys();
            }

            for (auto& pair : ui.image_cache) {
                if (pair.second) DeleteObject(pair.second);
            }
            ui.image_cache.clear();

            theme_destroy(ui.theme);
            g_settings_window = nullptr;
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}