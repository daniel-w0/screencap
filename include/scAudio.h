#ifndef SC_AUDIO_H
#define SC_AUDIO_H

#include "miniaudio.h"

#define SC_MAX_ACTIVE_RECORDERS 8

typedef struct {
  char szName[128];
  ma_device_id id;
  bool bIsPlayback;
  bool bIsEnabled;
} scAudioDevice;

typedef struct {
  ma_device device;
  ma_encoder encoder;
  wchar_t wszWavPath[SC_PATH_MAX_LEN];
  bool bIsActive;
} scAudioRecorder;

typedef struct {
  scAudioDevice* pCaptureDevices;
  uint32_t deviceCount;
  ma_context maCtx;
  scAudioRecorder aRecorders[SC_MAX_ACTIVE_RECORDERS];
  uint32_t activeRecorderCount;
} scAudio;

extern scAudio gAudio;

bool scAudioInit();
void scAudioDestroy();
void scAudioDeviceUpdated(u16 uDeviceIdx);

bool scAudioStartRecording(const wchar_t* wszTempDir);
void scAudioStopRecording(wchar_t pOutWavPaths[][SC_PATH_MAX_LEN], uint32_t* pOutCount);

void scAudioTest();

#endif // SC_AUDIO_H