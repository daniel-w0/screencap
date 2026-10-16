#include "pch.h"
#include "scUITheme.h"
#include "scApp.h"
#include "scLogging.h"

scInternal void
_scUIDestroyTheme(scUITheme* pTheme) {
  #define _DESTROY_OBJ(x) if (x) { DeleteObject(x); }
  _DESTROY_OBJ(pTheme->pFont)
  _DESTROY_OBJ(pTheme->pBoldFont)
  _DESTROY_OBJ(pTheme->hBackgroundBrush)
  _DESTROY_OBJ(pTheme->hCardBrush)
  scLogDebug("Destroyed theme");
}

scInternal void
_scUISetupTheme(scUITheme* pTheme, s32 iFontScale) {
  if (pTheme->hBackgroundBrush) {
    _scUIDestroyTheme(pTheme);
  }

  pTheme->pFont     = CreateFontA((int)iFontScale, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
  pTheme->pBoldFont = CreateFontA((int)iFontScale, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

  pTheme->dwAccent = scGetSystemAccentColor();
  pTheme->bDark  = scIsWindowsDarkTheme();

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
    pTheme->dwWarn        = RGB(230, 180, 80);
    pTheme->dwError       = RGB(255, 99, 97);
    pTheme->dwScrollThumb = RGB(170, 170, 170);
    pTheme->dwAccentHover = scBlendColor(pTheme->dwAccent, RGB(255, 255, 255), 0.15f);
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
    pTheme->dwWarn        = RGB(176, 124, 0);
    pTheme->dwError       = RGB(196, 43, 28);
    pTheme->dwScrollThumb = RGB(120, 120, 120);
    pTheme->dwAccentHover = scBlendColor(pTheme->dwAccent, RGB(0, 0, 0), 0.15f);
  }

  pTheme->hBackgroundBrush = CreateSolidBrush(pTheme->dwBackground);
  pTheme->hCardBrush = CreateSolidBrush(pTheme->dwCard);

  scLogDebug("Created theme, dark mode: %s", pTheme->bDark ? "true" : "false");
}

void scUIThemeGet(scUITheme* pTheme, s32 iFontScale) {
  _scUISetupTheme(pTheme, iFontScale);
}

COLORREF
scBlendColor(COLORREF a, COLORREF b, f32 f) {
  return RGB(
    scMix(GetRValue(a), GetRValue(b), f),
    scMix(GetGValue(a), GetGValue(b), f),
    scMix(GetBValue(a), GetBValue(b), f)
  );
}

bool
scColorIsLight(COLORREF c) {
  return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000 > 150;
}

bool
scIsWindowsDarkTheme() {
  if (gApp->bIsGeWin10) {
    DWORD val = 1, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
      return val == 0;
    }
  }
  return false;
}

COLORREF
scGetSystemAccentColor() {
  const COLORREF dwDefaultAccent = RGB(0, 120, 215);
  if (gApp->bIsGeWin10) {
    DWORD val = 0, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
      return RGB(val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF); // stored as ABGR
    }
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS) {
      return RGB((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF); // stored as ARGB
    }
  }
  return dwDefaultAccent;
}

s32 scMix(s32 x, s32 y, f32 f) {
  return (s32)(x + (y - x) * f);
}

f32 scGetDPIScale(HWND hWnd) {
  HDC hDC = GetDC(hWnd);
  if (hDC) {
    const s32 iDPI = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(hWnd, hDC);
    return iDPI / 96.0f;
  }
  return 1.0f;
}