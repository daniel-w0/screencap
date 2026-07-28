#ifndef SC_FFMPEG_HELPER_H
#define SC_FFMPEG_HELPER_H

#include "pch.h"
#include "scTypes.h"

typedef struct {
  HANDLE  hFFmpegProcess;
  HANDLE  hFFmpegStdin;
  wchar_t wszSavePath;
} scFFmpegInstance;

bool scFFmpegStartRecording(scFFmpegInstance* pFFmpeg, wchar_t wszSavePath[MAX_PATH], scRect rect);
bool scFFmpegStopRecording(scFFmpegInstance* pFFmpeg);
bool scFFmpegGetPath(wchar_t* wszOutPath, s32 nPathLen);

#endif // SC_FFMPEG_HELPER_H