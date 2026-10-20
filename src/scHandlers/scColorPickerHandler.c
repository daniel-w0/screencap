#include "pch.h"
#include "scApp.h"
#include "scLogging.h"
#include "scGdiPlus.h"
#include "scUITheme.h"
#include "scGui.h"

#include <windowsx.h>

#define WM_SC_COLOR_PICKER_START_PICK (WM_APP + 0x3A0)
#define WM_SC_COLOR_PICKER_ZOOM (WM_APP + 0x3A1)
#define WM_SC_COLOR_PICKER_LENS_CONTROL (WM_APP + 0x3A2)
#define SC_COLOR_PICKER_CLASS L"ScColorPickerWindow"
#define SC_COLOR_PICKER_PREVIEW_CLASS L"ScColorPickerPreviewWindow"
#define SC_COLOR_PICKER_WIDTH 500
#define SC_COLOR_PICKER_HEIGHT 330
#define SC_COLOR_PICKER_TITLEBAR_HEIGHT 38
#define SC_COLOR_PICKER_HISTORY 8
#define SC_COLOR_PICKER_ROWS 4
#define SC_COLOR_PICKER_PREVIEW_WIDTH 148
#define SC_COLOR_PICKER_PREVIEW_HEIGHT 46
#define SC_COLOR_PICKER_MAGNIFIER_CELLS 18
#define SC_COLOR_PICKER_ZOOM_MAX 10

scGlobal scUITheme gColorPickerTheme;
scGlobal bool gColorPickerThemeReady;
scGlobal f32 gColorPickerThemeScale;
scGlobal bool gColorPickerClassRegistered;
scGlobal bool gColorPickerPreviewClassRegistered;
scGlobal ULONG_PTR gColorPickerGdiPlusToken;
scGlobal HHOOK gColorPickerMouseHook;
scGlobal HHOOK gColorPickerKeyboardHook;
scGlobal struct scColorPickerContextTag* gColorPickerContext;

#define SC_PICKER_BG          (gColorPickerTheme.dwBackground)
#define SC_PICKER_PANEL       (gColorPickerTheme.dwCard)
#define SC_PICKER_BORDER      (gColorPickerTheme.dwStroke)
#define SC_PICKER_TEXT        (gColorPickerTheme.dwText)
#define SC_PICKER_ACCENT      (gColorPickerTheme.dwAccent)
#define SC_PICKER_STROKE_SOFT (gColorPickerTheme.dwStrokeSoft)
#define SC_PICKER_CLOSE_HOVER (gColorPickerTheme.dwError)
#define SC_PICKER_MUTED       (gColorPickerTheme.dwTextDim)

typedef struct scColorPickerContextTag {
  HWND hWnd;
  HWND hPreviewWnd;
  scGui* pGui;
  scGui* pPreviewGui;
  scCaptureContext* pCapture;
  COLORREF color;
  COLORREF previewColor;
  COLORREF history[SC_COLOR_PICKER_HISTORY];
  s32 nHistory;
  bool bPicking;
  bool bHasColor;
  bool bHasPreviewColor;
  POINT ptPreview;
  POINT ptCursor;
  POINT ptLensOrigin;
  POINT ptLensCell;
  bool bLensControl;
  int iZoom;
  int iPreviewWidth;
  int iPreviewHeight;
  int iPreviewBuiltZoom;
  bool bHasPreviewPosition;
} scColorPickerContext;

typedef struct {
  wchar_t wszHex[16];
  wchar_t wszRgb[64];
  wchar_t wszHsl[64];
  wchar_t wszVec4[80];
} scColorStrings;

void _getColorPickerWindows(HWND* h0, HWND* h1) {
  scColorPickerContext* ctx = gColorPickerContext;
  if (ctx) {
    *h0 = ctx->hWnd;
    *h1 = ctx->hPreviewWnd;
  } else {
    *h0 = NULL;
    *h1 = NULL;
  }
}

scInternal LRESULT CALLBACK
_scPickerLowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL && gColorPickerContext &&
      gColorPickerContext->bPicking) {
    HWND hTarget = gColorPickerContext->hWnd && IsWindow(gColorPickerContext->hWnd)
      ? gColorPickerContext->hWnd : gColorPickerContext->hPreviewWnd;
    if (hTarget && IsWindow(hTarget)) {
      MSLLHOOKSTRUCT* mouse = (MSLLHOOKSTRUCT*)lParam;
      int delta = (int)(SHORT)HIWORD(mouse->mouseData);
      if (delta) PostMessageW(hTarget, WM_SC_COLOR_PICKER_ZOOM, (WPARAM)(INT_PTR)delta, 0);
    }
  }
  return CallNextHookEx(gColorPickerMouseHook, nCode, wParam, lParam);
}

scInternal LRESULT CALLBACK
_scPickerLowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && gColorPickerContext && gColorPickerContext->bPicking) {
    bool bKeyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    bool bKeyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    KBDLLHOOKSTRUCT* key = (KBDLLHOOKSTRUCT*)lParam;
    if ((bKeyDown || bKeyUp) &&
        (key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT)) {
      HWND hTarget = gColorPickerContext->hPreviewWnd && IsWindow(gColorPickerContext->hPreviewWnd)
        ? gColorPickerContext->hPreviewWnd : gColorPickerContext->hWnd;
      if (hTarget && IsWindow(hTarget)) {
        PostMessageW(hTarget, WM_SC_COLOR_PICKER_LENS_CONTROL, bKeyDown ? TRUE : FALSE, (LPARAM)key->vkCode);
      }
    }
  }
  return CallNextHookEx(gColorPickerKeyboardHook, nCode, wParam, lParam);
}

scInternal void
_scPickerStartMouseHook(void) {
  if (!gColorPickerMouseHook) {
    gColorPickerMouseHook = SetWindowsHookExW(WH_MOUSE_LL, _scPickerLowLevelMouseProc,
                                               GetModuleHandleW(NULL), 0);
    if (!gColorPickerMouseHook) {
      scLogError("Failed to install color picker mouse wheel hook: %lu", GetLastError());
    }
  }
  if (!gColorPickerKeyboardHook) {
    gColorPickerKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, _scPickerLowLevelKeyboardProc,
                                                  GetModuleHandleW(NULL), 0);
    if (!gColorPickerKeyboardHook) {
      scLogError("Failed to install color picker keyboard hook: %lu", GetLastError());
    }
  }
}

scInternal void
_scPickerStopMouseHook(void) {
  if (gColorPickerMouseHook) {
    UnhookWindowsHookEx(gColorPickerMouseHook);
    gColorPickerMouseHook = NULL;
  }
  if (gColorPickerKeyboardHook) {
    UnhookWindowsHookEx(gColorPickerKeyboardHook);
    gColorPickerKeyboardHook = NULL;
  }
  if (gColorPickerContext) gColorPickerContext->bLensControl = false;
}

scInternal void
_scPickerDestroyTheme(void) {
  if (gColorPickerTheme.pFont) DeleteObject(gColorPickerTheme.pFont);
  if (gColorPickerTheme.pBoldFont) DeleteObject(gColorPickerTheme.pBoldFont);
  if (gColorPickerTheme.hBackgroundBrush) DeleteObject(gColorPickerTheme.hBackgroundBrush);
  if (gColorPickerTheme.hCardBrush) DeleteObject(gColorPickerTheme.hCardBrush);
  memset(&gColorPickerTheme, 0, sizeof(gColorPickerTheme));
  gColorPickerThemeReady = false;
}

scInternal void
_scPickerApplyGuiTheme(scGui* pGui) {
  if (!pGui) return;

  scUITheme* t = &gColorPickerTheme;
  scGuiTheme theme = { 0 };
  theme.dwText        = t->dwText;
  theme.dwTextDim     = t->dwTextDim;
  theme.dwTextFaint   = t->dwTextFaint;
  theme.dwBackground  = t->dwBackground;
  theme.dwPanel       = t->dwCard;
  theme.dwPopup       = t->dwPopup;
  theme.dwCard        = t->dwCard;
  theme.dwCardHover   = t->dwCardHover;
  theme.dwCardActive  = t->dwCardActive;
  theme.dwStroke      = t->dwStroke;
  theme.dwStrokeSoft  = t->dwStrokeSoft;
  theme.dwAccent      = t->dwAccent;
  theme.dwAccentHover = t->dwAccentHover;
  theme.dwAccentText  = RGB(255, 255, 255);
  theme.dwPillOff     = t->dwPillOff;
  theme.dwScrollThumb = t->dwScrollThumb;
  theme.hFont         = (HFONT)t->pFont;
  theme.hBoldFont     = (HFONT)t->pBoldFont;
  scGuiSetTheme(pGui, &theme);
}

scInternal void
_scPickerRefreshTheme(HWND hWnd) {
  f32 fUser = gApp->config.fGuiScale > 0.0f ? gApp->config.fGuiScale : 1.0f;
  f32 scale = scGetDPIScale(hWnd) * fUser;

  bool dark = scIsWindowsDarkTheme();
  COLORREF accent = scGetSystemAccentColor();

  if (gColorPickerThemeReady &&
      gColorPickerTheme.bDark == dark &&
      gColorPickerTheme.dwAccent == accent &&
      gColorPickerThemeScale == scale) {
    return;
  }

  _scPickerDestroyTheme();
  s32 fontScale = (s32)(15.0f * scale + 0.5f);
  scUIThemeGet(&gColorPickerTheme, fontScale);
  gColorPickerThemeScale = scale;
  gColorPickerThemeReady = true;

  if (gColorPickerContext) {
    _scPickerApplyGuiTheme(gColorPickerContext->pGui);
    _scPickerApplyGuiTheme(gColorPickerContext->pPreviewGui);
  }
}

scInternal bool
_scPickerEnsureGdiPlus(void) {
  if (!gColorPickerGdiPlusToken) {
    gpStartup(&gColorPickerGdiPlusToken);
  }
  return gColorPickerGdiPlusToken != 0;
}

scInternal void
_scPickerColorToHsl(COLORREF color, float* h, float* s, float* l) {
  float r = GetRValue(color) / 255.0f;
  float g = GetGValue(color) / 255.0f;
  float b = GetBValue(color) / 255.0f;
  float maxv = r > g ? (r > b ? r : b) : (g > b ? g : b);
  float minv = r < g ? (r < b ? r : b) : (g < b ? g : b);
  float d = maxv - minv;

  *l = (maxv + minv) * 0.5f;
  *h = 0.0f;
  *s = 0.0f;

  if (d == 0.0f) {
    return;
  }

  *s = *l > 0.5f ? d / (2.0f - maxv - minv) : d / (maxv + minv);
  if (maxv == r) {
    *h = (g - b) / d + (g < b ? 6.0f : 0.0f);
  } else if (maxv == g) {
    *h = (b - r) / d + 2.0f;
  } else {
    *h = (r - g) / d + 4.0f;
  }
  *h *= 60.0f;
}

scInternal void
_scPickerFormatColor(COLORREF color, scColorStrings* out) {
  float h, s, l;
  float r = GetRValue(color) / 255.0f;
  float g = GetGValue(color) / 255.0f;
  float b = GetBValue(color) / 255.0f;

  _scPickerColorToHsl(color, &h, &s, &l);
  swprintf_s(out->wszHex, ARRAYSIZE(out->wszHex), L"#%02X%02X%02X",
             (unsigned int)GetRValue(color),
             (unsigned int)GetGValue(color),
             (unsigned int)GetBValue(color));
  swprintf_s(out->wszRgb, ARRAYSIZE(out->wszRgb), L"rgb(%u, %u, %u)",
             (unsigned int)GetRValue(color),
             (unsigned int)GetGValue(color),
             (unsigned int)GetBValue(color));
  swprintf_s(out->wszHsl, ARRAYSIZE(out->wszHsl), L"hsl(%d, %d%%, %d%%)",
             (int)(h + 0.5f), (int)(s * 100.0f + 0.5f),
             (int)(l * 100.0f + 0.5f));
  swprintf_s(out->wszVec4, ARRAYSIZE(out->wszVec4), L"(%.3f, %.3f, %.3f, 1.000)", r, g, b);
}

scInternal void
_scPickerPushHistory(scColorPickerContext* ctx, COLORREF color) {
  if (!ctx) return;

  for (s32 i = 0; i < ctx->nHistory; ++i) {
    if (ctx->history[i] == color) {
      for (s32 j = i; j > 0; --j) ctx->history[j] = ctx->history[j - 1];
      ctx->history[0] = color;
      ctx->color = color;
      ctx->bHasColor = true;
      return;
    }
  }

  s32 last = ctx->nHistory < SC_COLOR_PICKER_HISTORY ? ctx->nHistory : SC_COLOR_PICKER_HISTORY - 1;
  for (s32 i = last; i > 0; --i) ctx->history[i] = ctx->history[i - 1];
  ctx->history[0] = color;
  if (ctx->nHistory < SC_COLOR_PICKER_HISTORY) ++ctx->nHistory;
  ctx->color = color;
  ctx->bHasColor = true;
}

scInternal void
_scPickerCopyText(HWND hWnd, const wchar_t* text) {
  if (!text) return;

  SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!hMem) return;

  void* pMem = GlobalLock(hMem);
  if (!pMem) {
    GlobalFree(hMem);
    return;
  }

  memcpy(pMem, text, bytes);
  GlobalUnlock(hMem);

  if (!OpenClipboard(hWnd)) {
    GlobalFree(hMem);
    return;
  }

  EmptyClipboard();
  if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
    GlobalFree(hMem);
  }
  CloseClipboard();
}

scInternal void
_scPickerFill(HDC hDC, RECT r, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  if (!brush) return;
  FillRect(hDC, &r, brush);
  DeleteObject(brush);
}

scInternal void
_scPickerText(HDC hDC, const wchar_t* text, RECT r, COLORREF color, UINT format, HFONT font) {
  HGDIOBJ oldFont = SelectObject(hDC, font);
  SetTextColor(hDC, color);
  SetBkMode(hDC, TRANSPARENT);
  DrawTextW(hDC, text, -1, &r, format | DT_NOPREFIX);
  SelectObject(hDC, oldFont);
}

scInternal void
_scPickerDrawSwatch(GpGraphics* graphics, int cx, int cy, int diameter, COLORREF color, bool selected) {
  float d = (float)diameter;
  float x = (float)cx - d * 0.5f;
  float y = (float)cy - d * 0.5f;
  gpFillEllipse(graphics, x - 2.0f, y - 2.0f, d + 4.0f, d + 4.0f,
                gpColor(selected ? SC_PICKER_TEXT : SC_PICKER_BORDER, 255));
  gpFillEllipse(graphics, x, y, d, d, gpColor(color, 255));
}

scInternal void
_scPickerHidePreview(scColorPickerContext* ctx);

typedef struct {
  scColorPickerContext* pCtx;
  s32                   iIndex;
} scPickerSwatchData;

scInternal void
_scPickerHostInvalidateMain(void* pUser, const RECT* pRect) {
  scColorPickerContext* ctx = (scColorPickerContext*)pUser;
  if (ctx->hWnd && IsWindow(ctx->hWnd)) InvalidateRect(ctx->hWnd, pRect, FALSE);
}

scInternal void
_scPickerHostCaptureMain(void* pUser, bool bCapture) {
  scColorPickerContext* ctx = (scColorPickerContext*)pUser;
  if (!ctx->hWnd || !IsWindow(ctx->hWnd)) return;
  if (bCapture) {
    SetCapture(ctx->hWnd);
  } else if (GetCapture() == ctx->hWnd) {
    ReleaseCapture();
  }
}

scInternal void
_scPickerHostInvalidatePreview(void* pUser, const RECT* pRect) {
  scColorPickerContext* ctx = (scColorPickerContext*)pUser;
  if (ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) InvalidateRect(ctx->hPreviewWnd, pRect, FALSE);
}

scInternal void
_scPickerOnTitleDrag(scNode* pNode) {
  (void)pNode;
  if (gColorPickerContext && gColorPickerContext->hWnd) {
    ReleaseCapture();
    SendMessageW(gColorPickerContext->hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
  }
}

scInternal void
_scPickerOnCloseClicked(scNode* pNode) {
  (void)pNode;
  if (gColorPickerContext && gColorPickerContext->hWnd) ShowWindow(gColorPickerContext->hWnd, SW_HIDE);
}

scInternal void
_scPickerOnPickClicked(s32 iParam) {
  (void)iParam;
  scColorPickerContext* ctx = gColorPickerContext;
  if (!ctx || !ctx->hWnd) return;

  ctx->bPicking = true;
  _scPickerHidePreview(ctx);
  ShowWindow(ctx->hWnd, SW_HIDE);
  if (!PostMessageW(ctx->hWnd, WM_SC_COLOR_PICKER_START_PICK, 0, 0)) {
    ctx->bPicking = false;
    ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(ctx->hWnd);
    scLogError("Failed to queue color picker capture request: %lu", GetLastError());
  }
}

scInternal void _scPickerRebuildMain(scColorPickerContext* ctx);

scInternal void
_scPickerOnSwatchClicked(scNode* pNode) {
  scPickerSwatchData* pData = (scPickerSwatchData*)scGetUserData(pNode);
  scColorPickerContext* ctx = pData->pCtx;
  ctx->color = ctx->history[pData->iIndex];
  ctx->bHasColor = true;
  _scPickerRebuildMain(ctx);
}

scInternal void
_scPickerOnCopyClicked(s32 iRow) {
  scColorPickerContext* ctx = gColorPickerContext;
  if (!ctx || iRow < 0 || iRow >= SC_COLOR_PICKER_ROWS) return;

  scColorStrings strings;
  _scPickerFormatColor(ctx->color, &strings);
  const wchar_t* values[SC_COLOR_PICKER_ROWS] = { strings.wszHex, strings.wszRgb, strings.wszHsl, strings.wszVec4 };
  _scPickerCopyText(ctx->hWnd, values[iRow]);
}

scInternal void
_scPickerDrawTitle(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  HFONT hFont = gColorPickerTheme.pBoldFont;
  if (!hFont) return;
  RECT rcText = { r.left + 18, r.top, r.right, r.bottom };
  _scPickerText(hDC, L"Color Picker", rcText, SC_PICKER_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hFont);
}

scInternal void
_scPickerDrawClose(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  if (bHovered) {
    GpGraphics* graphics = gpGraphicsBegin(hDC);
    gpFillRound(graphics, r, 5.0f, gpColor(SC_PICKER_CLOSE_HOVER, 255));
    gpGraphicsEnd(graphics);
  }

  int cx = (r.left + r.right) / 2;
  int cy = (r.top + r.bottom) / 2;
  HPEN pen = CreatePen(PS_SOLID, 2, bHovered ? RGB(255, 255, 255) : SC_PICKER_MUTED);
  if (pen) {
    HGDIOBJ oldPen = SelectObject(hDC, pen);
    MoveToEx(hDC, cx - 5, cy - 5, NULL);
    LineTo(hDC, cx + 5, cy + 5);
    MoveToEx(hDC, cx + 5, cy - 5, NULL);
    LineTo(hDC, cx - 5, cy + 5);
    SelectObject(hDC, oldPen);
    DeleteObject(pen);
  }
}

scInternal void
_scPickerDrawCurrent(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scColorPickerContext* ctx = *(scColorPickerContext**)scGetUserData(pNode);
  RECT current = { r.left, r.top + 3, r.right, r.top + 39 };

  GpGraphics* graphics = gpGraphicsBegin(hDC);
  gpFillEllipse(graphics, (float)current.left, (float)current.top,
                (float)(current.right - current.left), (float)(current.bottom - current.top),
                gpColor(SC_PICKER_BORDER, 255));
  gpFillEllipse(graphics, (float)(current.left + 2), (float)(current.top + 2),
                (float)(current.right - current.left - 4), (float)(current.bottom - current.top - 4),
                gpColor(ctx->color, 255));
  gpGraphicsEnd(graphics);
}

scInternal void
_scPickerDrawSwatchNode(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scPickerSwatchData* pData = (scPickerSwatchData*)scGetUserData(pNode);
  scColorPickerContext* ctx = pData->pCtx;
  COLORREF color = ctx->history[pData->iIndex];

  GpGraphics* graphics = gpGraphicsBegin(hDC);
  _scPickerDrawSwatch(graphics, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 19, color, color == ctx->color);
  gpGraphicsEnd(graphics);
}

scInternal const scCustomOps kPickerTitleOps   = { _scPickerDrawTitle,      NULL,                  _scPickerOnTitleDrag, NULL };
scInternal const scCustomOps kPickerCloseOps   = { _scPickerDrawClose,      _scPickerOnCloseClicked, NULL,               NULL };
scInternal const scCustomOps kPickerCurrentOps = { _scPickerDrawCurrent,    NULL,                  NULL,                 NULL };
scInternal const scCustomOps kPickerSwatchOps  = { _scPickerDrawSwatchNode, _scPickerOnSwatchClicked, NULL,              NULL };

scInternal void
_scPickerRebuildMain(scColorPickerContext* ctx) {
  if (!ctx || !ctx->pGui) return;

  scColorStrings strings;
  _scPickerFormatColor(ctx->color, &strings);

  scNode* pRoot = scCreateVStack(scLengthPercent(1.0f), scLengthPercent(1.0f));
  scSetOutline(pRoot, 7.0f);

  scNode* pTitle = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(SC_COLOR_PICKER_TITLEBAR_HEIGHT - 1));
  scSetPaddingEx(pTitle, 0, 0, 4, 0);
  scSetAlignment(pTitle, SC_ALIGN_MIDDLE);
  scAddChild(pTitle, scCreateCustom(scLengthStretch(1.0f), scLengthPercent(1.0f), &kPickerTitleOps, NULL, 0));
  scAddChild(pTitle, scCreateCustom(scLengthPixels(44), scLengthPixels(30), &kPickerCloseOps, NULL, 0));
  scAddChild(pRoot, pTitle);
  scAddChild(pRoot, scCreateDivider());

  scNode* pTop = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(68));
  scSetPaddingEx(pTop, 18, 16, 18, 0);
  scSetSpacing(pTop, 8);
  scNode* pPick = scCreateButton(L"Pick", _scPickerOnPickClicked, 0);
  scSetSize(pPick, scLengthPixels(86), scLengthPixels(42));
  scAddChild(pTop, pPick);
  scAddChild(pTop, scCreateCustom(scLengthPixels(36), scLengthPixels(42), &kPickerCurrentOps, &ctx, sizeof(ctx)));

  scNode* pInfo = scCreateVStack(scLengthStretch(1.0f), scLengthAuto());
  scSetPaddingEx(pInfo, 2, 0, 0, 0);
  scSetSpacing(pInfo, 4);
  scNode* pCurrentLabel = scCreateLabel(L"CURRENT COLOUR", true);
  scSetTextRole(pCurrentLabel, SC_TEXT_DIM);
  scAddChild(pInfo, pCurrentLabel);
  scAddChild(pInfo, scCreateLabel(strings.wszHex, false));
  scAddChild(pTop, pInfo);
  scAddChild(pRoot, pTop);
  scAddChild(pRoot, scCreateDivider());

  scNode* pRecent = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(38));
  scSetPaddingEx(pRecent, 18, 0, 18, 0);
  scSetSpacing(pRecent, 12);
  scSetAlignment(pRecent, SC_ALIGN_MIDDLE);
  scNode* pRecentLabel = scCreateLabel(L"Recent Colors", true);
  scSetTextRole(pRecentLabel, SC_TEXT_DIM);
  scSetSize(pRecentLabel, scLengthPixels(107), scLengthAuto());
  scAddChild(pRecent, pRecentLabel);
  for (s32 i = 0; i < ctx->nHistory; ++i) {
    scPickerSwatchData data = { ctx, i };
    scAddChild(pRecent, scCreateCustom(scLengthPixels(26), scLengthPixels(26), &kPickerSwatchOps, &data, sizeof(data)));
  }
  scAddChild(pRoot, pRecent);
  scAddChild(pRoot, scCreateSpacer(scLengthPixels(1), scLengthPixels(7)));

  const wchar_t* labels[SC_COLOR_PICKER_ROWS] = { L"HEX", L"RGB", L"HSL", L"VEC4" };
  const wchar_t* values[SC_COLOR_PICKER_ROWS] = { strings.wszHex, strings.wszRgb, strings.wszHsl, strings.wszVec4 };
  scNode* pRows = scCreateVStack(scLengthPercent(1.0f), scLengthAuto());
  scSetPaddingEx(pRows, 18, 0, 18, 0);
  scSetSpacing(pRows, 9);
  for (s32 i = 0; i < SC_COLOR_PICKER_ROWS; ++i) {
    scNode* pRow = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(42));
    scSetCard(pRow, 6.0f);
    scSetPaddingEx(pRow, 12, 5, 8, 5);
    scSetSpacing(pRow, 12);
    scSetAlignment(pRow, SC_ALIGN_MIDDLE);

    scNode* pLabel = scCreateLabel(labels[i], false);
    scSetTextRole(pLabel, SC_TEXT_DIM);
    scSetSize(pLabel, scLengthPixels(54), scLengthAuto());
    scAddChild(pRow, pLabel);

    scNode* pValue = scCreateLabel(values[i], false);
    scSetEllipsis(pValue, true);
    scSetSize(pValue, scLengthStretch(1.0f), scLengthAuto());
    scAddChild(pRow, pValue);

    scNode* pCopy = scCreateButton(L"Copy", _scPickerOnCopyClicked, i);
    scSetSize(pCopy, scLengthPixels(62), scLengthPixels(32));
    scAddChild(pRow, pCopy);
    scAddChild(pRows, pRow);
  }
  scAddChild(pRoot, pRows);

  scGuiSetRoot(ctx->pGui, pRoot);
}

scInternal void
_scPickerPaintContent(scColorPickerContext* ctx, HWND hWnd, HDC hDC, const RECT* pClip) {
  RECT client;
  GetClientRect(hWnd, &client);
  _scPickerFill(hDC, client, SC_PICKER_BG);
  scGuiPaint(ctx->pGui, hDC, pClip);
}

scInternal void
_scPickerPaint(scColorPickerContext* ctx, HWND hWnd, HDC hDC, const RECT* pClip) {
  RECT client;
  GetClientRect(hWnd, &client);
  int width = client.right - client.left;
  int height = client.bottom - client.top;
  if (width <= 0 || height <= 0) return;

  HDC hMemDC = CreateCompatibleDC(hDC);
  HBITMAP hBitmap = hMemDC ? CreateCompatibleBitmap(hDC, width, height) : NULL;
  if (!hMemDC || !hBitmap) {
    if (hBitmap) DeleteObject(hBitmap);
    if (hMemDC) DeleteDC(hMemDC);
    _scPickerPaintContent(ctx, hWnd, hDC, pClip);
    return;
  }

  HGDIOBJ hOldBitmap = SelectObject(hMemDC, hBitmap);
  _scPickerPaintContent(ctx, hWnd, hMemDC, pClip);
  BitBlt(hDC, 0, 0, width, height, hMemDC, 0, 0, SRCCOPY);
  SelectObject(hMemDC, hOldBitmap);
  DeleteObject(hBitmap);
  DeleteDC(hMemDC);
}

scInternal bool
_scPickerSample(scCaptureContext* pCtx, POINT pt, COLORREF* outColor) {
  if (!pCtx || !pCtx->hFrozenDC || !outColor) return false;

  int x = pt.x - pCtx->vCaptureRegion.x;
  int y = pt.y - pCtx->vCaptureRegion.y;
  int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  if (x < 0 || y < 0 || x >= width || y >= height) return false;

  COLORREF color = GetPixel(pCtx->hFrozenDC, x, y);
  if (color == CLR_INVALID) return false;

  *outColor = color;
  return true;
}

scInternal void
_scPickerHidePreview(scColorPickerContext* ctx) {
  if (ctx && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) {
    ShowWindow(ctx->hPreviewWnd, SW_HIDE);
  }
}

scInternal void _scPickerHandleZoom(scColorPickerContext* ctx, int delta);
scInternal void _scPickerSetLensControl(scColorPickerContext* ctx, bool bEnable);

scInternal int
_scPickerMagnifierCellSize(const scColorPickerContext* ctx) {
  return 5 + ctx->iZoom * 2;
}

scInternal void
_scPickerGetPreviewSize(const scColorPickerContext* ctx, int* width, int* height) {
  if (ctx->iZoom <= 0) {
    *width = SC_COLOR_PICKER_PREVIEW_WIDTH;
    *height = SC_COLOR_PICKER_PREVIEW_HEIGHT;
    return;
  }

  int lensSide = SC_COLOR_PICKER_MAGNIFIER_CELLS * _scPickerMagnifierCellSize(ctx);
  *width = lensSide + 12;
  if (*width < SC_COLOR_PICKER_PREVIEW_WIDTH) *width = SC_COLOR_PICKER_PREVIEW_WIDTH;
  *height = lensSide + 54;
}

scInternal void
_scPickerSetPreviewRegion(HWND hWnd, int width, int height) {
  HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
  if (region && !SetWindowRgn(hWnd, region, TRUE)) DeleteObject(region);
}

scInternal void
_scPickerDrawMagnifier(scColorPickerContext* ctx, HDC hDC, RECT lens) {
  if (!ctx || ctx->iZoom <= 0) return;

  int cellSize = _scPickerMagnifierCellSize(ctx);
  int lensSide = SC_COLOR_PICKER_MAGNIFIER_CELLS * cellSize;
  int left = lens.left;
  int top = lens.top;
  _scPickerFill(hDC, lens, SC_PICKER_PANEL);

  if (ctx->pCapture && ctx->pCapture->hFrozenDC) {
    int sourceX = (ctx->bLensControl ? ctx->ptLensOrigin.x : ctx->ptCursor.x - SC_COLOR_PICKER_MAGNIFIER_CELLS / 2)
      - ctx->pCapture->vCaptureRegion.x;
    int sourceY = (ctx->bLensControl ? ctx->ptLensOrigin.y : ctx->ptCursor.y - SC_COLOR_PICKER_MAGNIFIER_CELLS / 2)
      - ctx->pCapture->vCaptureRegion.y;
    int oldStretchMode = SetStretchBltMode(hDC, COLORONCOLOR);
    StretchBlt(hDC, left, top, lensSide, lensSide,
               ctx->pCapture->hFrozenDC, sourceX, sourceY,
               SC_COLOR_PICKER_MAGNIFIER_CELLS, SC_COLOR_PICKER_MAGNIFIER_CELLS, SRCCOPY);
    SetStretchBltMode(hDC, oldStretchMode);
  }

  HPEN gridPen = CreatePen(PS_SOLID, 1, SC_PICKER_STROKE_SOFT);
  if (gridPen) {
    HGDIOBJ oldPen = SelectObject(hDC, gridPen);
    for (int i = 0; i <= SC_COLOR_PICKER_MAGNIFIER_CELLS; ++i) {
      int offset = i * cellSize;
      MoveToEx(hDC, left + offset, top, NULL);
      LineTo(hDC, left + offset, top + lensSide);
      MoveToEx(hDC, left, top + offset, NULL);
      LineTo(hDC, left + lensSide, top + offset);
    }
    SelectObject(hDC, oldPen);
    DeleteObject(gridPen);
  }

  int selectedCellX = ctx->bLensControl ? (int)ctx->ptLensCell.x : SC_COLOR_PICKER_MAGNIFIER_CELLS / 2;
  int selectedCellY = ctx->bLensControl ? (int)ctx->ptLensCell.y : SC_COLOR_PICKER_MAGNIFIER_CELLS / 2;
  RECT center = {
    left + selectedCellX * cellSize,
    top + selectedCellY * cellSize,
    left + (selectedCellX + 1) * cellSize,
    top + (selectedCellY + 1) * cellSize
  };
  HPEN accentPen = CreatePen(PS_SOLID, 2, SC_PICKER_ACCENT);
  if (accentPen) {
    HGDIOBJ oldPen = SelectObject(hDC, accentPen);
    HGDIOBJ oldBrush = SelectObject(hDC, GetStockObject(NULL_BRUSH));
    Rectangle(hDC, center.left + 1, center.top + 1, center.right, center.bottom);
    SelectObject(hDC, oldBrush);
    SelectObject(hDC, oldPen);
    DeleteObject(accentPen);
  }
}

scInternal void
_scPickerDrawLensNode(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scColorPickerContext* ctx = *(scColorPickerContext**)scGetUserData(pNode);
  _scPickerDrawMagnifier(ctx, hDC, r);
}

scInternal void
_scPickerDrawPreviewSwatch(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scColorPickerContext* ctx = *(scColorPickerContext**)scGetUserData(pNode);
  GpGraphics* graphics = gpGraphicsBegin(hDC);
  gpFillRound(graphics, r, 5.0f, gpColor(ctx->previewColor, 255));
  gpStrokeRound(graphics, r, 5.0f, gpColor(SC_PICKER_BORDER, 255), 1.0f);
  gpGraphicsEnd(graphics);
}

scInternal void
_scPickerDrawPreviewHex(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scColorPickerContext* ctx = *(scColorPickerContext**)scGetUserData(pNode);
  HFONT hFont = gColorPickerTheme.pBoldFont;
  if (!hFont) return;

  wchar_t hex[16];
  swprintf_s(hex, ARRAYSIZE(hex), L"%02x%02x%02x",
             (unsigned int)GetRValue(ctx->previewColor),
             (unsigned int)GetGValue(ctx->previewColor),
             (unsigned int)GetBValue(ctx->previewColor));
  _scPickerText(hDC, hex, r, SC_PICKER_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hFont);
}

scInternal const scCustomOps kPickerLensOps          = { _scPickerDrawLensNode,      NULL, NULL, NULL };
scInternal const scCustomOps kPickerPreviewSwatchOps = { _scPickerDrawPreviewSwatch, NULL, NULL, NULL };
scInternal const scCustomOps kPickerPreviewHexOps    = { _scPickerDrawPreviewHex,    NULL, NULL, NULL };

scInternal void
_scPickerEnsurePreviewTree(scColorPickerContext* ctx) {
  if (!ctx || !ctx->pPreviewGui || ctx->iPreviewBuiltZoom == ctx->iZoom) return;
  ctx->iPreviewBuiltZoom = ctx->iZoom;

  int width, height;
  _scPickerGetPreviewSize(ctx, &width, &height);
  scGuiResize(ctx->pPreviewGui, width, height);

  scNode* pRoot = scCreateVStack(scLengthPercent(1.0f), scLengthPercent(1.0f));
  scSetOutline(pRoot, 8.0f);
  scSetAlignment(pRoot, SC_ALIGN_MIDDLE);

  if (ctx->iZoom > 0) {
    int lensSide = SC_COLOR_PICKER_MAGNIFIER_CELLS * _scPickerMagnifierCellSize(ctx);
    scSetPaddingEx(pRoot, 0, 6, 0, 0);
    scSetSpacing(pRoot, 4);
    scAddChild(pRoot, scCreateCustom(scLengthPixels((f32)lensSide), scLengthPixels((f32)lensSide), &kPickerLensOps, &ctx, sizeof(ctx)));
  }

  scNode* pInfo = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(ctx->iZoom > 0 ? 40.0f : 46.0f));
  scSetPaddingEx(pInfo, 8, 0, 8, 0);
  scSetSpacing(pInfo, 9);
  scSetAlignment(pInfo, SC_ALIGN_MIDDLE);
  scAddChild(pInfo, scCreateCustom(scLengthPixels(28), scLengthPixels(28), &kPickerPreviewSwatchOps, &ctx, sizeof(ctx)));
  scAddChild(pInfo, scCreateCustom(scLengthStretch(1.0f), scLengthPercent(1.0f), &kPickerPreviewHexOps, &ctx, sizeof(ctx)));
  scAddChild(pRoot, pInfo);

  scGuiSetRoot(ctx->pPreviewGui, pRoot);
}

scInternal LRESULT CALLBACK
_scColorPickerPreviewWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  scColorPickerContext* ctx = (scColorPickerContext*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

  switch (uMsg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
      return TRUE;
    }
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETTINGCHANGE:
    case WM_SC_COLOR_PICKER_REFRESH:
    case WM_THEMECHANGED:
      _scPickerRefreshTheme(ctx && ctx->hWnd ? ctx->hWnd : hWnd);
      InvalidateRect(hWnd, NULL, FALSE);
      if (ctx && ctx->hWnd && IsWindow(ctx->hWnd)) InvalidateRect(ctx->hWnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SC_COLOR_PICKER_ZOOM:
      _scPickerHandleZoom(ctx, (int)(INT_PTR)wParam);
      return 0;
    case WM_SC_COLOR_PICKER_LENS_CONTROL:
      if (ctx) {
        bool bShiftDown = wParam != 0;
        if (!wParam) {
          if ((UINT_PTR)lParam == VK_LSHIFT) {
            bShiftDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
          } else if ((UINT_PTR)lParam == VK_RSHIFT) {
            bShiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
          } else {
            bShiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
                         (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
          }
        }
        _scPickerSetLensControl(ctx, bShiftDown && ctx->iZoom > 0);
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hTargetDC = BeginPaint(hWnd, &ps);
      RECT client;
      GetClientRect(hWnd, &client);
      int width = client.right - client.left;
      int height = client.bottom - client.top;
      HDC hMemDC = CreateCompatibleDC(hTargetDC);
      HBITMAP hBitmap = (hMemDC && width > 0 && height > 0) ? CreateCompatibleBitmap(hTargetDC, width, height) : NULL;
      HDC hDC = hTargetDC;
      HGDIOBJ hOldBitmap = NULL;
      if (hMemDC && hBitmap) {
        hOldBitmap = SelectObject(hMemDC, hBitmap);
        hDC = hMemDC;
      }

      _scPickerFill(hDC, client, SC_PICKER_BG);
      if (ctx && ctx->bHasPreviewColor) scGuiPaint(ctx->pPreviewGui, hDC, &ps.rcPaint);

      if (hMemDC && hBitmap) BitBlt(hTargetDC, 0, 0, width, height, hMemDC, 0, 0, SRCCOPY);
      if (hMemDC && hOldBitmap) SelectObject(hMemDC, hOldBitmap);
      if (hBitmap) DeleteObject(hBitmap);
      if (hMemDC) DeleteDC(hMemDC);
      EndPaint(hWnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
      return DefWindowProcW(hWnd, uMsg, wParam, lParam);
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

scInternal HWND
_scColorPickerCreatePreviewWindow(scColorPickerContext* ctx) {
  if (!gColorPickerPreviewClassRegistered) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = _scColorPickerPreviewWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = SC_COLOR_PICKER_PREVIEW_CLASS;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      scLogError("Failed to register color preview window: %lu", GetLastError());
      return NULL;
    }
    gColorPickerPreviewClassRegistered = true;
  }

  if (!ctx->pPreviewGui) {
    scGuiHost host = { 0 };
    host.pUser         = ctx;
    host.pfnInvalidate = _scPickerHostInvalidatePreview;
    ctx->pPreviewGui = scGuiCreate(&host);
    ctx->iPreviewBuiltZoom = -1;
    _scPickerApplyGuiTheme(ctx->pPreviewGui);
  }

  HWND hWnd = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
    SC_COLOR_PICKER_PREVIEW_CLASS,
    L"Color Preview",
    WS_POPUP,
    0, 0, SC_COLOR_PICKER_PREVIEW_WIDTH, SC_COLOR_PICKER_PREVIEW_HEIGHT,
    NULL, NULL, GetModuleHandleW(NULL), ctx
  );

  if (!hWnd) {
    scLogError("Failed to create color preview window: %lu", GetLastError());
    return NULL;
  }

  SetLayeredWindowAttributes(hWnd, 0, 248, LWA_ALPHA);
  _scPickerSetPreviewRegion(hWnd, SC_COLOR_PICKER_PREVIEW_WIDTH, SC_COLOR_PICKER_PREVIEW_HEIGHT);
  return hWnd;
}

scInternal void
_scPickerShowPreview(scColorPickerContext* ctx, POINT pt, bool bColorChanged) {
  if (!ctx->hPreviewWnd || !IsWindow(ctx->hPreviewWnd)) {
    ctx->hPreviewWnd = _scColorPickerCreatePreviewWindow(ctx);
  }
  if (!ctx->hPreviewWnd) return;

  _scPickerEnsurePreviewTree(ctx);
  ctx->ptCursor = pt;
  int previewWidth, previewHeight;
  _scPickerGetPreviewSize(ctx, &previewWidth, &previewHeight);

  HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  RECT work = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
  if (monitor && GetMonitorInfoW(monitor, &mi)) work = mi.rcWork;

  int x = pt.x + 18;
  int y = pt.y + 18;
  if (x + previewWidth > work.right) x = pt.x - previewWidth - 16;
  if (y + previewHeight > work.bottom) y = pt.y - previewHeight - 16;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;
  if (x + previewWidth > work.right) x = work.right - previewWidth;
  if (y + previewHeight > work.bottom) y = work.bottom - previewHeight;

  bool bWasVisible = IsWindowVisible(ctx->hPreviewWnd) != FALSE;
  bool bPositionChanged = !ctx->bHasPreviewPosition || ctx->ptPreview.x != x || ctx->ptPreview.y != y;
  bool bSizeChanged = ctx->iPreviewWidth != previewWidth || ctx->iPreviewHeight != previewHeight;
  if (bColorChanged || bPositionChanged || bSizeChanged || !bWasVisible)
    InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
  if (bPositionChanged || bSizeChanged || !bWasVisible) {
    SetWindowPos(ctx->hPreviewWnd, HWND_TOPMOST, x, y, previewWidth, previewHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (bSizeChanged) _scPickerSetPreviewRegion(ctx->hPreviewWnd, previewWidth, previewHeight);
    ctx->ptPreview = (POINT){ x, y };
    ctx->iPreviewWidth = previewWidth;
    ctx->iPreviewHeight = previewHeight;
    ctx->bHasPreviewPosition = true;
  }
}

scInternal void
_scPickerSetLensControl(scColorPickerContext* ctx, bool bEnable) {
  if (!ctx || !ctx->bPicking) return;

  if (!bEnable || ctx->iZoom <= 0) {
    if (!ctx->bLensControl) return;

    /* Return to normal tracking without moving or constraining the OS cursor. */
    ctx->bLensControl = false;
    POINT target;
    if (GetCursorPos(&target)) {
      ctx->ptCursor = target;
      COLORREF color;
      if (ctx->pCapture && _scPickerSample(ctx->pCapture, target, &color)) {
        ctx->previewColor = color;
        ctx->bHasPreviewColor = true;
      }
      _scPickerShowPreview(ctx, target, true);
    }
    if (ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd))
      InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
    return;
  }

  if (ctx->bLensControl) return;

  /* Usually the preview is already visible. Only create/place it if needed. */
  if (!ctx->hPreviewWnd || !IsWindow(ctx->hPreviewWnd) ||
      !IsWindowVisible(ctx->hPreviewWnd)) {
    POINT target;
    if (GetCursorPos(&target)) ctx->ptCursor = target;
    COLORREF color;
    if (ctx->pCapture && _scPickerSample(ctx->pCapture, ctx->ptCursor, &color)) {
      ctx->previewColor = color;
      ctx->bHasPreviewColor = true;
    }
    _scPickerShowPreview(ctx, ctx->ptCursor, true);
  }
  if (!ctx->hPreviewWnd || !IsWindow(ctx->hPreviewWnd) ||
      !IsWindowVisible(ctx->hPreviewWnd)) {
    return;
  }

  /* Freeze the image currently shown in the lens. Do not move the window,
   * clip the mouse, or warp the native cursor. */
  POINT target = ctx->ptCursor;
  int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int virtualRight = virtualLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int virtualBottom = virtualTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  int originX = target.x - SC_COLOR_PICKER_MAGNIFIER_CELLS / 2;
  int originY = target.y - SC_COLOR_PICKER_MAGNIFIER_CELLS / 2;
  if (originX < virtualLeft) originX = virtualLeft;
  if (originY < virtualTop) originY = virtualTop;
  if (originX + SC_COLOR_PICKER_MAGNIFIER_CELLS > virtualRight)
    originX = virtualRight - SC_COLOR_PICKER_MAGNIFIER_CELLS;
  if (originY + SC_COLOR_PICKER_MAGNIFIER_CELLS > virtualBottom)
    originY = virtualBottom - SC_COLOR_PICKER_MAGNIFIER_CELLS;

  ctx->ptLensOrigin = (POINT){ originX, originY };
  ctx->ptLensCell = (POINT){ target.x - originX, target.y - originY };
  ctx->bLensControl = true;

  COLORREF color;
  if (ctx->pCapture && _scPickerSample(ctx->pCapture, target, &color)) {
    ctx->previewColor = color;
    ctx->bHasPreviewColor = true;
  }
  InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
}

scInternal void
_scPickerHandleZoom(scColorPickerContext* ctx, int delta) {
  if (!ctx || !ctx->bPicking || !delta) return;

  bool bWasLensControl = ctx->bLensControl;
  POINT lensTarget = ctx->ptCursor;
  if (bWasLensControl) {
    lensTarget.x = ctx->ptLensOrigin.x + ctx->ptLensCell.x;
    lensTarget.y = ctx->ptLensOrigin.y + ctx->ptLensCell.y;
    /* Preserve the selected source pixel while changing the magnification. */
    ctx->bLensControl = false;
    ctx->ptCursor = lensTarget;
  }

  int steps = delta / WHEEL_DELTA;
  if (steps == 0) steps = delta > 0 ? 1 : -1;
  int oldZoom = ctx->iZoom;
  ctx->iZoom += steps;
  if (ctx->iZoom < 0) ctx->iZoom = 0;
  if (ctx->iZoom > SC_COLOR_PICKER_ZOOM_MAX) ctx->iZoom = SC_COLOR_PICKER_ZOOM_MAX;
  _scPickerEnsurePreviewTree(ctx);

  if (bWasLensControl && ctx->iZoom > 0) {
    /* Resize in place: Shift still means the preview window stays put. */
    int width, height;
    _scPickerGetPreviewSize(ctx, &width, &height);
    bool bSizeChanged = ctx->iPreviewWidth != width || ctx->iPreviewHeight != height;
    ctx->bLensControl = true;
    COLORREF color;
    if (ctx->pCapture && _scPickerSample(ctx->pCapture, lensTarget, &color)) {
      ctx->previewColor = color;
      ctx->bHasPreviewColor = true;
    }
    if (bSizeChanged) {
      SetWindowPos(ctx->hPreviewWnd, HWND_TOPMOST, ctx->ptPreview.x, ctx->ptPreview.y,
                   width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      _scPickerSetPreviewRegion(ctx->hPreviewWnd, width, height);
      ctx->iPreviewWidth = width;
      ctx->iPreviewHeight = height;
    }
    InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
    return;
  }

  POINT pt;
  if (GetCursorPos(&pt)) {
    ctx->ptCursor = pt;
    COLORREF color;
    if (ctx->pCapture && _scPickerSample(ctx->pCapture, pt, &color)) {
      ctx->previewColor = color;
      ctx->bHasPreviewColor = true;
    }
    _scPickerShowPreview(ctx, pt, true);
  } else if (oldZoom != ctx->iZoom && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) {
    InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
  }

  if (ctx->iZoom > 0 &&
      ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000))) {
    _scPickerSetLensControl(ctx, true);
  }
}

scInternal void
_scPickerFreeContext(scColorPickerContext* ctx) {
  if (ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) DestroyWindow(ctx->hPreviewWnd);
  scGuiDestroy(ctx->pPreviewGui);
  scGuiDestroy(ctx->pGui);
  if (gColorPickerContext == ctx) gColorPickerContext = NULL;
  free(ctx);
}

scInternal bool
_scPickerBeginCapture(scColorPickerContext* ctx, scCaptureContext* pCtx) {
  if (!ctx || !pCtx) return false;

  ctx->bPicking = true;
  ctx->pCapture = pCtx;
  ctx->iZoom = 0;
  ctx->bHasPreviewColor = false;
  ctx->ptCursor = (POINT){ 0, 0 };
  pCtx->pUser = ctx;
  _scPickerHidePreview(ctx);
  if (ctx->hWnd && IsWindow(ctx->hWnd)) ShowWindow(ctx->hWnd, SW_HIDE);
  _scPickerStartMouseHook();

  if (!scCtxRequestCaptureArea(pCtx)) {
    _scPickerStopMouseHook();
    pCtx->pUser = NULL;
    ctx->pCapture = NULL;
    ctx->bPicking = false;
    return false;
  }

  return true;
}

scInternal LRESULT CALLBACK
_scColorPickerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  scColorPickerContext* ctx = (scColorPickerContext*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

  switch (uMsg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
      return TRUE;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
      _scPickerRefreshTheme(hWnd);
      InvalidateRect(hWnd, NULL, FALSE);
      if (ctx && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SC_COLOR_PICKER_ZOOM:
      _scPickerHandleZoom(ctx, (int)(INT_PTR)wParam);
      return 0;
    case WM_SC_COLOR_PICKER_LENS_CONTROL:
      if (ctx) {
        bool bShiftDown = wParam != 0;
        if (!wParam) {
          if ((UINT_PTR)lParam == VK_LSHIFT) {
            bShiftDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
          } else if ((UINT_PTR)lParam == VK_RSHIFT) {
            bShiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
          } else {
            bShiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
                         (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
          }
        }
        _scPickerSetLensControl(ctx, bShiftDown && ctx->iZoom > 0);
      }
      return 0;
    case WM_SC_COLOR_PICKER_START_PICK: {
      if (!ctx || !ctx->bPicking) return 0;
      scAppRunHandlerFromActionID(SC_HOTKEY_COLORPICKER);
      if (!ctx->pCapture || gApp->pCaptureContext != ctx->pCapture ||
          !ctx->pCapture->hOverlayWindow || !IsWindowVisible(ctx->pCapture->hOverlayWindow)) {
        _scPickerStopMouseHook();
        ctx->bPicking = false;
        ctx->pCapture = NULL;
        _scPickerHidePreview(ctx);
        ShowWindow(hWnd, SW_SHOWNORMAL);
        SetForegroundWindow(hWnd);
        InvalidateRect(hWnd, NULL, FALSE);
        scLogError("Color picker capture request failed");
      }
      return 0;
    }
    case WM_SIZE:
      if (ctx && wParam != SIZE_MINIMIZED) scGuiResize(ctx->pGui, LOWORD(lParam), HIWORD(lParam));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hDC = BeginPaint(hWnd, &ps);
      if (ctx && ctx->bHasColor) _scPickerPaint(ctx, hWnd, hDC, &ps.rcPaint);
      EndPaint(hWnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!ctx) break;
      scGuiMouseMove(ctx->pGui, (POINT){ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
      TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, HOVER_DEFAULT };
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (ctx) scGuiMouseLeave(ctx->pGui);
      return 0;
    case WM_SETCURSOR: {
      if (LOWORD(lParam) == HTCLIENT && ctx) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        SetCursor(LoadCursorW(NULL, scGuiHitClickable(ctx->pGui, pt) ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    }
    case WM_LBUTTONDOWN:
      if (ctx) scGuiMouseDown(ctx->pGui, (POINT){ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
      return 0;
    case WM_LBUTTONUP:
      if (ctx) scGuiMouseUp(ctx->pGui, (POINT){ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
      return 0;
    case WM_CAPTURECHANGED:
      if (ctx) scGuiCaptureLost(ctx->pGui);
      return 0;
    case WM_CLOSE:
      ShowWindow(hWnd, SW_HIDE);
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
      return DefWindowProcW(hWnd, uMsg, wParam, lParam);
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

scInternal HWND
_scColorPickerCreateWindow(scColorPickerContext* ctx) {
  if (!gColorPickerClassRegistered) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = _scColorPickerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = SC_COLOR_PICKER_CLASS;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      scLogError("Failed to register color picker window: %lu", GetLastError());
      return NULL;
    }
    gColorPickerClassRegistered = true;
  }

  DWORD style = WS_POPUP;
  DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  int windowWidth = SC_COLOR_PICKER_WIDTH;
  int windowHeight = SC_COLOR_PICKER_HEIGHT + SC_COLOR_PICKER_TITLEBAR_HEIGHT;
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int x = work.left + ((work.right - work.left) - windowWidth) / 2;
  int y = work.top + ((work.bottom - work.top) - windowHeight) / 2;

  if (!ctx->pGui) {
    scGuiHost host = { 0 };
    host.pUser         = ctx;
    host.pfnInvalidate = _scPickerHostInvalidateMain;
    host.pfnCapture    = _scPickerHostCaptureMain;
    ctx->pGui = scGuiCreate(&host);
    _scPickerApplyGuiTheme(ctx->pGui);
  }

  HWND hWnd = CreateWindowExW(
    exStyle,
    SC_COLOR_PICKER_CLASS,
    L"Color Picker",
    style,
    x, y, windowWidth, windowHeight,
    NULL, NULL, GetModuleHandleW(NULL), ctx
  );

  if (!hWnd) {
    scLogError("Failed to create color picker window: %lu", GetLastError());
    return NULL;
  }

  HRGN region = CreateRoundRectRgn(0, 0, windowWidth + 1, windowHeight + 1, 14, 14);
  if (region && !SetWindowRgn(hWnd, region, TRUE)) DeleteObject(region);
  scGuiResize(ctx->pGui, windowWidth, windowHeight);
  return hWnd;
}

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  if (!pCtx) return true;
  pCtx->bShouldUseMagnifier = false;
  pCtx->bShowCoordinates = false;

  _scPickerRefreshTheme(gColorPickerContext ? gColorPickerContext->hWnd : NULL);
  if (!_scPickerEnsureGdiPlus()) {
    scLogError("Failed to initialize GDI+ for color picker");
    return true;
  }

  scColorPickerContext* ctx = (scColorPickerContext*)pCtx->pUser;
  if (!ctx) ctx = gColorPickerContext;

  if (ctx) {
    if (ctx->bPicking && ctx->pCapture == pCtx) return false;
    if (ctx->bPicking && ctx->pCapture) return true;

    if (!_scPickerBeginCapture(ctx, pCtx)) {
      if (ctx->hWnd && IsWindow(ctx->hWnd)) {
        ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
        SetForegroundWindow(ctx->hWnd);
        InvalidateRect(ctx->hWnd, NULL, FALSE);
      }
      scLogError("Failed to start color picker capture");
      return true;
    }
    return false;
  }

  ctx = (scColorPickerContext*)calloc(1, sizeof(scColorPickerContext));
  if (!ctx) {
    scLogError("Failed to allocate color picker context");
    return true;
  }

  gColorPickerContext = ctx;

  if (!_scPickerBeginCapture(ctx, pCtx)) {
    _scPickerFreeContext(ctx);
    scLogError("Failed to start color picker capture");
    return true;
  }
  return false;
}

scInternal bool
cbOnAreaSelected(scCaptureContext* pCtx) {
  if (!pCtx || !pCtx->pUser) return true;

  scColorPickerContext* ctx = (scColorPickerContext*)pCtx->pUser;
  scRect selected = pCtx->stFinalRect;
  POINT pt = {
    selected.x + (selected.w > 0 ? (selected.w - 1) / 2 : 0),
    selected.y + (selected.h > 0 ? (selected.h - 1) / 2 : 0)
  };
  COLORREF color;

  _scPickerSetLensControl(ctx, false);
  _scPickerStopMouseHook();
  ctx->bPicking = false;
  ctx->pCapture = NULL;
  pCtx->pUser = NULL;
  ctx->bHasPreviewColor = false;
  ctx->iZoom = 0;
  _scPickerHidePreview(ctx);

  if (!_scPickerSample(pCtx, pt, &color)) {
    scLogError("Failed to sample selected screen pixel");
    if (ctx->hWnd && IsWindow(ctx->hWnd)) {
      ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
      SetForegroundWindow(ctx->hWnd);
      InvalidateRect(ctx->hWnd, NULL, FALSE);
    } else {
      _scPickerFreeContext(ctx);
    }
    return true;
  }

  _scPickerPushHistory(ctx, color);

  if (!ctx->hWnd || !IsWindow(ctx->hWnd)) {
    ctx->hWnd = _scColorPickerCreateWindow(ctx);
  }
  if (!ctx->hWnd) {
    _scPickerFreeContext(ctx);
    return true;
  }
  _scPickerRebuildMain(ctx);

  scLogInfo("Picked color: #%02X%02X%02X",
            (unsigned int)GetRValue(color),
            (unsigned int)GetGValue(color),
            (unsigned int)GetBValue(color));
  ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
  SetForegroundWindow(ctx->hWnd);
  InvalidateRect(ctx->hWnd, NULL, FALSE);
  return true;
}

scInternal void
cbOnCaptureCancelled(scCaptureContext* pCtx) {
  if (!pCtx || !pCtx->pUser) return;

  scColorPickerContext* ctx = (scColorPickerContext*)pCtx->pUser;
  _scPickerSetLensControl(ctx, false);
  _scPickerStopMouseHook();
  pCtx->pUser = NULL;
  ctx->pCapture = NULL;
  ctx->bPicking = false;
  ctx->iZoom = 0;
  ctx->bHasPreviewColor = false;
  _scPickerHidePreview(ctx);

  if (ctx->hWnd && IsWindow(ctx->hWnd)) {
    ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(ctx->hWnd);
    InvalidateRect(ctx->hWnd, NULL, FALSE);
    return;
  }

  _scPickerFreeContext(ctx);
}

scInternal HCURSOR
_scColorPickerOverlayCursor(scCaptureContext* pCtx) {
  (void)pCtx;
  return LoadCursorW(NULL, IDC_CROSS);
}

scInternal bool
_scColorPickerSnapSelection(scCaptureContext* pCtx, scRect rcInput, bool bDragging, scRect* pOut) {
  (void)bDragging;
  if (!pCtx || !pCtx->pUser || !pOut) return false;

  scColorPickerContext* ctx = (scColorPickerContext*)pCtx->pUser;
  if (!ctx->bPicking) return false;

  POINT pt;
  if (!GetCursorPos(&pt)) return false;

  bool bCellChanged = false;
  if (ctx->bLensControl) {
    int cellSize = _scPickerMagnifierCellSize(ctx);
    int lensSide = SC_COLOR_PICKER_MAGNIFIER_CELLS * cellSize;
    int lensLeft = ctx->ptPreview.x + ((ctx->iPreviewWidth - lensSide) / 2);
    int lensTop = ctx->ptPreview.y + 6;
    RECT lensRect = { lensLeft, lensTop, lensLeft + lensSide, lensTop + lensSide };

    if (PtInRect(&lensRect, pt)) {
      int cellX = (pt.x - lensLeft) / cellSize;
      int cellY = (pt.y - lensTop) / cellSize;
      if (cellX < 0) cellX = 0;
      if (cellX >= SC_COLOR_PICKER_MAGNIFIER_CELLS) cellX = SC_COLOR_PICKER_MAGNIFIER_CELLS - 1;
      if (cellY < 0) cellY = 0;
      if (cellY >= SC_COLOR_PICKER_MAGNIFIER_CELLS) cellY = SC_COLOR_PICKER_MAGNIFIER_CELLS - 1;
      bCellChanged = ctx->ptLensCell.x != cellX || ctx->ptLensCell.y != cellY;
      ctx->ptLensCell = (POINT){ cellX, cellY };
      pt = (POINT){ ctx->ptLensOrigin.x + cellX, ctx->ptLensOrigin.y + cellY };
    } else {
      /* Outside the frozen lens, keep the last cell selected. */
      pt = ctx->ptCursor;
    }
  }

  *pOut = (scRect){ pt.x, pt.y, 1, 1 };
  ctx->ptCursor = pt;
  COLORREF color;
  if (_scPickerSample(pCtx, pt, &color)) {
    bool bColorChanged = !ctx->bHasPreviewColor || ctx->previewColor != color;
    ctx->previewColor = color;
    ctx->bHasPreviewColor = true;
    if (ctx->bLensControl) {
      if ((bColorChanged || bCellChanged) && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd))
        InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
    } else {
      _scPickerShowPreview(ctx, pt, bColorChanged);
    }
  } else {
    ctx->bHasPreviewColor = false;
    if (!ctx->bLensControl) _scPickerHidePreview(ctx);
  }

  (void)rcInput;
  return true;
}

const scCaptureHandler scColorPickerHandler = {
  cbOnHotkeyPressed,
  cbOnAreaSelected,
  cbOnCaptureCancelled,
  _scColorPickerOverlayCursor,
  _scColorPickerSnapSelection
};
