#ifndef SC_AUDIO_H
#define SC_AUDIO_H

#include "miniaudio.h"

typedef struct {
  char szName[128];
  ma_device_id id;
  bool bIsPlayback;
  bool bIsEnabled;
} scAudioDevice;

typedef struct {
  scAudioDevice* pCaptureDevices;
  u32 deviceCount;
  ma_context maCtx;
} scAudio;

extern scAudio gAudio;

bool scAudioInit();
void scAudioDestroy();
void scAudioDeviceUpdated(u16 uDeviceIdx);

void scAudioTest();

#endif // SC_AUDIO_H