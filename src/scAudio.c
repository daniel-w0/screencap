#include "pch.h"
#include "scTypes.h"
#include "scAudio.h"
#include "scLogging.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

scAudio gAudio;

bool scAudioPopulateDevices();

bool scAudioInit() {
  gAudio.pCaptureDevices = NULL;
  gAudio.deviceCount = 0;

  ma_result maRes = ma_context_init(NULL, 0, NULL, &gAudio.maCtx);
  if (maRes != MA_SUCCESS) {
    scLogError("Failed to init audio context: %d", maRes);
    return false;
  }

  scAudioPopulateDevices();
  return true;
}

void scAudioDestroy() {
  if (gAudio.pCaptureDevices) {
    free(gAudio.pCaptureDevices);
    gAudio.pCaptureDevices = NULL;
  }
  gAudio.deviceCount = 0;
  ma_context_uninit(&gAudio.maCtx);
}

bool scAudioPopulateDevices() {
  ma_device_info* pPlaybackInfos;
  ma_uint32 playbackCount;
  ma_device_info* pCaptureInfos;
  ma_uint32 captureCount;

  ma_result maRes = ma_context_get_devices(&gAudio.maCtx, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
  if (maRes != MA_SUCCESS) {
    scLogError("Failed to retrieve audio devices: %d", maRes);
    return false;
  }

  if (gAudio.pCaptureDevices) {
    free(gAudio.pCaptureDevices);
    gAudio.pCaptureDevices = NULL;
  }

  uint32_t totalDevices = playbackCount + captureCount;
  if (totalDevices == 0) {
    gAudio.deviceCount = 0;
    return true;
  }

  gAudio.pCaptureDevices = (scAudioDevice*)malloc(sizeof(scAudioDevice) * totalDevices);
  if (!gAudio.pCaptureDevices) {
    return false;
  }

  uint32_t currentIdx = 0;

  for (ma_uint32 i = 0; i < playbackCount; i++) {
    snprintf(gAudio.pCaptureDevices[currentIdx].szName, sizeof(gAudio.pCaptureDevices[currentIdx].szName), "%s", pPlaybackInfos[i].name);
    gAudio.pCaptureDevices[currentIdx].id = pPlaybackInfos[i].id;
    gAudio.pCaptureDevices[currentIdx].bIsPlayback = true;
    currentIdx++;
  }

  for (ma_uint32 i = 0; i < captureCount; i++) {
    snprintf(gAudio.pCaptureDevices[currentIdx].szName, sizeof(gAudio.pCaptureDevices[currentIdx].szName), "%s", pCaptureInfos[i].name);
    gAudio.pCaptureDevices[currentIdx].id = pCaptureInfos[i].id;
    gAudio.pCaptureDevices[currentIdx].bIsPlayback = false;
    currentIdx++;
  }

  gAudio.deviceCount = currentIdx;

  scLogDebug("Devices:");
  for (uint32_t i = 0; i < gAudio.deviceCount; i++) {
    gAudio.pCaptureDevices[i].bIsEnabled = false;
    scLogDebug("  [%s] %d - %s", gAudio.pCaptureDevices[i].bIsPlayback ? "Playback/Loopback" : "Capture", i, gAudio.pCaptureDevices[i].szName);
  }

  return true;
}

void scAudioDeviceUpdated(u16 uDeviceIdx) {
  if (uDeviceIdx >= gAudio.deviceCount) {
    scLogWarn("Invalid audio device index! Passed in %u but device count is %d", uDeviceIdx, gAudio.deviceCount);
    return;
  }
  scLogInfo("Audio device '%s' is %d", gAudio.pCaptureDevices[uDeviceIdx].szName, gAudio.pCaptureDevices[uDeviceIdx].bIsEnabled ? "enabled" : "disabled");
}

void scAudioTest() {
  if (scAudioInit()) {
    scAudioPopulateDevices();
    scAudioDestroy();
  }
}