#include "pch.h"
#include "scTypes.h"
#include "scUI.h"
#include "scApp.h"
#include "scAssert.h"
#include "scLogging.h"
#include "scGdiPlus.h"

typedef enum {
  SC_WIDGET_TOGGLE,
  SC_WIDGET_DROPDOWN,
  SC_WIDGET_BUTTON,
  SC_WIDGET_IMAGE
} scWidgetType;

typedef struct {
  scWidgetType eType;
  scV2I vPos;
} scWidget;

typedef struct {
  scWidget base;
} scButtonWidget;

typedef struct {
  scWidget base;
  bool bToggled;
} scToggleWidget;

typedef struct {
  scWidget base;
} scDropdownWidget;

typedef struct {
  scWidget base;
} scImageWidget;

typedef struct {
  wchar_t wszName[64];
  scWidget* aWidgets;
  s32 nWidgetCount;
} scPage;

typedef struct {
  bool bDark;
  HFONT pFont;
  HFONT pBoldFont;

  COLORREF dwBackground;
  COLORREF dwSidebar;
  COLORREF dwCard;
  COLORREF dwCardHover;
  COLORREF dwCardActive;
  COLORREF dwPopup;
  COLORREF dwStroke;
  COLORREF dwStrokeSoft;
  COLORREF dwText;
  COLORREF dwTextDim;
  COLORREF dwTextFaint;
  COLORREF dwAccent;
  COLORREF dwAccentHover;
  COLORREF dwPillOff;
  COLORREF dwOk;
  COLORREF dwError;
  COLORREF dwScrollThumb;

  HBRUSH hBackgroundBrush;
  HBRUSH hCardBrush;
} scUITheme;

typedef struct {
  scUITheme theme;
  HWND hWindow;
  bool bRegisteredClass;
  f32 fUIScale;

  scPage aPages[_SC_PAGE_COUNT];
  scPageID eCurrentPage;
  scPageID eHoveredTab;
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

scInternal s32 _scMix(s32 x, s32 y, f32 f) {
  return (int)(x + (y - x) * f);
}

scInternal COLORREF
_scBlendColor(COLORREF a, COLORREF b, f32 f) {
  return RGB(
    _scMix(GetRValue(a), GetRValue(b), f),
    _scMix(GetGValue(a), GetGValue(b), f),
    _scMix(GetBValue(a), GetBValue(b), f)
  );
}

scInternal bool
_scIsWindowsDarkTheme() {
  if (gApp->bIsGeWin10) {
    DWORD val = 1, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
      return val == 0;
    }
  }
  return false;
}

scInternal COLORREF
_scGetSystemAccentColor() {
  const COLORREF dwDefaultAccent = RGB(0, 120, 215);
  if (gApp->bIsGeWin10) {
    DWORD val = 0, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
      return RGB((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
    } else {
      return dwDefaultAccent;
    }
  } else {
    return dwDefaultAccent;
  }
}

//------------------------------------------------------------------------
// Widgets
//------------------------------------------------------------------------
scInternal void
_scWidgetRenderButton(scButtonWidget* pButton) {
  scWidget* pBase = &pButton->base;
}

//------------------------------------------------------------------------
// Pages
//------------------------------------------------------------------------
scInternal void
_scUpdateCurrentPage() {
  scPage* pPage = &gUI.aPages[gUI.eCurrentPage];
  for (s32 i = 0; i < pPage->nWidgetCount; ++i) {
    scWidget* pWidget = &pPage->aWidgets[i];
    switch (pWidget->eType) {
      case SC_WIDGET_BUTTON:
      case SC_WIDGET_TOGGLE:
      case SC_WIDGET_DROPDOWN:
      default:
        break;
    }
  }
}

scInternal void
_scRenderCurrentPage() {
  scPage* pPage = &gUI.aPages[gUI.eCurrentPage];
  for (s32 i = 0; i < pPage->nWidgetCount; ++i) {
    scWidget* pWidget = &pPage->aWidgets[i];
    switch (pWidget->eType) {
      case SC_WIDGET_BUTTON:
      case SC_WIDGET_TOGGLE:
      case SC_WIDGET_DROPDOWN:
      default:
        break;
    }
  }
}

//------------------------------------------------------------------------
// UI
//------------------------------------------------------------------------
//scInternal RECT
//_scEnsureLayout() {
  
//}

scInternal RECT
_scSidebarRect(int i) {
    int top = 20 + i * 35;
    return (RECT){ _scScale(10), _scScale(top), _scScale(140), _scScale(top + 30) };
}

scInternal void
_scRender(HDC hDC) {
  RECT cr;
  GetClientRect(gUI.hWindow, &cr);

  HDC hMemDC = CreateCompatibleDC(hDC);
  HBITMAP hMemBmp = CreateCompatibleBitmap(hDC, cr.right, cr.bottom);
  HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hMemBmp);

  FillRect(hMemDC, &cr, gUI.theme.hBackgroundBrush);

  { // Main Rendering
    { // Sidebar
      { // Background
        GpGraphics* pGraphics = gpGraphicsBegin(hMemDC);
        gpFillRectI(pGraphics, 0, 0, _scScale(150), cr.bottom, gpColor(gUI.theme.dwSidebar, 255));

        for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
          scPage* pPage = &gUI.aPages[i];
          RECT tabRect = _scSidebarRect(i);
          bool bActive = i == (int)gUI.eCurrentPage;
          bool bHot = bActive || (i == (int)gUI.eHoveredTab);

          if (bHot) {
            gpFillRound(pGraphics, tabRect, 5.0f * gUI.fUIScale, gpColor(gUI.theme.dwCardHover, 255));
          }
          if (bActive) {
            int cy = (tabRect.top - tabRect.bottom) / 2;
            RECT ind = { tabRect.left + _scScale(3), cy - _scScale(8), tabRect.left + _scScale(6), cy + _scScale(8) };
            gpFillRound(pGraphics, ind, 1.5f * gUI.fUIScale, gpColor(gUI.theme.dwAccent, 255));
          }
        }

        gpGraphicsEnd(pGraphics);
      }

      SetBkMode(hMemDC, TRANSPARENT);

      { // Text
        for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
          scPage* pPage = &gUI.aPages[i];
          RECT tabRect = _scSidebarRect(i);
          bool bActive = i == (int)gUI.eCurrentPage;

          SelectObject(hMemDC, bActive ? gUI.theme.pBoldFont : gUI.theme.pFont);
          SetTextColor(hMemDC, bActive ? gUI.theme.dwText : gUI.theme.dwTextDim);

          TextOutW(hMemDC, tabRect.left + _scScale(15), tabRect.top + _scScale(7), pPage->wszName, lstrlen(pPage->wszName));
        }
      }
    }

    _scRenderCurrentPage();
  }

  BitBlt(hDC, 0, 0, cr.right, cr.bottom, hMemDC, 0, 0, SRCCOPY);
  SelectObject(hDC, hOldBmp);
  DeleteObject(hMemBmp);
  DeleteDC(hMemDC);
}

//------------------------------------------------------------------------
// Window Procedure
//------------------------------------------------------------------------
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
    case WM_MOUSEMOVE: {
      POINT pt = { LOWORD(lParam), HIWORD(lParam) };
      bool bShouldInvalidate = false;

      { // Update tab hover
        // note: we will always get `SC_PAGE_NONE` when moving from tab to tab.
        // maybe I can increase the size

        scPageID ePrevTab = gUI.eHoveredTab;
        bool bHovered = false;
        for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
          RECT tabRect = _scSidebarRect(i);
          if (PtInRect(&tabRect, pt)) {
            bHovered = true;
            if (i != ePrevTab) {
              gUI.eHoveredTab = i;
              bShouldInvalidate = true;
              break;
            }
          }
        }
        if (ePrevTab != SC_PAGE_NONE && !bHovered) {
          gUI.eHoveredTab = SC_PAGE_NONE;
          bShouldInvalidate = true;
        }
      }

      if (bShouldInvalidate) {
        InvalidateRect(hWnd, NULL, FALSE);
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, HOVER_DEFAULT };
        TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT pt = { LOWORD(lParam), HIWORD(lParam) };
      for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
        RECT tabRect = _scSidebarRect(i);
        if (PtInRect(&tabRect, pt)) {
          scUISetCurrentPage((scPageID)i);
        }
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hDC = BeginPaint(hWnd, &ps);
      _scRender(hDC);
      EndPaint(hWnd, &ps);
      return 0;
    }
    case WM_DESTROY: {
      scUICloseWindow();
      return 0;
    }
  }
  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

//------------------------------------------------------------------------
// SC UI
//------------------------------------------------------------------------
scInternal void
_scUIDestroyTheme() {
  #define _DESTROY_OBJ(x) if (x) { DeleteObject(x); }
  _DESTROY_OBJ(gUI.theme.pFont)
  _DESTROY_OBJ(gUI.theme.pBoldFont)
  _DESTROY_OBJ(gUI.theme.hBackgroundBrush)
  _DESTROY_OBJ(gUI.theme.hCardBrush)
  scLogDebug("Destroyed theme");
}

scInternal void
_scUISetupThemeTheme() {
  scUITheme* pTheme = &gUI.theme;
  if (pTheme->hBackgroundBrush) {
    _scUIDestroyTheme();
  }

  pTheme->pFont     = CreateFontA(_scScale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
  pTheme->pBoldFont = CreateFontA(_scScale(15), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

  pTheme->dwAccent = _scGetSystemAccentColor();
  pTheme->bDark  = _scIsWindowsDarkTheme();

  if (pTheme->bDark) {
    pTheme->dwBackground  = RGB(28, 28, 28);
    pTheme->dwSidebar     = RGB(35, 35, 35);
    pTheme->dwCard        = RGB(43, 43, 43);
    pTheme->dwCardHover   = RGB(52, 52, 52);
    pTheme->dwCardActive  = RGB(60, 60, 60);
    pTheme->dwPopup       = RGB(45, 45, 45);
    pTheme->dwStroke      = RGB(70, 70, 70);
    pTheme->dwStrokeSoft  = RGB(50, 50, 50);
    pTheme->dwText        = RGB(243, 243, 243);
    pTheme->dwTextDim     = RGB(176, 176, 176);
    pTheme->dwTextFaint   = RGB(122, 122, 122);
    pTheme->dwPillOff     = RGB(158, 158, 158);
    pTheme->dwOk          = RGB(108, 203, 95);
    pTheme->dwError       = RGB(255, 99, 97);
    pTheme->dwScrollThumb = RGB(170, 170, 170);
    pTheme->dwAccentHover = _scBlendColor(pTheme->dwAccent, RGB(255, 255, 255), 0.15f);
  } else {
    pTheme->dwBackground  = RGB(243, 243, 243);
    pTheme->dwSidebar     = RGB(236, 236, 236);
    pTheme->dwCard        = RGB(251, 251, 251);
    pTheme->dwCardHover   = RGB(245, 245, 245);
    pTheme->dwCardActive  = RGB(238, 238, 238);
    pTheme->dwPopup       = RGB(252, 252, 252);
    pTheme->dwStroke      = RGB(208, 208, 208);
    pTheme->dwStrokeSoft  = RGB(224, 224, 224);
    pTheme->dwText        = RGB(27, 27, 27);
    pTheme->dwTextDim     = RGB(96, 96, 96);
    pTheme->dwTextFaint   = RGB(142, 142, 142);
    pTheme->dwPillOff     = RGB(134, 134, 134);
    pTheme->dwOk          = RGB(15, 123, 15);
    pTheme->dwError       = RGB(196, 43, 28);
    pTheme->dwScrollThumb = RGB(120, 120, 120);
    pTheme->dwAccentHover = _scBlendColor(pTheme->dwAccent, RGB(0, 0, 0), 0.15f);
  }

  pTheme->hBackgroundBrush = CreateSolidBrush(pTheme->dwBackground);
  pTheme->hCardBrush = CreateSolidBrush(pTheme->dwCard);

  scLogDebug("Created theme, dark mode: %s", pTheme->bDark ? "true" : "false");
}

scInternal void
_scSetupPages() {
  { // General
    scPage* pPage = &gUI.aPages[SC_PAGE_GENERAL];
    wcscpy(pPage->wszName, L"General");
  }

  { // General
    scPage* pPage = &gUI.aPages[SC_PAGE_SETTINGS];
    wcscpy(pPage->wszName, L"Settings");
  }

  { // General
    scPage* pPage = &gUI.aPages[SC_PAGE_GALLERY];
    wcscpy(pPage->wszName, L"Gallery");
  }
}

void scUIOpenWindow() {
  static ULONG_PTR pGdiPlusToken = 0;
  if (!pGdiPlusToken) {
    gp_startup(&pGdiPlusToken);
    scLogDebug("pGdiPlusToken: %p", pGdiPlusToken);
    if (!pGdiPlusToken) {
      scLogError("Failed to get gdiplus token, unable to show UI");
      return;
    }

    scUISetCurrentPage(SC_PAGE_GENERAL);
  }

  _scSetupPages();

  gUI.fUIScale = _scGetDPIScale();
  _scUISetupThemeTheme();

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
  // todo: maybe free the pages
}

void scUISetCurrentPage(scPageID ePageID) {
  scLogDebug("Changing current page from %d to %d", (int)gUI.eCurrentPage, (int)ePageID);
  gUI.eCurrentPage = ePageID;
  if (gUI.hWindow) {
    InvalidateRect(gUI.hWindow, NULL, true);
  }
}