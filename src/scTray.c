#include "pch.h"
#include "scTray.h"
#include "scApp.h"
#include "scUI.h"
#include "scLogging.h"
#include "scAssert.h"

#define WM_TRAYICON (WM_USER + 1)
static scTrayMenu gTray;

enum {
    TRAY_MENU_SETTINGS = 1001,
    TRAY_MENU_EXIT,
    _TRAY_MENU_LAST_ACTION_IDX
};

#define TRAY_ACTION_ID(action) (action + (_TRAY_MENU_LAST_ACTION_IDX))

LRESULT CALLBACK TrayUtilityWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_TRAYICON: {
      if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);

        const wchar_t* wszSettingsText = L"Settings...";
        const wchar_t* wszExitText     = L"Exit";

        HMENU hActionMenu = CreatePopupMenu();
        for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
          if (i != SC_HOTKEY_ACTIVE_WINDOW && i != SC_HOTKEY_ACTIVE_MONITOR) {
            AppendMenuA(hActionMenu, MF_STRING, TRAY_ACTION_ID(i), scCaptureActionNames[i]);
          }
        }

        HMENU hMenu = CreatePopupMenu();
        AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hActionMenu, "Actions");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, TRAY_MENU_SETTINGS, wszSettingsText);
        AppendMenuW(hMenu, MF_STRING, TRAY_MENU_EXIT, wszExitText);

        SetForegroundWindow(hWnd);
        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(hMenu);
      }
      return 0;
    }
    case WM_COMMAND: {
      switch (LOWORD(wParam)) {
        case TRAY_ACTION_ID(SC_HOTKEY_SCREENSHOT):
        case TRAY_ACTION_ID(SC_HOTKEY_CLIPBOARD):
        case TRAY_ACTION_ID(SC_HOTKEY_OCR):
        //case TRAY_ACTION_ID(SC_HOTKEY_ACTIVE_WINDOW):
        //case TRAY_ACTION_ID(SC_HOTKEY_ACTIVE_MONITOR):
        case TRAY_ACTION_ID(SC_HOTKEY_FALLBACK_SCREENSHOT):
        case TRAY_ACTION_ID(SC_HOTKEY_RECORD):
          scHotkeyID hkID = (scHotkeyID)(LOWORD(wParam) - _TRAY_MENU_LAST_ACTION_IDX);
          scAppRunHandlerFromActionID(hkID);
          break;
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
          PostQuitMessage(0);
        }
      }
      return 0;
    }
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

void scTrayInitialize() {
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
    0, "ScTrayUtilityWindow", NULL, 0,
    0, 0, 0, 0,
    HWND_MESSAGE,
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

  nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
  strcpy_s(nid.szTip, sizeof(nid.szTip), "Screencap Utility");

  Shell_NotifyIconA(NIM_ADD, &nid);
}