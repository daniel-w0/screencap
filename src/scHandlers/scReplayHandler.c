#include "pch.h"
#include "scApp.h"
#include "scLogging.h"

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  scReplayBufferSave();
  return false;
}

const scCaptureHandler scReplayHandler = {
  cbOnHotkeyPressed,
  NULL
};