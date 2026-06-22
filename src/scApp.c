#include "pch.h"
#include "scApp.h"
#include "scAssert.h"
#include "scLogging.h"
#include "stb_image_write.h"
#include "stb_image.h"

static scApp* gApp = NULL;
static const char* OVERLAY_CLASS_NAME = "ScOverlayWindow";

#define SC_MAG_SIZE 120
#define SC_MAG_SRC  (SC_MAG_SIZE / 4)   // 4x zoom

//------------------------------------------------------------------------
// Util
//------------------------------------------------------------------------
// Source - https://stackoverflow.com/a/6218957
// Posted by Zach Burlingame, modified by community. See post 'Timeline' for change history
// Retrieved 2026-06-21, License - CC BY-SA 3.0
scInternal BOOL
_scFileExists(LPCWSTR szPath) {
  DWORD dwAttrib = GetFileAttributesW(szPath);
  return (dwAttrib != INVALID_FILE_ATTRIBUTES && 
         !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

scInternal void
_scGetSystemLanguage(char szLanguageCode[4]) {
  szLanguageCode[0] = '\0';

  wchar_t wszFullLocale[LOCALE_NAME_MAX_LENGTH];
  if (GetUserDefaultLocaleName(wszFullLocale, LOCALE_NAME_MAX_LENGTH) > 0) {
    char szFullLocale[LOCALE_NAME_MAX_LENGTH];
    WideCharToMultiByte(CP_UTF8, 0, wszFullLocale, -1, szFullLocale, LOCALE_NAME_MAX_LENGTH, NULL, NULL);

    char* pszHyphen = strchr(szFullLocale, '-');
    if (pszHyphen) {
        *pszHyphen = '\0';
    }
    strcpy_s(szLanguageCode, 4, szFullLocale);
  }

  if (szLanguageCode[0] == '\0') {
    strcpy_s(szLanguageCode, 4, "en");
  }
}

scInternal void
_scGetDefaultSaveRootPath(wchar_t wszPath[SC_PATH_MAX_LEN]) {
  wszPath[0] = L'\0';

  PWSTR pwszPicturesPath = NULL;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pwszPicturesPath))) {
    wcscpy_s(wszPath, SC_PATH_MAX_LEN, pwszPicturesPath);
    CoTaskMemFree(pwszPicturesPath);

    wcscat_s(wszPath, SC_PATH_MAX_LEN, L"\\Screencap");
    CreateDirectoryW(wszPath, NULL);

    if (GetFileAttributesW(wszPath) != INVALID_FILE_ATTRIBUTES) {
      return;
    }
  } else {
    scLogWarn("Failed getting root save path, defaulting to current directory. Reason: %d", GetLastError());
    GetCurrentDirectoryW(SC_PATH_MAX_LEN, wszPath);
  }
}

//------------------------------------------------------------------------
// Config
//------------------------------------------------------------------------
scInternal bool
_scGetConfigFilepath(wchar_t wszPath[SC_PATH_MAX_LEN]) {
  wszPath[0] = L'\0';

  PWSTR pwszAppDataPath = NULL;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &pwszAppDataPath))) {
    wchar_t wszConfigDir[MAX_PATH];

    wcscpy_s(wszConfigDir, MAX_PATH, pwszAppDataPath);
    CoTaskMemFree(pwszAppDataPath);

    wcscat_s(wszConfigDir, MAX_PATH, L"\\Screencap");
    CreateDirectoryW(wszConfigDir, NULL);

    _snwprintf_s(wszPath, SC_PATH_MAX_LEN, _TRUNCATE, L"%s\\config.ini", wszConfigDir);
    return true;
  } else {
    scLogError("SHGetKnownFolderPath Failed! Reason: %d", GetLastError());
    return false;
  }
}

scInternal void
_scConfigCreateDefaults(scAppConfig* pOutConfig) {
  pOutConfig->aHotkeys[SC_HOTKEY_SCREENSHOT]          = (scHotkey){ .eID = SC_HOTKEY_SCREENSHOT         , .uModifiers = 0,                       .uKey = VK_SNAPSHOT, .bRegistered = false };
  pOutConfig->aHotkeys[SC_HOTKEY_CLIPBOARD]           = (scHotkey){ .eID = SC_HOTKEY_CLIPBOARD          , .uModifiers = MOD_CONTROL | MOD_SHIFT, .uKey = VK_SNAPSHOT, .bRegistered = false };
  pOutConfig->aHotkeys[SC_HOTKEY_OCR]                 = (scHotkey){ .eID = SC_HOTKEY_OCR                , .uModifiers = MOD_CONTROL | MOD_ALT,   .uKey = VK_SNAPSHOT, .bRegistered = false };
  pOutConfig->aHotkeys[SC_HOTKEY_ACTIVE_WINDOW]       = (scHotkey){ .eID = SC_HOTKEY_ACTIVE_WINDOW       , .uModifiers = MOD_ALT,                .uKey = VK_SNAPSHOT, .bRegistered = false };
  pOutConfig->aHotkeys[SC_HOTKEY_ACTIVE_MONITOR]      = (scHotkey){ .eID = SC_HOTKEY_ACTIVE_MONITOR      , .uModifiers = MOD_CONTROL,            .uKey = VK_SNAPSHOT, .bRegistered = false };
  pOutConfig->aHotkeys[SC_HOTKEY_FALLBACK_SCREENSHOT] = (scHotkey){ .eID = SC_HOTKEY_FALLBACK_SCREENSHOT, .uModifiers = MOD_CONTROL | MOD_ALT,   .uKey = 'C',         .bRegistered = false };
  pOutConfig->aHotkeys[SC_HOTKEY_RECORD]              = (scHotkey){ .eID = SC_HOTKEY_RECORD              , .uModifiers = MOD_SHIFT,              .uKey = VK_SNAPSHOT, .bRegistered = false };

  _scGetDefaultSaveRootPath(pOutConfig->wszSavePath); // defaults to current directory if fails
  _scGetSystemLanguage(pOutConfig->sLanguageCode); // defaults to en if fails
    
  pOutConfig->bCopyToClipboard   = true;
  pOutConfig->bRunAtStartup      = false;
  pOutConfig->bPlaySoundOnAction = true;
}

scInternal void
_scConfigWriteVersion(wchar_t* wszPath) {
  wchar_t szVersionMajor[4];
  wchar_t szVersionMinor[4];
  wchar_t szVersionPatch[4];
  _snwprintf_s(szVersionMajor, 4, _TRUNCATE, L"%u", SC_VERSION_MAJOR);
  WritePrivateProfileStringW(L"version", L"major", szVersionMajor, wszPath);
  _snwprintf_s(szVersionMinor, 4, _TRUNCATE, L"%u", SC_VERSION_MINOR);
  WritePrivateProfileStringW(L"version", L"minor", szVersionMinor, wszPath);
  _snwprintf_s(szVersionPatch, 4, _TRUNCATE, L"%u", SC_VERSION_PATCH);
  WritePrivateProfileStringW(L"version", L"patch", szVersionPatch, wszPath);
}

scInternal bool
_scWriteConfig(scAppConfig* pConfig, wchar_t* wszPath) {
  wchar_t wszLocalPath[SC_PATH_MAX_LEN];
  wchar_t* wszActualPath = wszPath;

  if (!wszActualPath) {
    if (!_scGetConfigFilepath(wszLocalPath)) {
      scLogError("Failed writing config, path is null and could not resolve default filepath");
      return false;
    }
    wszActualPath = wszLocalPath;
  }

  _scConfigWriteVersion(wszActualPath);

  WritePrivateProfileStringW(L"options", L"copy_to_clipboard", pConfig->bCopyToClipboard ? L"1" : L"0", wszActualPath);
  WritePrivateProfileStringW(L"options", L"run_on_startup",    pConfig->bRunAtStartup ? L"1" : L"0", wszActualPath);
  WritePrivateProfileStringW(L"options", L"play_sound",        pConfig->bPlaySoundOnAction ? L"1" : L"0", wszActualPath);

  wchar_t wszLang[4];
  MultiByteToWideChar(CP_UTF8, 0, pConfig->sLanguageCode, -1, wszLang, 4);
  WritePrivateProfileStringW(L"options", L"language", wszLang, wszActualPath);
  WritePrivateProfileStringW(L"options", L"save_path", pConfig->wszSavePath, wszActualPath);

  for (int i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    const scHotkey* pHk = &pConfig->aHotkeys[i];
    const char* pszIdStr = scHotkeyIdNames[pHk->eID];

    wchar_t wszIdStr[64];
    MultiByteToWideChar(CP_UTF8, 0, pszIdStr, -1, wszIdStr, 64);

    wchar_t wszKeyName[128];
    wchar_t wszModName[128];
    _snwprintf_s(wszKeyName, 128, _TRUNCATE, L"%s_key", wszIdStr);
    _snwprintf_s(wszModName, 128, _TRUNCATE, L"%s_modifiers", wszIdStr);

    wchar_t wszValueBuf[32];

    _snwprintf_s(wszValueBuf, 32, _TRUNCATE, L"%u", pHk->uKey);
    WritePrivateProfileStringW(L"hotkeys", wszKeyName, wszValueBuf, wszActualPath);

    _snwprintf_s(wszValueBuf, 32, _TRUNCATE, L"%u", pHk->uModifiers);
    WritePrivateProfileStringW(L"hotkeys", wszModName, wszValueBuf, wszActualPath);
  }

  return true;
}

scInternal bool
_scConfigReadInto(scAppConfig* pConfig, wchar_t* wszPath) {
  wchar_t wszLocalPath[SC_PATH_MAX_LEN];
  wchar_t* wszActualPath = wszPath;

  if (!wszActualPath) {
    if (!_scGetConfigFilepath(wszLocalPath)) {
      scLogError("Failed writing config, path is null and could not resolve default filepath");
      return false;
    }
    wszActualPath = wszLocalPath;
  }

  pConfig->bCopyToClipboard   = GetPrivateProfileIntW(L"options", L"copy_to_clipboard", pConfig->bCopyToClipboard, wszActualPath) != 0;
  pConfig->bRunAtStartup      = GetPrivateProfileIntW(L"options", L"run_on_startup",    pConfig->bRunAtStartup, wszActualPath) != 0;
  pConfig->bPlaySoundOnAction = GetPrivateProfileIntW(L"options", L"play_sound",        pConfig->bPlaySoundOnAction, wszActualPath) != 0;

  wchar_t wszDefaultLang[4];
  MultiByteToWideChar(CP_UTF8, 0, pConfig->sLanguageCode, -1, wszDefaultLang, 4);

  wchar_t wszLang[4];
  GetPrivateProfileStringW(L"options", L"language", wszDefaultLang, wszLang, 4, wszActualPath);
  WideCharToMultiByte(CP_UTF8, 0, wszLang, -1, pConfig->sLanguageCode, sizeof(pConfig->sLanguageCode), NULL, NULL);

  GetPrivateProfileStringW(L"options", L"save_path", pConfig->wszSavePath, pConfig->wszSavePath, SC_PATH_MAX_LEN, wszActualPath);

  for (int i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    scHotkey* pHk = &pConfig->aHotkeys[i];
    pHk->eID = i; // must be in order
    const char* pszIdStr = scHotkeyIdNames[pHk->eID];

    wchar_t wszIdStr[64];
    MultiByteToWideChar(CP_UTF8, 0, pszIdStr, -1, wszIdStr, 64);

    wchar_t wszKeyName[128];
    wchar_t wszModName[128];
    _snwprintf_s(wszKeyName, 128, _TRUNCATE, L"%s_key", wszIdStr);
    _snwprintf_s(wszModName, 128, _TRUNCATE, L"%s_modifiers", wszIdStr);

    pHk->uKey       = (u32)GetPrivateProfileIntW(L"hotkeys", wszKeyName, (int)pHk->uKey, wszActualPath);
    pHk->uModifiers = (u32)GetPrivateProfileIntW(L"hotkeys", wszModName, (int)pHk->uModifiers, wszActualPath);
  }

  return true;
}

scInternal void
_scConfigGetVersion(wchar_t* wszPath, s32* iMajor, s32* iMinor, s32* iPatch) {
  *iMajor = GetPrivateProfileIntW(L"version", L"major", 0, wszPath);
  *iMinor = GetPrivateProfileIntW(L"version", L"minor", 0, wszPath);
  *iPatch = GetPrivateProfileIntW(L"version", L"patch", 0, wszPath);
}

scInternal bool
_scConfigMigrateIfNeeded(scAppConfig* pConfig, wchar_t* wszPath, s32 iConfigMajor, s32 iConfigMinor, s32 iConfigPatch) {
  if (iConfigMajor == 0 && iConfigMinor == 0 && iConfigPatch == 0) {
    // check if utf8 with bom, if so, resave as utf8. also strip the spaces
    FILE* pFile = NULL;
    if (_wfopen_s(&pFile, wszPath, L"rb") == 0 && pFile) {
      fseek(pFile, 0, SEEK_END);
      long lSize = ftell(pFile);
      fseek(pFile, 0, SEEK_SET);

      char* pszBuffer = (char*)malloc(lSize + 1);
      if (pszBuffer) {
        fread(pszBuffer, 1, lSize, pFile);
        pszBuffer[lSize] = '\0';
        fclose(pFile);

        char* pszContent = pszBuffer;
        long lContentSize = lSize;

        // skip bom
        if (lSize >= 3 &&
          (unsigned char)pszBuffer[0] == 0xEF &&
          (unsigned char)pszBuffer[1] == 0xBB &&
          (unsigned char)pszBuffer[2] == 0xBF)
        {
          pszContent = pszBuffer + 3;
          lContentSize = lSize - 3;
        }

        // remove spaces around =
        char* pszRead = pszContent;
        char* pszWrite = pszContent;
        while (*pszRead != '\0') {
          if (*pszRead == ' ' && *(pszRead + 1) == '=') {
            pszRead++;
            continue;
          }
          if (*pszRead == '=') {
            *pszWrite++ = *pszRead++;
            while (*pszRead == ' ') {
              pszRead++; // Drop every trailing space cleanly
            }
            continue;
          }
          *pszWrite++ = *pszRead++;
        }
        *pszWrite = '\0';
        lContentSize = (long)strlen(pszContent);

        if (_wfopen_s(&pFile, wszPath, L"wb") == 0 && pFile) {
          fwrite(pszContent, 1, lContentSize, pFile);
          fclose(pFile);
        }
        free(pszBuffer);
      }
    }

    _scConfigReadInto(pConfig, wszPath);

    wchar_t wszValue[16];

    GetPrivateProfileStringW(L"options", L"copy_to_clipboard", pConfig->bCopyToClipboard ? L"true" : L"false", wszValue, sizeof(wszValue) / sizeof(wchar_t), wszPath);
    pConfig->bCopyToClipboard = (wcsstr(wszValue, L"true") != NULL || wcsstr(wszValue, L"1") != NULL);

    GetPrivateProfileStringW(L"options", L"run_on_startup", pConfig->bRunAtStartup ? L"true" : L"false", wszValue, sizeof(wszValue) / sizeof(wchar_t), wszPath);
    pConfig->bRunAtStartup = (wcsstr(wszValue, L"true") != NULL || wcsstr(wszValue, L"1") != NULL);

    GetPrivateProfileStringW(L"options", L"play_sound", pConfig->bPlaySoundOnAction ? L"true" : L"false", wszValue, sizeof(wszValue) / sizeof(wchar_t), wszPath);
    pConfig->bPlaySoundOnAction = (wcsstr(wszValue, L"true") != NULL || wcsstr(wszValue, L"1") != NULL);

    _scWriteConfig(pConfig, wszPath);
    _scConfigWriteVersion(wszPath);
    return true;
  }
  return false;
}

scInternal bool
scConfigLoad() {
  scAssert(gApp, "gpApp is NULL!");

  wchar_t wszConfigPath[SC_PATH_MAX_LEN];
  if (!_scGetConfigFilepath(wszConfigPath)) {
    return false;
  }

  _scConfigCreateDefaults(&gApp->config); // initialize it with the defaults in case of any issues below

  if (!_scFileExists(wszConfigPath)) {
    _scWriteConfig(&gApp->config, wszConfigPath); // it's okay for this to fail because we already have the defaults
    return true;
  } else {
    s32 iMajor, iMinor, iPatch;
    _scConfigGetVersion(wszConfigPath, &iMajor, &iMinor, &iPatch);
    if (_scConfigMigrateIfNeeded(&gApp->config, wszConfigPath, iMajor, iMinor, iPatch)) {
      scLogInfo("Migrated from %d.%d.%d to %d.%d.%d", iMajor, iMinor, iPatch, SC_VERSION_MAJOR, SC_VERSION_MINOR, SC_VERSION_PATCH);
    } else {
      _scConfigReadInto(&gApp->config, wszConfigPath);
      _scWriteConfig(&gApp->config, wszConfigPath);
    }
  }

  return true;
}

//------------------------------------------------------------------------
// Window Procedure
//------------------------------------------------------------------------
typedef struct {
  POINT pt;
  HWND  hOverlay;
  HWND  hResult;
} _scHoverHit;

scInternal BOOL CALLBACK
_scHoverEnumProc(HWND hWnd, LPARAM lParam) {
  _scHoverHit* pHit = (_scHoverHit*)lParam;
  if (hWnd == pHit->hOverlay)    return TRUE;
  if (!IsWindowVisible(hWnd))    return TRUE;
  if (IsIconic(hWnd))            return TRUE;

  RECT wr;
  if (DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(RECT)) != S_OK) {
    if (!GetWindowRect(hWnd, &wr)) return TRUE;
  }
  if (!PtInRect(&wr, pHit->pt))  return TRUE;

  pHit->hResult = hWnd;
  return FALSE; // stop on first match
}

scInternal void
_scUpdateHoverRect(scCaptureContext* pCtx, POINT pt) {
  _scHoverHit hit = { pt, pCtx->hOverlayWindow, NULL };
  EnumWindows(_scHoverEnumProc, (LPARAM)&hit);
  if (!hit.hResult) {
    return;
  }
  RECT wr;
  if (DwmGetWindowAttribute(hit.hResult, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(RECT)) != S_OK) {
    GetWindowRect(hit.hResult, &wr);
  }
  pCtx->hHoveredWindow = hit.hResult;
  pCtx->stSelectedRect = (scRect){
    .X = wr.left,
    .Y = wr.top,
    .W = wr.right  - wr.left,
    .H = wr.bottom - wr.top,
  };
}

scInternal void
_scUpdateMagnifier(scCaptureContext* pCtx, POINT pt) {
  if (!pCtx->hFrozenDC) {
    return;
  }

  // Lazily create the magnifier back buffer.
  if (!pCtx->hMagDC) {
    HDC hScreenDC = GetDC(NULL);
    pCtx->hMagDC     = CreateCompatibleDC(hScreenDC);
    pCtx->hMagBitmap = CreateCompatibleBitmap(hScreenDC, SC_MAG_SIZE, SC_MAG_SIZE);
    SelectObject(pCtx->hMagDC, pCtx->hMagBitmap);
    SetStretchBltMode(pCtx->hMagDC, COLORONCOLOR);
    ReleaseDC(NULL, hScreenDC);
  }

  s32 vx = pCtx->vCaptureRegion.X;
  s32 vy = pCtx->vCaptureRegion.Y;

  HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = { sizeof(MONITORINFO) };
  GetMonitorInfo(hMon, &mi);

  pCtx->iMagDestX = (pt.x + SC_MAG_SIZE < mi.rcMonitor.right)
                  ? pt.x - vx + 20
                  : pt.x - vx - SC_MAG_SIZE - 20;
  pCtx->iMagDestY = (pt.y + SC_MAG_SIZE < mi.rcMonitor.bottom)
                  ? pt.y - vy + 20
                  : pt.y - vy - SC_MAG_SIZE - 20;

  StretchBlt(pCtx->hMagDC, 0, 0, SC_MAG_SIZE, SC_MAG_SIZE,
             pCtx->hFrozenDC,
             pt.x - SC_MAG_SRC / 2 - vx,
             pt.y - SC_MAG_SRC / 2 - vy,
             SC_MAG_SRC, SC_MAG_SRC, SRCCOPY);

  pCtx->bMagValid = true;
}

scInternal void
_scPaintMagnifier(scCaptureContext* pCtx, HDC hMemDC) {
  if (!pCtx->bMagValid) {
    return;
  }

  s32 destX = pCtx->iMagDestX;
  s32 destY = pCtx->iMagDestY;

  BitBlt(hMemDC, destX, destY, SC_MAG_SIZE, SC_MAG_SIZE, pCtx->hMagDC, 0, 0, SRCCOPY);

  HPEN hPen    = CreatePen(PS_SOLID, 1, RGB(156, 215, 228));
  HPEN hOldPen = SelectObject(hMemDC, hPen);

  // Crosshair.
  MoveToEx(hMemDC, destX + SC_MAG_SIZE / 2, destY, NULL);
  LineTo  (hMemDC, destX + SC_MAG_SIZE / 2, destY + SC_MAG_SIZE);
  MoveToEx(hMemDC, destX, destY + SC_MAG_SIZE / 2, NULL);
  LineTo  (hMemDC, destX + SC_MAG_SIZE, destY + SC_MAG_SIZE / 2);

  // Border.
  HBRUSH hOldBrush = SelectObject(hMemDC, GetStockObject(NULL_BRUSH));
  Rectangle(hMemDC, destX, destY, destX + SC_MAG_SIZE, destY + SC_MAG_SIZE);

  SelectObject(hMemDC, hOldBrush);
  SelectObject(hMemDC, hOldPen);
  DeleteObject(hPen);
}

LRESULT CALLBACK
OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  scCaptureContext* pCtx = gApp->pCaptureContext;
  scAssert(pCtx, "pCtx is null in window procedure!");
  if (!pCtx) {
    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
  }

  switch (uMsg) {
    case WM_SETCURSOR: {
      SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_CROSS));
      return TRUE;
    }

    case WM_MOUSEMOVE: {
      POINT stPoint;
      GetCursorPos(&stPoint);

      if (pCtx->bMouseDown) {
        if (!pCtx->bDragging && (abs(stPoint.x - pCtx->stDragStart.X) > 3 || abs(stPoint.y - pCtx->stDragStart.Y) > 3)) {
          pCtx->bDragging = true;
        }
        if (pCtx->bDragging) {
          pCtx->stSelectedRect = (scRect){
            .X = min(pCtx->stDragStart.X, stPoint.x),
            .Y = min(pCtx->stDragStart.Y, stPoint.y),
            .W = abs(stPoint.x - pCtx->stDragStart.X),
            .H = abs(stPoint.y - pCtx->stDragStart.Y),
          };
        }
      } else {
        _scUpdateHoverRect(pCtx, stPoint);
      }

      _scUpdateMagnifier(pCtx, stPoint);
      InvalidateRect(hWnd, NULL, FALSE);
      return 0;
    }

    case WM_LBUTTONDOWN: {
      POINT stPoint;
      GetCursorPos(&stPoint);
      pCtx->bMouseDown  = true;
      pCtx->stDragStart = (scV2I){ stPoint.x, stPoint.y };
      SetCapture(hWnd);
      return 0;
    }

    case WM_LBUTTONUP: {
      if (pCtx->bMouseDown) {
        pCtx->bMouseDown   = false;
        pCtx->bWasDragging = pCtx->bDragging;
        pCtx->bDragging    = false;
        pCtx->stFinalRect  = pCtx->stSelectedRect;
        ReleaseCapture();
        ShowWindow(hWnd, SW_HIDE);

        scAssert(gApp->pActiveHandler, "pActiveHandler is null in WM_LBUTTONUP!");
        if (gApp->pActiveHandler && gApp->pActiveHandler->cbOnAreaSelected) {
          gApp->pActiveHandler->cbOnAreaSelected(pCtx);
        }
      }
      return 0;
    }

    case WM_KEYDOWN: {
      if (wParam == VK_ESCAPE) {
        if (pCtx->bMouseDown) {
          // Cancel the in-progress drag, but stay in capture mode.
          ReleaseCapture();
          pCtx->bMouseDown = false;
          pCtx->bDragging  = false;
          POINT stPoint;
          GetCursorPos(&stPoint);
          _scUpdateHoverRect(pCtx, stPoint);
          InvalidateRect(hWnd, NULL, FALSE);
        } else {
          // Cancel the whole capture.
          ShowWindow(hWnd, SW_HIDE);
          if (gApp->pActiveHandler && gApp->pActiveHandler->cbOnCaptureCancelled) {
            gApp->pActiveHandler->cbOnCaptureCancelled(pCtx);
          }
        }
      }
      return 0;
    }

    case WM_PAINT: {
      PAINTSTRUCT stPaint;
      HDC hDC = BeginPaint(hWnd, &stPaint);
      RECT cr;
      GetClientRect(hWnd, &cr);

      // (Re)create the back buffer when missing or resized.
      if (!pCtx->hOverlayMemDC           ||
          pCtx->iOverlayMemW != cr.right ||
          pCtx->iOverlayMemH != cr.bottom)
      {
        if (pCtx->hOverlayMemDC)     DeleteDC(pCtx->hOverlayMemDC);
        if (pCtx->hOverlayMemBitmap) DeleteObject(pCtx->hOverlayMemBitmap);

        pCtx->hOverlayMemDC     = CreateCompatibleDC(hDC);
        pCtx->hOverlayMemBitmap = CreateCompatibleBitmap(hDC, cr.right, cr.bottom);
        SelectObject(pCtx->hOverlayMemDC, pCtx->hOverlayMemBitmap); // <- was missing
        pCtx->iOverlayMemW = cr.right;
        pCtx->iOverlayMemH = cr.bottom;
      }

      HDC hMemDC = pCtx->hOverlayMemDC;

      // Frozen screenshot as the background.
      if (pCtx->hFrozenDC) {
        BitBlt(hMemDC, 0, 0, cr.right, cr.bottom, pCtx->hFrozenDC, 0, 0, SRCCOPY);
      }

      const s32 vx = pCtx->vCaptureRegion.X;
      const s32 vy = pCtx->vCaptureRegion.Y;

      RECT r = {
        pCtx->stSelectedRect.X - vx,
        pCtx->stSelectedRect.Y - vy,
        pCtx->stSelectedRect.X - vx + pCtx->stSelectedRect.W,
        pCtx->stSelectedRect.Y - vy + pCtx->stSelectedRect.H
      };

      // Translucent fill over the selection (1x1 source stretched via AlphaBlend).
      {
        HDC     hAlphaDC     = CreateCompatibleDC(hMemDC);
        HBITMAP hAlphaBmp    = CreateCompatibleBitmap(hMemDC, 1, 1);
        HBITMAP hOldAlphaBmp = SelectObject(hAlphaDC, hAlphaBmp);

        SetPixel(hAlphaDC, 0, 0, RGB(155, 155, 215));
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 32, 0 };
        GdiAlphaBlend(hMemDC, r.left, r.top, r.right - r.left, r.bottom - r.top, hAlphaDC, 0, 0, 1, 1, bf);

        SelectObject(hAlphaDC, hOldAlphaBmp);
        DeleteObject(hAlphaBmp);
        DeleteDC(hAlphaDC);
      }

      // Dotted selection border.
      {
        HPEN   hPen      = CreatePen(PS_DOT, 1, RGB(156, 215, 228));
        HPEN   hOldPen   = SelectObject(hMemDC, hPen);
        HBRUSH hOldBrush = SelectObject(hMemDC, GetStockObject(NULL_BRUSH));

        SetBkMode(hMemDC, TRANSPARENT);
        Rectangle(hMemDC, r.left, r.top, r.right, r.bottom);

        SelectObject(hMemDC, hOldBrush);
        SelectObject(hMemDC, hOldPen);
        DeleteObject(hPen);
      }

      // "W x H" size label.
      if (pCtx->stSelectedRect.W > 0 && pCtx->stSelectedRect.H > 0) {
        HFONT hOldFont = SelectObject(hMemDC, GetStockObject(DEFAULT_GUI_FONT));

        char szSize[64];
        snprintf(szSize, sizeof(szSize), "%d x %d", pCtx->stSelectedRect.W, pCtx->stSelectedRect.H);

        SIZE stTextSize;
        GetTextExtentPoint32A(hMemDC, szSize, (int)strlen(szSize), &stTextSize);

        int iTextX = r.left;
        int iTextY = r.top - stTextSize.cy - 4;
        if (iTextY < 0) {
          iTextY = r.top + 4;
        }

        RECT stTextBg = {
          iTextX, iTextY,
          iTextX + stTextSize.cx + 6,
          iTextY + stTextSize.cy + 4
        };
        FillRect(hMemDC, &stTextBg, (HBRUSH)GetStockObject(BLACK_BRUSH)); // stock: don't delete

        SetTextColor(hMemDC, RGB(255, 255, 255));
        TextOutA(hMemDC, iTextX + 3, iTextY + 2, szSize, (int)strlen(szSize));

        SelectObject(hMemDC, hOldFont);
      }

      _scPaintMagnifier(pCtx, hMemDC);

      // Present.
      BitBlt(hDC, 0, 0, cr.right, cr.bottom, hMemDC, 0, 0, SRCCOPY);
      EndPaint(hWnd, &stPaint);
      return 0;
    }

    case WM_ERASEBKGND: {
      return TRUE;
    }
  }
  return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

//------------------------------------------------------------------------
// Application Helpers
scInternal void
_scUnregisterHotkeys() {
  for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    scHotkey* pHk = &gApp->config.aHotkeys[i];
    if (pHk->bRegistered) {
      if (!UnregisterHotKey(NULL, pHk->eID)) {
        scLogWarn("Failed to unregister hotkey %s: %d", scHotkeyIdNames[pHk->eID], GetLastError());
      }
      pHk->bRegistered = false;
    }
  }
}

scInternal void
_scRegisterHotkeys() {
  for (s32 i = 0; i < _SC_HOTKEY_COUNT; ++i) {
    scHotkey* pHk = &gApp->config.aHotkeys[i];
    if (pHk->bRegistered) {
      scLogWarn("Tried to reregister hotkey '%s' even though it's already registered", scHotkeyIdNames[pHk->eID]);
    }

    pHk->bRegistered = RegisterHotKey(NULL, pHk->eID, pHk->uModifiers | MOD_NOREPEAT, pHk->uKey);
    if (pHk->bRegistered) {
      scLogInfo("Successfully registered hotkey '%s'", scHotkeyIdNames[pHk->eID]);
    }
  }
}

scInternal bool
_scRegisterOverlayWindowClass() {
  WNDCLASSA stOverlayClass = { 0 };
  stOverlayClass.lpfnWndProc   = OverlayWndProc;
  stOverlayClass.hInstance     = GetModuleHandleA(NULL);
  stOverlayClass.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_CROSS);
  stOverlayClass.lpszClassName = OVERLAY_CLASS_NAME;

  ATOM wResult = RegisterClassA(&stOverlayClass);
  if (!wResult) {
    scLogError("Failed to register window overlay class: %d", GetLastError());
  }

  return wResult != 0;
}

scInternal void
_scGetSystemMetrics(s32* X, s32* Y, s32* W, s32* H) {
  *X = GetSystemMetrics(SM_XVIRTUALSCREEN);
  *Y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  *W = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  *H = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

scInternal void
_scDestroyCaptureContext() {
  scCaptureContext* pCtx = gApp->pCaptureContext;
  scAssert(pCtx, "What are we trying to destroy? There is no context... Fix it fucker.");
  if (!pCtx) {
    return;
  }

  if (pCtx->hOverlayWindow) {
    DestroyWindow(pCtx->hOverlayWindow);
    scLogDebug("Destroyed overlay window");
  }
  if (pCtx->hFrozenDC) {
    DeleteDC(pCtx->hFrozenDC);
    scLogDebug("Destroyed frozen dc");
  }
  if (pCtx->hFrozenBitmap) {
    DeleteObject(pCtx->hFrozenBitmap);
    scLogDebug("Destroyed frozen bitmap");
  }
  if (pCtx->hMagDC) {
    DeleteDC(pCtx->hMagDC);
    scLogDebug("Destroyed magnifier dc");
  }
  if (pCtx->hMagBitmap) {
    DeleteObject(pCtx->hMagBitmap);
    scLogDebug("Destroyed magnifier bitmap");
  }

  free(pCtx);
  gApp->pCaptureContext = NULL;
}

scInternal bool
_scBeginCaptureContext() {
  scAssert(!gApp->pCaptureContext, "Attmempted to create a new context when we already have one");
  if (gApp->pCaptureContext) {
    scLogWarn("Cleaned up capture context, however, this could lead to issues! Fix it fucker!");
    _scDestroyCaptureContext();
  }
  gApp->pCaptureContext = (scCaptureContext*)calloc(1, sizeof(scCaptureContext));
  return gApp->pCaptureContext != NULL;
}

scInternal bool
_scCtxCreateCaptureWindow(scCaptureContext* pCtx) {
  s32 iScreenX, iScreenY, iScreenW, iScreenH;
  _scGetSystemMetrics(&iScreenX, &iScreenY, &iScreenW, &iScreenH);

  pCtx->vCaptureRegion.X = iScreenX;
  pCtx->vCaptureRegion.Y = iScreenY;

  { // Populate FrozenDC/Bitmap
    HDC hScreenDC = GetDC(NULL);
    pCtx->hFrozenDC = CreateCompatibleDC(hScreenDC);
    pCtx->hFrozenBitmap = CreateCompatibleBitmap(hScreenDC, iScreenW, iScreenH);
    SelectObject(pCtx->hFrozenDC, pCtx->hFrozenBitmap);
    BitBlt(pCtx->hFrozenDC, 0, 0, iScreenW, iScreenH, hScreenDC, iScreenX, iScreenY, SRCCOPY);
    ReleaseDC(NULL, hScreenDC);
  }

  // Create and show overlay window
  pCtx->hOverlayWindow = CreateWindowExA(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    OVERLAY_CLASS_NAME, "ScreenCapOverlayWindow",
    WS_POPUP,
    iScreenX, iScreenY, iScreenW, iScreenH,
    NULL, NULL, GetModuleHandleA(NULL), NULL);
  if (!pCtx->hOverlayWindow) {
    scLogError("Failed to create overlay window: %d", GetLastError());
    return false;
  }

  ShowWindow(pCtx->hOverlayWindow, SW_SHOW);
  SetForegroundWindow(pCtx->hOverlayWindow);
  SetFocus(pCtx->hOverlayWindow);
  return true;
}

//------------------------------------------------------------------------
// Application
//------------------------------------------------------------------------
bool scAppInit() {
  gApp = (scApp*)malloc(sizeof(scApp));
  memset(gApp, 0, sizeof(scApp));

  if (!scConfigLoad()) {
    return false;
  }

  if (!_scRegisterOverlayWindowClass()) {
    return false;
  }

  scAppRegisterHotkeys();
  scAppSetupCallbackHandler();
  return true;
}

void scAppUpdate() {
  MSG msg = { 0 };
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    if (msg.message == WM_HOTKEY) {
      s32 iHotkeyID = (s32)msg.wParam;
      scCaptureHandler* pHandler = gApp->aCaptureHandlers[iHotkeyID];
      if (pHandler && pHandler->cbOnHotkeyPressed) {
        bool bHasValidContext = gApp->pCaptureContext != NULL;
        if (!bHasValidContext) {
          // we might want to keep a valid context, recording would be one (and the only) use case for this.
          bHasValidContext = _scBeginCaptureContext();
        }
        if (bHasValidContext) {
          gApp->pActiveHandler = pHandler;
          pHandler->cbOnHotkeyPressed(gApp->pCaptureContext);
        } else {
          scLogError("Skipping screen capture due to invalid context!");
        }
      }
    }

    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
}

void scAppDestroy() {
  _scUnregisterHotkeys();
  free(gApp);
}

//------------------------------------------------------------------------
// Image
//------------------------------------------------------------------------
typedef struct {
  u8*    pData;
  s32    nSize;
  s32    nCap;
  bool   bFailed;
} _scPngBuffer;

bool scCtxCopyAreaToImage(scCaptureContext* pCtx, scImage* pOutImage, scRect rect) {
  *pOutImage = (scImage){ 0 };
  if (rect.W <= 0 || rect.H <= 0 || !pCtx->hFrozenDC) {
    return false;
  }

  BITMAPINFO bi = { 0 };
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       =  rect.W;
  bi.bmiHeader.biHeight      = -rect.H;   // negative => top-down rows
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void* pBits = NULL;
  HBITMAP hDib = CreateDIBSection(pCtx->hFrozenDC, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
  if (!hDib) {
    return false;
  }

  HDC     hDibDC = CreateCompatibleDC(pCtx->hFrozenDC);
  HBITMAP hOld   = SelectObject(hDibDC, hDib);

  s32 srcX = rect.X - pCtx->vCaptureRegion.X;
  s32 srcY = rect.Y - pCtx->vCaptureRegion.Y;
  BitBlt(hDibDC, 0, 0, rect.W, rect.H, pCtx->hFrozenDC, srcX, srcY, SRCCOPY);
  GdiFlush(); // ensure the blit completes before the CPU reads pBits

  SelectObject(hDibDC, hOld);
  DeleteDC(hDibDC);

  pOutImage->W       = rect.W;
  pOutImage->H       = rect.H;
  pOutImage->iStride = rect.W * 4;
  pOutImage->pPixels = (u8*)pBits;
  pOutImage->hBitmap = hDib;
  return true;
}

scInternal void
_scPngWriteFunc(void* pContext, void* pData, int nSize) {
  _scPngBuffer* pBuf = (_scPngBuffer*)pContext;
  if (pBuf->bFailed) {
    return;
  }

  if (pBuf->nSize + nSize > pBuf->nCap) {
    s32 nNewCap = pBuf->nCap ? pBuf->nCap * 2 : 64 * 1024;
    while (nNewCap < pBuf->nSize + nSize) {
      nNewCap *= 2;
    }
    u8* pNew = (u8*)realloc(pBuf->pData, (size_t)nNewCap);
    if (!pNew) {
      pBuf->bFailed = true; // keep the old buffer so the caller can free it
      return;
    }
    pBuf->pData = pNew;
    pBuf->nCap  = nNewCap;
  }

  memcpy(pBuf->pData + pBuf->nSize, pData, (size_t)nSize);
  pBuf->nSize += nSize;
}

u8* _scBitmapToPNG(const scImage* pImage, s32* pOutSize) {
  if (!pImage->pPixels || pImage->W <= 0 || pImage->H <= 0) {
    return NULL;
  }

  u32 pixelCount = (u32)pImage->W * (u32)pImage->H;
  u8* pRGBA = (u8*)malloc((size_t)pixelCount * 4);
  if (!pRGBA) {
    return NULL;
  }

  // BGRA -> RGBA for stb, and force opaque alpha (BitBlt leaves it 0).
  for (s32 y = 0; y < pImage->H; ++y) {
    const u8* pSrcRow = pImage->pPixels + (size_t)y * pImage->iStride;
    u8*       pDstRow = pRGBA           + (size_t)y * pImage->W * 4;
    for (s32 x = 0; x < pImage->W; ++x) {
      pDstRow[x*4 + 0] = pSrcRow[x*4 + 2]; // R <- B
      pDstRow[x*4 + 1] = pSrcRow[x*4 + 1]; // G
      pDstRow[x*4 + 2] = pSrcRow[x*4 + 0]; // B <- R
      pDstRow[x*4 + 3] = 0xFF;             // A
    }
  }

  _scPngBuffer buf = { 0 };
  int ok = stbi_write_png_to_func(_scPngWriteFunc, &buf,
                                  pImage->W, pImage->H, 4,
                                  pRGBA, pImage->W * 4);
  free(pRGBA);

  if (!ok || buf.bFailed || !buf.pData) {
    free(buf.pData);
    return NULL;
  }

  *pOutSize = buf.nSize;
  return buf.pData;
}

void scImageFree(scImage* pImage) {
  if (pImage->hBitmap) {
    DeleteObject(pImage->hBitmap);
  }
  *pImage = (scImage){ 0 };
}

bool scImageToFile(const scImage* pImage) {
  s32 pngSize = 0;
  u8* pPng = _scBitmapToPNG(pImage, &pngSize);
  if (!pPng) {
    scLogError("Failed to encode capture PNG data");
    return false;
  }

  bool ok = scSaveDataToFile(pPng, pngSize, ".png");
  free(pPng);
  return ok;
}

bool scSaveDataToFile(const u8* pData, s32 nSize, const char* sExtension) {
  if (!pData || nSize <= 0) {
    return false;
  }

  SYSTEMTIME st;
  GetLocalTime(&st);

  // Directory
  wchar_t wszDir[MAX_PATH];
  int dn = swprintf(wszDir, MAX_PATH,
                    L"%ls\\%02d-%02d-%04d",
                    gApp->config.wszSavePath,
                    st.wDay, st.wMonth, st.wYear);
  if (dn < 0 || dn >= MAX_PATH) {
    return false;
  }

  int dirResult = SHCreateDirectoryExW(NULL, wszDir, NULL);
  if (dirResult != ERROR_SUCCESS && dirResult != ERROR_ALREADY_EXISTS) {
    scLogError("Failed to create directory '%ls': %d", wszDir, dirResult);
    return false;
  }

  // Filename
  wchar_t wszPath[MAX_PATH];
  int n = swprintf(wszPath, MAX_PATH,
                   L"%ls\\%02d-%02d-%02d%hs",
                   wszDir,
                   st.wHour, st.wMinute, st.wSecond,
                   sExtension);
  if (n < 0 || n >= MAX_PATH) {
    return false;
  }

  HANDLE hFile = CreateFileW(wszPath, GENERIC_WRITE, 0, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE) {
    scLogError("Failed to open '%ls' for writing: %d", wszPath, GetLastError());
    return false;
  }

  DWORD written = 0;
  bool ok = WriteFile(hFile, pData, (DWORD)nSize, &written, NULL) && written == (DWORD)nSize;
  CloseHandle(hFile);

  if (ok) {
    scLogDebug("Saved %d bytes to '%ls'", nSize, wszPath);
  }
  return ok;
}

//------------------------------------------------------------------------
// Other Application
void scCtxCaptureArea(scCaptureContext* pCtx) {
  scAssert(pCtx, "pCtx is null!");
  if (!_scCtxCreateCaptureWindow(gApp->pCaptureContext)) {
    _scDestroyCaptureContext(pCtx);
  }
}

void scAppRegisterHotkeys() {
  _scUnregisterHotkeys();
  _scRegisterHotkeys();
}

extern scCaptureHandler scScreenshotHandler;
scCaptureHandler scClipboardHandler          = { NULL, NULL, NULL };
scCaptureHandler scOcrHandler                = { NULL, NULL, NULL };
scCaptureHandler scActiveWindowHandler       = { NULL, NULL, NULL };
scCaptureHandler scActiveMonitorHandler      = { NULL, NULL, NULL };
scCaptureHandler scScreenshotFallbackHandler = { NULL, NULL, NULL };
scCaptureHandler scRecordHandler             = { NULL, NULL, NULL };

void scAppSetupCallbackHandler() {
  scHotkey* pHotkeys = gApp->config.aHotkeys;
  #define _scRegisterHandler(eHotkeyID, pHandler)                           \
    if (pHotkeys[eHotkeyID].bRegistered) {                                  \
      gApp->aCaptureHandlers[eHotkeyID] = pHandler;                         \
      scLogInfo("Registered Handler for '%s'", scHotkeyIdNames[eHotkeyID]); \
    }                                                                       \

  _scRegisterHandler(SC_HOTKEY_SCREENSHOT, &scScreenshotHandler);
  _scRegisterHandler(SC_HOTKEY_CLIPBOARD, &scClipboardHandler);
  _scRegisterHandler(SC_HOTKEY_OCR, &scOcrHandler);
  _scRegisterHandler(SC_HOTKEY_ACTIVE_WINDOW, &scActiveWindowHandler);
  _scRegisterHandler(SC_HOTKEY_ACTIVE_MONITOR, &scActiveMonitorHandler);
  _scRegisterHandler(SC_HOTKEY_FALLBACK_SCREENSHOT, &scScreenshotFallbackHandler);
  _scRegisterHandler(SC_HOTKEY_RECORD, &scRecordHandler);
}