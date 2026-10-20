#include "pch.h"
#include "scTypes.h"
#include "scAudio.h"
#include "scLogging.h"
#include "scApp.h"
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_NULL
#define MA_NO_MP3
#define MA_NO_FLAC
#define MA_NO_VORBIS
#define MA_NO_M4A
#define MA_NO_WAV
#define MA_NO_DECODERS
#include "miniaudio.h"

scAudio gAudio;

typedef struct {
  scAudioCapture* pCapture;
  ma_device device;
  bool bDeviceInitialized;
  bool bPlaced;
  uint32_t iTrack;
  int64_t nExpectedFrame;
} scAudioCaptureDevice;

struct scAudioCapture {
  scAudioCaptureDevice aDevices[SC_MAX_ACTIVE_RECORDERS];
  uint32_t nDevices;
  CRITICAL_SECTION csMix;
  bool bMixLockInitialized;
  int32_t* pMix;
  int64_t anEmittedFrame[SC_AUDIO_MAX_TRACKS];
  bool abTracksUsed[SC_AUDIO_MAX_TRACKS];
  LARGE_INTEGER qpcStart;
  LARGE_INTEGER qpcFrequency;
  volatile LONG bTimelineSet;
};

bool scAudioPopulateDevices();

static uint32_t
_scAudioSelectionTotalCount(void) {
  return gAudio.inputCount + gAudio.outputCount;
}

bool scAudioInit() {
  memset(&gAudio, 0, sizeof(gAudio));

  ma_result maRes = ma_context_init(NULL, 0, NULL, &gAudio.maCtx);
  if (maRes != MA_SUCCESS) {
    scLogError("Failed to init audio context: %d", maRes);
    return false;
  }

  scAudioPopulateDevices();
  scAudioSetSelectionsFromString(gApp->config.szSelectedAudioDevices);
  return true;
}

void scAudioDestroy() {
  free(gAudio.pCaptureDevices);
  free(gAudio.pPlaybackDevices);
  gAudio.pCaptureDevices = NULL;
  gAudio.pPlaybackDevices = NULL;
  gAudio.captureDeviceCount = 0;
  gAudio.playbackDeviceCount = 0;
  ma_context_uninit(&gAudio.maCtx);
}

bool scAudioPopulateDevices() {
  ma_device_info* pPlaybackInfos = NULL;
  ma_uint32 playbackCount = 0;
  ma_device_info* pCaptureInfos = NULL;
  ma_uint32 captureCount = 0;

  ma_result maRes = ma_context_get_devices(&gAudio.maCtx, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
  if (maRes != MA_SUCCESS) {
    scLogError("Failed to retrieve audio devices: %d", maRes);
    return false;
  }

  scAudioDevice* pCaptureDevices = NULL;
  scAudioDevice* pPlaybackDevices = NULL;
  if (captureCount > 0) {
    pCaptureDevices = (scAudioDevice*)calloc(captureCount, sizeof(scAudioDevice));
    if (!pCaptureDevices) {
      scLogError("Failed to allocate capture device list");
      return false;
    }
  }
  if (playbackCount > 0) {
    pPlaybackDevices = (scAudioDevice*)calloc(playbackCount, sizeof(scAudioDevice));
    if (!pPlaybackDevices) {
      free(pCaptureDevices);
      scLogError("Failed to allocate playback device list");
      return false;
    }
  }

  for (ma_uint32 i = 0; i < captureCount; ++i) {
    snprintf(pCaptureDevices[i].szName, sizeof(pCaptureDevices[i].szName), "%s", pCaptureInfos[i].name);
    pCaptureDevices[i].id = pCaptureInfos[i].id;
  }
  for (ma_uint32 i = 0; i < playbackCount; ++i) {
    snprintf(pPlaybackDevices[i].szName, sizeof(pPlaybackDevices[i].szName), "%s", pPlaybackInfos[i].name);
    pPlaybackDevices[i].id = pPlaybackInfos[i].id;
  }

  free(gAudio.pCaptureDevices);
  free(gAudio.pPlaybackDevices);
  gAudio.pCaptureDevices = pCaptureDevices;
  gAudio.captureDeviceCount = captureCount;
  gAudio.pPlaybackDevices = pPlaybackDevices;
  gAudio.playbackDeviceCount = playbackCount;

  scLogDebug("Capture devices:");
  for (uint32_t i = 0; i < gAudio.captureDeviceCount; ++i) {
    scLogDebug("  [Input] %u - %s", i, gAudio.pCaptureDevices[i].szName);
  }
  scLogDebug("Playback devices:");
  for (uint32_t i = 0; i < gAudio.playbackDeviceCount; ++i) {
    scLogDebug("  [Output/Loopback] %u - %s", i, gAudio.pPlaybackDevices[i].szName);
  }

  return true;
}

static void
_scAudioSaveSelections(void) {
  scAudioGetSelectionsString(gApp->config.szSelectedAudioDevices,
                             sizeof(gApp->config.szSelectedAudioDevices));
  scSaveConfig();
}

static bool
_scAudioSelectionAdd(scAudioSelection* aSelections, uint32_t* pCount) {
  if (!aSelections || !pCount || *pCount >= SC_AUDIO_MAX_DEVICE_SELECTIONS ||
      _scAudioSelectionTotalCount() >= SC_AUDIO_MAX_DEVICE_SELECTIONS) {
    return false;
  }
  aSelections[*pCount].iDevice = -1;
  aSelections[*pCount].iTrack = 0;
  ++(*pCount);
  _scAudioSaveSelections();
  return true;
}

static void
_scAudioSelectionRemove(scAudioSelection* aSelections, uint32_t* pCount, uint32_t iSelection) {
  if (!aSelections || !pCount || iSelection >= *pCount) {
    return;
  }
  if (iSelection + 1 < *pCount) {
    memmove(&aSelections[iSelection], &aSelections[iSelection + 1],
            (*pCount - iSelection - 1) * sizeof(aSelections[0]));
  }
  --(*pCount);
  _scAudioSaveSelections();
}

static void
_scAudioSelectionSetDevice(scAudioSelection* aSelections, uint32_t nSelections,
                           uint32_t nDevices, uint32_t iSelection, int32_t iDevice) {
  if (!aSelections || iSelection >= nSelections || iDevice < -1 ||
      (iDevice >= 0 && (uint32_t)iDevice >= nDevices)) {
    return;
  }
  aSelections[iSelection].iDevice = iDevice;
  _scAudioSaveSelections();
}

static void
_scAudioSelectionSetTrack(scAudioSelection* aSelections, uint32_t nSelections,
                          uint32_t iSelection, int32_t iTrack) {
  if (!aSelections || iSelection >= nSelections || iTrack < 0 || iTrack >= SC_AUDIO_MAX_TRACKS) {
    return;
  }
  aSelections[iSelection].iTrack = iTrack;
  _scAudioSaveSelections();
}

bool scAudioInputSelectionAdd(void) {
  return _scAudioSelectionAdd(gAudio.aInputs, &gAudio.inputCount);
}

void scAudioInputSelectionRemove(uint32_t iSelection) {
  _scAudioSelectionRemove(gAudio.aInputs, &gAudio.inputCount, iSelection);
}

void scAudioInputSelectionSetDevice(uint32_t iSelection, int32_t iDevice) {
  _scAudioSelectionSetDevice(gAudio.aInputs, gAudio.inputCount, gAudio.captureDeviceCount, iSelection, iDevice);
}

void scAudioInputSelectionSetTrack(uint32_t iSelection, int32_t iTrack) {
  _scAudioSelectionSetTrack(gAudio.aInputs, gAudio.inputCount, iSelection, iTrack);
}

bool scAudioOutputSelectionAdd(void) {
  return _scAudioSelectionAdd(gAudio.aOutputs, &gAudio.outputCount);
}

void scAudioOutputSelectionRemove(uint32_t iSelection) {
  _scAudioSelectionRemove(gAudio.aOutputs, &gAudio.outputCount, iSelection);
}

void scAudioOutputSelectionSetDevice(uint32_t iSelection, int32_t iDevice) {
  _scAudioSelectionSetDevice(gAudio.aOutputs, gAudio.outputCount, gAudio.playbackDeviceCount, iSelection, iDevice);
}

void scAudioOutputSelectionSetTrack(uint32_t iSelection, int32_t iTrack) {
  _scAudioSelectionSetTrack(gAudio.aOutputs, gAudio.outputCount, iSelection, iTrack);
}

/* Keep the original API as wrappers for input selections. */
bool scAudioSelectionAdd(void) { return scAudioInputSelectionAdd(); }
void scAudioSelectionRemove(uint32_t iSelection) { scAudioInputSelectionRemove(iSelection); }
void scAudioSelectionSetDevice(uint32_t iSelection, int32_t iDevice) { scAudioInputSelectionSetDevice(iSelection, iDevice); }
void scAudioSelectionSetTrack(uint32_t iSelection, int32_t iTrack) { scAudioInputSelectionSetTrack(iSelection, iTrack); }

static bool
_scAudioAppendSelection(char* szOut, size_t nOutCap, size_t* pLen, char cType,
                        const scAudioSelection* pSelection,
                        const scAudioDevice* pDevices, uint32_t nDevices) {
  const char* szName = "";
  if (pSelection->iDevice >= 0 && (uint32_t)pSelection->iDevice < nDevices) {
    szName = pDevices[pSelection->iDevice].szName;
  }

  if (*pLen >= nOutCap) {
    return false;
  }
  int nWritten = snprintf(szOut + *pLen, nOutCap - *pLen, "%s%c:%d@%s",
                          *pLen ? "|" : "", cType, pSelection->iTrack, szName);
  if (nWritten < 0 || (size_t)nWritten >= nOutCap - *pLen) {
    szOut[*pLen] = '\0';
    scLogWarn("Audio selections string truncated, buffer too small");
    return false;
  }
  *pLen += (size_t)nWritten;
  return true;
}

void scAudioGetSelectionsString(char* szOut, size_t nOutCap) {
  if (nOutCap == 0) {
    return;
  }
  szOut[0] = '\0';
  size_t len = 0;

  for (uint32_t i = 0; i < gAudio.inputCount; ++i) {
    if (!_scAudioAppendSelection(szOut, nOutCap, &len, 'I', &gAudio.aInputs[i],
                                 gAudio.pCaptureDevices, gAudio.captureDeviceCount)) {
      return;
    }
  }
  for (uint32_t i = 0; i < gAudio.outputCount; ++i) {
    if (!_scAudioAppendSelection(szOut, nOutCap, &len, 'O', &gAudio.aOutputs[i],
                                 gAudio.pPlaybackDevices, gAudio.playbackDeviceCount)) {
      return;
    }
  }
}

static bool
_scAudioFindDeviceByName(const scAudioDevice* pDevices, uint32_t nDevices,
                         const char* szName, int32_t* pDevice) {
  if (!szName || !szName[0]) {
    return false;
  }
  for (uint32_t i = 0; i < nDevices; ++i) {
    if (_stricmp(pDevices[i].szName, szName) == 0) {
      *pDevice = (int32_t)i;
      return true;
    }
  }
  return false;
}

void scAudioSetSelectionsFromString(const char* szSelections) {
  gAudio.inputCount = 0;
  gAudio.outputCount = 0;
  if (!szSelections || !szSelections[0]) {
    return;
  }

  char szBuf[SC_AUDIO_DEVICES_STRSIZE];
  strncpy(szBuf, szSelections, sizeof(szBuf) - 1);
  szBuf[sizeof(szBuf) - 1] = '\0';

  char* pCtx = NULL;
  char* pTok = strtok_s(szBuf, ",|", &pCtx);
  while (pTok && _scAudioSelectionTotalCount() < SC_AUDIO_MAX_DEVICE_SELECTIONS) {
    bool bExplicitType = false;
    bool bOutput = false;
    char* pValue = pTok;
    if ((pTok[0] == 'I' || pTok[0] == 'i' || pTok[0] == 'O' || pTok[0] == 'o') && pTok[1] == ':') {
      bExplicitType = true;
      bOutput = (pTok[0] == 'O' || pTok[0] == 'o');
      pValue += 2;
    }

    int32_t iTrack = 0;
    char* szName = pValue;
    char* pAt = strchr(pValue, '@');
    if (pAt && pAt != pValue) {
      bool bNumeric = true;
      for (char* p = pValue; p < pAt; ++p) {
        if (*p < '0' || *p > '9') {
          bNumeric = false;
          break;
        }
      }
      if (bNumeric) {
        char cAt = *pAt;
        *pAt = '\0';
        char* pEnd = NULL;
        long nTrack = strtol(pValue, &pEnd, 10);
        if (pEnd && *pEnd == '\0' && nTrack >= 0 && nTrack < SC_AUDIO_MAX_TRACKS) {
          iTrack = (int32_t)nTrack;
          szName = pAt + 1;
        } else {
          *pAt = cAt;
        }
      }
    }

    int32_t iDevice = -1;
    if (bExplicitType) {
      if (bOutput) {
        _scAudioFindDeviceByName(gAudio.pPlaybackDevices, gAudio.playbackDeviceCount, szName, &iDevice);
      } else {
        _scAudioFindDeviceByName(gAudio.pCaptureDevices, gAudio.captureDeviceCount, szName, &iDevice);
      }
    } else if (szName[0]) {
      /* The old combined list placed playback devices before capture devices. */
      if (_scAudioFindDeviceByName(gAudio.pPlaybackDevices, gAudio.playbackDeviceCount, szName, &iDevice)) {
        bOutput = true;
      } else {
        _scAudioFindDeviceByName(gAudio.pCaptureDevices, gAudio.captureDeviceCount, szName, &iDevice);
      }
    }

    if (bOutput) {
      if (gAudio.outputCount < SC_AUDIO_MAX_DEVICE_SELECTIONS) {
        scAudioSelection* pSelection = &gAudio.aOutputs[gAudio.outputCount++];
        pSelection->iDevice = iDevice;
        pSelection->iTrack = iTrack;
      }
    } else {
      if (gAudio.inputCount < SC_AUDIO_MAX_DEVICE_SELECTIONS) {
        scAudioSelection* pSelection = &gAudio.aInputs[gAudio.inputCount++];
        pSelection->iDevice = iDevice;
        pSelection->iTrack = iTrack;
      }
    }

    pTok = strtok_s(NULL, ",|", &pCtx);
  }
}

scInternal int16_t _scAudioClampS16(int32_t value) {
  if (value < -32768) return -32768;
  if (value > 32767) return 32767;
  return (int16_t)value;
}

int64_t scAudioCaptureQpcToFrame(const scAudioCapture* pCap, int64_t qpc) {
  return ((qpc - pCap->qpcStart.QuadPart) * SC_AUDIO_SAMPLE_RATE) / pCap->qpcFrequency.QuadPart;
}

/* Every device callback places its samples on a shared timeline measured from
   the recording's QPC origin and adds them into a mix ring. Samples are placed
   contiguously after the previous chunk unless the callback arrived too far
   from where it was expected (loopback devices deliver nothing during silence,
   and device clocks drift against QPC), in which case the device is resynced
   to the QPC-derived position. Unfilled regions of the ring stay zero. */
scInternal void _scAudioMixCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
  scAudioCaptureDevice* pDev = (scAudioCaptureDevice*)pDevice->pUserData;
  scAudioCapture* pCap = pDev ? pDev->pCapture : NULL;
  const int16_t* pSamples = (const int16_t*)pInput;
  LARGE_INTEGER qpcNow;
  int64_t tStart;
  int64_t tEnd;
  int64_t first;
  int64_t last;
  int64_t t;
  (void)pOutput;

  if (!pCap || !pSamples || frameCount == 0) {
    return;
  }
  if (InterlockedCompareExchange(&pCap->bTimelineSet, 0, 0) == 0) {
    return;
  }
  if (!QueryPerformanceCounter(&qpcNow)) {
    return;
  }

  tStart = scAudioCaptureQpcToFrame(pCap, qpcNow.QuadPart) - (int64_t)frameCount;
  if (!pDev->bPlaced || llabs(tStart - pDev->nExpectedFrame) > SC_AUDIO_RESYNC_FRAMES) {
    pDev->nExpectedFrame = tStart;
    pDev->bPlaced = true;
  }
  tStart = pDev->nExpectedFrame;
  tEnd = tStart + (int64_t)frameCount;
  pDev->nExpectedFrame = tEnd;

  if (pDev->iTrack >= SC_AUDIO_MAX_TRACKS) {
    return;
  }

  EnterCriticalSection(&pCap->csMix);
  first = pCap->anEmittedFrame[pDev->iTrack];
  last = first + SC_AUDIO_MIX_FRAMES;
  for (t = (tStart > first ? tStart : first); t < tEnd && t < last; ++t) {
    const size_t src = (size_t)(t - tStart) * SC_AUDIO_CHANNELS;
    const size_t dst = ((size_t)pDev->iTrack * SC_AUDIO_MIX_FRAMES +
                        (size_t)(t % SC_AUDIO_MIX_FRAMES)) * SC_AUDIO_CHANNELS;
    pCap->pMix[dst + 0] += pSamples[src + 0];
    pCap->pMix[dst + 1] += pSamples[src + 1];
  }
  LeaveCriticalSection(&pCap->csMix);
}

scAudioCapture* scAudioCaptureStart(void) {
  scAudioCapture* pCap = (scAudioCapture*)calloc(1, sizeof(scAudioCapture));
  if (!pCap) {
    scLogError("Failed to allocate audio capture");
    return NULL;
  }

  pCap->pMix = (int32_t*)calloc((size_t)SC_AUDIO_MAX_TRACKS * SC_AUDIO_MIX_FRAMES * SC_AUDIO_CHANNELS,
                                sizeof(int32_t));
  if (!pCap->pMix) {
    scLogError("Failed to allocate audio mix buffer");
    free(pCap);
    return NULL;
  }
  InitializeCriticalSection(&pCap->csMix);
  pCap->bMixLockInitialized = true;

  for (uint32_t iType = 0; iType < 2; ++iType) {
    const bool bPlayback = (iType == 1);
    const scAudioSelection* aSelections = bPlayback ? gAudio.aOutputs : gAudio.aInputs;
    const uint32_t nSelections = bPlayback ? gAudio.outputCount : gAudio.inputCount;
    const scAudioDevice* pDevices = bPlayback ? gAudio.pPlaybackDevices : gAudio.pCaptureDevices;
    const uint32_t nDevices = bPlayback ? gAudio.playbackDeviceCount : gAudio.captureDeviceCount;

    for (uint32_t i = 0; i < nSelections; ++i) {
      const scAudioSelection* pSelection = &aSelections[i];
      const scAudioDevice* pAudioDevice;
      scAudioCaptureDevice* pDev;
      ma_device_config devConfig;

      if (pSelection->iDevice < 0 || (uint32_t)pSelection->iDevice >= nDevices ||
          pSelection->iTrack < 0 || pSelection->iTrack >= SC_AUDIO_MAX_TRACKS) {
        continue;
      }
      if (pCap->nDevices >= SC_MAX_ACTIVE_RECORDERS) {
        scLogWarn("Reached maximum active audio recorders (%d)", SC_MAX_ACTIVE_RECORDERS);
        break;
      }

      pAudioDevice = &pDevices[pSelection->iDevice];
      pDev = &pCap->aDevices[pCap->nDevices];
      memset(pDev, 0, sizeof(*pDev));
      pDev->pCapture = pCap;
      pDev->iTrack = (uint32_t)pSelection->iTrack;

      devConfig = ma_device_config_init(bPlayback ? ma_device_type_loopback : ma_device_type_capture);
      devConfig.capture.pDeviceID = &pAudioDevice->id;
      devConfig.capture.format = ma_format_s16;
      devConfig.capture.channels = SC_AUDIO_CHANNELS;
      devConfig.sampleRate = SC_AUDIO_SAMPLE_RATE;
      devConfig.periodSizeInMilliseconds = 10;
      devConfig.dataCallback = _scAudioMixCallback;
      devConfig.pUserData = pDev;

      if (ma_device_init(&gAudio.maCtx, &devConfig, &pDev->device) != MA_SUCCESS) {
        scLogError("Failed to initialize %s audio device %s", bPlayback ? "loopback" : "capture", pAudioDevice->szName);
        continue;
      }
      pDev->bDeviceInitialized = true;

      if (ma_device_start(&pDev->device) != MA_SUCCESS) {
        scLogError("Failed to start %s audio device %s", bPlayback ? "loopback" : "capture", pAudioDevice->szName);
        ma_device_uninit(&pDev->device);
        pDev->bDeviceInitialized = false;
        continue;
      }

      scLogInfo("Audio %s device '%s' started on track %d", bPlayback ? "output/loopback" : "input", pAudioDevice->szName, pSelection->iTrack);
      pCap->abTracksUsed[pDev->iTrack] = true;
      pCap->nDevices++;
    }
  }

  if (pCap->nDevices == 0) {
    scAudioCaptureDestroy(pCap);
    return NULL;
  }

  scLogInfo("Started audio capture across %u devices", pCap->nDevices);
  return pCap;
}

bool scAudioCaptureHasTrack(const scAudioCapture* pCap, uint32_t iTrack) {
  return pCap && iTrack < SC_AUDIO_MAX_TRACKS && pCap->abTracksUsed[iTrack];
}

void scAudioCaptureSetTimeline(scAudioCapture* pCap, LARGE_INTEGER qpcStart, LARGE_INTEGER qpcFrequency) {
  pCap->qpcStart = qpcStart;
  pCap->qpcFrequency = qpcFrequency;
  InterlockedExchange(&pCap->bTimelineSet, 1);
}

uint32_t scAudioCapturePull(scAudioCapture* pCap, uint32_t iTrack, int64_t upToFrame,
                            int16_t* pOut, uint32_t maxFrames, int64_t* pFirstFrame) {
  int64_t first;
  int64_t count;

  if (!pCap || iTrack >= SC_AUDIO_MAX_TRACKS || !pOut || !pFirstFrame) {
    return 0;
  }

  EnterCriticalSection(&pCap->csMix);
  first = pCap->anEmittedFrame[iTrack];
  count = upToFrame - first;
  if (count > (int64_t)maxFrames) {
    count = (int64_t)maxFrames;
  }
  if (count <= 0) {
    LeaveCriticalSection(&pCap->csMix);
    return 0;
  }

  for (int64_t i = 0; i < count; i++) {
    const size_t idx = ((size_t)iTrack * SC_AUDIO_MIX_FRAMES +
                        (size_t)((first + i) % SC_AUDIO_MIX_FRAMES)) * SC_AUDIO_CHANNELS;
    pOut[(size_t)i * SC_AUDIO_CHANNELS + 0] = _scAudioClampS16(pCap->pMix[idx + 0]);
    pOut[(size_t)i * SC_AUDIO_CHANNELS + 1] = _scAudioClampS16(pCap->pMix[idx + 1]);
    pCap->pMix[idx + 0] = 0;
    pCap->pMix[idx + 1] = 0;
  }
  pCap->anEmittedFrame[iTrack] = first + count;
  LeaveCriticalSection(&pCap->csMix);

  *pFirstFrame = first;
  return (uint32_t)count;
}

void scAudioCaptureStop(scAudioCapture* pCap) {
  for (uint32_t i = 0; i < SC_MAX_ACTIVE_RECORDERS; i++) {
    scAudioCaptureDevice* pDev = &pCap->aDevices[i];
    if (pDev->bDeviceInitialized) {
      ma_device_uninit(&pDev->device);
      pDev->bDeviceInitialized = false;
    }
  }
}

void scAudioCaptureDestroy(scAudioCapture* pCap) {
  if (!pCap) {
    return;
  }
  scAudioCaptureStop(pCap);
  if (pCap->bMixLockInitialized) {
    DeleteCriticalSection(&pCap->csMix);
  }
  free(pCap->pMix);
  free(pCap);
  scLogInfo("Stopped audio capture");
}
