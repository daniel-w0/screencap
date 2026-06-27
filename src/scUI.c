#include "pch.h"
#include "scTypes.h"
#include "scUI.h"
#include "scApp.h"
#include "scAssert.h"
#include "scLogging.h"
#include "scGdiPlus.h"
#include "stb_image.h"

#include <windowsx.h>
#include <shobjidl.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define WM_HOTKEY_RECORDED (WM_APP + 100)

//------------------------------------------------------------------------
// Types
//------------------------------------------------------------------------
typedef enum {
  SC_WIDGET_LABEL,
  SC_WIDGET_TOGGLE,
  SC_WIDGET_DROPDOWN,
  SC_WIDGET_BUTTON,
  SC_WIDGET_HOTKEY,
  SC_WIDGET_IMAGE
} scWidgetType;

typedef struct {
  scWidgetType eType;
  RECT         rcLayout;
  wchar_t      wszText[128];
  char         szPath[MAX_PATH];
  union {
    struct { bool bBold; }             label;
    struct { bool* pValue; }           toggle;
    struct { wchar_t wszValue[64]; }   dropdown;
    struct { void (*pfnClick)(void); } button;
    struct { scHotkeyID eHotkey; }     hotkey;
  } u;
} scWidget;

typedef struct {
  s32 nScrollY;
  s32 nMaxScrollY;
  s32 nContentH;
} scScrollContext;

typedef struct scPage {
  wchar_t         wszName[64];
  scWidget*       aWidgets;
  s32             nWidgetCount;
  s32             nWidgetCap;
  scScrollContext scroll;
  void (*pfnLayout)(struct scPage* pPage, RECT rcContent);
} scPage;

typedef struct {
  char    szPath[MAX_PATH];
  HBITMAP hBitmap;
} scThumb;

typedef struct {
  bool bVisible;
  RECT rcTrack;
  RECT rcThumb;
  s32  iTrackRange;
} scScrollGeom;

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
  HWND      hWindow;
  bool      bRegisteredClass;
  f32       fUIScale;

  scPage   aPages[_SC_PAGE_COUNT];
  scPageID eCurrentPage;
  scPageID eHoveredTab;
  s32      iHoveredWidget;

  bool bNeedsLayout;
  RECT rcLastClient;

  s32 iEditingHotkey;

  bool bDraggingScroll;
  bool bHoveredScroll;
  s32  iDragStartY;
  s32  iDragStartScroll;
  s32  iDragTrackRange;
  s32  iDragMaxScroll;

  scThumb* aThumbs;
  s32      nThumbCount;
  s32      nThumbCap;
} scUI;

static scUI gUI;
static HHOOK gKeyboardHook;

#define WINDOW_MIN_WIDTH  ((s32)550)
#define WINDOW_MIN_HEIGHT ((s32)328)

#define SIDEBAR_WIDTH 150
#define CONTENT_LEFT  160

scInternal void _scBeginHotkeyRecording(s32 iIndex);
scInternal void _scCancelHotkeyRecording();
scInternal void _scUISetupTheme();

//------------------------------------------------------------------------
// Utility
//------------------------------------------------------------------------
scInternal s32 _scScale(s32 nVal)   { return (s32)(nVal * gUI.fUIScale); }
scInternal s32 _scUnscale(s32 nVal) { return (s32)(nVal / gUI.fUIScale); }
scInternal s32 _scClamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
scInternal f32 _scRoundRad() { return 6.0f * gUI.fUIScale; }

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
  return (s32)(x + (y - x) * f);
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
_scColorIsLight(COLORREF c) {
  return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000 > 150;
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
    }
  }
  return dwDefaultAccent;
}

scInternal void
_scHotkeyDisplayString(u32 uModifiers, u32 uKey, char* szOut, s32 nCap) {
  szOut[0] = '\0';
  if (uKey == 0) {
    strcpy_s(szOut, nCap, "None");
    return;
  }

  if (uModifiers & MOD_CONTROL) strcat_s(szOut, nCap, "Ctrl + ");
  if (uModifiers & MOD_SHIFT)   strcat_s(szOut, nCap, "Shift + ");
  if (uModifiers & MOD_ALT)     strcat_s(szOut, nCap, "Alt + ");
  if (uModifiers & MOD_WIN)     strcat_s(szOut, nCap, "Win + ");

  if (uKey == VK_SNAPSHOT) {
    strcat_s(szOut, nCap, "PrintScreen");
  } else {
    char szName[32] = { 0 };
    UINT uScanCode = MapVirtualKeyA(uKey, MAPVK_VK_TO_VSC);
    LONG lParamValue = (uScanCode & 0xFF) << 16;
    if (uKey >= VK_PRIOR && uKey <= VK_HELP) {
      lParamValue |= (1 << 24);
    }
    if (GetKeyNameTextA(lParamValue, szName, sizeof(szName)) > 0) {
      strcat_s(szOut, nCap, szName);
    } else {
      char szChar[2] = { (char)uKey, '\0' };
      strcat_s(szOut, nCap, szChar);
    }
  }
}

//------------------------------------------------------------------------
// Thumbnail cache
//------------------------------------------------------------------------
scInternal HBITMAP
_scLoadThumbnail(const char* szPath) {
  s32 iSrcW, iSrcH, iChannels;
  u8* pData = stbi_load(szPath, &iSrcW, &iSrcH, &iChannels, 4);
  if (!pData) {
    return NULL;
  }

  s32 iDstW = iSrcW;
  s32 iDstH = iSrcH;
  const s32 iMaxDim = 640;
  if (iSrcW > iMaxDim || iSrcH > iMaxDim) {
    f32 fScale = (f32)iMaxDim / (iSrcW > iSrcH ? iSrcW : iSrcH);
    iDstW = (s32)(iSrcW * fScale);
    iDstH = (s32)(iSrcH * fScale);
  }
  if (iDstW < 1) iDstW = 1;
  if (iDstH < 1) iDstH = 1;

  BITMAPINFO bi = { 0 };
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       =  iDstW;
  bi.bmiHeader.biHeight      = -iDstH;
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void* pBits = NULL;
  HDC hScreenDC = GetDC(NULL);
  HBITMAP hBitmap = CreateDIBSection(hScreenDC, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
  ReleaseDC(NULL, hScreenDC);

  if (hBitmap && pBits) {
    u8* pDst = (u8*)pBits;
    for (s32 y = 0; y < iDstH; ++y) {
      for (s32 x = 0; x < iDstW; ++x) {
        s32 iSrc = ((y * iSrcH / iDstH) * iSrcW + (x * iSrcW / iDstW)) * 4;
        s32 iDst = (y * iDstW + x) * 4;
        pDst[iDst + 0] = pData[iSrc + 2];
        pDst[iDst + 1] = pData[iSrc + 1];
        pDst[iDst + 2] = pData[iSrc + 0];
        pDst[iDst + 3] = pData[iSrc + 3];
      }
    }
  }

  stbi_image_free(pData);
  return hBitmap;
}

scInternal HBITMAP
_scThumbGet(const char* szPath) {
  for (s32 i = 0; i < gUI.nThumbCount; ++i) {
    if (strcmp(gUI.aThumbs[i].szPath, szPath) == 0) {
      return gUI.aThumbs[i].hBitmap;
    }
  }

  if (gUI.nThumbCount >= gUI.nThumbCap) {
    gUI.nThumbCap = gUI.nThumbCap ? gUI.nThumbCap * 2 : 16;
    gUI.aThumbs   = (scThumb*)realloc(gUI.aThumbs, gUI.nThumbCap * sizeof(scThumb));
  }

  scThumb* pThumb = &gUI.aThumbs[gUI.nThumbCount++];
  strcpy_s(pThumb->szPath, MAX_PATH, szPath);
  pThumb->hBitmap = _scLoadThumbnail(szPath);
  return pThumb->hBitmap;
}

scInternal void
_scThumbCacheClear() {
  for (s32 i = 0; i < gUI.nThumbCount; ++i) {
    if (gUI.aThumbs[i].hBitmap) {
      DeleteObject(gUI.aThumbs[i].hBitmap);
    }
  }
  free(gUI.aThumbs);
  gUI.aThumbs     = NULL;
  gUI.nThumbCount = 0;
  gUI.nThumbCap   = 0;
}

//------------------------------------------------------------------------
// Widgets
//------------------------------------------------------------------------
scInternal scWidget*
_scPagePush(scPage* pPage, scWidget widget) {
  if (pPage->nWidgetCount >= pPage->nWidgetCap) {
    pPage->nWidgetCap = pPage->nWidgetCap ? pPage->nWidgetCap * 2 : 8;
    pPage->aWidgets   = (scWidget*)realloc(pPage->aWidgets, pPage->nWidgetCap * sizeof(scWidget));
  }
  pPage->aWidgets[pPage->nWidgetCount] = widget;
  return &pPage->aWidgets[pPage->nWidgetCount++];
}

scInternal scWidget
_scMakeLabel(RECT rcLayout, const wchar_t* wszText, bool bBold) {
  scWidget w = { 0 };
  w.eType        = SC_WIDGET_LABEL;
  w.rcLayout     = rcLayout;
  w.u.label.bBold = bBold;
  wcscpy_s(w.wszText, 128, wszText);
  return w;
}

scInternal scWidget
_scMakeToggle(RECT rcLayout, const wchar_t* wszText, bool* pValue) {
  scWidget w = { 0 };
  w.eType         = SC_WIDGET_TOGGLE;
  w.rcLayout      = rcLayout;
  w.u.toggle.pValue = pValue;
  wcscpy_s(w.wszText, 128, wszText);
  return w;
}

scInternal scWidget
_scMakeButton(RECT rcLayout, const wchar_t* wszText, void (*pfnClick)()) {
  scWidget w = { 0 };
  w.eType           = SC_WIDGET_BUTTON;
  w.rcLayout        = rcLayout;
  w.u.button.pfnClick = pfnClick;
  wcscpy_s(w.wszText, 128, wszText);
  return w;
}

scInternal scWidget
_scMakeHotkey(RECT rcLayout, scHotkeyID eHotkey) {
  scWidget w = { 0 };
  w.eType          = SC_WIDGET_HOTKEY;
  w.rcLayout       = rcLayout;
  w.u.hotkey.eHotkey = eHotkey;
  return w;
}

scInternal scWidget
_scMakeImage(RECT rcLayout, const wchar_t* wszName, const char* szPath) {
  scWidget w = { 0 };
  w.eType    = SC_WIDGET_IMAGE;
  w.rcLayout = rcLayout;
  wcscpy_s(w.wszText, 128, wszName);
  strcpy_s(w.szPath, MAX_PATH, szPath);
  return w;
}

scInternal RECT
_scWidgetScaledRect(scPage* pPage, scWidget* pWidget) {
  RECT r = pWidget->rcLayout;
  s32 iOffset = -pPage->scroll.nScrollY;
  return (RECT){
    _scScale(r.left),
    _scScale(r.top    + iOffset),
    _scScale(r.right),
    _scScale(r.bottom + iOffset)
  };
}

scInternal bool
_scWidgetIsInteractive(scWidgetType eType) {
  return eType == SC_WIDGET_TOGGLE || eType == SC_WIDGET_BUTTON ||
         eType == SC_WIDGET_HOTKEY || eType == SC_WIDGET_IMAGE;
}

scInternal s32
_scWidgetAt(scPage* pPage, POINT pt) {
  for (s32 i = 0; i < pPage->nWidgetCount; ++i) {
    if (!_scWidgetIsInteractive(pPage->aWidgets[i].eType)) {
      continue;
    }
    RECT r = _scWidgetScaledRect(pPage, &pPage->aWidgets[i]);
    if (PtInRect(&r, pt)) {
      return i;
    }
  }
  return -1;
}

scInternal void
_scDrawLabel(HDC hDC, scWidget* pWidget, RECT r) {
  SelectObject(hDC, pWidget->u.label.bBold ? gUI.theme.pBoldFont : gUI.theme.pFont);
  SetTextColor(hDC, gUI.theme.dwText);
  TextOutW(hDC, r.left, r.top, pWidget->wszText, lstrlenW(pWidget->wszText));
}

scInternal void
_scDrawToggle(HDC hDC, scWidget* pWidget, RECT r, bool bHovered) {
  scUITheme* t = &gUI.theme;
  bool bOn = *pWidget->u.toggle.pValue;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, _scRoundRad(), gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  gp_stroke_round(pGraphics, r, _scRoundRad(), gpColor(t->dwStrokeSoft, 255), 1.0f);

  RECT pill = { r.right - _scScale(55), r.top + _scScale(10), r.right - _scScale(15), r.top + _scScale(30) };
  f32 fPillRad = (pill.bottom - pill.top) / 2.0f;
  if (bOn) {
    gpFillRound(pGraphics, pill, fPillRad, gpColor(bHovered ? t->dwAccentHover : t->dwAccent, 255));
  } else {
    gpFillRound(pGraphics, pill, fPillRad, gpColor(t->dwCardActive, 255));
    gp_stroke_round(pGraphics, pill, fPillRad, gpColor(t->dwPillOff, 255), 1.0f);
  }

  f32 fInset = 3.0f * gUI.fUIScale;
  f32 fThumb = (pill.bottom - pill.top) - fInset * 2.0f;
  f32 fThumbX = bOn ? (pill.right - fInset - fThumb) : (pill.left + fInset);
  COLORREF dwThumb = bOn ? (_scColorIsLight(t->dwAccent) ? RGB(0, 0, 0) : RGB(255, 255, 255)) : t->dwPillOff;
  gp_fill_ellipse(pGraphics, fThumbX, pill.top + fInset, fThumb, fThumb, gpColor(dwThumb, 255));
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwText);
  TextOutW(hDC, r.left + _scScale(15), r.top + _scScale(10), pWidget->wszText, lstrlenW(pWidget->wszText));
}

scInternal void
_scDrawButton(HDC hDC, scWidget* pWidget, RECT r, bool bHovered) {
  scUITheme* t = &gUI.theme;
  f32 fRad = 4.0f * gUI.fUIScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  gp_stroke_round(pGraphics, r, fRad, gpColor(bHovered ? t->dwAccent : t->dwStroke, 255), 1.0f);
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwText);
  RECT rText = r;
  DrawTextW(hDC, pWidget->wszText, -1, &rText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

scInternal void
_scDrawHotkey(HDC hDC, scWidget* pWidget, RECT r, bool bHovered) {
  scUITheme* t = &gUI.theme;
  scHotkey* pHk = &gApp->config.aHotkeys[pWidget->u.hotkey.eHotkey];
  bool bEditing = (gUI.iEditingHotkey == (s32)pWidget->u.hotkey.eHotkey);

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, _scRoundRad(), gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  if (bEditing) {
    gpFillRound(pGraphics, r, _scRoundRad(), gpColor(t->dwAccent, 36));
    gp_stroke_round(pGraphics, r, _scRoundRad(), gpColor(t->dwAccent, 255), 1.5f);
  } else {
    gp_stroke_round(pGraphics, r, _scRoundRad(), gpColor(t->dwStrokeSoft, 255), 1.0f);
  }

  COLORREF dwIndicator = (pHk->uKey == 0) ? t->dwPillOff : (pHk->bRegistered ? t->dwOk : t->dwError);
  gp_fill_ellipse(pGraphics, (f32)(r.left + _scScale(11)), (f32)(r.top + _scScale(10)), (f32)_scScale(9), (f32)_scScale(9), gpColor(dwIndicator, 255));
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwText);
  const char* szName = scCaptureActionNames[pHk->eID];
  TextOutA(hDC, r.left + _scScale(30), r.top + _scScale(6), szName, (int)strlen(szName));

  char szBind[128];
  if (bEditing) {
    strcpy_s(szBind, sizeof(szBind), "Press a key... (Esc to cancel, Del to remove)");
    SetTextColor(hDC, t->dwTextDim);
  } else {
    _scHotkeyDisplayString(pHk->uModifiers, pHk->uKey, szBind, sizeof(szBind));
  }

  SIZE stTextSize;
  GetTextExtentPoint32A(hDC, szBind, (int)strlen(szBind), &stTextSize);
  TextOutA(hDC, r.right - stTextSize.cx - _scScale(15), r.top + _scScale(6), szBind, (int)strlen(szBind));
}

scInternal void
_scDrawImage(HDC hDC, scWidget* pWidget, RECT r, bool bHovered) {
  scUITheme* t = &gUI.theme;
  f32 fRad = 8.0f * gUI.fUIScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  gpGraphicsEnd(pGraphics);

  HRGN hClip = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, (int)(fRad * 2), (int)(fRad * 2));
  SelectClipRgn(hDC, hClip);
  SetBkMode(hDC, TRANSPARENT);

  HBITMAP hThumb = _scThumbGet(pWidget->szPath);
  RECT rImage = { r.left + _scScale(2), r.top + _scScale(2), r.right - _scScale(2), r.bottom - _scScale(30) };
  if (hThumb) {
    BITMAP bmp;
    GetObject(hThumb, sizeof(BITMAP), &bmp);

    HDC hMemDC = CreateCompatibleDC(hDC);
    HGDIOBJ hOldBmp = SelectObject(hMemDC, hThumb);

    s32 iBoxW = rImage.right - rImage.left;
    s32 iBoxH = rImage.bottom - rImage.top;
    f32 fSrcAspect = (f32)bmp.bmWidth / (f32)bmp.bmHeight;
    f32 fBoxAspect = (f32)iBoxW / (f32)iBoxH;

    s32 iDrawW = iBoxW, iDrawH = iBoxH, iDrawX = rImage.left, iDrawY = rImage.top;
    if (fSrcAspect > fBoxAspect) {
      iDrawH = (s32)(iBoxW / fSrcAspect);
      iDrawY += (iBoxH - iDrawH) / 2;
    } else {
      iDrawW = (s32)(iBoxH * fSrcAspect);
      iDrawX += (iBoxW - iDrawW) / 2;
    }

    s32 iOldMode = SetStretchBltMode(hDC, HALFTONE);
    StretchBlt(hDC, iDrawX, iDrawY, iDrawW, iDrawH, hMemDC, 0, 0, bmp.bmWidth, bmp.bmHeight, SRCCOPY);
    SetStretchBltMode(hDC, iOldMode);

    SelectObject(hMemDC, hOldBmp);
    DeleteDC(hMemDC);
  } else {
    SelectObject(hDC, t->pFont);
    SetTextColor(hDC, t->dwTextDim);
    const wchar_t* wszExt = wcsrchr(pWidget->wszText, L'.');
    DrawTextW(hDC, wszExt ? wszExt + 1 : pWidget->wszText, -1, &rImage, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  RECT rText = { r.left + _scScale(5), r.bottom - _scScale(30), r.right - _scScale(5), r.bottom };
  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwText);
  DrawTextW(hDC, pWidget->wszText, -1, &rText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  SelectClipRgn(hDC, NULL);
  DeleteObject(hClip);

  pGraphics = gpGraphicsBegin(hDC);
  gp_stroke_round(pGraphics, r, fRad, gpColor(bHovered ? t->dwAccent : t->dwStrokeSoft, 255), bHovered ? 1.5f : 1.0f);
  gpGraphicsEnd(pGraphics);
}

scInternal void
_scDrawWidget(HDC hDC, scWidget* pWidget, RECT r, bool bHovered) {
  switch (pWidget->eType) {
    case SC_WIDGET_LABEL:  _scDrawLabel (hDC, pWidget, r);           break;
    case SC_WIDGET_TOGGLE: _scDrawToggle(hDC, pWidget, r, bHovered); break;
    case SC_WIDGET_BUTTON: _scDrawButton(hDC, pWidget, r, bHovered); break;
    case SC_WIDGET_HOTKEY: _scDrawHotkey(hDC, pWidget, r, bHovered); break;
    case SC_WIDGET_IMAGE:  _scDrawImage (hDC, pWidget, r, bHovered); break;
    case SC_WIDGET_DROPDOWN: // todo: expandable popup (needs localization options)
    default:
      break;
  }
}

//------------------------------------------------------------------------
// Scrolling
//------------------------------------------------------------------------
scInternal scScrollGeom
_scComputeScrollGeom(RECT rcTrack, s32 iContentH, s32 iViewportH, s32 iScroll, s32 iMaxScroll) {
  scScrollGeom sg = { 0 };
  if (iMaxScroll <= 0 || iContentH <= iViewportH) {
    return sg;
  }
  sg.bVisible = true;
  sg.rcTrack  = rcTrack;

  s32 iTrackH = rcTrack.bottom - rcTrack.top;
  s32 iThumbH = _scClamp((s32)((s64)iTrackH * iViewportH / iContentH), _scScale(24), iTrackH);
  sg.iTrackRange = iTrackH - iThumbH;

  s32 iY = rcTrack.top + (s32)((s64)sg.iTrackRange * _scClamp(iScroll, 0, iMaxScroll) / iMaxScroll);
  sg.rcThumb = (RECT){ rcTrack.left, iY, rcTrack.right, iY + iThumbH };
  return sg;
}

scInternal scScrollGeom
_scGalleryScrollbar(RECT cr) {
  scPage* pPage = &gUI.aPages[SC_PAGE_GALLERY];
  if (pPage->scroll.nMaxScrollY <= 0) {
    return (scScrollGeom){ 0 };
  }
  RECT rcTrack = { cr.right - _scScale(14), _scScale(8), cr.right - _scScale(6), cr.bottom - _scScale(8) };
  return _scComputeScrollGeom(rcTrack, _scScale(pPage->scroll.nContentH), cr.bottom, pPage->scroll.nScrollY, pPage->scroll.nMaxScrollY);
}

//------------------------------------------------------------------------
// Pages
//------------------------------------------------------------------------
scInternal void
_scOnBrowseClicked() {
  bool bComInit = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));

  IFileOpenDialog* pDialog = NULL;
  if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&pDialog))) {
    FILEOPENDIALOGOPTIONS dwOptions = 0;
    pDialog->lpVtbl->GetOptions(pDialog, &dwOptions);
    pDialog->lpVtbl->SetOptions(pDialog, dwOptions | FOS_PICKFOLDERS);

    if (SUCCEEDED(pDialog->lpVtbl->Show(pDialog, gUI.hWindow))) {
      IShellItem* pItem = NULL;
      if (SUCCEEDED(pDialog->lpVtbl->GetResult(pDialog, &pItem))) {
        PWSTR pszPath = NULL;
        if (SUCCEEDED(pItem->lpVtbl->GetDisplayName(pItem, SIGDN_FILESYSPATH, &pszPath))) {
          wcscpy_s(gApp->config.wszSavePath, SC_PATH_MAX_LEN, pszPath);
          scSaveConfig();
          CoTaskMemFree(pszPath);

          gUI.aPages[SC_PAGE_GALLERY].scroll.nScrollY = 0;
          gUI.bNeedsLayout = true;
          InvalidateRect(gUI.hWindow, NULL, TRUE);
        }
        pItem->lpVtbl->Release(pItem);
      }
    }
    pDialog->lpVtbl->Release(pDialog);
  }

  if (bComInit) {
    CoUninitialize();
  }
}

scInternal void
_scLayoutGeneral(scPage* pPage, RECT rc) {
  pPage->nWidgetCount = 0;
  _scPagePush(pPage, _scMakeLabel((RECT){ CONTENT_LEFT, 20, 500, 40 }, L"Screenshot Destination", true));
  _scPagePush(pPage, _scMakeButton((RECT){ rc.right - 120, 42, rc.right - 20, 72 }, L"Browse...", _scOnBrowseClicked));

  s32 iY = 90;
  _scPagePush(pPage, _scMakeToggle((RECT){ CONTENT_LEFT, iY, rc.right - 20, iY + 40 }, L"Run on Startup", &gApp->config.bRunAtStartup));
  iY += 45;
  _scPagePush(pPage, _scMakeToggle((RECT){ CONTENT_LEFT, iY, rc.right - 20, iY + 40 }, L"Play sound on capture", &gApp->config.bPlaySoundOnAction));
}

scInternal void
_scLayoutSettings(scPage* pPage, RECT rc) {
  pPage->nWidgetCount = 0;
  for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    s32 iTop = 20 + i * 35;
    _scPagePush(pPage, _scMakeHotkey((RECT){ CONTENT_LEFT, iTop, rc.right - 20, iTop + 30 }, (scHotkeyID)i));
  }
}

scInternal bool
_scIsImageFile(const wchar_t* wszName) {
  const wchar_t* wszExt = wcsrchr(wszName, L'.');
  if (!wszExt) {
    return false;
  }
  static const wchar_t* kExtensions[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".mp4" };
  for (s32 i = 0; i < (s32)(sizeof(kExtensions) / sizeof(kExtensions[0])); ++i) {
    if (_wcsicmp(wszExt, kExtensions[i]) == 0) {
      return true;
    }
  }
  return false;
}

typedef struct {
  wchar_t  wszName[MAX_PATH];
  FILETIME ftWrite;
} _scFileEntry;

scInternal int
_scCompareFileTime(const void* a, const void* b) {
  const _scFileEntry* pA = (const _scFileEntry*)a;
  const _scFileEntry* pB = (const _scFileEntry*)b;
  return CompareFileTime(&pB->ftWrite, &pA->ftWrite); // newest first
}

scInternal void
_scLayoutGallery(scPage* pPage, RECT rc) {
  pPage->nWidgetCount = 0;

  wchar_t wszDir[MAX_PATH];
  if (!scGetSavePath(wszDir, MAX_PATH)) {
    pPage->scroll.nContentH   = 0;
    pPage->scroll.nMaxScrollY = 0;
    return;
  }

  _scFileEntry* aFiles = NULL;
  s32 nFiles = 0, nFileCap = 0;

  wchar_t wszPattern[MAX_PATH];
  _snwprintf_s(wszPattern, MAX_PATH, _TRUNCATE, L"%ls\\*", wszDir);

  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(wszPattern, &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_scIsImageFile(fd.cFileName)) {
        continue;
      }
      if (nFiles >= nFileCap) {
        nFileCap = nFileCap ? nFileCap * 2 : 32;
        aFiles   = (_scFileEntry*)realloc(aFiles, nFileCap * sizeof(_scFileEntry));
      }
      wcscpy_s(aFiles[nFiles].wszName, MAX_PATH, fd.cFileName);
      aFiles[nFiles].ftWrite = fd.ftLastWriteTime;
      ++nFiles;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
  }

  if (nFiles == 0) {
    pPage->scroll.nContentH  = 0;
    pPage->scroll.nMaxScrollY = 0;
    free(aFiles);
    return;
  }

  qsort(aFiles, nFiles, sizeof(_scFileEntry), _scCompareFileTime);

  const s32 iItemW = 320, iItemH = 260, iGap = 20, iStartX = 170;
  s32 iAvailable = rc.right - iStartX - 10;
  s32 iCols = (iAvailable + iGap) / (iItemW + iGap);
  if (iCols < 1) iCols = 1;
  s32 iRows = (nFiles + iCols - 1) / iCols;

  pPage->scroll.nContentH  = iRows * (iItemH + iGap) + 20;
  pPage->scroll.nMaxScrollY = pPage->scroll.nContentH - rc.bottom;
  if (pPage->scroll.nMaxScrollY < 0) pPage->scroll.nMaxScrollY = 0;
  pPage->scroll.nScrollY = _scClamp(pPage->scroll.nScrollY, 0, pPage->scroll.nMaxScrollY);

  for (s32 i = 0; i < nFiles; ++i) {
    s32 iX = iStartX + (i % iCols) * (iItemW + iGap);
    s32 iY = 20 + (i / iCols) * (iItemH + iGap);

    wchar_t wszFull[MAX_PATH];
    _snwprintf_s(wszFull, MAX_PATH, _TRUNCATE, L"%ls\\%ls", wszDir, aFiles[i].wszName);
    char szFull[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wszFull, -1, szFull, MAX_PATH, NULL, NULL);

    _scPagePush(pPage, _scMakeImage((RECT){ iX, iY, iX + iItemW, iY + iItemH }, aFiles[i].wszName, szFull));
  }

  free(aFiles);
}

scInternal RECT
_scEnsureLayout() {
  RECT cr;
  GetClientRect(gUI.hWindow, &cr);
  if (gUI.bNeedsLayout || cr.right != gUI.rcLastClient.right || cr.bottom != gUI.rcLastClient.bottom) {
    scPage* pPage = &gUI.aPages[gUI.eCurrentPage];
    if (pPage->pfnLayout) {
      RECT rcLogical = { 0, 0, _scUnscale(cr.right), _scUnscale(cr.bottom) };
      pPage->pfnLayout(pPage, rcLogical);
    }
    gUI.bNeedsLayout    = false;
    gUI.rcLastClient    = cr;
  }
  return cr;
}

scInternal void
_scRenderCurrentPage(HDC hDC, RECT cr) {
  scPage* pPage = &gUI.aPages[gUI.eCurrentPage];

  for (s32 i = 0; i < pPage->nWidgetCount; ++i) {
    scWidget* pWidget = &pPage->aWidgets[i];
    RECT r = _scWidgetScaledRect(pPage, pWidget);
    if (r.bottom < 0 || r.top > cr.bottom) {
      continue;
    }
    _scDrawWidget(hDC, pWidget, r, i == gUI.iHoveredWidget);
  }
}

//------------------------------------------------------------------------
// Rendering
//------------------------------------------------------------------------
scInternal RECT
_scSidebarRect(s32 i) {
  s32 iTop = 20 + i * 35;
  return (RECT){ _scScale(10), _scScale(iTop), _scScale(140), _scScale(iTop + 30) };
}

scInternal void
_scRenderSidebar(HDC hDC, RECT cr) {
  scUITheme* t = &gUI.theme;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRectI(pGraphics, 0, 0, _scScale(SIDEBAR_WIDTH), cr.bottom, gpColor(t->dwSidebar, 255));

  for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
    RECT rcTab  = _scSidebarRect(i);
    bool bActive = i == (s32)gUI.eCurrentPage;
    bool bHot    = bActive || (i == (s32)gUI.eHoveredTab);

    if (bHot) {
      gpFillRound(pGraphics, rcTab, 5.0f * gUI.fUIScale, gpColor(bActive ? t->dwCardHover : t->dwCard, 255));
    }
    if (bActive) {
      s32 iCenterY = (rcTab.top + rcTab.bottom) / 2;
      RECT rcInd = { rcTab.left + _scScale(3), iCenterY - _scScale(8), rcTab.left + _scScale(6), iCenterY + _scScale(8) };
      gpFillRound(pGraphics, rcInd, 1.5f * gUI.fUIScale, gpColor(t->dwAccent, 255));
    }
  }
  gpGraphicsEnd(pGraphics);

  SetBkMode(hDC, TRANSPARENT);
  for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
    scPage* pPage = &gUI.aPages[i];
    RECT rcTab    = _scSidebarRect(i);
    bool bActive  = i == (s32)gUI.eCurrentPage;

    SelectObject(hDC, bActive ? t->pBoldFont : t->pFont);
    SetTextColor(hDC, bActive ? t->dwText : t->dwTextDim);
    TextOutW(hDC, rcTab.left + _scScale(15), rcTab.top + _scScale(7), pPage->wszName, lstrlenW(pPage->wszName));
  }

  const wchar_t* wszVersion = SC_VERSION_STRING_FULL_W;
  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwTextFaint);
  RECT rcVersion = { 0, cr.bottom - _scScale(30), _scScale(SIDEBAR_WIDTH), cr.bottom };
  DrawTextW(hDC, wszVersion, -1, &rcVersion, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

scInternal void
_scRenderPathField(HDC hDC, RECT cr) {
  scUITheme* t = &gUI.theme;
  f32 fRad = 4.0f * gUI.fUIScale;
  RECT box = { _scScale(CONTENT_LEFT), _scScale(42), cr.right - _scScale(130), _scScale(72) };

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, box, fRad, gpColor(t->dwCard, 255));
  gp_stroke_round(pGraphics, box, fRad, gpColor(t->dwStroke, 255), 1.0f);
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwText);
  RECT rText = { box.left + _scScale(10), box.top, box.right - _scScale(10), box.bottom };
  DrawTextW(hDC, gApp->config.wszSavePath, -1, &rText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
}

scInternal void
_scRenderScrollbar(HDC hDC, scScrollGeom sg) {
  if (!sg.bVisible) {
    return;
  }
  scUITheme* t = &gUI.theme;
  f32 fRad = (sg.rcTrack.right - sg.rcTrack.left) / 2.0f;
  bool bHot = gUI.bHoveredScroll || gUI.bDraggingScroll;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, sg.rcTrack, fRad, gpColor(t->dwScrollThumb, 32));
  gpFillRound(pGraphics, sg.rcThumb, fRad, gpColor(t->dwScrollThumb, bHot ? 230 : 150));
  gpGraphicsEnd(pGraphics);
}

scInternal void
_scRender(HDC hDC) {
  RECT cr = _scEnsureLayout();

  HDC hMemDC = CreateCompatibleDC(hDC);
  HBITMAP hMemBmp = CreateCompatibleBitmap(hDC, cr.right, cr.bottom);
  HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hMemBmp);

  FillRect(hMemDC, &cr, gUI.theme.hBackgroundBrush);

  _scRenderSidebar(hMemDC, cr);
  if (gUI.eCurrentPage == SC_PAGE_GENERAL) {
    _scRenderPathField(hMemDC, cr);
  }
  _scRenderCurrentPage(hMemDC, cr);
  if (gUI.eCurrentPage == SC_PAGE_GALLERY) {
    _scRenderScrollbar(hMemDC, _scGalleryScrollbar(cr));
  }

  BitBlt(hDC, 0, 0, cr.right, cr.bottom, hMemDC, 0, 0, SRCCOPY);
  SelectObject(hMemDC, hOldBmp);
  DeleteObject(hMemBmp);
  DeleteDC(hMemDC);
}

//------------------------------------------------------------------------
// Input
//------------------------------------------------------------------------
scInternal void
_scOnWidgetClicked(scWidget* pWidget) {
  switch (pWidget->eType) {
    case SC_WIDGET_TOGGLE: {
      *pWidget->u.toggle.pValue = !*pWidget->u.toggle.pValue;
      scSaveConfig();
      InvalidateRect(gUI.hWindow, NULL, FALSE);
      break;
    }
    case SC_WIDGET_BUTTON: {
      if (pWidget->u.button.pfnClick) {
        pWidget->u.button.pfnClick();
      }
      break;
    }
    case SC_WIDGET_HOTKEY: {
      _scBeginHotkeyRecording((s32)pWidget->u.hotkey.eHotkey);
      InvalidateRect(gUI.hWindow, NULL, FALSE);
      break;
    }
    case SC_WIDGET_IMAGE: {
      ShellExecuteA(NULL, "open", pWidget->szPath, NULL, NULL, SW_SHOWNORMAL);
      break;
    }
    default:
      break;
  }
}

scInternal bool
_scHandleMouseMove(POINT pt, RECT cr) {
  if (gUI.bDraggingScroll) {
    scPage* pPage = &gUI.aPages[gUI.eCurrentPage];
    s32 iDelta = pt.y - gUI.iDragStartY;
    s32 iScroll = gUI.iDragStartScroll + (gUI.iDragTrackRange > 0 ? (s32)((s64)iDelta * gUI.iDragMaxScroll / gUI.iDragTrackRange) : 0);
    iScroll = _scClamp(iScroll, 0, gUI.iDragMaxScroll);
    if (iScroll != pPage->scroll.nScrollY) {
      pPage->scroll.nScrollY = iScroll;
      InvalidateRect(gUI.hWindow, NULL, FALSE);
    }
    return false;
  }

  scPageID ePrevTab    = gUI.eHoveredTab;
  s32      iPrevWidget = gUI.iHoveredWidget;
  bool     bPrevScroll = gUI.bHoveredScroll;

  gUI.eHoveredTab = SC_PAGE_NONE;
  for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
    RECT rcTab = _scSidebarRect(i);
    if (PtInRect(&rcTab, pt)) {
      gUI.eHoveredTab = (scPageID)i;
      break;
    }
  }

  gUI.bHoveredScroll = false;
  if (gUI.eCurrentPage == SC_PAGE_GALLERY) {
    scScrollGeom sg = _scGalleryScrollbar(cr);
    if (sg.bVisible && PtInRect(&sg.rcThumb, pt)) {
      gUI.bHoveredScroll = true;
    }
  }

  s32 iHit = gUI.bHoveredScroll ? -1 : _scWidgetAt(&gUI.aPages[gUI.eCurrentPage], pt);
  gUI.iHoveredWidget = iHit;

  return gUI.eHoveredTab != ePrevTab || gUI.iHoveredWidget != iPrevWidget || gUI.bHoveredScroll != bPrevScroll;
}

scInternal void
_scHandleLeftDown(POINT pt, RECT cr) {
  if (gUI.eCurrentPage == SC_PAGE_GALLERY) {
    scScrollGeom sg = _scGalleryScrollbar(cr);
    if (sg.bVisible) {
      scPage* pPage = &gUI.aPages[SC_PAGE_GALLERY];
      bool bOnThumb = PtInRect(&sg.rcThumb, pt);
      RECT rcTrackHit = sg.rcTrack;
      InflateRect(&rcTrackHit, _scScale(3), 0);
      if (!bOnThumb && PtInRect(&rcTrackHit, pt)) {
        s32 iThumbH = sg.rcThumb.bottom - sg.rcThumb.top;
        s32 iRel = pt.y - sg.rcTrack.top - iThumbH / 2;
        pPage->scroll.nScrollY = sg.iTrackRange > 0 ? _scClamp((s32)((s64)iRel * pPage->scroll.nMaxScrollY / sg.iTrackRange), 0, pPage->scroll.nMaxScrollY) : 0;
        bOnThumb = true;
      }
      if (bOnThumb) {
        gUI.bDraggingScroll  = true;
        gUI.iDragStartY      = pt.y;
        gUI.iDragStartScroll = pPage->scroll.nScrollY;
        gUI.iDragTrackRange  = sg.iTrackRange;
        gUI.iDragMaxScroll   = pPage->scroll.nMaxScrollY;
        SetCapture(gUI.hWindow);
        InvalidateRect(gUI.hWindow, NULL, FALSE);
        return;
      }
    }
  }

  for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
    RECT rcTab = _scSidebarRect(i);
    if (PtInRect(&rcTab, pt)) {
      scUISetCurrentPage((scPageID)i);
      return;
    }
  }

  scPage* pPage = &gUI.aPages[gUI.eCurrentPage];
  s32 iHit = _scWidgetAt(pPage, pt);
  if (iHit >= 0) {
    _scOnWidgetClicked(&pPage->aWidgets[iHit]);
  }
}

//------------------------------------------------------------------------
// Hotkey recording
//------------------------------------------------------------------------
scInternal void
_scStopHotkeyHook() {
  if (gKeyboardHook) {
    UnhookWindowsHookEx(gKeyboardHook);
    gKeyboardHook = NULL;
  }
}

scInternal LRESULT CALLBACK
_scLLKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && gUI.iEditingHotkey != -1 && gUI.hWindow) {
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
      KBDLLHOOKSTRUCT* pKb = (KBDLLHOOKSTRUCT*)lParam;
      UINT vk = pKb->vkCode;

      bool bIsModifier = (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                          vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                          vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                          vk == VK_LWIN || vk == VK_RWIN);

      if (!bIsModifier) {
        u32 uMods = 0;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) uMods |= MOD_WIN;
        if (GetKeyState(VK_CONTROL) & 0x8000) uMods |= MOD_CONTROL;
        if (GetKeyState(VK_SHIFT)   & 0x8000) uMods |= MOD_SHIFT;
        if ((pKb->flags & LLKHF_ALTDOWN) || (GetAsyncKeyState(VK_MENU) & 0x8000)) uMods |= MOD_ALT;

        PostMessageW(gUI.hWindow, WM_HOTKEY_RECORDED, (WPARAM)vk, (LPARAM)uMods);
        return 1; // swallow
      }
    }
  }
  return CallNextHookEx(gKeyboardHook, nCode, wParam, lParam);
}

scInternal void
_scBeginHotkeyRecording(s32 iIndex) {
  if (gUI.iEditingHotkey != -1) {
    _scCancelHotkeyRecording();
  }

  for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    scHotkey* pHk = &gApp->config.aHotkeys[i];
    if (pHk->bRegistered) {
      UnregisterHotKey(NULL, pHk->eID);
      pHk->bRegistered = false;
    }
  }

  gUI.iEditingHotkey = iIndex;
  SetFocus(gUI.hWindow);
  gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, _scLLKeyboardProc, GetModuleHandleW(NULL), 0);
}

scInternal void
_scCancelHotkeyRecording() {
  _scStopHotkeyHook();
  gUI.iEditingHotkey = -1;
  scAppRegisterHotkeys();
  scAppSetupCallbackHandler();
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
      gUI.bNeedsLayout = true;
      InvalidateRect(hWnd, NULL, TRUE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      RECT cr = _scEnsureLayout();
      if (_scHandleMouseMove(pt, cr)) {
        InvalidateRect(hWnd, NULL, FALSE);
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, HOVER_DEFAULT };
        TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE: {
      gUI.eHoveredTab    = SC_PAGE_NONE;
      gUI.iHoveredWidget = -1;
      gUI.bHoveredScroll = false;
      InvalidateRect(hWnd, NULL, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      if (gUI.eCurrentPage == SC_PAGE_GALLERY) {
        scPage* pPage = &gUI.aPages[SC_PAGE_GALLERY];
        if (pPage->scroll.nMaxScrollY > 0) {
          s32 iDelta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
          pPage->scroll.nScrollY = _scClamp(pPage->scroll.nScrollY - iDelta * 40, 0, pPage->scroll.nMaxScrollY);
          InvalidateRect(hWnd, NULL, FALSE);
        }
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      RECT cr = _scEnsureLayout();
      _scHandleLeftDown(pt, cr);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (gUI.bDraggingScroll) {
        gUI.bDraggingScroll = false;
        ReleaseCapture();
        InvalidateRect(hWnd, NULL, FALSE);
      }
      return 0;
    }
    case WM_CAPTURECHANGED: {
      gUI.bDraggingScroll = false;
      return 0;
    }
    case WM_HOTKEY_RECORDED: {
      if (gUI.iEditingHotkey == -1) {
        return 0;
      }
      UINT vk    = (UINT)wParam;
      u32  uMods = (u32)lParam;
      _scStopHotkeyHook();

      scHotkey* pHk = &gApp->config.aHotkeys[gUI.iEditingHotkey];
      if (vk == VK_DELETE) {
        pHk->uKey       = 0;
        pHk->uModifiers = 0;
        scSaveConfig();
      } else if (vk != VK_ESCAPE) {
        pHk->uKey       = vk;
        pHk->uModifiers = uMods;
        scSaveConfig();
      }

      gUI.iEditingHotkey = -1;
      scAppRegisterHotkeys();
      scAppSetupCallbackHandler();
      InvalidateRect(hWnd, NULL, FALSE);
      return 0;
    }
    case WM_ACTIVATE: {
      if (LOWORD(wParam) == WA_INACTIVE && gUI.iEditingHotkey != -1) {
        _scCancelHotkeyRecording();
        InvalidateRect(hWnd, NULL, FALSE);
      }
      return 0;
    }
    case WM_SETTINGCHANGE: {
      if (gApp->bIsGeWin10 && lParam && wcscmp((const wchar_t*)lParam, L"ImmersiveColorSet") == 0) {
        _scUISetupTheme();
        BOOL bDark = gUI.theme.bDark;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &bDark, sizeof(bDark));
        InvalidateRect(hWnd, NULL, TRUE);
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
_scUISetupTheme() {
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
  scPage* pGeneral = &gUI.aPages[SC_PAGE_GENERAL];
  wcscpy_s(pGeneral->wszName, 64, L"General");
  pGeneral->pfnLayout = _scLayoutGeneral;

  scPage* pSettings = &gUI.aPages[SC_PAGE_SETTINGS];
  wcscpy_s(pSettings->wszName, 64, L"Settings");
  pSettings->pfnLayout = _scLayoutSettings;

  scPage* pGallery = &gUI.aPages[SC_PAGE_GALLERY];
  wcscpy_s(pGallery->wszName, 64, L"Gallery");
  pGallery->pfnLayout = _scLayoutGallery;
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

    gUI.eCurrentPage   = SC_PAGE_GENERAL;
    gUI.eHoveredTab    = SC_PAGE_NONE;
    gUI.iHoveredWidget = -1;
    gUI.iEditingHotkey = -1;
  }

  _scSetupPages();

  gUI.fUIScale = _scGetDPIScale();
  gUI.bNeedsLayout = true;
  _scUISetupTheme();

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

    if (gApp->bIsGeWin10) {
      BOOL bDark = gUI.theme.bDark;
      DwmSetWindowAttribute(gUI.hWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &bDark, sizeof(bDark));
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
  if (gUI.iEditingHotkey != -1) {
    _scCancelHotkeyRecording();
  }

  _scThumbCacheClear();
  for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
    free(gUI.aPages[i].aWidgets);
    gUI.aPages[i].aWidgets     = NULL;
    gUI.aPages[i].nWidgetCount = 0;
    gUI.aPages[i].nWidgetCap   = 0;
  }

  if (gUI.hWindow) {
    gUI.hWindow = NULL;
    scLogDebug("Destroyed main window");
  }
}

void scUISetCurrentPage(scPageID ePageID) {
  scLogDebug("Changing current page from %d to %d", (int)gUI.eCurrentPage, (int)ePageID);

  if (gUI.iEditingHotkey != -1) {
    _scCancelHotkeyRecording();
  }

  gUI.eCurrentPage   = ePageID;
  gUI.iHoveredWidget = -1;
  gUI.bNeedsLayout   = true;
  if (ePageID == SC_PAGE_GALLERY) {
    gUI.aPages[ePageID].scroll.nScrollY = 0;
  }

  if (gUI.hWindow) {
    InvalidateRect(gUI.hWindow, NULL, TRUE);
  }
}

void scUIOnCaptureSaved(const wchar_t* wszPath) {
  (void)wszPath;
  if (gUI.hWindow && gUI.eCurrentPage == SC_PAGE_GALLERY) {
    gUI.bNeedsLayout = true;
    InvalidateRect(gUI.hWindow, NULL, FALSE);
  }
}
