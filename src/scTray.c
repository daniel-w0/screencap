#include "pch.h"
#include "scTray.h"
#include "scApp.h"
#include "scUI.h"
#include "scLocale.h"
#include "scLogging.h"
#include "scGui.h"
#include "scAssert.h"

#define WM_TRAYICON (WM_USER + 1)
static scTrayMenu gTray;

enum {
    TRAY_MENU_SETTINGS = 1001,
    TRAY_MENU_EXIT,
    _TRAY_MENU_LAST_ACTION_IDX
};

#define TRAY_ACTION_ID(action) (action + (_TRAY_MENU_LAST_ACTION_IDX))

static void
_scTraySetIcon(bool bThemed) {
  int   iSize = GetSystemMetrics(SM_CXSMICON);
  HICON hNew  = scGetThemedIcon(iSize, iSize, gTray.dwAccent, bThemed);
  if (!hNew) {
    return;
  }

  NOTIFYICONDATAA nid = { 0 };
  nid.cbSize = sizeof(NOTIFYICONDATAA);
  nid.hWnd   = gTray.hWindow;
  nid.uID    = 1;
  nid.uFlags = NIF_ICON;
  nid.hIcon  = hNew;

  if (Shell_NotifyIconA(NIM_MODIFY, &nid)) {
    if (gTray.hIcon) DestroyIcon(gTray.hIcon);
    gTray.hIcon = hNew;
  } else {
    DestroyIcon(hNew);
  }
}

LRESULT CALLBACK TrayUtilityWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_TRAYICON: {
      if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);

        scGuiMenuItem aItems[_SC_HOTKEY_COUNT + 3] = { 0 };
        wchar_t aActionNames[_SC_HOTKEY_COUNT][128] = { 0 };
        s32 nItems = 0;

        aItems[nItems++] = (scGuiMenuItem){ scLocaleGet("Actions"), 0, false, false, false, true };
        for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
          if (i == SC_HOTKEY_ACTIVE_WINDOW || (i == SC_HOTKEY_OCR && !gApp->bIsGeWin10)) {
            continue;
          }
          if (!MultiByteToWideChar(CP_ACP, 0, scCaptureActionNames[i], -1,
                                   aActionNames[i], ARRAYSIZE(aActionNames[i]))) {
            aActionNames[i][0] = L'\0';
          }
          aItems[nItems++] = (scGuiMenuItem){ aActionNames[i], TRAY_ACTION_ID(i), true, false, false, false };
        }
        aItems[nItems++] = (scGuiMenuItem){ NULL, 0, false, false, true, false };
        aItems[nItems++] = (scGuiMenuItem){ scLocaleGet("Open Window"), TRAY_MENU_SETTINGS, true, false, false, false };
        aItems[nItems++] = (scGuiMenuItem){ scLocaleGet("Exit"), TRAY_MENU_EXIT, true, true, false, false };

        f32 fScale = scGetDPIScale(hWnd);
        if (gApp->config.fGuiScale > 0.0f) fScale *= gApp->config.fGuiScale;
        int iCmd = scGuiShowContextMenu(hWnd, pt, NULL, fScale, aItems, nItems);
        if (iCmd) SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(iCmd, 0), 0);
      }
      return 0;
    }
    case WM_COMMAND: {
      s32 iCommand = LOWORD(wParam);
      if (iCommand >= _TRAY_MENU_LAST_ACTION_IDX &&
          iCommand < _TRAY_MENU_LAST_ACTION_IDX + _SC_HOTKEY_COUNT) {
        scHotkeyID hkID = (scHotkeyID)(iCommand - _TRAY_MENU_LAST_ACTION_IDX);
        if (hkID != SC_HOTKEY_ACTIVE_WINDOW &&
            (hkID != SC_HOTKEY_OCR || gApp->bIsGeWin10)) {
          scAppRunHandlerFromActionID(hkID);
        }
        return 0;
      }
      switch (iCommand) {
        case TRAY_MENU_SETTINGS: {
          scUIOpenWindow();
          break;
        }
        case TRAY_MENU_EXIT: {
          NOTIFYICONDATAA nid = {0};
          nid.cbSize = sizeof(NOTIFYICONDATAA);
          nid.hWnd   = hWnd;
          nid.uID    = 1;
          Shell_NotifyIconA(NIM_DELETE, &nid);
          if (gTray.hIcon) {
            DestroyIcon(gTray.hIcon);
            gTray.hIcon = NULL;
          }
          PostQuitMessage(0);
          break;
        }
      }
      return 0;
    }
    case WM_SETTINGCHANGE: {
      COLORREF crAccent = scGetSystemAccentColor();
      if (crAccent != gTray.dwAccent) {
        gTray.dwAccent = crAccent;
        if (gApp->config.bAccentIcon) {
          _scTraySetIcon(true);
        }
      }
      return 0;
    }
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

void scTrayInitialize() {
  gTray.dwAccent = scGetSystemAccentColor();

  WNDCLASSEXA wc   = { 0 };
  wc.cbSize        = sizeof(WNDCLASSEXA);
  wc.lpfnWndProc   = TrayUtilityWndProc;
  wc.hInstance     = GetModuleHandle(NULL);
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon         = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
  wc.hIconSm       = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
  wc.lpszClassName = "ScTrayUtilityWindow";
  if (!RegisterClassExA(&wc)) {
    scLogError("Failed to register tray menu: %d", GetLastError());
    return;
  }

  gTray.hWindow = CreateWindowExA(
    WS_EX_TOOLWINDOW, "ScTrayUtilityWindow", NULL, WS_POPUP,
    0, 0, 0, 0,
    NULL,
    NULL, GetModuleHandle(NULL), NULL
  );
  if (!gTray.hWindow) {
    scLogError("Failed to create tray window: %d", GetLastError());
    return;
  }

  scLogInfo("Successfully created tray window");

  // Register tray icon
  NOTIFYICONDATAA nid  = { 0 };
  nid.cbSize           = sizeof(NOTIFYICONDATAA);
  nid.hWnd             = gTray.hWindow;
  nid.uID              = 1;
  nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;

  HICON hIcon = scGetThemedIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                gTray.dwAccent, gApp->config.bAccentIcon);
  nid.hIcon   = hIcon ? hIcon : LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
  gTray.hIcon = hIcon;
  strcpy_s(nid.szTip, sizeof(nid.szTip), "Screencap Utility");

  Shell_NotifyIconA(NIM_ADD, &nid);
}

void scTrayUpdateIcon() {
  _scTraySetIcon(gApp->config.bAccentIcon);
}