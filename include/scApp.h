#ifndef SC_APP_H
#define SC_APP_H

#include "pch.h"
#include "scTypes.h"

#define SC_VERSION_MAJOR 1
#define SC_VERSION_MINOR 3
#define SC_VERSION_PATCH 0

#define SC_STRINGIZE_(x) #x
#define SC_STRINGIZE(x) SC_STRINGIZE_(x)
#define SC_VERSION_STRING SC_STRINGIZE(SC_VERSION_MAJOR) "." SC_STRINGIZE(SC_VERSION_MINOR) "." SC_STRINGIZE(SC_VERSION_PATCH)
#define SC_VERSION_STRING_W L"" SC_VERSION_STRING
#define SC_VERSION_STRING_FULL_W L"v" SC_VERSION_STRING

typedef enum {
  SC_HOTKEY_SCREENSHOT,
  SC_HOTKEY_CLIPBOARD,
  SC_HOTKEY_OCR,
  SC_HOTKEY_ACTIVE_WINDOW,
  SC_HOTKEY_ACTIVE_MONITOR,
  SC_HOTKEY_FALLBACK_SCREENSHOT,
  SC_HOTKEY_RECORD,
  _SC_HOTKEY_COUNT
} scHotkeyID;

typedef enum {
  SC_SOUND_SCREENSHOT,
  SC_SOUND_SCREENSHOT_QUICK,
} scSoundID;

static const char* scHotkeyIdNames[_SC_HOTKEY_COUNT] = {
  "screenshot",
  "clipboard",
  "ocr",
  "active_window",
  "current_monitor",
  "fallback_screenshot",
  "record"
};

static const char* scCaptureActionNames[_SC_HOTKEY_COUNT] = {
  "Take Screenshot",
  "Take Screenshot (Clipboard)",
  "OCR Capture",
  "Take Screenshot (Window)",
  "Take Screenshot (Desktop)",
  "Take Screenshot (Alt)",
  "Start/Stop Recording"
};

typedef struct {
  scHotkeyID eID;
  u32        uModifiers;
  u32        uKey;
  bool       bRegistered;
} scHotkey;

typedef struct {
  scHotkey aHotkeys[_SC_HOTKEY_COUNT];
  wchar_t  wszSavePath[SC_PATH_MAX_LEN];
  s32      iFFmpegFramerate;
  char     sLanguageCode[16];
  bool     bCopyToClipboard;
  bool     bRunAtStartup;
  bool     bPlaySoundOnAction;
  bool     bShowNotification;
} scAppConfig;

typedef struct {
  // Private
  scV2I   vCaptureRegion;
  HWND    hOverlayWindow;
  HDC     hFrozenDC;
  HBITMAP hFrozenBitmap;
  HDC hOverlayMemDC;
  HBITMAP hOverlayMemBitmap;
  s32 iOverlayMemW;
  s32 iOverlayMemH;
  bool bMouseDown;
  bool bDragging;
  bool bWasDragging;
  scV2I stDragStart;
  scRect stFinalRect;
  HWND hHoveredWindow;

  // Magnifier stuff..
  HDC     hMagDC;
  HBITMAP hMagBitmap;
  s32     iMagDestX;
  s32     iMagDestY;
  bool    bMagValid;

  // Public
  scRect stSelectedRect;
  scHotkeyID eHotkeyID;

  // Modifiable by handler:
  void* pUser;
} scCaptureContext;

typedef struct {
  s32     W;
  s32     H;
  s32     iStride;
  u8*     pPixels;
  HBITMAP hBitmap;
} scImage;

typedef enum {
  SC_CLIP_NONE,
  SC_CLIP_FILE,
  SC_CLIP_MEMORY
} scClipboardSource;

typedef struct {
  HWND hWnd;
  UINT cfPng;
  scClipboardSource eSource;
  wchar_t wszPath[SC_PATH_MAX_LEN];
  scImage img;
} scClipboard;

typedef struct scCaptureHandler {
  bool (*cbOnHotkeyPressed)(scCaptureContext*    pCtx);
  bool (*cbOnAreaSelected)(scCaptureContext*     pCtx);
  void (*cbOnCaptureCancelled)(scCaptureContext* pCtx);

  // Optional overlay hooks (may be NULL).
  HCURSOR (*cbOverlayCursor)(scCaptureContext* pCtx);
  bool    (*cbSnapSelection)(scCaptureContext* pCtx, scRect rcInput, bool bDragging, scRect* pOut);
} scCaptureHandler;

typedef struct {
  scAppConfig config;
  scCaptureHandler* aCaptureHandlers[_SC_HOTKEY_COUNT];
  scCaptureContext* pCaptureContext;
  scCaptureHandler* pActiveHandler;
  scClipboard stClipboard;
  bool bIsGeWin10;
} scApp;

extern scApp* gApp;

//------------------------------------------------------------------------
// Core Application
bool scAppInit();
void scAppUpdate();
void scAppDestroy();

void scAppRunHandlerFromActionID(scHotkeyID iHotkeyID);

void scSaveConfig();

//------------------------------------------------------------------------
// Utils
bool scGetSavePath(wchar_t* wszOut, s32 nOutCap);
bool scGetFilename(wchar_t* wszOut, s32 nOutCap, const char* sExtension);

//------------------------------------------------------------------------
// Clipboard
//------------------------------------------------------------------------
bool scClipboardSetFile(scClipboard* pCb, const wchar_t* wszPath);
bool scClipboardSetImage(scClipboard* pCb, scImage* pImg);

//------------------------------------------------------------------------
// Other Application
bool scGetWindowRect(HWND hWnd, RECT* wr);

void scDestroyCaptureContext(scCaptureContext* pCtx);
void scCtxRequestCaptureArea(scCaptureContext* pCtx);

bool scCopyWindowToImage(HWND hWnd, scImage* pOutImage);
bool scCopyAreaToImage(scCaptureContext* pCtx, scImage* pOutImage, scRect rect);

bool scCtxCopyToImage(scCaptureContext* pCtx, scImage* pOutImage, scRect rect);
void scImageFree(scImage* pImage);
bool scImageToFile(const scImage* pImage, wchar_t* wszOutPath, s32 nOutCap);
bool scSaveDataToFile(const u8* pData, s32 nSize, const char* sExtension, wchar_t* wszOutPath, s32 nOutCap);
void scSaveImage(scImage* pImage, bool bWriteToDisk);

void scPlaySoundOrSkip(scSoundID eSoundID);

void scAppRegisterHotkeys();
void scAppSetupCallbackHandler();

#endif // SC_APP_H