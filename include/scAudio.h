#ifndef SC_AUDIO_H
#define SC_AUDIO_H

#include "miniaudio.h"

#define SC_MAX_ACTIVE_RECORDERS 8
#define SC_AUDIO_MAX_TRACKS SC_MAX_ACTIVE_RECORDERS
#define SC_AUDIO_MAX_DEVICE_SELECTIONS SC_MAX_ACTIVE_RECORDERS

#define SC_AUDIO_SAMPLE_RATE 48000
#define SC_AUDIO_CHANNELS 2
#define SC_AUDIO_MIX_FRAMES (SC_AUDIO_SAMPLE_RATE * 4)
#define SC_AUDIO_RESYNC_FRAMES (SC_AUDIO_SAMPLE_RATE / 25)
#define SC_AUDIO_MARGIN_FRAMES (SC_AUDIO_SAMPLE_RATE / 6)

typedef struct {
  char szName[128];
  ma_device_id id;
} scAudioDevice;

typedef struct {
  int32_t iDevice;
  int32_t iTrack;
} scAudioSelection;

typedef struct {
  scAudioDevice* pCaptureDevices;
  uint32_t captureDeviceCount;
  scAudioDevice* pPlaybackDevices;
  uint32_t playbackDeviceCount;

  scAudioSelection aInputs[SC_AUDIO_MAX_DEVICE_SELECTIONS];
  uint32_t inputCount;
  scAudioSelection aOutputs[SC_AUDIO_MAX_DEVICE_SELECTIONS];
  uint32_t outputCount;

  ma_context maCtx;
} scAudio;

typedef struct scAudioCapture scAudioCapture;

extern scAudio gAudio;

bool scAudioInit();
void scAudioDestroy();
bool scAudioInputSelectionAdd(void);
void scAudioInputSelectionRemove(uint32_t iSelection);
void scAudioInputSelectionSetDevice(uint32_t iSelection, int32_t iDevice);
void scAudioInputSelectionSetTrack(uint32_t iSelection, int32_t iTrack);

bool scAudioOutputSelectionAdd(void);
void scAudioOutputSelectionRemove(uint32_t iSelection);
void scAudioOutputSelectionSetDevice(uint32_t iSelection, int32_t iDevice);
void scAudioOutputSelectionSetTrack(uint32_t iSelection, int32_t iTrack);

/* Legacy entry points retained for callers that use the input selection list. */
bool scAudioSelectionAdd(void);
void scAudioSelectionRemove(uint32_t iSelection);
void scAudioSelectionSetDevice(uint32_t iSelection, int32_t iDevice);
void scAudioSelectionSetTrack(uint32_t iSelection, int32_t iTrack);

scAudioCapture* scAudioCaptureStart(void);
bool scAudioCaptureHasTrack(const scAudioCapture* pCap, uint32_t iTrack);
void scAudioCaptureSetTimeline(scAudioCapture* pCap, LARGE_INTEGER qpcStart, LARGE_INTEGER qpcFrequency);
int64_t scAudioCaptureQpcToFrame(const scAudioCapture* pCap, int64_t qpc);
uint32_t scAudioCapturePull(scAudioCapture* pCap, uint32_t iTrack, int64_t upToFrame,
                            int16_t* pOut, uint32_t maxFrames, int64_t* pFirstFrame);
void scAudioCaptureStop(scAudioCapture* pCap);
void scAudioCaptureDestroy(scAudioCapture* pCap);

void scAudioGetSelectionsString(char* szOut, size_t nOutCap);
void scAudioSetSelectionsFromString(const char* szSelections);

void scAudioTest();

#endif // SC_AUDIO_H
