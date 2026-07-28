#include "pch.h"
#include "scApp.h"
#include "scLogging.h"
#include "scAssert.h"
#include "scFfmpegHelper.h"

typedef struct {
  scFFmpegInstance ffmpeg;
  wchar_t wszSavePath[SC_PATH_MAX_LEN];
} scRecordContext;

scInternal bool
_startRecording(scRecordContext* pCtx, scRect rect) {
  wchar_t wszDir[MAX_PATH];
  wchar_t wszName[MAX_PATH];
  if (!scGetSavePath(wszDir, MAX_PATH) || !scGetFilename(wszName, MAX_PATH, ".mp4")) {
    return false;
  }

  swprintf(pCtx->wszSavePath, MAX_PATH, L"%ls\\%ls", wszDir, wszName);

  bool ok = scFFmpegStartRecording(&pCtx->ffmpeg, pCtx->wszSavePath, rect);
  if (ok) {
    scPlaySoundOrSkip(SC_SOUND_SCREENSHOT_QUICK);
  }

  return ok;
}

scInternal void
_stopRecording(scRecordContext* pCtx) {
  scFFmpegStopRecording(&pCtx->ffmpeg);
  scPlaySoundOrSkip(SC_SOUND_SCREENSHOT);
}

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  if (pCtx->pUser) {
    _stopRecording((scRecordContext*)pCtx->pUser);
    return true;
  } else {
    scRecordContext* pRecordCtx = (scRecordContext*)calloc(1, sizeof(scRecordContext));
    pCtx->pUser = (void*)pRecordCtx;

    wchar_t wszFFmpegPath[SC_PATH_MAX_LEN] = { 0 };
    if (!scFFmpegGetPath((wchar_t*)wszFFmpegPath, SC_PATH_MAX_LEN)) {
      scLogError("Unable to locate ffmpeg.exe");
      return true;
    }

    scCtxRequestCaptureArea(pCtx);
    return false;
  }
}

scInternal bool
cbOnAreaSelected(scCaptureContext* pCtx) {
  scAssert(pCtx->pUser, "pUser is null!");
  if (!pCtx->pUser) {
    scLogError("Recording context is null in cbOnAreaSelected!");
    return true;
  }
  if (!_startRecording((scRecordContext*)pCtx->pUser, pCtx->stSelectedRect)) {
    return true;
  } else {
    return false;
  }
}

const scCaptureHandler scRecordHandler = {
  cbOnHotkeyPressed,
  cbOnAreaSelected,
  NULL
};