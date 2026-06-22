#include "pch.h"
#include "scApp.h"
#include "scLogging.h"

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  scCtxRequestCaptureArea(pCtx);
  return false;
}

scInternal bool
cbOnAreaSelected(scCaptureContext* pCtx) {
  scLogDebug("Captured Area: { %d, %d, %d, %d }", pCtx->stSelectedRect.X, pCtx->stSelectedRect.Y, pCtx->stSelectedRect.W, pCtx->stSelectedRect.H);
  scImage stImage = { 0 };

  if (!scCtxCopyToImage(pCtx, &stImage, pCtx->stSelectedRect)) {
    return true;
  }

  if (pCtx->eHotkeyID == SC_HOTKEY_CLIPBOARD) {
    scLogInfo("Copying to clipboard... (not implemented!");
  } else {
    scImageToFile(&stImage);
  }

  scImageFree(&stImage);
  return true;
}

//scInternal void
//cbOnCaptureCancelled(scCaptureContext* pCtx) {

//}

const scCaptureHandler scScreenshotHandler = {
  cbOnHotkeyPressed,
  cbOnAreaSelected,
  NULL
};