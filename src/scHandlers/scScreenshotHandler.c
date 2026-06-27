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
  scLogDebug("Captured Area: { %d, %d, %d, %d }", pCtx->stSelectedRect.x, pCtx->stSelectedRect.y, pCtx->stSelectedRect.w, pCtx->stSelectedRect.h);
  scImage stImage = { 0 };
  if (!scCtxCopyToImage(pCtx, &stImage, pCtx->stSelectedRect)) {
    return true;
  }

  scSaveImage(&stImage, !(pCtx->eHotkeyID == SC_HOTKEY_CLIPBOARD));
  return true;
}

const scCaptureHandler scScreenshotHandler = {
  cbOnHotkeyPressed,
  cbOnAreaSelected,
  NULL
};