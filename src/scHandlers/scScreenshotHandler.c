#include "pch.h"
#include "scApp.h"
#include "scLogging.h"

scInternal void
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  scCtxCaptureArea(pCtx);
}

scInternal void
cbOnAreaSelected(scCaptureContext* pCtx) {
  scLogDebug("Captured Area: { %d, %d, %d, %d }", pCtx->stSelectedRect.X, pCtx->stSelectedRect.Y, pCtx->stSelectedRect.W, pCtx->stSelectedRect.H);
}

//scInternal void
//cbOnCaptureCancelled(scCaptureContext* pCtx) {

//}

const scCaptureHandler scScreenshotHandler = {
  cbOnHotkeyPressed,
  cbOnAreaSelected,
  NULL
};