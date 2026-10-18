#include "pch.h"
#include "scTypes.h"
#include "scUI.h"
#include "scGui.h"
#include "scApp.h"
#include "scAssert.h"
#include "scLogging.h"
#include "scGdiPlus.h"
#include "scLocale.h"
#include "stb_image.h"
#include "scUITheme.h"
#include "scTray.h"
#include "scAudio.h"

#include <windowsx.h>
#include <shobjidl.h>
#include <shlobj.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define WM_HOTKEY_RECORDED (WM_APP + 100)
#define WM_THUMB_LOADED    (WM_APP + 101)
#define WM_LOG_PUSHED      (WM_APP + 102)
#define WM_REBUILD_UI      (WM_APP + 103)

#define SC_GALLERY_HISTORY_MAX 12 // For back and forward buttons on mouse, likely don't need much history.

//------------------------------------------------------------------------
// Types
//------------------------------------------------------------------------
typedef struct {
  char    szPath[MAX_PATH];
  HBITMAP hBitmap;
} scThumb;

typedef struct {
  IDropSource base;
  LONG        cRef;
} scDropSource;

typedef struct scThumbTask {
  char szPath[MAX_PATH];
  struct scThumbTask* pNext;
} scThumbTask;

typedef struct {
  HANDLE hThread;
  HANDLE hEvent;
  CRITICAL_SECTION csQueue;
  CRITICAL_SECTION csCache;
  scThumbTask* pHead;
  scThumbTask* pTail;
  bool bRunning;
} scThumbQueue;

typedef struct {
  bool    bIsFolder;
  bool    bIsUp;
  s32     nItemCount;
  wchar_t wszName[MAX_PATH];
  char    szPath[MAX_PATH]; // should be wchar_t...
} scGalleryItem;

typedef struct {
  const char* szText;
  s32         nLen;
  s32         iHeight;
  s32         iTop;
} scLogLine;

typedef struct {
  scUITheme theme;
  scGui*    pGui;
  scNode*   pContent;
  scNode*   pLogView;
  HWND      hWindow;
  bool      bRegisteredClass;
  bool      bTrackingMouse;
  LONG      lLogPending;
  f32       fUIScale;

  HICON hIconBig;
  HICON hIconSmall;

  scPageID eCurrentPage;
  s32      aPageScrollY[_SC_PAGE_COUNT];

  HBITMAP hBackBitmap;
  s32     iBackWidth;
  s32     iBackHeight;

  s32 iEditingHotkey;

  scLogLine* aLogLines;
  s32        nLogLines;
  s32        nLogCap;
  s32        iLogWidth;

  scThumbQueue qThumbQueue;
  scThumb* aThumbs;
  s32      nThumbCount;
  s32      nThumbCap;

  wchar_t wszGalleryDir[MAX_PATH]; // current folder being browsed in the gallery explorer
  wchar_t aGalleryHistory[SC_GALLERY_HISTORY_MAX][MAX_PATH];
  s32     anGalleryScrollY[SC_GALLERY_HISTORY_MAX];
  s32     nGalleryHistoryCount;
  s32     nGalleryHistoryPos;
} scUI;

static scUI gUI;
static HHOOK gKeyboardHook;

#define WINDOW_MIN_WIDTH  ((s32)550)
#define WINDOW_MIN_HEIGHT ((s32)340)

#define SIDEBAR_WIDTH   150

static const char* const kPageKeys[_SC_PAGE_COUNT] = { "General", "Settings", "Gallery", "Logs", "Credits" };

scInternal void _scBeginHotkeyRecording(s32 iIndex);
scInternal void _scCancelHotkeyRecording();
scInternal void _scOnVersionLinkClicked(s32 iParam);
scInternal void _scRebuild();

//------------------------------------------------------------------------
// Utility
//------------------------------------------------------------------------
scInternal s32 _scScale(s32 nVal) { return (s32)(nVal * gUI.fUIScale); }
scInternal f32 _scRoundRad() { return 6.0f * gUI.fUIScale; }

scInternal s32
_scCurrentScrollY() {
  return gUI.pContent ? scGetScrollY(gUI.pContent) : 0;
}

scInternal void
_scSavePageScroll() {
  if (gUI.pContent) {
    gUI.aPageScrollY[gUI.eCurrentPage] = scGetScrollY(gUI.pContent);
  }
}

scInternal void
_scHostInvalidate(void* pUser, const RECT* pRect) {
  (void)pUser;
  if (gUI.hWindow) {
    InvalidateRect(gUI.hWindow, pRect, FALSE);
  }
}

scInternal void
_scHostCapture(void* pUser, bool bCapture) {
  (void)pUser;
  if (!gUI.hWindow) {
    return;
  }
  if (bCapture) {
    SetCapture(gUI.hWindow);
  } else if (GetCapture() == gUI.hWindow) {
    ReleaseCapture();
  }
}

scInternal void
_scHostValueChanged(void* pUser) {
  (void)pUser;
  scSaveConfig();
}

scInternal void
_scApplyTheme() {
  if (!gUI.pGui) {
    return;
  }
  scUITheme* t = &gUI.theme;
  scGuiTheme theme = { 0 };
  theme.dwText        = t->dwText;
  theme.dwTextDim     = t->dwTextDim;
  theme.dwTextFaint   = t->dwTextFaint;
  theme.dwBackground  = t->dwBackground;
  theme.dwPanel       = t->dwSidebar;
  theme.dwPopup       = t->dwPopup;
  theme.dwCard        = t->dwCard;
  theme.dwCardHover   = t->dwCardHover;
  theme.dwCardActive  = t->dwCardActive;
  theme.dwStroke      = t->dwStroke;
  theme.dwStrokeSoft  = t->dwStrokeSoft;
  theme.dwAccent      = t->dwAccent;
  theme.dwAccentHover = t->dwAccentHover;
  theme.dwAccentText  = scColorIsLight(t->dwAccent) ? RGB(0, 0, 0) : RGB(255, 255, 255);
  theme.dwPillOff     = t->dwPillOff;
  theme.dwScrollThumb = t->dwScrollThumb;
  theme.hFont         = (HFONT)t->pFont;
  theme.hBoldFont     = (HFONT)t->pBoldFont;
  scGuiSetTheme(gUI.pGui, &theme);
  scGuiSetScale(gUI.pGui, gUI.fUIScale);
}

#define ICON_BASE_HUE 140.0f
#define ICON_BASE_SAT 0.80f
#define ICON_BASE_VAL 0.90f

scInternal void
_scRGBToHSV(float r, float g, float b, float* h, float* s, float* v) {
  float fMax = fmaxf(r, fmaxf(g, b));
  float fMin = fminf(r, fminf(g, b));
  float fD   = fMax - fMin;

  *v = fMax;
  *s = (fMax <= 0.0f) ? 0.0f : fD / fMax;

  if (fD <= 0.0f) {
    *h = 0.0f;
  } else if (fMax == r) {
    *h = 60.0f * fmodf((g - b) / fD, 6.0f);
  } else if (fMax == g) {
    *h = 60.0f * (((b - r) / fD) + 2.0f);
  } else {
    *h = 60.0f * (((r - g) / fD) + 4.0f);
  }
  if (*h < 0.0f) *h += 360.0f;
}

scInternal void
_scHSVToRGB(float h, float s, float v, float* r, float* g, float* b) {
  float fC = v * s;
  float fX = fC * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float fM = v - fC;
  float fR = 0, fG = 0, fB = 0;

  if      (h < 60.0f)  { fR = fC; fG = fX; }
  else if (h < 120.0f) { fR = fX; fG = fC; }
  else if (h < 180.0f) { fG = fC; fB = fX; }
  else if (h < 240.0f) { fG = fX; fB = fC; }
  else if (h < 300.0f) { fR = fX; fB = fC; }
  else                 { fR = fC; fB = fX; }

  *r = fR + fM;
  *g = fG + fM;
  *b = fB + fM;
}

HICON scGetThemedIcon(int iWidth, int iHeight, COLORREF crAccent, bool bThemed) {
  HICON hSrc = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDI_APP_ICON),
                                 IMAGE_ICON, iWidth, iHeight, LR_DEFAULTCOLOR);
  if (!hSrc) {
    scLogError("Failed to load icon: %d", GetLastError());
    return NULL;
  }

  if (!bThemed) {
    return hSrc;
  }

  ICONINFO ii = { 0 };
  if (!GetIconInfo(hSrc, &ii)) {
    DestroyIcon(hSrc);
    return NULL;
  }

  HICON hResult = NULL;
  BITMAP bm = { 0 };
  GetObject(ii.hbmColor, sizeof(bm), &bm);

  BITMAPINFO bi = { 0 };
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       = bm.bmWidth;
  bi.bmiHeader.biHeight      = -bm.bmHeight; // top-down
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  int      iCount  = bm.bmWidth * bm.bmHeight;
  uint32_t* pPixels = (uint32_t*)malloc((size_t)iCount * sizeof(uint32_t));
  HDC      hDC     = GetDC(NULL);

  if (pPixels && GetDIBits(hDC, ii.hbmColor, 0, bm.bmHeight, pPixels, &bi, DIB_RGB_COLORS)) {
    // Hue of the accent color
    // HSV of the accent color
    float fAccentH, fAccentS, fAccentV;
    _scRGBToHSV(GetRValue(crAccent) / 255.0f, GetGValue(crAccent) / 255.0f,
                GetBValue(crAccent) / 255.0f, &fAccentH, &fAccentS, &fAccentV);

    // Measure the icon's dominant hue (circular mean, weighted by saturation * alpha)
    float fSumX = 0.0f, fSumY = 0.0f;
    for (int i = 0; i < iCount; i++) {
      uint32_t px = pPixels[i];
      float    fA = ((px >> 24) & 0xFF) / 255.0f;
      float    r  = ((px >> 16) & 0xFF) / 255.0f;
      float    g  = ((px >> 8)  & 0xFF) / 255.0f;
      float    b  = ( px        & 0xFF) / 255.0f;

      float h, s, v;
      _scRGBToHSV(r, g, b, &h, &s, &v);

      float fW   = s * v * fA;
      float fRad = h * (3.14159265f / 180.0f);
      fSumX += cosf(fRad) * fW;
      fSumY += sinf(fRad) * fW;
    }

    float fBaseH = atan2f(fSumY, fSumX) * (180.0f / 3.14159265f);
    if (fBaseH < 0.0f) fBaseH += 360.0f;

    float fShift = fAccentH - fBaseH;

    float fSatScale = fAccentS / ICON_BASE_SAT;
    float fValScale = fAccentV / ICON_BASE_VAL;

    for (int i = 0; i < iCount; i++) {
      uint32_t px = pPixels[i];
      uint8_t  a  = (px >> 24) & 0xFF;
      float    r  = ((px >> 16) & 0xFF) / 255.0f;
      float    g  = ((px >> 8)  & 0xFF) / 255.0f;
      float    b  = ( px        & 0xFF) / 255.0f;

      float h, s, v;
      _scRGBToHSV(r, g, b, &h, &s, &v);

      h = fmodf(h + fShift + 360.0f, 360.0f);

      // Weight the value scale by saturation so white/gray details
      // (highlights, outlines) keep their brightness
      v = fminf(1.0f, v * (1.0f + (fValScale - 1.0f) * s));
      s = fminf(1.0f, s * fSatScale);

      _scHSVToRGB(h, s, v, &r, &g, &b);

      pPixels[i] = ((uint32_t)a << 24)
                 | ((uint32_t)(r * 255.0f + 0.5f) << 16)
                 | ((uint32_t)(g * 255.0f + 0.5f) << 8)
                 |  (uint32_t)(b * 255.0f + 0.5f);
    }

    SetDIBits(hDC, ii.hbmColor, 0, bm.bmHeight, pPixels, &bi, DIB_RGB_COLORS);
    hResult = CreateIconIndirect(&ii);
  }

  ReleaseDC(NULL, hDC);
  free(pPixels);
  DeleteObject(ii.hbmColor);
  DeleteObject(ii.hbmMask);
  DestroyIcon(hSrc);
  return hResult;
}

scInternal void
_scUpdateWindowIcon(HWND hWnd, bool bThemed) {
  COLORREF crAccent = gUI.theme.dwAccent;

  HICON hBig   = scGetThemedIcon(GetSystemMetrics(SM_CXICON),   GetSystemMetrics(SM_CYICON),   crAccent, bThemed);
  HICON hSmall = scGetThemedIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), crAccent, bThemed);
  if (!hBig || !hSmall) {
    if (hBig)   DestroyIcon(hBig);
    if (hSmall) DestroyIcon(hSmall);
    return; // keep whatever icon is currently set
  }

  SendMessageW(hWnd, WM_SETICON, ICON_BIG,   (LPARAM)hBig);
  SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);

  // Destroy the old ones only after they've been replaced
  if (gUI.hIconBig)   DestroyIcon(gUI.hIconBig);
  if (gUI.hIconSmall) DestroyIcon(gUI.hIconSmall);
  gUI.hIconBig   = hBig;
  gUI.hIconSmall = hSmall;
}

scInternal HRESULT STDMETHODCALLTYPE
_scDropSourceQueryInterface(IDropSource* pThis, REFIID riid, void** ppv) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
    *ppv = pThis;
    pThis->lpVtbl->AddRef(pThis);
    return S_OK;
  }
  *ppv = NULL;
  return E_NOINTERFACE;
}

scInternal ULONG STDMETHODCALLTYPE
_scDropSourceAddRef(IDropSource* pThis) {
  return InterlockedIncrement(&((scDropSource*)pThis)->cRef);
}

scInternal ULONG STDMETHODCALLTYPE
_scDropSourceRelease(IDropSource* pThis) {
  scDropSource* p = (scDropSource*)pThis;
  LONG c = InterlockedDecrement(&p->cRef);
  if (c == 0) {
    free(p);
  }
  return c;
}

scInternal HRESULT STDMETHODCALLTYPE
_scDropSourceQueryContinueDrag(IDropSource* pThis, BOOL fEscapePressed, DWORD grfKeyState) {
  (void)pThis;
  if (fEscapePressed) {
    return DRAGDROP_S_CANCEL;
  }
  if (!(grfKeyState & MK_LBUTTON)) {
    return DRAGDROP_S_DROP;
  }
  return S_OK;
}

scInternal HRESULT STDMETHODCALLTYPE
_scDropSourceGiveFeedback(IDropSource* pThis, DWORD dwEffect) {
  (void)pThis; (void)dwEffect;
  return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl gDropSourceVtbl = {
  _scDropSourceQueryInterface,
  _scDropSourceAddRef,
  _scDropSourceRelease,
  _scDropSourceQueryContinueDrag,
  _scDropSourceGiveFeedback
};

scInternal IDropSource*
_scCreateDropSource() {
  scDropSource* p = (scDropSource*)malloc(sizeof(scDropSource));
  p->base.lpVtbl = &gDropSourceVtbl;
  p->cRef        = 1;
  return (IDropSource*)p;
}

scInternal void
_scStartImageDrag(const char* szPath) {
  wchar_t wszPath[MAX_PATH];
  MultiByteToWideChar(CP_ACP, 0, szPath, -1, wszPath, MAX_PATH);

  IShellItem* pItem = NULL;
  if (FAILED(SHCreateItemFromParsingName(wszPath, NULL, &IID_IShellItem, (void**)&pItem))) {
    return;
  }

  IShellItemArray* pItemArray = NULL;
  if (FAILED(SHCreateShellItemArrayFromShellItem(pItem, &IID_IShellItemArray, (void**)&pItemArray))) {
    pItem->lpVtbl->Release(pItem);
    return;
  }

  IDataObject* pDataObject = NULL;
  if (SUCCEEDED(pItemArray->lpVtbl->BindToHandler(pItemArray, NULL, &BHID_DataObject, &IID_IDataObject, (void**)&pDataObject))) {
    IDropSource* pDropSource = _scCreateDropSource();
    DWORD dwEffect = 0;
    DoDragDrop(pDataObject, pDropSource, DROPEFFECT_COPY, &dwEffect);
    pDropSource->lpVtbl->Release(pDropSource);
    pDataObject->lpVtbl->Release(pDataObject);
  }

  pItemArray->lpVtbl->Release(pItemArray);
  pItem->lpVtbl->Release(pItem);
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
// TODO: #define STBI_WINDOWS_UTF8

scInternal HBITMAP
_scLoadImageThumbnail(const char* szPath) {
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
_scLoadVideoThumbnail(const char* szPath) {
  wchar_t wszPath[MAX_PATH];
  if (MultiByteToWideChar(CP_UTF8, 0, szPath, -1, wszPath, MAX_PATH) == 0) {
    scLogError("MultiByteToWideChar failed in _scLoadVideoThumbnail, path: %s", szPath);
    return NULL;
  }

  IShellItem* pItem = NULL;
  HRESULT hResult = SHCreateItemFromParsingName(wszPath, NULL, &IID_IShellItem, (void**)&pItem);
  if (FAILED(hResult) || !pItem) {
    scLogError("SHCreateItemFromParsingName failed in _scLoadVideoThumbnail, path: %s", szPath);
    return NULL;
  }

  IShellItemImageFactory* pFactory = NULL;
  hResult = pItem->lpVtbl->QueryInterface(pItem, &IID_IShellItemImageFactory, (void**)&pFactory);
  pItem->lpVtbl->Release(pItem);
  if (FAILED(hResult) || !pFactory) {
    scLogError("QueryInterface(IID_IShellItemImageFactory) failed in _scLoadVideoThumbnail, path: %s", szPath);
    return NULL;
  }

  const SIZE size = { 640, 640 };
  HBITMAP hBitmap = NULL;
  hResult = pFactory->lpVtbl->GetImage(pFactory, size, SIIGBF_RESIZETOFIT, &hBitmap);
  pFactory->lpVtbl->Release(pFactory);

  return SUCCEEDED(hResult) ? hBitmap : NULL;
}

scInternal HBITMAP
_scLoadThumbnail(const char* szPath) {
  const char* szExt = strrchr(szPath, '.');
  if (!szExt) {
    return NULL;
  }
  if (_stricmp(szExt, ".mp4") == 0 || _stricmp(szExt, ".mov") == 0 || _stricmp(szExt, ".avi") == 0) {
    return _scLoadVideoThumbnail(szPath);
  } else {
    return _scLoadImageThumbnail(szPath);
  }
}

scInternal HBITMAP
_scThumbGet(const char* szPath) {
  EnterCriticalSection(&gUI.qThumbQueue.csCache);

  for (s32 i = 0; i < gUI.nThumbCount; ++i) {
    if (strcmp(gUI.aThumbs[i].szPath, szPath) == 0) {
      HBITMAP hBmp = gUI.aThumbs[i].hBitmap;
      LeaveCriticalSection(&gUI.qThumbQueue.csCache);
      return hBmp;
    }
  }

  if (gUI.nThumbCount >= gUI.nThumbCap) {
    gUI.nThumbCap = gUI.nThumbCap ? gUI.nThumbCap * 2 : 16;
    gUI.aThumbs = (scThumb*)realloc(gUI.aThumbs, gUI.nThumbCap * sizeof(scThumb));
  }

  scThumb* pThumb = &gUI.aThumbs[gUI.nThumbCount++];
  strcpy_s(pThumb->szPath, MAX_PATH, szPath);
  pThumb->hBitmap = NULL;

  LeaveCriticalSection(&gUI.qThumbQueue.csCache);

  scThumbTask* pTask = (scThumbTask*)malloc(sizeof(scThumbTask));
  strcpy_s(pTask->szPath, MAX_PATH, szPath);
  pTask->pNext = NULL;

  EnterCriticalSection(&gUI.qThumbQueue.csQueue);
  if (gUI.qThumbQueue.pTail) {
    gUI.qThumbQueue.pTail->pNext = pTask;
    gUI.qThumbQueue.pTail = pTask;
  } else {
    gUI.qThumbQueue.pHead = gUI.qThumbQueue.pTail = pTask;
  }
  SetEvent(gUI.qThumbQueue.hEvent);
  LeaveCriticalSection(&gUI.qThumbQueue.csQueue);

  return NULL;
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

static DWORD WINAPI _scThumbWorkerProc(LPVOID pParam) {
  while (gUI.qThumbQueue.bRunning) {
    WaitForSingleObject(gUI.qThumbQueue.hEvent, INFINITE);
    if (!gUI.qThumbQueue.bRunning) break;

    scThumbTask* pTask = NULL;

    EnterCriticalSection(&gUI.qThumbQueue.csQueue);
    if (gUI.qThumbQueue.pHead) {
      pTask = gUI.qThumbQueue.pHead;
      gUI.qThumbQueue.pHead = pTask->pNext;
      if (!gUI.qThumbQueue.pHead) {
        gUI.qThumbQueue.pTail = NULL;
      }
    } else {
      ResetEvent(gUI.qThumbQueue.hEvent);
    }
    LeaveCriticalSection(&gUI.qThumbQueue.csQueue);

    if (!pTask) continue;

    HBITMAP hBmp = _scLoadThumbnail(pTask->szPath);

    EnterCriticalSection(&gUI.qThumbQueue.csCache);
    for (s32 i = 0; i < gUI.nThumbCount; ++i) {
      if (strcmp(gUI.aThumbs[i].szPath, pTask->szPath) == 0) {
        gUI.aThumbs[i].hBitmap = hBmp;
        break;
      }
    }
    LeaveCriticalSection(&gUI.qThumbQueue.csCache);

    free(pTask);

    if (gUI.hWindow && hBmp) {
      PostMessageW(gUI.hWindow, WM_THUMB_LOADED, 0, 0);
    }
  }

  return 0;
}

scInternal void _scInitThumbWorker() {
  scLogDebug("Initializing background worker for thumbnails");

  InitializeCriticalSection(&gUI.qThumbQueue.csQueue);
  InitializeCriticalSection(&gUI.qThumbQueue.csCache);
  gUI.qThumbQueue.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  gUI.qThumbQueue.bRunning = true;
  gUI.qThumbQueue.hThread = CreateThread(NULL, 0, _scThumbWorkerProc, NULL, 0, NULL);
}

scInternal void _scShutdownThumbWorker() {
  if (!gUI.qThumbQueue.hThread) return;
  scLogDebug("Shutting down background worker for thumbnails");

  gUI.qThumbQueue.bRunning = false;
  SetEvent(gUI.qThumbQueue.hEvent);

  WaitForSingleObject(gUI.qThumbQueue.hThread, INFINITE);
  CloseHandle(gUI.qThumbQueue.hThread);
  CloseHandle(gUI.qThumbQueue.hEvent);

  EnterCriticalSection(&gUI.qThumbQueue.csQueue);
  scThumbTask* pCurr = gUI.qThumbQueue.pHead;
  while (pCurr) {
    scThumbTask* pNext = pCurr->pNext;
    free(pCurr);
    pCurr = pNext;
  }
  gUI.qThumbQueue.pHead = gUI.qThumbQueue.pTail = NULL;
  LeaveCriticalSection(&gUI.qThumbQueue.csQueue);

  DeleteCriticalSection(&gUI.qThumbQueue.csQueue);
  DeleteCriticalSection(&gUI.qThumbQueue.csCache);
  gUI.qThumbQueue.hThread = NULL;
}

//------------------------------------------------------------------------
// Custom node drawing
//------------------------------------------------------------------------
scInternal void
_scDrawHotkey(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scUITheme* t = &gUI.theme;
  scHotkeyID eHotkey = *(scHotkeyID*)scGetUserData(pNode);
  scHotkey* pHk = &gApp->config.aHotkeys[eHotkey];
  bool bEditing = (gUI.iEditingHotkey == (s32)eHotkey);

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, _scRoundRad(), gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  if (bEditing) {
    gpFillRound(pGraphics, r, _scRoundRad(), gpColor(t->dwAccent, 36));
    gpStrokeRound(pGraphics, r, _scRoundRad(), gpColor(t->dwAccent, 255), 1.5f);
  } else {
    gpStrokeRound(pGraphics, r, _scRoundRad(), gpColor(t->dwStrokeSoft, 255), 1.0f);
  }

  COLORREF dwIndicator = (pHk->uKey == 0) ? t->dwPillOff : (pHk->bRegistered ? t->dwOk : t->dwError);
  gpFillEllipse(pGraphics, (f32)(r.left + _scScale(11)), (f32)(r.top + _scScale(10)), (f32)_scScale(9), (f32)_scScale(9), gpColor(dwIndicator, 255));
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
_scDrawFolderIcon(HDC hDC, RECT rImage, bool bUp, s32 nItemCount) {
  scUITheme* t = &gUI.theme;
  s32 iCX = (rImage.left + rImage.right) / 2;
  s32 iCY = (rImage.top + rImage.bottom) / 2;
  s32 iW  = _scScale(84), iH = _scScale(60);

  RECT rBody = { iCX - iW / 2, iCY - iH / 2 + _scScale(8), iCX + iW / 2, iCY + iH / 2 + _scScale(8) };

  s32 iTabXOffset = _scScale(4);
  s32 iTabYOffset = _scScale(4);
  s32 iTabW = _scScale(32), iTabH = _scScale(14);

  RECT rTab = {
    rBody.left + iTabXOffset,
    rBody.top - iTabH + iTabYOffset,
    rBody.left + iTabXOffset + iTabW,
    rBody.top + iTabYOffset
  };

  f32 fTabRad  = 3.0f * gUI.fUIScale;
  f32 fBodyRad = 6.0f * gUI.fUIScale;

  GpPath* pPath = NULL;
  GdipCreatePath(FillModeWinding, &pPath);

  gp__round_figure(pPath, (float)rTab.left, (float)rTab.top,
                    (float)(rTab.right - rTab.left), (float)(rTab.bottom - rTab.top), fTabRad);
  gp__round_figure(pPath, (float)rBody.left, (float)rBody.top,
                    (float)(rBody.right - rBody.left), (float)(rBody.bottom - rBody.top), fBodyRad);

  COLORREF dwFill = t->dwAccent;
  BYTE     nAlpha = bUp ? 130 : 190;
  ARGB argb = ((ARGB)nAlpha << 24) | (GetRValue(dwFill) << 16) | (GetGValue(dwFill) << 8) | GetBValue(dwFill);

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  GpBrush* pBrush = NULL;
  GdipCreateSolidFill(argb, (GpSolidFill**)&pBrush);
  GdipFillPath(pGraphics, pBrush, pPath);
  GdipDeleteBrush(pBrush);
  gpGraphicsEnd(pGraphics);

  GdipDeletePath(pPath);

  if (bUp) {
    SelectObject(hDC, t->pBoldFont);
    SetTextColor(hDC, RGB(255, 255, 255));
    RECT rArrow = rBody;
    DrawTextW(hDC, L"\x2191", -1, &rArrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  } else {
    wchar_t wszCount[16];
    _snwprintf_s(wszCount, 16, _TRUNCATE, L"%d", nItemCount);

    SelectObject(hDC, t->pBoldFont);
    SetTextColor(hDC, RGB(255, 255, 255));
    RECT rCount = rBody;
    DrawTextW(hDC, wszCount, -1, &rCount, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}

scInternal void
_scDrawGalleryItem(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  scUITheme* t = &gUI.theme;
  f32 fRad = 8.0f * gUI.fUIScale;
  scGalleryItem* pItem = (scGalleryItem*)scGetUserData(pNode);
  bool bIsUp     = pItem->bIsUp;
  bool bIsFolder = pItem->bIsFolder;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  gpGraphicsEnd(pGraphics);

  HRGN hClip = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, (int)(fRad * 2), (int)(fRad * 2));
  int iSavedDC = SaveDC(hDC);
  ExtSelectClipRgn(hDC, hClip, RGN_AND);
  SetBkMode(hDC, TRANSPARENT);

  RECT rImage = { r.left + _scScale(2), r.top + _scScale(2), r.right - _scScale(2), r.bottom - _scScale(30) };

  if (bIsUp || bIsFolder) {
    _scDrawFolderIcon(hDC, rImage, bIsUp, pItem->nItemCount);
  } else {
    HBITMAP hThumb = _scThumbGet(pItem->szPath);
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
      const wchar_t* wszExt = wcsrchr(pItem->wszName, L'.');
      DrawTextW(hDC, wszExt ? wszExt + 1 : pItem->wszName, -1, &rImage, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  RECT rText = { r.left + _scScale(5), r.bottom - _scScale(30), r.right - _scScale(5), r.bottom };
  SelectObject(hDC, t->pFont);
  SetTextColor(hDC, t->dwText);
  const wchar_t* wszLabel = bIsUp ? scLocaleGet("Back") : pItem->wszName;
  DrawTextW(hDC, wszLabel, -1, &rText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  RestoreDC(hDC, iSavedDC);
  DeleteObject(hClip);

  pGraphics = gpGraphicsBegin(hDC);
  gpStrokeRound(pGraphics, r, fRad, gpColor(bHovered ? t->dwAccent : t->dwStrokeSoft, 255), bHovered ? 1.5f : 1.0f);
  gpGraphicsEnd(pGraphics);
}

scInternal COLORREF
_scLogColor(scLogLevel eLevel) {
  switch (eLevel) {
    case SC_LOG_ERROR: return gUI.theme.dwError;
    case SC_LOG_WARN:  return gUI.theme.dwWarn;
    case SC_LOG_INFO:  return gUI.theme.dwText;
    case SC_LOG_DEBUG:
    default:           return gUI.theme.dwTextDim;
  }
}

scInternal s32
_scMeasureLogs(scNode* pNode, HDC hDC, s32 iWidth) {
  s32 iCount = scLogStoreCount();
  if (iCount > gUI.nLogCap) {
    gUI.nLogCap   = iCount * 2;
    gUI.aLogLines = (scLogLine*)realloc(gUI.aLogLines, gUI.nLogCap * sizeof(scLogLine));
  }
  if (iWidth != gUI.iLogWidth) {
    gUI.iLogWidth = iWidth;
    gUI.nLogLines = 0;
  }

  HGDIOBJ hOldFont = SelectObject(hDC, gUI.theme.pFont);
  s32 iTop = 0;
  for (s32 i = 0; i < iCount; ++i) {
    scLogLevel eLevel;
    const char* szText = scLogStoreGet(i, &eLevel);
    if (!szText) {
      szText = "";
    }
    s32 nLen = (s32)strlen(szText);

    scLogLine* pLine = &gUI.aLogLines[i];
    if (i >= gUI.nLogLines || pLine->szText != szText || pLine->nLen != nLen) {
      RECT rc = { 0, 0, iWidth > 1 ? iWidth : 1, 0 };
      DrawTextA(hDC, nLen ? szText : " ", nLen ? nLen : 1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      pLine->szText  = szText;
      pLine->nLen    = nLen;
      pLine->iHeight = rc.bottom + _scScale(4);
    }
    pLine->iTop = iTop;
    iTop += pLine->iHeight;
  }
  gUI.nLogLines = iCount;

  SelectObject(hDC, hOldFont);
  return iTop;
}

scInternal void
_scDrawLogs(scNode* pNode, HDC hDC, RECT r, RECT rcVisible, bool bHovered) {
  s32 nLines = gUI.nLogLines < scLogStoreCount() ? gUI.nLogLines : scLogStoreCount();

  SelectObject(hDC, gUI.theme.pFont);
  SetBkMode(hDC, TRANSPARENT);
  for (s32 i = 0; i < nLines; ++i) {
    scLogLine* pLine = &gUI.aLogLines[i];
    s32 iTop = r.top + pLine->iTop;
    if (iTop + pLine->iHeight <= rcVisible.top) {
      continue;
    }
    if (iTop >= rcVisible.bottom) {
      break;
    }
    scLogLevel eLevel;
    const char* szText = scLogStoreGet(i, &eLevel);
    if (!szText) {
      continue;
    }
    SetTextColor(hDC, _scLogColor(eLevel));
    RECT rcLine = { r.left, iTop, r.right, iTop + pLine->iHeight };
    DrawTextA(hDC, szText, -1, &rcLine, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  }
}

//------------------------------------------------------------------------
// Gallery
//------------------------------------------------------------------------
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
  FILETIME ftCreate;
} _scFileEntry;

scInternal s32
_scCountFolderItems(const wchar_t* wszPath) {
  wchar_t wszPattern[MAX_PATH];
  _snwprintf_s(wszPattern, MAX_PATH, _TRUNCATE, L"%ls\\*", wszPath);

  s32 nCount = 0;
  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(wszPattern, &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_scIsImageFile(fd.cFileName)) {
        continue;
      }
      ++nCount;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
  }
  return nCount;
}

scInternal int
_scCompareFileTime(const void* a, const void* b) {
  const _scFileEntry* pA = (const _scFileEntry*)a;
  const _scFileEntry* pB = (const _scFileEntry*)b;
  return CompareFileTime(&pB->ftCreate, &pA->ftCreate); // newest first
}

scInternal void
_scGalleryLoadImages(const wchar_t* wszPath, _scFileEntry** paOutFiles, s32* pnOutCount, s32* pnOutCap) {
  wchar_t wszPattern[MAX_PATH];
  _snwprintf_s(wszPattern, MAX_PATH, _TRUNCATE, L"%ls\\*", wszPath);

  _scFileEntry* aFiles = *paOutFiles;
  s32 nFiles = *pnOutCount;
  s32 nFileCap = *pnOutCap;

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
      aFiles[nFiles].ftCreate = fd.ftCreationTime;
      ++nFiles;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
  }

  *paOutFiles = aFiles;
  *pnOutCount = nFiles;
  *pnOutCap   = nFileCap;
}

scInternal void
_scGalleryLoadFolders(const wchar_t* wszPath, _scFileEntry** paOutFolders, s32* pnOutCount, s32* pnOutCap) {
  wchar_t wszPattern[MAX_PATH];
  _snwprintf_s(wszPattern, MAX_PATH, _TRUNCATE, L"%ls\\*", wszPath);

  _scFileEntry* aFolders = *paOutFolders;
  s32 nFolders = *pnOutCount;
  s32 nFolderCap = *pnOutCap;

  WIN32_FIND_DATAW fd;
  HANDLE hFind = FindFirstFileW(wszPattern, &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        continue;
      }
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
        continue;
      }
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }
      if (nFolders >= nFolderCap) {
        nFolderCap = nFolderCap ? nFolderCap * 2 : 16;
        aFolders   = (_scFileEntry*)realloc(aFolders, nFolderCap * sizeof(_scFileEntry));
      }
      wcscpy_s(aFolders[nFolders].wszName, MAX_PATH, fd.cFileName);
      aFolders[nFolders].ftCreate = fd.ftCreationTime;
      ++nFolders;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
  }

  *paOutFolders = aFolders;
  *pnOutCount = nFolders;
  *pnOutCap   = nFolderCap;
}

scInternal void
_scPathStripTrailingSlash(wchar_t* wszPath) {
  s32 iLen = (s32)wcslen(wszPath);
  while (iLen > 0 && wszPath[iLen - 1] == L'\\') {
    wszPath[--iLen] = 0;
  }
}

scInternal bool
_scPathsEqualNormalized(const wchar_t* wszA, const wchar_t* wszB) {
  wchar_t wszNormA[MAX_PATH], wszNormB[MAX_PATH];
  wcscpy_s(wszNormA, MAX_PATH, wszA);
  wcscpy_s(wszNormB, MAX_PATH, wszB);
  _scPathStripTrailingSlash(wszNormA);
  _scPathStripTrailingSlash(wszNormB);
  return _wcsicmp(wszNormA, wszNormB) == 0;
}

scInternal bool
_scPathIsUnderRoot(const wchar_t* wszChild, const wchar_t* wszRoot) {
  wchar_t wszNormChild[MAX_PATH], wszNormRoot[MAX_PATH];
  wcscpy_s(wszNormChild, MAX_PATH, wszChild);
  wcscpy_s(wszNormRoot, MAX_PATH, wszRoot);
  _scPathStripTrailingSlash(wszNormChild);
  _scPathStripTrailingSlash(wszNormRoot);

  s32 iRootLen = (s32)wcslen(wszNormRoot);
  if (_wcsnicmp(wszNormChild, wszNormRoot, iRootLen) != 0) {
    return false;
  }
  return wszNormChild[iRootLen] == 0 || wszNormChild[iRootLen] == L'\\';
}

scInternal void
_scGalleryRefresh(s32 nRestoreScrollY) {
  _scThumbCacheClear();
  gUI.aPageScrollY[SC_PAGE_GALLERY] = nRestoreScrollY;
  gUI.pContent = NULL;
  _scRebuild();
}

scInternal void
_scGalleryHistorySaveScroll() {
  if (gUI.nGalleryHistoryCount > 0) {
    gUI.anGalleryScrollY[gUI.nGalleryHistoryPos] = _scCurrentScrollY();
  }
}

scInternal void
_scGalleryHistoryPush(const wchar_t* wszDir) {
  gUI.nGalleryHistoryCount = (gUI.nGalleryHistoryCount == 0) ? 0 : gUI.nGalleryHistoryPos + 1;

  if (gUI.nGalleryHistoryCount >= SC_GALLERY_HISTORY_MAX) {
    memmove(gUI.aGalleryHistory[0], gUI.aGalleryHistory[1], (SC_GALLERY_HISTORY_MAX - 1) * sizeof(gUI.aGalleryHistory[0]));
    --gUI.nGalleryHistoryCount;
  }

  wcscpy_s(gUI.aGalleryHistory[gUI.nGalleryHistoryCount], MAX_PATH, wszDir);
  gUI.anGalleryScrollY[gUI.nGalleryHistoryCount] = 0;
  ++gUI.nGalleryHistoryCount;
  gUI.nGalleryHistoryPos = gUI.nGalleryHistoryCount - 1;
}

scInternal void
_scGalleryNavigateInto(const char* szFolderPath) {
  _scGalleryHistorySaveScroll();
  wchar_t wszPath[MAX_PATH];
  MultiByteToWideChar(CP_ACP, 0, szFolderPath, -1, wszPath, MAX_PATH);
  wcscpy_s(gUI.wszGalleryDir, MAX_PATH, wszPath);
  _scGalleryHistoryPush(gUI.wszGalleryDir);
  _scGalleryRefresh(0);
}

scInternal void
_scGalleryHistoryBack() {
  if (gUI.nGalleryHistoryPos <= 0) {
    return;
  }
  _scGalleryHistorySaveScroll();
  --gUI.nGalleryHistoryPos;
  wcscpy_s(gUI.wszGalleryDir, MAX_PATH, gUI.aGalleryHistory[gUI.nGalleryHistoryPos]);
  _scGalleryRefresh(gUI.anGalleryScrollY[gUI.nGalleryHistoryPos]);
}

scInternal void
_scGalleryNavigateUp() {
  _scGalleryHistorySaveScroll();
  wchar_t wszParent[MAX_PATH];
  wcscpy_s(wszParent, MAX_PATH, gUI.wszGalleryDir);
  _scPathStripTrailingSlash(wszParent);

  wchar_t* pSlash = wcsrchr(wszParent, L'\\');
  if (pSlash) {
    *pSlash = 0;
  }

  wchar_t wszRoot[MAX_PATH];
  wcscpy_s(wszRoot, MAX_PATH, gApp->config.wszSavePath);

  // Never let the explorer wander above the configured screenshot root folder.
  if (!_scPathIsUnderRoot(wszParent, wszRoot)) {
    wcscpy_s(wszParent, MAX_PATH, wszRoot);
  }

  if (gUI.nGalleryHistoryPos > 0 &&
    _scPathsEqualNormalized(wszParent, gUI.aGalleryHistory[gUI.nGalleryHistoryPos - 1])) {
    _scGalleryHistoryBack();
    return;
  }

  wcscpy_s(gUI.wszGalleryDir, MAX_PATH, wszParent);
  _scGalleryHistoryPush(gUI.wszGalleryDir);
  _scGalleryRefresh(0);
}

scInternal void
_scGalleryHistoryForward() {
  if (gUI.nGalleryHistoryPos >= gUI.nGalleryHistoryCount - 1) {
    return;
  }
  _scGalleryHistorySaveScroll();
  ++gUI.nGalleryHistoryPos;
  wcscpy_s(gUI.wszGalleryDir, MAX_PATH, gUI.aGalleryHistory[gUI.nGalleryHistoryPos]);
  _scGalleryRefresh(gUI.anGalleryScrollY[gUI.nGalleryHistoryPos]);
}

scInternal void
_scOnGalleryClick(scNode* pNode) {
  scGalleryItem* pItem = (scGalleryItem*)scGetUserData(pNode);
  if (pItem->bIsUp) {
    _scGalleryNavigateUp();
  } else if (pItem->bIsFolder) {
    _scGalleryNavigateInto(pItem->szPath);
  } else {
    ShellExecuteA(NULL, "open", pItem->szPath, NULL, NULL, SW_SHOWNORMAL);
  }
}

scInternal void
_scOnGalleryDrag(scNode* pNode) {
  scGalleryItem* pItem = (scGalleryItem*)scGetUserData(pNode);
  char szPath[MAX_PATH];
  strcpy_s(szPath, MAX_PATH, pItem->szPath);
  _scStartImageDrag(szPath);
}

scInternal void
_scOnGalleryContext(scNode* pNode, POINT pt) {
  scGalleryItem* pItem = (scGalleryItem*)scGetUserData(pNode);
  HWND hWnd = gUI.hWindow;
  bool bIsFolder = pItem->bIsFolder;
  char szPath[MAX_PATH];
  strcpy_s(szPath, MAX_PATH, pItem->szPath);

  HMENU hMenu = CreatePopupMenu();

  MENUINFO mi = { sizeof(MENUINFO) };
  mi.fMask   = MIM_BACKGROUND | MIM_STYLE;
  mi.dwStyle = MNS_NOCHECK;
  mi.hbrBack = gUI.theme.hBackgroundBrush;
  SetMenuInfo(hMenu, &mi);

  AppendMenuW(hMenu, MF_OWNERDRAW, 2, (LPCWSTR)scLocaleGet("Open Folder"));
  if (!bIsFolder) {
    AppendMenuW(hMenu, MF_OWNERDRAW, 1, (LPCWSTR)scLocaleGet("Copy to Clipboard"));
    AppendMenuW(hMenu, MF_OWNERDRAW, 3, (LPCWSTR)scLocaleGet("Open"));
    AppendMenuW(hMenu, MF_OWNERDRAW, 4, (LPCWSTR)scLocaleGet("Delete"));
  }

  POINT ptScreen = pt;
  ClientToScreen(hWnd, &ptScreen);
  int iCmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, ptScreen.x, ptScreen.y, 0, hWnd, NULL);
  DestroyMenu(hMenu);

  switch (iCmd) {
    case 1: {
      wchar_t wszPath[MAX_PATH];
      MultiByteToWideChar(CP_ACP, 0, szPath, -1, wszPath, MAX_PATH);
      scClipboardSetFile(&gApp->stClipboard, wszPath);
      break;
    }
    case 2: {
      char szArg[MAX_PATH + 16];
      if (!bIsFolder) {
        _snprintf_s(szArg, sizeof(szArg), _TRUNCATE, "/select,\"%s\"", szPath);
      } else {
        _snprintf_s(szArg, sizeof(szArg), _TRUNCATE, "%s", szPath);
      }
      ShellExecuteA(NULL, "open", "explorer.exe", szArg, NULL, SW_SHOWNORMAL);
      break;
    }
    case 3: {
      ShellExecuteA(NULL, "open", szPath, NULL, NULL, SW_SHOWNORMAL);
      break;
    }
    case 4: {
      if (MessageBoxW(hWnd, scLocaleGet("Are you sure you want to delete this screenshot?"), scLocaleGet("Confirm Delete"), MB_ICONWARNING | MB_YESNO) == IDYES) {
        wchar_t wszPath[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, szPath, -1, wszPath, MAX_PATH);
        DeleteFileW(wszPath);
        _scRebuild();
      }
      break;
    }
  }
}

scInternal void
_scOnHotkeyClick(scNode* pNode) {
  _scBeginHotkeyRecording((s32)*(scHotkeyID*)scGetUserData(pNode));
  scGuiInvalidate(gUI.pGui);
}

static const scCustomOps kHotkeyOps        = { _scDrawHotkey,      _scOnHotkeyClick,  NULL,             NULL };
static const scCustomOps kLogOps           = { _scDrawLogs,        NULL,              NULL,             NULL, _scMeasureLogs };
static const scCustomOps kGalleryUpOps     = { _scDrawGalleryItem, _scOnGalleryClick, NULL,             NULL };
static const scCustomOps kGalleryFolderOps = { _scDrawGalleryItem, _scOnGalleryClick, NULL,             _scOnGalleryContext };
static const scCustomOps kGalleryFileOps   = { _scDrawGalleryItem, _scOnGalleryClick, _scOnGalleryDrag, _scOnGalleryContext };

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
// Pages
//------------------------------------------------------------------------
static const char* const kFramerateOptions[] = { "60", "30", "24" };
static const s32         kFramerateValues[]  = { 60, 30, 24 };

static const char* const kRecordQualityOptions[] = { "High", "Medium", "Low" };
static const s32         kRecordQualityValues[]  = { SC_QUALITY_HIGH, SC_QUALITY_MEDIUM, SC_QUALITY_LOW };

static const char* const kRecordResOptions[] = { "Native", "1080p", "720p" };
static const s32         kRecordResValues[]  = { SC_MAXRES_NATIVE, SC_MAXRES_1080P, SC_MAXRES_720P };

#define SC_FRAMERATE_COUNT 3

static const char* const kAudioTrackOptions[] = { "0", "1", "2", "3", "4", "5", "6", "7" };

static const char** gAudioDeviceOptions;
static uint32_t gAudioDeviceOptionsCap;

scInternal bool
_scBuildAudioDeviceOptions(void) {
  uint32_t nRequired = gAudio.deviceCount + 1;
  if (nRequired > gAudioDeviceOptionsCap) {
    const char** aOptions = (const char**)realloc(gAudioDeviceOptions, nRequired * sizeof(char*));
    if (!aOptions) {
      return false;
    }
    gAudioDeviceOptions = aOptions;
    gAudioDeviceOptionsCap = nRequired;
  }

  gAudioDeviceOptions[0] = "";
  for (uint32_t i = 0; i < gAudio.deviceCount; ++i) {
    gAudioDeviceOptions[i + 1] = gAudio.pCaptureDevices[i].szName;
  }
  return true;
}

scInternal void
_scOnBrowseClicked(s32 iParam) {
  (void)iParam;
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

          gUI.aPageScrollY[SC_PAGE_GALLERY] = 0;
          _scRebuild();
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
_scOnLanguageSelected(s32 iIndex, s32 iParam) {
  (void)iParam;
  scLocaleSet(scLocaleCode(iIndex));
  _scRebuild();
}

scInternal void
_scOnCaptureAudioToggled(bool bValue, s32 iParam) {
  (void)bValue; (void)iParam;
  _scRebuild();
}

scInternal void
_scOnAudioDeviceSelected(s32 iIndex, s32 iParam) {
  scAudioSelectionSetDevice((uint32_t)iParam, iIndex - 1);
  _scRebuild();
}

scInternal void
_scOnAudioTrackSelected(s32 iIndex, s32 iParam) {
  scAudioSelectionSetTrack((uint32_t)iParam, iIndex);
  _scRebuild();
}

scInternal void
_scOnAudioAddDevice(s32 iParam) {
  (void)iParam;
  if (scAudioSelectionAdd()) {
    _scRebuild();
  }
}

scInternal void
_scOnAudioRemoveDevice(s32 iParam) {
  scAudioSelectionRemove((uint32_t)iParam);
  _scRebuild();
}

scInternal void
_scOnFramerateSet(s32 iIndex, s32 iParam) {
  (void)iParam;
  if (iIndex >= 0 && iIndex < SC_FRAMERATE_COUNT) {
    gApp->config.iFFmpegFramerate = kFramerateValues[iIndex];
    scSaveConfig();
  }
}

scInternal void
_scOnRecordQualitySet(s32 iIndex, s32 iParam) {
  (void)iParam;
  if (iIndex >= 0 && iIndex < SC_QUALITY_COUNT) {
    gApp->config.eRecordQuality = kRecordQualityValues[iIndex];
    scSaveConfig();
  }
}

scInternal void
_scOnRecordResSet(s32 iIndex, s32 iParam) {
  (void)iParam;
  if (iIndex >= 0 && iIndex < SC_MAXRES_COUNT) {
    gApp->config.eRecordMaxRes = kRecordResValues[iIndex];
    scSaveConfig();
  }
}

scInternal void
_scUpdateIconOption(bool bEnabled, s32 _) {
  _scUpdateWindowIcon(gUI.hWindow, bEnabled);
  scTrayUpdateIcon();
}


scInternal void
_scOnTabClicked(s32 iParam) {
  scUISetCurrentPage((scPageID)iParam);
}

scInternal s32
_scFindIndex(const s32* aValues, s32 nCount, s32 iValue) {
  for (s32 i = 0; i < nCount; ++i) {
    if (aValues[i] == iValue) {
      return i;
    }
  }
  return 0;
}

scInternal void
_scAddToggle(scNode* pParent, const char* szKey, bool* pValue, void (*pfnChange)(bool bValue, s32 iParam)) {
  scAddChild(pParent, scCreateToggle(scLocaleGet(szKey), pValue, 0, pfnChange));
}

scInternal scNode*
_scCreatePage() {
  scNode* pPage = scCreateVStack(scLengthStretch(1.0f), scLengthPercent(1.0f));
  scSetPaddingEx(pPage, 10, 20, 20, 20);
  scSetSpacing(pPage, 5);
  scSetScrollable(pPage, true);
  return pPage;
}

scInternal scNode*
_scBuildGeneral() {
  scNode* pPage = _scCreatePage();
  for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    if (i == SC_HOTKEY_OCR && !gApp->bIsGeWin10) {
      continue;
    }
    scNode* pButton = scCreateButton(scLocaleGet(scCaptureActionNames[i]), scAppRunHandlerFromActionID, i);
    scSetSize(pButton, scLengthStretch(1.0f), scLengthPixels(35));
    scAddChild(pPage, pButton);
  }
  return pPage;
}

scInternal scNode*
_scBuildSettings() {
  scNode* pPage = _scCreatePage();

  scAddChild(pPage, scCreateLabel(scLocaleGet("Screenshot Destination"), true));

  scNode* pPathRow = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(30));
  scSetSpacing(pPathRow, 10);
  scAddChild(pPathRow, scCreateField(gApp->config.wszSavePath));
  scNode* pBrowse = scCreateButton(scLocaleGet("Browse..."), _scOnBrowseClicked, 0);
  scSetSize(pBrowse, scLengthPixels(100), scLengthPixels(30));
  scAddChild(pPathRow, pBrowse);
  scAddChild(pPage, pPathRow);
  scAddChild(pPage, scCreateSpacer(scLengthPixels(1), scLengthPixels(8)));

  s32 iLanguage = 0;
  for (s32 i = 0; i < scLocaleCount(); ++i) {
    if (strcmp(scLocaleCode(i), scLocaleCurrent()) == 0) {
      iLanguage = i;
      break;
    }
  }
  scAddChild(pPage, scCreateDropdown(scLocaleGet("Language"), scLocaleCodes(), scLocaleCount(), iLanguage, _scOnLanguageSelected, 0));

  scAddChild(pPage, scCreateDropdown(scLocaleGet("Recording Framerate"), kFramerateOptions, SC_FRAMERATE_COUNT,
    _scFindIndex(kFramerateValues, SC_FRAMERATE_COUNT, gApp->config.iFFmpegFramerate), _scOnFramerateSet, 0));
  scAddChild(pPage, scCreateDropdown(scLocaleGet("Recording Quality"), kRecordQualityOptions, SC_QUALITY_COUNT,
    _scFindIndex(kRecordQualityValues, SC_QUALITY_COUNT, (s32)gApp->config.eRecordQuality), _scOnRecordQualitySet, 0));
  scAddChild(pPage, scCreateDropdown(scLocaleGet("Recording Output Resolution"), kRecordResOptions, SC_MAXRES_COUNT,
    _scFindIndex(kRecordResValues, SC_MAXRES_COUNT, (s32)gApp->config.eRecordMaxRes), _scOnRecordResSet, 0));

  _scAddToggle(pPage, "Show Cursor (Recording)", &gApp->config.bRecordShowCursor, NULL);
  _scAddToggle(pPage, "Force Software Encoding", &gApp->config.bRecordForceSoftware, NULL);
  _scAddToggle(pPage, "Capture Audio", &gApp->config.bCaptureAudio, _scOnCaptureAudioToggled);

  if (gApp->config.bCaptureAudio) {
    bool bHaveDeviceOptions = _scBuildAudioDeviceOptions();
    const char* const* aDeviceOptions = bHaveDeviceOptions ? gAudioDeviceOptions : NULL;
    s32 nDeviceOptions = bHaveDeviceOptions ? (s32)gAudio.deviceCount + 1 : 0;

    for (uint32_t i = 0; i < gAudio.selectionCount; ++i) {
      s32 iSelected = 0;
      if (gAudio.aSelections[i].iDevice >= 0 &&
          (uint32_t)gAudio.aSelections[i].iDevice < gAudio.deviceCount) {
        iSelected = gAudio.aSelections[i].iDevice + 1;
      }

      scNode* pRow = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(40));
      scSetSpacing(pRow, 8);
      scSetAlignment(pRow, SC_ALIGN_MIDDLE);
      scAddChild(pRow, scCreateSpacer(scLengthPixels(12), scLengthPixels(1)));
      scAddChild(pRow, scCreateDropdown(L"", aDeviceOptions, nDeviceOptions, iSelected, _scOnAudioDeviceSelected, (s32)i));

      scNode* pTrack = scCreateDropdown(L"", kAudioTrackOptions, ARRAYSIZE(kAudioTrackOptions), gAudio.aSelections[i].iTrack, _scOnAudioTrackSelected, (s32)i);
      scSetSize(pTrack, scLengthPixels(45), scLengthPixels(40));
      scAddChild(pRow, pTrack);

      scNode* pRemove = scCreateButton(L"-", _scOnAudioRemoveDevice, (s32)i);
      scSetSize(pRemove, scLengthPixels(28), scLengthPixels(32));
      scAddChild(pRow, pRemove);
      scAddChild(pPage, pRow);
    }

    if (gAudio.selectionCount < SC_AUDIO_MAX_DEVICE_SELECTIONS) {
      scNode* pRow = scCreateHStack(scLengthPercent(1.0f), scLengthPixels(35));
      scAddChild(pRow, scCreateSpacer(scLengthPixels(20), scLengthPixels(1)));
      scNode* pAdd = scCreateButton(scLocaleGet("Add Device"), _scOnAudioAddDevice, 0);
      scSetSize(pAdd, scLengthPixels(130), scLengthPixels(35));
      scAddChild(pRow, pAdd);
      scAddChild(pPage, pRow);
    }
  }

  scAddChild(pPage, scCreateSpacer(scLengthPixels(1), scLengthPixels(15)));

  _scAddToggle(pPage, "Copy screenshot to Clipboard", &gApp->config.bCopyToClipboard, NULL);
  _scAddToggle(pPage, "Run on Startup", &gApp->config.bRunAtStartup, scSetRunOnStartup);
  _scAddToggle(pPage, "Play sound on capture", &gApp->config.bPlaySoundOnAction, NULL);
  if (gApp->bIsGeWin10) {
    _scAddToggle(pPage, "Show notification on capture", &gApp->config.bShowNotification, NULL);
  }
  _scAddToggle(pPage, "Start Minimized", &gApp->config.bStartMinimized, NULL);
  _scAddToggle(pPage, "Icon uses Accent Color", &gApp->config.bAccentIcon, _scUpdateIconOption);

  scAddChild(pPage, scCreateLabel(L"Input Settings", true));
  for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    scHotkeyID eHotkey = (scHotkeyID)i;
    scAddChild(pPage, scCreateCustom(scLengthStretch(1.0f), scLengthPixels(30), &kHotkeyOps, &eHotkey, sizeof(eHotkey)));
  }

  return pPage;
}

scInternal void
_scAddGalleryItem(scNode* pGrid, const scCustomOps* pOps, const wchar_t* wszName, const char* szPath, bool bIsUp, bool bIsFolder, s32 nItemCount) {
  scGalleryItem item = { 0 };
  item.bIsUp      = bIsUp;
  item.bIsFolder  = bIsFolder;
  item.nItemCount = nItemCount;
  wcscpy_s(item.wszName, MAX_PATH, wszName);
  strcpy_s(item.szPath, MAX_PATH, szPath);
  scAddChild(pGrid, scCreateCustom(scLengthPixels(320), scLengthPixels(260), pOps, &item, sizeof(item)));
}

scInternal scNode*
_scBuildGallery() {
  scNode* pPage = _scCreatePage();
  scSetPaddingEx(pPage, 20, 20, 10, 20);

  if (gUI.wszGalleryDir[0] == 0) {
    if (!scGetSavePath(gUI.wszGalleryDir, MAX_PATH)) {
      return pPage;
    }
    _scGalleryHistoryPush(gUI.wszGalleryDir);
  }

  wchar_t wszDir[MAX_PATH];
  wcscpy_s(wszDir, MAX_PATH, gUI.wszGalleryDir);

  wchar_t wszRoot[MAX_PATH];
  wcscpy_s(wszRoot, MAX_PATH, gApp->config.wszSavePath);
  bool bAtRoot = _scPathsEqualNormalized(wszDir, wszRoot);

  _scFileEntry* aFolders = NULL;
  s32 nFolders = 0, nFolderCap = 0;
  _scGalleryLoadFolders(wszDir, &aFolders, &nFolders, &nFolderCap);
  if (nFolders > 0) {
    qsort(aFolders, nFolders, sizeof(_scFileEntry), _scCompareFileTime);
  }

  _scFileEntry* aFiles = NULL;
  s32 nFiles = 0, nFileCap = 0;
  _scGalleryLoadImages(wszDir, &aFiles, &nFiles, &nFileCap);
  if (nFiles > 0) {
    qsort(aFiles, nFiles, sizeof(_scFileEntry), _scCompareFileTime);
  }

  if (bAtRoot && nFolders + nFiles == 0) {
    free(aFolders);
    free(aFiles);
    return pPage;
  }

  scNode* pGrid = scCreateHStack(scLengthStretch(1.0f), scLengthAuto());
  scSetWrap(pGrid, true);
  scSetSpacing(pGrid, 20);

  if (!bAtRoot) {
    _scAddGalleryItem(pGrid, &kGalleryUpOps, L"..", "", true, false, 0);
  }

  for (s32 i = 0; i < nFolders; ++i) {
    wchar_t wszFull[MAX_PATH];
    _snwprintf_s(wszFull, MAX_PATH, _TRUNCATE, L"%ls\\%ls", wszDir, aFolders[i].wszName);
    char szFull[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wszFull, -1, szFull, MAX_PATH, NULL, NULL);
    _scAddGalleryItem(pGrid, &kGalleryFolderOps, aFolders[i].wszName, szFull, false, true, _scCountFolderItems(wszFull));
  }

  for (s32 i = 0; i < nFiles; ++i) {
    wchar_t wszFull[MAX_PATH];
    _snwprintf_s(wszFull, MAX_PATH, _TRUNCATE, L"%ls\\%ls", wszDir, aFiles[i].wszName);
    char szFull[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wszFull, -1, szFull, MAX_PATH, NULL, NULL);
    _scAddGalleryItem(pGrid, &kGalleryFileOps, aFiles[i].wszName, szFull, false, false, 0);
  }

  scAddChild(pPage, pGrid);
  free(aFolders);
  free(aFiles);
  return pPage;
}

scInternal scNode*
_scBuildLogs() {
  scNode* pPage = _scCreatePage();
  scSetPaddingEx(pPage, 10, 14, 20, 6);
  scSetSpacing(pPage, 0);
  scSetStickBottom(pPage, true);

  gUI.pLogView = scCreateCustom(scLengthStretch(1.0f), scLengthAuto(), &kLogOps, NULL, 0);
  scAddChild(pPage, gUI.pLogView);
  return pPage;
}

scInternal scNode*
_scBuildCredits() {
  typedef struct {
    const wchar_t* pName;
    const wchar_t* pText;
  } scLicenseExample;

  static const scLicenseExample kLicenses[] = {
    {
      L"synthesthea (https://art.synthesthea.com/)",
      L"Screenshot sounds\n"
      L"The new icon :)"
    },
    {
      L"miniaudio (MIT-0)",
      L"Copyright 2025 David Reid\n"
      L"\n"
      L"Permission is hereby granted, free of charge, to any person obtaining a copy of\n"
      L"this software and associated documentation files (the \"Software\"), to deal in\n"
      L"the Software without restriction, including without limitation the rights to\n"
      L"use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies\n"
      L"of the Software, and to permit persons to whom the Software is furnished to do\n"
      L"so.\n"
      L"\n"
      L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
      L"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
      L"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
      L"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
      L"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
      L"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
      L"SOFTWARE."
    },
    {
      L"stb (MIT)",
      L"Copyright (c) 2017 Sean Barrett\n"
      L"\n"
      L"Permission is hereby granted, free of charge, to any person obtaining a copy of\n"
      L"this software and associated documentation files (the \"Software\"), to deal in\n"
      L"the Software without restriction, including without limitation the rights to\n"
      L"use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies\n"
      L"of the Software, and to permit persons to whom the Software is furnished to do\n"
      L"so, subject to the following conditions:\n"
      L"\n"
      L"The above copyright notice and this permission notice shall be included in all\n"
      L"copies or substantial portions of the Software.\n"
      L"\n"
      L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
      L"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
      L"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
      L"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
      L"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
      L"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
      L"SOFTWARE."
    }
  };

  scNode* pPage = _scCreatePage();
  scAddChild(pPage, scCreateLabel(L"Credits and Third-Party Licenses", true));

  for (s32 i = 0; i < (s32)ARRAYSIZE(kLicenses); ++i) {
    scAddChild(pPage, scCreateLabel(kLicenses[i].pName, true));

    scNode* pBlock = scCreateVStack(scLengthPercent(1.0f), scLengthAuto());
    scSetPaddingEx(pBlock, 10, 0, 0, 0);
    scNode* pText = scCreateLabel(kLicenses[i].pText, false);
    scSetWrap(pText, true);
    scSetSize(pText, scLengthStretch(1.0f), scLengthAuto());
    scAddChild(pBlock, pText);
    scAddChild(pPage, pBlock);
    scAddChild(pPage, scCreateSpacer(scLengthPixels(1), scLengthPixels(5)));
  }
  return pPage;
}

scInternal scNode*
_scBuildPage(scPageID ePage) {
  switch (ePage) {
    case SC_PAGE_GENERAL:  return _scBuildGeneral();
    case SC_PAGE_SETTINGS: return _scBuildSettings();
    case SC_PAGE_GALLERY:  return _scBuildGallery();
    case SC_PAGE_LOGS:     return _scBuildLogs();
    case SC_PAGE_CREDITS:  return _scBuildCredits();
    default:               return _scCreatePage();
  }
}

scInternal void
_scOnVersionLinkClicked(s32 iParam) {
  (void)iParam;
  char cLink[1024] = {0};
  snprintf(cLink, sizeof(cLink), "https://www.github.com/daniel-w0/screencap/releases/tag/%s", SC_VERSION_STRING);
  ShellExecuteA(NULL, "open", cLink, NULL, NULL, SW_SHOW);
}

scInternal scNode*
_scBuildSidebar() {
  scNode* pSidebar = scCreateVStack(scLengthPixels(SIDEBAR_WIDTH), scLengthPercent(1.0f));
  scSetPaddingEx(pSidebar, 10, 20, 10, 0);
  scSetSpacing(pSidebar, 5);
  scSetSurface(pSidebar, true);

  for (s32 i = 0; i < _SC_PAGE_COUNT; ++i) {
    scAddChild(pSidebar, scCreateTab(scLocaleGet(kPageKeys[i]), i == (s32)gUI.eCurrentPage, _scOnTabClicked, i));
  }
  scAddChild(pSidebar, scCreateSpacer(scLengthPixels(1), scLengthStretch(1.0f)));
  scAddChild(pSidebar, scCreateLink(SC_VERSION_STRING_FULL_W, _scOnVersionLinkClicked, 0));
  return pSidebar;
}

scInternal void
_scRebuild() {
  if (!gUI.pGui) {
    return;
  }
  _scSavePageScroll();
  gUI.pLogView = NULL;

  scNode* pRoot = scCreateHStack(scLengthPercent(1.0f), scLengthPercent(1.0f));
  scAddChild(pRoot, _scBuildSidebar());
  gUI.pContent = _scBuildPage(gUI.eCurrentPage);
  scSetScrollY(gUI.pContent, gUI.aPageScrollY[gUI.eCurrentPage]);
  scAddChild(pRoot, gUI.pContent);
  scGuiSetRoot(gUI.pGui, pRoot);
}

//------------------------------------------------------------------------
// Rendering
//------------------------------------------------------------------------
scInternal void
_scRender(HDC hDC, const RECT* pClip) {
  RECT cr;
  GetClientRect(gUI.hWindow, &cr);
  if (!gUI.pGui || cr.right <= 0 || cr.bottom <= 0 || IsRectEmpty(pClip)) {
    return;
  }

  if (!gUI.hBackBitmap || gUI.iBackWidth != cr.right || gUI.iBackHeight != cr.bottom) {
    if (gUI.hBackBitmap) {
      DeleteObject(gUI.hBackBitmap);
    }
    gUI.hBackBitmap = CreateCompatibleBitmap(hDC, cr.right, cr.bottom);
    gUI.iBackWidth  = cr.right;
    gUI.iBackHeight = cr.bottom;
  }

  HDC hMemDC = CreateCompatibleDC(hDC);
  HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, gUI.hBackBitmap);
  IntersectClipRect(hMemDC, pClip->left, pClip->top, pClip->right, pClip->bottom);

  FillRect(hMemDC, pClip, gUI.theme.hBackgroundBrush);
  scGuiPaint(gUI.pGui, hMemDC, pClip);

  BitBlt(hDC, pClip->left, pClip->top, pClip->right - pClip->left, pClip->bottom - pClip->top, hMemDC, pClip->left, pClip->top, SRCCOPY);
  SelectObject(hMemDC, hOldBmp);
  DeleteDC(hMemDC);
}

//------------------------------------------------------------------------
// Window Procedure
//------------------------------------------------------------------------
LRESULT CALLBACK UIWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_THUMB_LOADED: {
      InvalidateRect(hWnd, NULL, FALSE);
      return 0;
    }
    case WM_REBUILD_UI: {
      if (gUI.eCurrentPage == (scPageID)wParam) {
        _scRebuild();
      }
      return 0;
    }
    case WM_LOG_PUSHED: {
      InterlockedExchange(&gUI.lLogPending, 0);
      if (gUI.pLogView) {
        scGuiInvalidateLayout(gUI.pGui);
      }
      return 0;
    }
    case WM_GETMINMAXINFO: {
      MINMAXINFO* pMmi = (MINMAXINFO*)lParam;
      pMmi->ptMinTrackSize.x = _scScale(WINDOW_MIN_WIDTH);
      pMmi->ptMinTrackSize.y = _scScale(WINDOW_MIN_HEIGHT);
      return 0;
    }
    case WM_SIZE: {
      if (wParam != SIZE_MINIMIZED) {
        scGuiResize(gUI.pGui, LOWORD(lParam), HIWORD(lParam));
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      scGuiMouseMove(gUI.pGui, pt);
      if (!gUI.bTrackingMouse) {
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, HOVER_DEFAULT };
        gUI.bTrackingMouse = TrackMouseEvent(&tme) != 0;
      }
      return 0;
    }
    case WM_MOUSELEAVE: {
      gUI.bTrackingMouse = false;
      scGuiMouseLeave(gUI.pGui);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      ScreenToClient(hWnd, &pt);
      scGuiMouseWheel(gUI.pGui, pt, GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      scGuiMouseDown(gUI.pGui, pt);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      scGuiMouseUp(gUI.pGui, pt);
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
      scGuiMouseRightUp(gUI.pGui, pt);
      return 0;
    }
    case WM_XBUTTONUP: {
      if (gUI.eCurrentPage == SC_PAGE_GALLERY) {
        s32 iButton = GET_XBUTTON_WPARAM(wParam);
        if (iButton == XBUTTON1) {
          _scGalleryHistoryBack();
        } else if (iButton == XBUTTON2) {
          _scGalleryHistoryForward();
        }
      }
      return TRUE;
    }
    case WM_CAPTURECHANGED: {
      scGuiCaptureLost(gUI.pGui);
      return 0;
    }
    case WM_DRAWITEM: {
      LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
      if (pDIS->CtlType == ODT_MENU) {
        FillRect(pDIS->hDC, &pDIS->rcItem, gUI.theme.hBackgroundBrush);
        if (pDIS->itemState & ODS_SELECTED) {
          RECT rcSel = pDIS->rcItem;
          InflateRect(&rcSel, -_scScale(3), -_scScale(2));
          GpGraphics* pGraphics = gpGraphicsBegin(pDIS->hDC);
          gpFillRound(pGraphics, rcSel, 4.0f * gUI.fUIScale, gpColor(gUI.theme.dwCardHover, 255));
          gpGraphicsEnd(pGraphics);
        }
        if (pDIS->itemData) {
          SetBkMode(pDIS->hDC, TRANSPARENT);
          SetTextColor(pDIS->hDC, gUI.theme.dwText);
          SelectObject(pDIS->hDC, gUI.theme.pFont);
          RECT rcText = pDIS->rcItem;
          rcText.left += _scScale(12);
          DrawTextW(pDIS->hDC, (const wchar_t*)pDIS->itemData, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        return TRUE;
      }
      break;
    }
    case WM_MEASUREITEM: {
      LPMEASUREITEMSTRUCT pMIS = (LPMEASUREITEMSTRUCT)lParam;
      if (pMIS->CtlType == ODT_MENU) {
        UINT uTextWidth = 0;
        if (pMIS->itemData) {
          const wchar_t* wszText = (const wchar_t*)pMIS->itemData;
          HDC hDC = GetDC(gUI.hWindow);
          HFONT hOldFont = (HFONT)SelectObject(hDC, gUI.theme.pFont);
          SIZE stSize;
          if (GetTextExtentPoint32W(hDC, wszText, lstrlenW(wszText), &stSize)) {
            uTextWidth = (UINT)stSize.cx;
          }
          SelectObject(hDC, hOldFont);
          ReleaseDC(gUI.hWindow, hDC);
        }
        UINT uMinWidth = _scScale(150);
        UINT uWidth    = uTextWidth + _scScale(30);
        pMIS->itemWidth  = uWidth > uMinWidth ? uWidth : uMinWidth;
        pMIS->itemHeight = _scScale(24);
        return TRUE;
      }
      break;
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
        float fUIScale = scGetDPIScale(gUI.hWindow);
        if (scIsWindowsDarkTheme() != gUI.theme.bDark || scGetSystemAccentColor() != gUI.theme.dwAccent || gUI.fUIScale != fUIScale) {
          gUI.fUIScale = fUIScale;
          scUIThemeGet(&gUI.theme, _scScale(15));
          _scApplyTheme();
          BOOL bDark = gUI.theme.bDark;
          DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &bDark, sizeof(bDark));
          if (gApp->config.bAccentIcon) {
            _scUpdateWindowIcon(hWnd, true);
          }
        }
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hDC = BeginPaint(hWnd, &ps);
      _scRender(hDC, &ps.rcPaint);
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
_scOnLogPushed(void) {
  if (gUI.hWindow && gUI.eCurrentPage == SC_PAGE_LOGS && InterlockedExchange(&gUI.lLogPending, 1) == 0) {
    PostMessageW(gUI.hWindow, WM_LOG_PUSHED, 0, 0);
  }
}

void scUIOpenWindow() {
  static ULONG_PTR pGdiPlusToken = 0;
  if (!pGdiPlusToken) {
    scLogSetSink(_scOnLogPushed);

    gpStartup(&pGdiPlusToken);
    scLogDebug("pGdiPlusToken: %p", pGdiPlusToken);
    if (!pGdiPlusToken) {
      scLogError("Failed to get gdiplus token, unable to show UI");
      return;
    }

    gUI.eCurrentPage   = SC_PAGE_GENERAL;
    gUI.iEditingHotkey = -1;
  }

  if (!gUI.pGui) {
    scGuiHost host = { 0 };
    host.pfnInvalidate   = _scHostInvalidate;
    host.pfnCapture      = _scHostCapture;
    host.pfnValueChanged = _scHostValueChanged;
    gUI.pGui = scGuiCreate(&host);
  }

  gUI.fUIScale = scGetDPIScale(gUI.hWindow);
  scUIThemeGet(&gUI.theme, _scScale(15));
  _scApplyTheme();
  _scRebuild();

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

    RECT rcClient;
    GetClientRect(gUI.hWindow, &rcClient);
    scGuiResize(gUI.pGui, rcClient.right, rcClient.bottom);

    if (gApp->bIsGeWin10) {
      BOOL bDark = gUI.theme.bDark;
      DwmSetWindowAttribute(gUI.hWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &bDark, sizeof(bDark));
    }
    if (gApp->config.bAccentIcon) {
      _scUpdateWindowIcon(gUI.hWindow, true);
    }
    scLogDebug("Created main window");

    _scInitThumbWorker();
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

  _scShutdownThumbWorker();
  _scThumbCacheClear();

  scGuiDestroy(gUI.pGui);
  gUI.pGui     = NULL;
  gUI.pContent = NULL;
  gUI.pLogView = NULL;
  if (gUI.hBackBitmap) {
    DeleteObject(gUI.hBackBitmap);
    gUI.hBackBitmap = NULL;
  }
  gUI.iBackWidth  = 0;
  gUI.iBackHeight = 0;

  free(gUI.aLogLines);
  gUI.aLogLines = NULL;
  gUI.nLogLines = 0;
  gUI.nLogCap   = 0;
  gUI.iLogWidth = 0;

  free(gAudioDeviceOptions);
  gAudioDeviceOptions = NULL;
  gAudioDeviceOptionsCap = 0;

  if (gUI.hWindow) {
    gUI.hWindow = NULL;
    scLogDebug("Destroyed main window");
  }
}

void scUISetCurrentPage(scPageID ePageID) {
  if (gUI.iEditingHotkey != -1) {
    _scCancelHotkeyRecording();
  }

  _scSavePageScroll();
  gUI.pContent     = NULL;
  gUI.eCurrentPage = ePageID;
  if (ePageID == SC_PAGE_GALLERY) {
    gUI.aPageScrollY[ePageID] = 0;
  }

  _scRebuild();
}

void scUIOnCaptureSaved(const wchar_t* wszPath) {
  (void)wszPath;
  if (gUI.hWindow && gUI.eCurrentPage == SC_PAGE_GALLERY) {
    PostMessageW(gUI.hWindow, WM_REBUILD_UI, SC_PAGE_GALLERY, 0);
  }
}

void scUIOnAudioDevicesUpdated() {
  if (gUI.hWindow) {
    PostMessageW(gUI.hWindow, WM_REBUILD_UI, SC_PAGE_SETTINGS, 0);
  }
}
