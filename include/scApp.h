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

static const char* scHotkeyIdNames[_SC_HOTKEY_COUNT] = {
  "screenshot",
  "clipboard",
  "ocr",
  "active_window",
  "current_monitor",
  "fallback_screenshot",
  "record"
};

typedef struct {
  s32 X, Y;
  s32 W, H;
} scRect;

typedef struct {
  s32 X, Y;
} scV2I;

typedef struct {
  scHotkeyID eID;
  u32        uModifiers;
  u32        uKey;
  bool       bRegistered;
} scHotkey;

typedef struct {
  scHotkey aHotkeys[_SC_HOTKEY_COUNT];
  wchar_t  wszSavePath[SC_PATH_MAX_LEN];
  char     sLanguageCode[4];
  bool     bCopyToClipboard;
  bool     bRunAtStartup;
  bool     bPlaySoundOnAction;
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

  // Modifiable by handler:
  bool  bRequstCaptureArea;
  void* pUser;
} scCaptureContext;

typedef struct scCaptureHandler {
  void (*cbOnHotkeyPressed)(scCaptureContext*    pCtx);
  void (*cbOnAreaSelected)(scCaptureContext*     pCtx);
  void (*cbOnCaptureCancelled)(scCaptureContext* pCtx);
  bool bCreateFrozenWindow;
} scCaptureHandler;

typedef struct {
  scAppConfig config;
  scCaptureHandler* aCaptureHandlers[_SC_HOTKEY_COUNT];
  scCaptureContext* pCaptureContext;
  scCaptureHandler* pActiveHandler;
} scApp;

//------------------------------------------------------------------------
// Core Application
bool scAppInit();
void scAppUpdate();
void scAppDestroy();

//------------------------------------------------------------------------
// Other Application
void scAppCaptureArea();

void scAppRegisterHotkeys();
void scAppSetupCallbackHandler();

#endif // SC_APP_H