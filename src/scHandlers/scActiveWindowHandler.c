#include "pch.h"
#include "scApp.h"
#include "scLogging.h"

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  HWND hActiveWindow = GetForegroundWindow();
  if (!hActiveWindow) {
    scLogError("Failed getting active window: %d", GetLastError());
    return true;
  }

  scImage stImage;
  if (!scCopyWindowToImage(hActiveWindow, &stImage)) {
    return true;
  }

  scImageToFile(&stImage);
  scImageFree(&stImage);
  return true;
}

const scCaptureHandler scActiveWindowHandler = {
  cbOnHotkeyPressed,
  NULL,
  NULL
};