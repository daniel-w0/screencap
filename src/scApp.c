#include "pch.h"
#include "scApp.h"
#include "scAssert.h"
#include "scLogging.h"

static scApp* gApp = NULL;
static const char* OVERLAY_CLASS_NAME = "ScOverlayWindow";

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
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_MOUSEMOVE: {
      if (!gApp->pCaptureContext) {
        break;
      }
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

  free(pCtx);
  gApp->pCaptureContext = NULL;
}

scInternal bool
_scBeginCaptureContext() {
  scCaptureContext* pCtx = gApp->pCaptureContext;
  scAssert(!pCtx, "Attmempted to create a new context when we already have one");
  if (pCtx) {
    scLogWarn("Cleaned up capture context, however, this could lead to issues! Fix it fucker!");
    _scDestroyCaptureContext();
  }
  pCtx = (scCaptureContext*)calloc(1, sizeof(scCaptureContext));

  pCtx->hOverlayWindow = CreateWindowExA(
    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    OVERLAY_CLASS_NAME, "ScreenCapOverlayWindow",
    WS_POPUP,
    0, 0, 0, 0,
    NULL, NULL, GetModuleHandleA(NULL), NULL);
  if (!pCtx->hOverlayWindow) {
    scLogError("Failed to create overlay window: %d", GetLastError());
    return false;
  }

  

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
      const scCaptureHandler* pHandler = gApp->aCaptureHandlers[iHotkeyID];
      if (pHandler && pHandler->cbOnHotkeyPressed) {
        bool bHasValidContext = gApp->pCaptureContext != NULL;
        if (!bHasValidContext) {
          // we might want to keep a valid context, recording would be one (and the only) use case for this.
          bHasValidContext = _scBeginCaptureContext();
        }
        if (bHasValidContext) {
          pHandler->cbOnHotkeyPressed(NULL);
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
// Other Application
void scAppCaptureArea() {

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