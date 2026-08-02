#include "pch.h"
#include "scTypes.h"
#include "scAudio.h"
#include "scLogging.h"
#include "scApp.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

scAudio gAudio;

scInternal void _scAudioDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
  scAudioRecorder* pRecorder = (scAudioRecorder*)pDevice->pUserData;
  if (pRecorder && pRecorder->bIsActive && pInput) {
    ma_encoder_write_pcm_frames(&pRecorder->encoder, pInput, frameCount, NULL);
  }
  (void)pOutput;
}

bool scAudioPopulateDevices();

bool scAudioInit() {
  gAudio.pCaptureDevices = NULL;
  gAudio.deviceCount = 0;
  gAudio.activeRecorderCount = 0;

  ma_result maRes = ma_context_init(NULL, 0, NULL, &gAudio.maCtx);
  if (maRes != MA_SUCCESS) {
    scLogError("Failed to init audio context: %d", maRes);
    return false;
  }

  scAudioPopulateDevices();
  scAudioSetEnabledDevicesFromString(gApp->config.szSelectedAudioDevices);
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
    gAudio.pCaptureDevices[currentIdx].bIsEnabled = false;
    currentIdx++;
  }

  for (ma_uint32 i = 0; i < captureCount; i++) {
    snprintf(gAudio.pCaptureDevices[currentIdx].szName, sizeof(gAudio.pCaptureDevices[currentIdx].szName), "%s", pCaptureInfos[i].name);
    gAudio.pCaptureDevices[currentIdx].id = pCaptureInfos[i].id;
    gAudio.pCaptureDevices[currentIdx].bIsPlayback = false;
    gAudio.pCaptureDevices[currentIdx].bIsEnabled = false;
    currentIdx++;
  }

  gAudio.deviceCount = currentIdx;

  scLogDebug("Devices:");
  for (uint32_t i = 0; i < gAudio.deviceCount; i++) {
    scLogDebug("  [%s] %d - %s", gAudio.pCaptureDevices[i].bIsPlayback ? "Playback/Loopback" : "Capture", i, gAudio.pCaptureDevices[i].szName);
  }

  return true;
}

void scAudioDeviceUpdated(u16 uDeviceIdx) {
  if (uDeviceIdx >= gAudio.deviceCount) {
    scLogWarn("Invalid audio device index! Passed in %u but device count is %d", uDeviceIdx, gAudio.deviceCount);
    return;
  }
  scLogInfo("Audio device '%s' is %s", gAudio.pCaptureDevices[uDeviceIdx].szName, gAudio.pCaptureDevices[uDeviceIdx].bIsEnabled ? "enabled" : "disabled");
  scAudioGetEnabledDevicesString(gApp->config.szSelectedAudioDevices, sizeof(gApp->config.szSelectedAudioDevices));
  scSaveConfig();
}

void scAudioGetEnabledDevicesString(char* szOut, size_t nOutCap) {
  if (nOutCap == 0) {
    return;
  }
  szOut[0] = '\0';

  size_t len = 0;
  for (uint32_t i = 0; i < gAudio.deviceCount; i++) {
    if (!gAudio.pCaptureDevices[i].bIsEnabled) {
      continue;
    }

    size_t nameLen = strlen(gAudio.pCaptureDevices[i].szName);
    size_t sepLen = (len > 0) ? 1 : 0;

    if (len + sepLen + nameLen >= nOutCap) {
      scLogWarn("Enabled audio devices string truncated, buffer too small");
      break;
    }

    if (sepLen) {
      szOut[len++] = '|';
    }
    memcpy(szOut + len, gAudio.pCaptureDevices[i].szName, nameLen);
    len += nameLen;
    szOut[len] = '\0';
  }
}

void scAudioSetEnabledDevicesFromString(const char* szDevices) {
  for (uint32_t i = 0; i < gAudio.deviceCount; i++) {
    gAudio.pCaptureDevices[i].bIsEnabled = false;
  }

  if (!szDevices || !szDevices[0]) {
    return;
  }

  char szBuf[SC_AUDIO_DEVICES_STRSIZE];
  strncpy(szBuf, szDevices, sizeof(szBuf) - 1);
  szBuf[sizeof(szBuf) - 1] = '\0';

  char* pCtx = NULL;
  char* pTok = strtok_s(szBuf, ",|", &pCtx);
  while (pTok) {
    for (uint32_t i = 0; i < gAudio.deviceCount; i++) {
      if (_stricmp(gAudio.pCaptureDevices[i].szName, pTok) == 0) {
        gAudio.pCaptureDevices[i].bIsEnabled = true;
        break;
      }
    }
    pTok = strtok_s(NULL, ",|", &pCtx);
  }
}

bool scAudioStartRecording(const wchar_t* wszTempDir) {
  gAudio.activeRecorderCount = 0;

  for (uint32_t i = 0; i < gAudio.deviceCount; i++) {
    if (!gAudio.pCaptureDevices[i].bIsEnabled) {
      continue;
    }

    if (gAudio.activeRecorderCount >= SC_MAX_ACTIVE_RECORDERS) {
      scLogWarn("Reached maximum active audio recorders (%d)", SC_MAX_ACTIVE_RECORDERS);
      break;
    }

    scAudioRecorder* pRec = &gAudio.aRecorders[gAudio.activeRecorderCount];
    memset(pRec, 0, sizeof(scAudioRecorder));

    swprintf(pRec->wszWavPath, MAX_PATH, L"%ls\\sc_temp_audio_%u.wav", wszTempDir, gAudio.activeRecorderCount);

    ma_encoder_config encoderConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 2, 48000);
    if (ma_encoder_init_file_w(pRec->wszWavPath, &encoderConfig, &pRec->encoder) != MA_SUCCESS) {
      scLogError("Failed to create WAV encoder for audio device %u", i);
      continue;
    }

    ma_device_config devConfig = ma_device_config_init(gAudio.pCaptureDevices[i].bIsPlayback ? ma_device_type_loopback : ma_device_type_capture);
    devConfig.capture.pDeviceID = &gAudio.pCaptureDevices[i].id;
    devConfig.capture.format = ma_format_s16;
    devConfig.capture.channels = 2;
    devConfig.sampleRate = 48000;
    devConfig.dataCallback = _scAudioDataCallback;
    devConfig.pUserData = pRec;

    if (ma_device_init(&gAudio.maCtx, &devConfig, &pRec->device) != MA_SUCCESS) {
      scLogError("Failed to initialize capture device %s", gAudio.pCaptureDevices[i].szName);
      ma_encoder_uninit(&pRec->encoder);
      continue;
    }

    pRec->bIsActive = true;
    if (ma_device_start(&pRec->device) != MA_SUCCESS) {
      scLogError("Failed to start capture device %s", gAudio.pCaptureDevices[i].szName);
      ma_device_uninit(&pRec->device);
      ma_encoder_uninit(&pRec->encoder);
      pRec->bIsActive = false;
      continue;
    }
    QueryPerformanceCounter(&pRec->liStartTime);
    scLogInfo("Audio device '%s' started (perf counter: %lld)", gAudio.pCaptureDevices[i].szName, pRec->liStartTime.QuadPart);
    gAudio.activeRecorderCount++;
  }

  scLogInfo("Started audio recording across %u devices", gAudio.activeRecorderCount);
  return gAudio.activeRecorderCount > 0;
}

void scAudioStopRecording(scAudioTrackInfo pOutTracks[], uint32_t* pOutCount) {
  *pOutCount = 0;

  for (uint32_t i = 0; i < gAudio.activeRecorderCount; i++) {
    scAudioRecorder* pRec = &gAudio.aRecorders[i];
    if (pRec->bIsActive) {
      ma_device_uninit(&pRec->device);
      ma_encoder_uninit(&pRec->encoder);
      pRec->bIsActive = false;

      wcsncpy(pOutTracks[*pOutCount].wszPath, pRec->wszWavPath, SC_PATH_MAX_LEN - 1);
      pOutTracks[*pOutCount].wszPath[SC_PATH_MAX_LEN - 1] = L'\0';
      pOutTracks[*pOutCount].liStartTime = pRec->liStartTime;
      (*pOutCount)++;
    }
  }

  gAudio.activeRecorderCount = 0;
  scLogInfo("Stopped audio recording");
}