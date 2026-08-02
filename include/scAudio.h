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
  LARGE_INTEGER liStartTime;
} scAudioRecorder;

typedef struct {
  scAudioDevice* pCaptureDevices;
  uint32_t deviceCount;
  ma_context maCtx;
  scAudioRecorder aRecorders[SC_MAX_ACTIVE_RECORDERS];
  uint32_t activeRecorderCount;
} scAudio;

typedef struct {
  wchar_t wszPath[SC_PATH_MAX_LEN];
  LARGE_INTEGER liStartTime;
} scAudioTrackInfo;

extern scAudio gAudio;

bool scAudioInit();
void scAudioDestroy();
void scAudioDeviceUpdated(u16 uDeviceIdx);

bool scAudioStartRecording(const wchar_t* wszTempDir);
void scAudioStopRecording(scAudioTrackInfo pOutTracks[], uint32_t* pOutCount);

void scAudioGetEnabledDevicesString(char* szOut, size_t nOutCap);
void scAudioSetEnabledDevicesFromString(const char* szDevices);

void scAudioTest();

#endif // SC_AUDIO_H