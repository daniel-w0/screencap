#include "pch.h"
#include "scTypes.h"
#include "scUI.h"
#include "scAssert.h"
#include "scLogging.h"

typedef struct {
  HWND hWindow;
  bool bRegisteredClass;
  f32 fUIScale;
} scUI;

static scUI gUI;

#define WINDOW_MIN_WIDTH  ((s32)550)
#define WINDOW_MIN_HEIGHT ((s32)328)

//------------------------------------------------------------------------
// Utility
//------------------------------------------------------------------------
scInternal s32 _scScale(s32 nVal)   { return (s32)(nVal * gUI.fUIScale); }
scInternal s32 _scUnscale(s32 nVal) { return (s32)(nVal / gUI.fUIScale); }
scInternal f32 _scGetDPIScale() {
  HDC hDC = GetDC(gUI.hWindow);
  if (hDC) {
    const s32 iDPI = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(gUI.hWindow, hDC);
    return iDPI / 96.0f;
  }
  return 1.0f;
}

LRESULT CALLBACK UIWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_GETMINMAXINFO: {
      MINMAXINFO* pMmi = (MINMAXINFO*)lParam;
      pMmi->ptMinTrackSize.x = _scScale(WINDOW_MIN_WIDTH);
      pMmi->ptMinTrackSize.y = _scScale(WINDOW_MIN_HEIGHT);
      return 0;
    }
    case WM_SIZE: {
      InvalidateRect(hWnd, NULL, TRUE);
      return 0;
    }
    case WM_SETTINGCHANGE: {
      return 0;
    }
    case WM_DESTROY: {
      scUICloseWindow();
      return 0;
    }
  }
  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void scUIOpenWindow() {
  static ULONG_PTR pGdiPlusToken = 0;
  if (!pGdiPlusToken) {
    // die
  }

  gUI.fUIScale = _scGetDPIScale();

  if (!gUI.bRegisteredClass) {
    WNDCLASSEXW wc   = { 0 };
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = UIWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hIconSm       = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON));
    wc.lpszClassName = L"ScWindow";
    if (RegisterClassExW(&wc)) {
      gUI.bRegisteredClass = true;
      scLogDebug("Registered UI Window Class");
    } else {
      scLogError("Failed to register main window ui class: %d", GetLastError());
      return;
    }
  }

  if (!gUI.hWindow) {
    gUI.hWindow = CreateWindowExW(0, L"ScWindow", L"Screencap",
      WS_OVERLAPPEDWINDOW | WS_THICKFRAME | WS_CLIPCHILDREN,
      CW_USEDEFAULT, CW_USEDEFAULT, _scScale(WINDOW_MIN_WIDTH), _scScale(WINDOW_MIN_HEIGHT),
      NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!gUI.hWindow) {
      scLogError("Failed to create main window: %d", GetLastError());
      return;
    }
    scLogDebug("Created main window");
  } else {
    BringWindowToTop(gUI.hWindow);
    SetForegroundWindow(gUI.hWindow);
    scLogDebug("Focused main window");
  }
  ShowWindow(gUI.hWindow, SW_SHOW);
  UpdateWindow(gUI.hWindow);
}

void scUICloseWindow() {
  if (gUI.hWindow) {
    gUI.hWindow = NULL;
    scLogDebug("Destroyed main window");
  }
}