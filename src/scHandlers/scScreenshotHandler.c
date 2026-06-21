#include "pch.h"
#include "scApp.h"
#include "scLogging.h"

scInternal void
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  pCtx->bRequstCaptureArea = true;
}

scInternal void
cbOnAreaSelected(scCaptureContext* pCtx) {

}

scInternal void
cbOnCaptureCancelled(scCaptureContext* pCtx) {

}

const scCaptureHandler scScreenshotHandler = { cbOnHotkeyPressed, cbOnAreaSelected, cbOnCaptureCancelled };