#include "pch.h"
#include "scApp.h"
#include "scLogging.h"
#include "scGdiPlus.h"
#include "scUITheme.h"

#include <windowsx.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define WM_SC_COLOR_PICKER_START_PICK (WM_APP + 0x3A0)
#define SC_COLOR_PICKER_CLASS L"ScColorPickerWindow"
#define SC_COLOR_PICKER_PREVIEW_CLASS L"ScColorPickerPreviewWindow"
#define SC_COLOR_PICKER_WIDTH 500
#define SC_COLOR_PICKER_HEIGHT 330
#define SC_COLOR_PICKER_HISTORY 8
#define SC_COLOR_PICKER_ROWS 4
#define SC_COLOR_PICKER_PREVIEW_WIDTH 190
#define SC_COLOR_PICKER_PREVIEW_HEIGHT 64

#define SC_PICKER_HIT_NONE 0
#define SC_PICKER_HIT_PICK 1
#define SC_PICKER_HIT_SWATCH_BASE 10
#define SC_PICKER_HIT_COPY_BASE 30

static scUITheme gColorPickerTheme;
static bool gColorPickerThemeReady;
static f32 gColorPickerThemeScale;
static bool gColorPickerClassRegistered;
static bool gColorPickerPreviewClassRegistered;
static ULONG_PTR gColorPickerGdiPlusToken;
static struct scColorPickerContextTag* gColorPickerContext;

#define SC_PICKER_BG          (gColorPickerTheme.dwBackground)
#define SC_PICKER_PANEL       (gColorPickerTheme.dwCard)
#define SC_PICKER_PANEL_HOVER (gColorPickerTheme.dwCardHover)
#define SC_PICKER_BORDER      (gColorPickerTheme.dwStroke)
#define SC_PICKER_TEXT        (gColorPickerTheme.dwText)
#define SC_PICKER_MUTED       (gColorPickerTheme.dwTextDim)
#define SC_PICKER_ACCENT      (gColorPickerTheme.dwAccent)
#define SC_PICKER_STROKE_SOFT (gColorPickerTheme.dwStrokeSoft)
#define SC_PICKER_CARD_ACTIVE (gColorPickerTheme.dwCardActive)

typedef struct scColorPickerContextTag {
  HWND hWnd;
  HWND hPreviewWnd;
  scCaptureContext* pCapture;
  COLORREF color;
  COLORREF previewColor;
  COLORREF history[SC_COLOR_PICKER_HISTORY];
  s32 nHistory;
  s32 iHover;
  bool bPicking;
  bool bHasColor;
  bool bHasPreviewColor;
  POINT ptPreview;
  bool bHasPreviewPosition;
} scColorPickerContext;

typedef struct {
  wchar_t wszHex[16];
  wchar_t wszRgb[64];
  wchar_t wszHsl[64];
  wchar_t wszVec4[80];
} scColorStrings;

static void _scPickerDestroyTheme(void) {
  if (gColorPickerTheme.pFont) DeleteObject(gColorPickerTheme.pFont);
  if (gColorPickerTheme.pBoldFont) DeleteObject(gColorPickerTheme.pBoldFont);
  if (gColorPickerTheme.hBackgroundBrush) DeleteObject(gColorPickerTheme.hBackgroundBrush);
  if (gColorPickerTheme.hCardBrush) DeleteObject(gColorPickerTheme.hCardBrush);
  memset(&gColorPickerTheme, 0, sizeof(gColorPickerTheme));
  gColorPickerThemeReady = false;
}

static void _scPickerRefreshTheme(HWND hWnd) {
  f32 scale = _scGetDPIScale(hWnd);
  bool dark = _scIsWindowsDarkTheme();
  COLORREF accent = _scGetSystemAccentColor();

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
}

static void _scPickerApplyTitleBarTheme(HWND hWnd) {
  BOOL dark = gColorPickerTheme.bDark ? TRUE : FALSE;
  DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static bool _scPickerEnsureGdiPlus(void) {
  if (!gColorPickerGdiPlusToken) {
    gpStartup(&gColorPickerGdiPlusToken);
  }
  return gColorPickerGdiPlusToken != 0;
}

static void _scPickerColorToHsl(COLORREF color, float* h, float* s, float* l) {
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

static void _scPickerFormatColor(COLORREF color, scColorStrings* out) {
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

static void _scPickerPushHistory(scColorPickerContext* ctx, COLORREF color) {
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

static void _scPickerCopyText(HWND hWnd, const wchar_t* text) {
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

static void _scPickerFill(HDC hDC, RECT r, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  if (!brush) return;
  FillRect(hDC, &r, brush);
  DeleteObject(brush);
}

static void _scPickerText(HDC hDC, const wchar_t* text, RECT r, COLORREF color, UINT format, HFONT font) {
  HGDIOBJ oldFont = SelectObject(hDC, font);
  SetTextColor(hDC, color);
  SetBkMode(hDC, TRANSPARENT);
  DrawTextW(hDC, text, -1, &r, format | DT_NOPREFIX);
  SelectObject(hDC, oldFont);
}

static RECT _scPickerPickRect(void) {
  return (RECT){ 18, 16, 104, 58 };
}

static RECT _scPickerRowRect(int index, int clientWidth) {
  int top = 114 + index * 51;
  return (RECT){ 18, top, clientWidth - 18, top + 42 };
}

static RECT _scPickerCopyRect(int index, int clientWidth) {
  RECT row = _scPickerRowRect(index, clientWidth);
  return (RECT){ row.right - 70, row.top + 5, row.right - 8, row.bottom - 5 };
}

static POINT _scPickerSwatchCenter(int index) {
  return (POINT){ 150 + index * 38, 88 };
}

static int _scPickerHitTest(scColorPickerContext* ctx, POINT pt, int clientWidth) {
  RECT pick = _scPickerPickRect();
  if (PtInRect(&pick, pt)) return SC_PICKER_HIT_PICK;

  for (int i = 0; i < ctx->nHistory; ++i) {
    POINT c = _scPickerSwatchCenter(i);
    int dx = pt.x - c.x;
    int dy = pt.y - c.y;
    if (dx * dx + dy * dy <= 13 * 13) return SC_PICKER_HIT_SWATCH_BASE + i;
  }

  for (int i = 0; i < SC_COLOR_PICKER_ROWS; ++i) {
    RECT copy = _scPickerCopyRect(i, clientWidth);
    if (PtInRect(&copy, pt)) return SC_PICKER_HIT_COPY_BASE + i;
  }

  return SC_PICKER_HIT_NONE;
}

static void _scPickerDrawSwatch(GpGraphics* graphics, int cx, int cy, int diameter, COLORREF color, bool selected) {
  float d = (float)diameter;
  float x = (float)cx - d * 0.5f;
  float y = (float)cy - d * 0.5f;
  gpFillEllipse(graphics, x - 2.0f, y - 2.0f, d + 4.0f, d + 4.0f,
                gpColor(selected ? SC_PICKER_TEXT : SC_PICKER_BORDER, 255));
  gpFillEllipse(graphics, x, y, d, d, gpColor(color, 255));
}

static void _scPickerPaintContent(scColorPickerContext* ctx, HWND hWnd, HDC hDC) {
  RECT client;
  GetClientRect(hWnd, &client);
  int width = client.right;

  _scPickerFill(hDC, client, SC_PICKER_BG);

  HFONT hFont = gColorPickerTheme.pFont;
  HFONT hSmallFont = gColorPickerTheme.pBoldFont;
  if (!hFont || !hSmallFont) return;

  RECT pick = _scPickerPickRect();
  RECT current = { 112, 19, 148, 55 };
  RECT divider = { 0, 68, width, 69 };

  _scPickerFill(hDC, divider, SC_PICKER_STROKE_SOFT);
  GpGraphics* graphics = gpGraphicsBegin(hDC);
  gpFillRound(graphics, pick, 7.0f, gpColor(ctx->iHover == SC_PICKER_HIT_PICK ? SC_PICKER_PANEL_HOVER : SC_PICKER_PANEL, 255));
  gpStrokeRound(graphics, pick, 7.0f, gpColor(ctx->iHover == SC_PICKER_HIT_PICK ? SC_PICKER_ACCENT : SC_PICKER_BORDER, 255), 1.0f);

  gpFillEllipse(graphics, (float)current.left, (float)current.top,
                (float)(current.right - current.left), (float)(current.bottom - current.top),
                gpColor(SC_PICKER_BORDER, 255));
  gpFillEllipse(graphics, (float)(current.left + 2), (float)(current.top + 2),
                (float)(current.right - current.left - 4), (float)(current.bottom - current.top - 4),
                gpColor(ctx->color, 255));

  for (int i = 0; i < ctx->nHistory; ++i) {
    POINT c = _scPickerSwatchCenter(i);
    _scPickerDrawSwatch(graphics, c.x, c.y, 19, ctx->history[i], ctx->history[i] == ctx->color);
  }

  for (int i = 0; i < SC_COLOR_PICKER_ROWS; ++i) {
    RECT row = _scPickerRowRect(i, width);
    RECT copy = _scPickerCopyRect(i, width);
    COLORREF fill = ctx->iHover == SC_PICKER_HIT_COPY_BASE + i ? SC_PICKER_PANEL_HOVER : SC_PICKER_PANEL;
    gpFillRound(graphics, row, 6.0f, gpColor(fill, 255));
    gpFillRound(graphics, copy, 5.0f, gpColor(ctx->iHover == SC_PICKER_HIT_COPY_BASE + i ? SC_PICKER_PANEL_HOVER : SC_PICKER_CARD_ACTIVE, 255));
  }
  gpGraphicsEnd(graphics);

  RECT pickText = { pick.left + 12, pick.top + 1, pick.right - 6, pick.bottom - 1 };
  _scPickerText(hDC, L"Pick", pickText, SC_PICKER_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hFont);

  RECT recentLabel = { 18, 74, 132, 101 };
  _scPickerText(hDC, L"Recent Colors", recentLabel, SC_PICKER_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hSmallFont);

  RECT titleLabel = { 158, 15, width - 14, 31 };
  RECT titleValue = { 158, 31, width - 14, 57 };
  _scPickerText(hDC, L"CURRENT COLOUR", titleLabel, SC_PICKER_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hSmallFont);

  scColorStrings strings;
  _scPickerFormatColor(ctx->color, &strings);
  _scPickerText(hDC, strings.wszHex, titleValue, SC_PICKER_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hFont);

  const wchar_t* labels[SC_COLOR_PICKER_ROWS] = { L"HEX", L"RGB", L"HSL", L"VEC4" };
  const wchar_t* values[SC_COLOR_PICKER_ROWS] = { strings.wszHex, strings.wszRgb, strings.wszHsl, strings.wszVec4 };
  for (int i = 0; i < SC_COLOR_PICKER_ROWS; ++i) {
    RECT row = _scPickerRowRect(i, width);
    RECT labelRect = { row.left + 12, row.top + 1, row.left + 65, row.bottom - 1 };
    RECT valueRect = { row.left + 78, row.top + 1, row.right - 82, row.bottom - 1 };
    RECT copyText = _scPickerCopyRect(i, width);
    _scPickerText(hDC, labels[i], labelRect, SC_PICKER_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hFont);
    _scPickerText(hDC, values[i], valueRect, SC_PICKER_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, hFont);
    _scPickerText(hDC, L"Copy", copyText, SC_PICKER_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE, hSmallFont);
  }

}

static void _scPickerPaint(scColorPickerContext* ctx, HWND hWnd, HDC hDC) {
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
    _scPickerPaintContent(ctx, hWnd, hDC);
    return;
  }

  HGDIOBJ hOldBitmap = SelectObject(hMemDC, hBitmap);
  _scPickerPaintContent(ctx, hWnd, hMemDC);
  BitBlt(hDC, 0, 0, width, height, hMemDC, 0, 0, SRCCOPY);
  SelectObject(hMemDC, hOldBitmap);
  DeleteObject(hBitmap);
  DeleteDC(hMemDC);
}

static bool _scPickerSample(scCaptureContext* pCtx, POINT pt, COLORREF* outColor) {
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

static void _scPickerHidePreview(scColorPickerContext* ctx) {
  if (ctx && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) {
    ShowWindow(ctx->hPreviewWnd, SW_HIDE);
  }
}

static LRESULT CALLBACK _scColorPickerPreviewWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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
    case WM_THEMECHANGED:
      _scPickerRefreshTheme(ctx && ctx->hWnd ? ctx->hWnd : hWnd);
      InvalidateRect(hWnd, NULL, FALSE);
      if (ctx && ctx->hWnd && IsWindow(ctx->hWnd)) InvalidateRect(ctx->hWnd, NULL, FALSE);
      if (ctx && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
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
      if (ctx && ctx->bHasPreviewColor) {
        HFONT hFont = gColorPickerTheme.pBoldFont;
        HFONT hSmallFont = gColorPickerTheme.pFont;
        GpGraphics* graphics = gpGraphicsBegin(hDC);
        gpFillRound(graphics, client, 10.0f, gpColor(SC_PICKER_BG, 250));
        gpStrokeRound(graphics, client, 10.0f, gpColor(SC_PICKER_BORDER, 255), 1.0f);
        gpFillRound(graphics, (RECT){ 10, 10, 48, 48 }, 7.0f, gpColor(ctx->previewColor, 255));
        gpStrokeRound(graphics, (RECT){ 10, 10, 48, 48 }, 7.0f, gpColor(SC_PICKER_BORDER, 255), 1.0f);
        gpGraphicsEnd(graphics);
        if (hFont && hSmallFont) {
          scColorStrings strings;
          _scPickerFormatColor(ctx->previewColor, &strings);
          RECT hexRect = { 59, 8, SC_COLOR_PICKER_PREVIEW_WIDTH - 8, 34 };
          RECT rgbRect = { 59, 32, SC_COLOR_PICKER_PREVIEW_WIDTH - 8, 56 };
          _scPickerText(hDC, strings.wszHex, hexRect, SC_PICKER_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hFont);
          _scPickerText(hDC, strings.wszRgb, rgbRect, SC_PICKER_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE, hSmallFont);
        }
      }
      if (hMemDC && hBitmap) {
        BitBlt(hTargetDC, 0, 0, width, height, hMemDC, 0, 0, SRCCOPY);
      }
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

static HWND _scColorPickerCreatePreviewWindow(scColorPickerContext* ctx) {
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
  HRGN region = CreateRoundRectRgn(0, 0, SC_COLOR_PICKER_PREVIEW_WIDTH + 1,
                                   SC_COLOR_PICKER_PREVIEW_HEIGHT + 1, 18, 18);
  if (region && !SetWindowRgn(hWnd, region, TRUE)) {
    DeleteObject(region);
  }
  return hWnd;
}

static void _scPickerShowPreview(scColorPickerContext* ctx, POINT pt, bool bColorChanged) {
  if (!ctx->hPreviewWnd || !IsWindow(ctx->hPreviewWnd)) {
    ctx->hPreviewWnd = _scColorPickerCreatePreviewWindow(ctx);
  }
  if (!ctx->hPreviewWnd) return;

  HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(mi) };
  RECT work = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
  if (monitor && GetMonitorInfoW(monitor, &mi)) {
    work = mi.rcWork;
  }

  int x = pt.x + 18;
  int y = pt.y + 18;
  if (x + SC_COLOR_PICKER_PREVIEW_WIDTH > work.right) x = pt.x - SC_COLOR_PICKER_PREVIEW_WIDTH - 16;
  if (y + SC_COLOR_PICKER_PREVIEW_HEIGHT > work.bottom) y = pt.y - SC_COLOR_PICKER_PREVIEW_HEIGHT - 16;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;
  if (x + SC_COLOR_PICKER_PREVIEW_WIDTH > work.right) x = work.right - SC_COLOR_PICKER_PREVIEW_WIDTH;
  if (y + SC_COLOR_PICKER_PREVIEW_HEIGHT > work.bottom) y = work.bottom - SC_COLOR_PICKER_PREVIEW_HEIGHT;

  bool bWasVisible = IsWindowVisible(ctx->hPreviewWnd) != FALSE;
  bool bPositionChanged = !ctx->bHasPreviewPosition || ctx->ptPreview.x != x || ctx->ptPreview.y != y;
  if (bColorChanged || !bWasVisible) InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
  if (bPositionChanged || !bWasVisible) {
    SetWindowPos(ctx->hPreviewWnd, HWND_TOPMOST, x, y,
                 SC_COLOR_PICKER_PREVIEW_WIDTH, SC_COLOR_PICKER_PREVIEW_HEIGHT,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ctx->ptPreview = (POINT){ x, y };
    ctx->bHasPreviewPosition = true;
  }
}

static LRESULT CALLBACK _scColorPickerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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
      _scPickerApplyTitleBarTheme(hWnd);
      InvalidateRect(hWnd, NULL, FALSE);
      if (ctx && ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) InvalidateRect(ctx->hPreviewWnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SC_COLOR_PICKER_START_PICK: {
      if (!ctx || !ctx->bPicking) return 0;
      scAppRunHandlerFromActionID(SC_HOTKEY_COLORPICKER);
      if (!ctx->pCapture || gApp->pCaptureContext != ctx->pCapture ||
          !ctx->pCapture->hOverlayWindow || !IsWindowVisible(ctx->pCapture->hOverlayWindow)) {
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
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hDC = BeginPaint(hWnd, &ps);
      if (ctx && ctx->bHasColor) _scPickerPaint(ctx, hWnd, hDC);
      EndPaint(hWnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!ctx) break;
      RECT client;
      GetClientRect(hWnd, &client);
      int hover = _scPickerHitTest(ctx, (POINT){ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, client.right);
      if (hover != ctx->iHover) {
        ctx->iHover = hover;
        InvalidateRect(hWnd, NULL, FALSE);
      }
      TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, HOVER_DEFAULT };
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (ctx && ctx->iHover) {
        ctx->iHover = 0;
        InvalidateRect(hWnd, NULL, FALSE);
      }
      return 0;
    case WM_SETCURSOR: {
      if (LOWORD(lParam) == HTCLIENT && ctx) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        RECT client;
        GetClientRect(hWnd, &client);
        int hit = _scPickerHitTest(ctx, pt, client.right);
        SetCursor(LoadCursorW(NULL, hit == SC_PICKER_HIT_NONE ? IDC_ARROW : IDC_HAND));
        return TRUE;
      }
      break;
    }
    case WM_LBUTTONUP: {
      if (!ctx) return 0;
      RECT client;
      GetClientRect(hWnd, &client);
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      int hit = _scPickerHitTest(ctx, pt, client.right);

      if (hit == SC_PICKER_HIT_PICK) {
        ctx->bPicking = true;
        _scPickerHidePreview(ctx);
        ShowWindow(hWnd, SW_HIDE);
        if (!PostMessageW(hWnd, WM_SC_COLOR_PICKER_START_PICK, 0, 0)) {
          ctx->bPicking = false;
          ShowWindow(hWnd, SW_SHOWNORMAL);
          SetForegroundWindow(hWnd);
          scLogError("Failed to queue color picker capture request: %lu", GetLastError());
        }
        return 0;
      }

      if (hit >= SC_PICKER_HIT_SWATCH_BASE && hit < SC_PICKER_HIT_SWATCH_BASE + ctx->nHistory) {
        ctx->color = ctx->history[hit - SC_PICKER_HIT_SWATCH_BASE];
        ctx->bHasColor = true;
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
      }

      if (hit >= SC_PICKER_HIT_COPY_BASE && hit < SC_PICKER_HIT_COPY_BASE + SC_COLOR_PICKER_ROWS) {
        scColorStrings strings;
        _scPickerFormatColor(ctx->color, &strings);
        const wchar_t* values[SC_COLOR_PICKER_ROWS] = { strings.wszHex, strings.wszRgb, strings.wszHsl, strings.wszVec4 };
        _scPickerCopyText(hWnd, values[hit - SC_PICKER_HIT_COPY_BASE]);
        return 0;
      }
      return 0;
    }
    case WM_CLOSE:
      ShowWindow(hWnd, SW_HIDE);
      return 0;
    case WM_NCDESTROY:
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
      return DefWindowProcW(hWnd, uMsg, wParam, lParam);
  }

  return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static HWND _scColorPickerCreateWindow(scColorPickerContext* ctx) {
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

  RECT r = { 0, 0, SC_COLOR_PICKER_WIDTH, SC_COLOR_PICKER_HEIGHT };
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
  AdjustWindowRectEx(&r, style, FALSE, exStyle);

  int windowWidth = r.right - r.left;
  int windowHeight = r.bottom - r.top;
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int x = work.left + ((work.right - work.left) - windowWidth) / 2;
  int y = work.top + ((work.bottom - work.top) - windowHeight) / 2;

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

  _scPickerApplyTitleBarTheme(hWnd);
  return hWnd;
}

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  if (!pCtx) return true;
  pCtx->bShouldUseMagnifier = false;

  _scPickerRefreshTheme(gColorPickerContext ? gColorPickerContext->hWnd : NULL);
  if (!_scPickerEnsureGdiPlus()) {
    scLogError("Failed to initialize GDI+ for color picker");
    return true;
  }

  scColorPickerContext* ctx = (scColorPickerContext*)pCtx->pUser;
  if (!ctx) ctx = gColorPickerContext;

  if (ctx) {
    if (!ctx->bPicking) {
      if (ctx->hWnd && IsWindow(ctx->hWnd)) {
        ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
        SetForegroundWindow(ctx->hWnd);
        InvalidateRect(ctx->hWnd, NULL, FALSE);
      }
      return true;
    }

    if (ctx->pCapture == pCtx) {
      return false;
    }

    ctx->pCapture = pCtx;
    pCtx->pUser = ctx;
    if (!scCtxRequestCaptureArea(pCtx)) {
      pCtx->pUser = NULL;
      ctx->pCapture = NULL;
      ctx->bPicking = false;
      if (ctx->hWnd && IsWindow(ctx->hWnd)) {
        ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
        SetForegroundWindow(ctx->hWnd);
        InvalidateRect(ctx->hWnd, NULL, FALSE);
      } else {
        if (gColorPickerContext == ctx) gColorPickerContext = NULL;
        free(ctx);
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

  ctx->pCapture = pCtx;
  ctx->bPicking = true;
  ctx->iHover = SC_PICKER_HIT_NONE;
  pCtx->pUser = ctx;
  gColorPickerContext = ctx;

  if (!scCtxRequestCaptureArea(pCtx)) {
    pCtx->pUser = NULL;
    ctx->pCapture = NULL;
    if (gColorPickerContext == ctx) gColorPickerContext = NULL;
    free(ctx);
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

  ctx->bPicking = false;
  ctx->pCapture = NULL;
  pCtx->pUser = NULL;
  ctx->bHasPreviewColor = false;
  _scPickerHidePreview(ctx);

  if (!_scPickerSample(pCtx, pt, &color)) {
    scLogError("Failed to sample selected screen pixel");
    if (ctx->hWnd && IsWindow(ctx->hWnd)) {
      ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
      SetForegroundWindow(ctx->hWnd);
      InvalidateRect(ctx->hWnd, NULL, FALSE);
    } else {
      if (ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) DestroyWindow(ctx->hPreviewWnd);
      ctx->hPreviewWnd = NULL;
      if (gColorPickerContext == ctx) gColorPickerContext = NULL;
      free(ctx);
    }
    return true;
  }

  _scPickerPushHistory(ctx, color);

  if (!ctx->hWnd || !IsWindow(ctx->hWnd)) {
    ctx->hWnd = _scColorPickerCreateWindow(ctx);
  }
  if (!ctx->hWnd) {
    if (ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) DestroyWindow(ctx->hPreviewWnd);
    ctx->hPreviewWnd = NULL;
    if (gColorPickerContext == ctx) gColorPickerContext = NULL;
    free(ctx);
    return true;
  }

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
  pCtx->pUser = NULL;
  ctx->pCapture = NULL;
  ctx->bPicking = false;
  ctx->bHasPreviewColor = false;
  _scPickerHidePreview(ctx);

  if (ctx->hWnd && IsWindow(ctx->hWnd)) {
    ShowWindow(ctx->hWnd, SW_SHOWNORMAL);
    SetForegroundWindow(ctx->hWnd);
    InvalidateRect(ctx->hWnd, NULL, FALSE);
    return;
  }

  if (ctx->hPreviewWnd && IsWindow(ctx->hPreviewWnd)) DestroyWindow(ctx->hPreviewWnd);
  ctx->hPreviewWnd = NULL;
  if (gColorPickerContext == ctx) gColorPickerContext = NULL;
  free(ctx);
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

  *pOut = (scRect){ pt.x, pt.y, 1, 1 };
  COLORREF color;
  if (_scPickerSample(pCtx, pt, &color)) {
    bool bColorChanged = !ctx->bHasPreviewColor || ctx->previewColor != color;
    ctx->previewColor = color;
    ctx->bHasPreviewColor = true;
    _scPickerShowPreview(ctx, pt, bColorChanged);
  } else {
    ctx->bHasPreviewColor = false;
    _scPickerHidePreview(ctx);
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
