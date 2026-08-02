#include "pch.h"
#include "scApp.h"
#include "scLogging.h"
#include "scAssert.h"
#include "scUI.h"
#include "scAudio.h"

typedef struct {
  HANDLE hFFmpegProcess;
  HANDLE hFFmpegStdin;
  HANDLE hFFmpegStderrRead;
  HANDLE hStderrThread;
  wchar_t wszFFmpegPath[SC_PATH_MAX_LEN];
  wchar_t wszSavePath[SC_PATH_MAX_LEN];
  wchar_t wszTempVideoPath[SC_PATH_MAX_LEN];
  wchar_t wszTempDir[SC_PATH_MAX_LEN];
  bool bHasAudio;
  LARGE_INTEGER liVideoStart;
  volatile LONG bVideoStartFound;
} scRecordContext;

scInternal bool
_scProbeRegPath(HKEY hRoot, const wchar_t* wszSubkey, const wchar_t* wszExeName, wchar_t* wszOutPath, s32 nOutCap) {
  HKEY hKey;
  if (RegOpenKeyExW(hRoot, wszSubkey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
    return false;
  }

  bool  bFound = false;
  DWORD dwType = 0;
  DWORD dwSize = 0;

  // Query size first.
  if (RegQueryValueExW(hKey, L"Path", NULL, &dwType, NULL, &dwSize) != ERROR_SUCCESS || dwSize == 0) {
    RegCloseKey(hKey);
    return false;
  }

  wchar_t* wszRaw = (wchar_t*)malloc(dwSize + sizeof(wchar_t)); // +1 wchar for our own terminator
  if (!wszRaw) {
    RegCloseKey(hKey);
    return false;
  }

  if (RegQueryValueExW(hKey, L"Path", NULL, &dwType, (LPBYTE)wszRaw, &dwSize) == ERROR_SUCCESS) {
    wszRaw[dwSize / sizeof(wchar_t)] = L'\0'; // reg strings aren't guaranteed terminated

    // Expand %VARS% if needed.
    wchar_t* wszPath     = wszRaw;
    wchar_t* wszExpanded = NULL;
    if (dwType == REG_EXPAND_SZ) {
      DWORD dwNeed = ExpandEnvironmentStringsW(wszRaw, NULL, 0); // count in CHARS incl. null
      if (dwNeed > 0) {
        wszExpanded = (wchar_t*)malloc((size_t)dwNeed * sizeof(wchar_t));
        if (wszExpanded) {
          ExpandEnvironmentStringsW(wszRaw, wszExpanded, dwNeed);
          wszPath = wszExpanded;
        }
      }
    }

    // Walk the ';'-separated dirs.
    const s32 nLen   = (s32)wcslen(wszPath);
    s32       iStart = 0;
    while (iStart <= nLen && !bFound) {
      const wchar_t* pSemi = wcschr(wszPath + iStart, L';');
      const s32      iEnd  = pSemi ? (s32)(pSemi - wszPath) : nLen;
      const s32      iSeg  = iStart;
      s32            nDir  = iEnd - iStart;
      iStart = iEnd + 1;

      wchar_t wszDir[MAX_PATH];
      if (nDir <= 0 || nDir >= (s32)ARRAYSIZE(wszDir)) {
        continue;
      }
      memcpy(wszDir, wszPath + iSeg, (size_t)nDir * sizeof(wchar_t));
      wszDir[nDir] = L'\0';

      // Strip surrounding quotes..
      if (nDir >= 2 && wszDir[0] == L'"' && wszDir[nDir - 1] == L'"') {
        memmove(wszDir, wszDir + 1, (size_t)(nDir - 2) * sizeof(wchar_t));
        nDir -= 2;
        wszDir[nDir] = L'\0';
      }
      if (nDir <= 0) {
        continue;
      }

      const wchar_t  cLast  = wszDir[nDir - 1];
      const wchar_t* wszSep = (cLast == L'\\' || cLast == L'/') ? L"" : L"\\";

      wchar_t wszCandidate[MAX_PATH];
      if (swprintf(wszCandidate, ARRAYSIZE(wszCandidate), L"%ls%ls%ls", wszDir, wszSep, wszExeName) < 0) {
        continue;
      }

      DWORD dwAttrs = GetFileAttributesW(wszCandidate);
      if (dwAttrs != INVALID_FILE_ATTRIBUTES && !(dwAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        wcsncpy(wszOutPath, wszCandidate, nOutCap - 1);
        wszOutPath[nOutCap - 1] = L'\0';
        bFound = true;
      }
    }

    free(wszExpanded);
  }

  free(wszRaw);
  RegCloseKey(hKey);
  return bFound;
}

scInternal bool
_scFindExecutable(const wchar_t* wszExeName, wchar_t* wszOutPath, s32 nOutCap) {
  wchar_t wszSearchBuf[MAX_PATH];
  DWORD   dwLen = SearchPathW(NULL, wszExeName, NULL, ARRAYSIZE(wszSearchBuf), wszSearchBuf, NULL);
  if (dwLen > 0 && dwLen < ARRAYSIZE(wszSearchBuf)) {
    wcsncpy(wszOutPath, wszSearchBuf, nOutCap - 1);
    wszOutPath[nOutCap - 1] = L'\0';
    return true;
  }

  if (_scProbeRegPath(HKEY_CURRENT_USER, L"Environment", wszExeName, wszOutPath, nOutCap)) {
    return true;
  }
  if (_scProbeRegPath(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", wszExeName, wszOutPath, nOutCap)) {
    return true;
  }

  return false;
}

scInternal DWORD WINAPI
_scFFmpegStderrThread(LPVOID pParam) {
  scRecordContext* pCtx = (scRecordContext*)pParam;
  char buf[256];
  char lineBuf[512];
  size_t lineLen = 0;
  DWORD dwRead;

  while (ReadFile(pCtx->hFFmpegStderrRead, buf, sizeof(buf), &dwRead, NULL) && dwRead > 0) {
    for (DWORD i = 0; i < dwRead; i++) {
      char c = buf[i];
      if (c == '\r' || c == '\n') {
        if (lineLen > 0) {
          lineBuf[lineLen] = '\0';

          if (strstr(lineBuf, "frame=")) {
            const char* pSpeed = strstr(lineBuf, "speed=");
            if (pSpeed) {
              scLogDebug("ffmpeg progress: %s", lineBuf);
            }
          }

          if (!pCtx->bVideoStartFound) {
            const char* pFrame = strstr(lineBuf, "frame=");
            if (pFrame) {
              long lFrameNum = strtol(pFrame + 6, NULL, 10);
              if (lFrameNum > 0) {
                double dEncodedSec = 0.0;
                const char* pTime = strstr(lineBuf, "time=");
                if (pTime) {
                  int h = 0, m = 0;
                  double s = 0.0;
                  if (sscanf(pTime + 5, "%d:%d:%lf", &h, &m, &s) == 3) {
                    dEncodedSec = h * 3600.0 + m * 60.0 + s;
                  }
                }

                LARGE_INTEGER liNow, liFreq;
                QueryPerformanceCounter(&liNow);
                QueryPerformanceFrequency(&liFreq);

                LONGLONG llBackoff = (LONGLONG)(dEncodedSec * (double)liFreq.QuadPart);
                pCtx->liVideoStart.QuadPart = liNow.QuadPart - llBackoff;

                InterlockedExchange(&pCtx->bVideoStartFound, 1);
                scLogInfo("First real ffmpeg frame, corrected start (perf counter: %lld, backoff %.3fs): %s",
                          pCtx->liVideoStart.QuadPart, dEncodedSec, lineBuf);
              }
            }
          }

          lineLen = 0;
        }
      } else if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
    }
  }
  return 0;
}

scInternal void
_scMergeAudioAndVideo(scRecordContext* pCtx, scAudioTrackInfo aTracks[], uint32_t trackCount) {
  wchar_t wszCmd[4096];
  int nOffset = 0;

  LARGE_INTEGER liFreq;
  QueryPerformanceFrequency(&liFreq);

  nOffset += swprintf(wszCmd + nOffset, ARRAYSIZE(wszCmd) - nOffset, L"\"%ls\" -y -i \"%ls\"", pCtx->wszFFmpegPath, pCtx->wszTempVideoPath);

  for (uint32_t i = 0; i < trackCount; i++) {
    double dOffsetSec = (double)(pCtx->liVideoStart.QuadPart - aTracks[i].liStartTime.QuadPart) / (double)liFreq.QuadPart;
    if (dOffsetSec < 0.0) dOffsetSec = 0.0;
    scLogInfo("Track %u offset: %.3f sec", i, dOffsetSec);
    nOffset += swprintf(wszCmd + nOffset, ARRAYSIZE(wszCmd) - nOffset, L" -ss %.3f -i \"%ls\"", dOffsetSec, aTracks[i].wszPath);
  }

  if (trackCount == 1) {
    nOffset += swprintf(wszCmd + nOffset, ARRAYSIZE(wszCmd) - nOffset, L" -avoid_negative_ts make_zero -c:v copy -c:a aac \"%ls\"", pCtx->wszSavePath);
  } else {
    nOffset += swprintf(wszCmd + nOffset, ARRAYSIZE(wszCmd) - nOffset, L" -filter_complex amix=inputs=%u:duration=longest -avoid_negative_ts make_zero -c:v copy -c:a aac \"%ls\"", trackCount, pCtx->wszSavePath);
  }

  STARTUPINFOW si = { sizeof(STARTUPINFOW) };
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = { 0 };

  scLogInfo("Merge command: %ls", wszCmd);

  if (CreateProcessW(NULL, wszCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    scLogError("Failed to merge audio and video with FFmpeg: %lu", GetLastError());
  }

  DeleteFileW(pCtx->wszTempVideoPath);
  for (uint32_t i = 0; i < trackCount; i++) {
    DeleteFileW(aTracks[i].wszPath);
  }
}

scInternal bool
_startRecording(scRecordContext* pCtx, scRect rect) {
  scPlaySoundOrSkip(SC_SOUND_SCREENSHOT_QUICK);

  HANDLE hReadPipe, hWritePipe;
  SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
    scLogError("Failed to start recording, CreatePipe failed: %lu", GetLastError());
    return false;
  }

  SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

  if (rect.x < 0) rect.x = 0;
  if (rect.y < 0) rect.y = 0;
  const s32 w = rect.w & ~1;
  const s32 h = rect.h & ~1;

  if (w <= 0 || h <= 0) {
    scLogError("Failed to start recording, invalid capture size %d x %d", rect.w, rect.h);
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return false;
  }

  wchar_t wszDir[MAX_PATH];
  wchar_t wszName[MAX_PATH];
  if (!scGetSavePath(wszDir, MAX_PATH) || !scGetFilename(wszName, MAX_PATH, ".mp4")) {
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return false;
  }

  GetTempPathW(MAX_PATH, pCtx->wszTempDir);
  swprintf(pCtx->wszSavePath, MAX_PATH, L"%ls\\%ls", wszDir, wszName);

  if (gApp->config.bCaptureAudio) {
    pCtx->bHasAudio = scAudioStartRecording(pCtx->wszTempDir);
  } else {
    pCtx->bHasAudio = false;
  }

  if (pCtx->bHasAudio) {
    swprintf(pCtx->wszTempVideoPath, MAX_PATH, L"%ls\\sc_temp_video.mp4", pCtx->wszTempDir);
  } else {
    wcsncpy(pCtx->wszTempVideoPath, pCtx->wszSavePath, MAX_PATH);
  }

  wchar_t wszCmd[1024];
  if (gApp->bIsGeWin10) {
    swprintf(wszCmd, ARRAYSIZE(wszCmd),
             L"\"%ls\" -y -f gdigrab -framerate %d -offset_x %d -offset_y %d "
             L"-video_size %dx%d -i desktop -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \"%ls\"",
             pCtx->wszFFmpegPath, gApp->config.iFFmpegFramerate, rect.x, rect.y, w, h, pCtx->wszTempVideoPath);
  } else {
    swprintf(wszCmd, ARRAYSIZE(wszCmd),
             L"\"%ls\" -y -f gdigrab -framerate %d -offset_x %d -offset_y %d "
             L"-video_size %dx%d -i desktop -c:v mpeg4 -qscale:v 4 \"%ls\"",
             pCtx->wszFFmpegPath, gApp->config.iFFmpegFramerate, rect.x, rect.y, w, h, pCtx->wszTempVideoPath);
  }

  HANDLE hStderrRead, hStderrWrite;
  SECURITY_ATTRIBUTES saPipe = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  if (!CreatePipe(&hStderrRead, &hStderrWrite, &saPipe, 0)) {
    scLogError("Failed to create stderr pipe: %lu", GetLastError());
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return false;
  }
  SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = { sizeof(STARTUPINFOW) };
  si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput   = hReadPipe;
  si.hStdOutput  = hStderrWrite;
  si.hStdError   = hStderrWrite;

  PROCESS_INFORMATION pi = { 0 };
  bool ok = false;
  if (CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    pCtx->hFFmpegProcess = pi.hProcess;
    pCtx->hFFmpegStdin = hWritePipe;
    pCtx->hFFmpegStderrRead = hStderrRead;
    CloseHandle(pi.hThread);
    CloseHandle(hStderrWrite); // child's dup is enough once it has inherited it

    pCtx->bVideoStartFound = 0;
    pCtx->hStderrThread = CreateThread(NULL, 0, _scFFmpegStderrThread, pCtx, 0, NULL);

    ok = true;
  } else {
    scLogError("Failed to start recording, CreateProcessW failed: %lu", GetLastError());
    CloseHandle(hWritePipe);
    CloseHandle(hStderrWrite);
    CloseHandle(hStderrRead);
  }

  CloseHandle(hReadPipe);

  //if (hOut != INVALID_HANDLE_VALUE) {
  //  CloseHandle(hOut);
  //}

  scLogInfo("Started recording: { %d, %d, %d, %d }", rect.x, rect.y, w, h);
  return ok;
}

scInternal void
_stopRecording(scRecordContext* pCtx) {
  if (pCtx->hFFmpegProcess && pCtx->hFFmpegStdin) {
    DWORD dwCode = STILL_ACTIVE;
    if (GetExitCodeProcess(pCtx->hFFmpegProcess, &dwCode) && dwCode != STILL_ACTIVE) {
      scLogError("Failed to stop recording, it seems ffmpeg has already closed");
    } else {
      DWORD dwWritten = 0;
      if (!WriteFile(pCtx->hFFmpegStdin, "q\n", 2, &dwWritten, NULL)) {
        scLogError("Failed to stop recording, unable to write to stdin: %lu", GetLastError());
      }

      if (WaitForSingleObject(pCtx->hFFmpegProcess, 5000) == WAIT_TIMEOUT) {
        scLogError("Failed to stop recording, WaitForSingleObject failed: %lu", GetLastError());
      }
    }

    CloseHandle(pCtx->hFFmpegProcess);
    CloseHandle(pCtx->hFFmpegStdin);
    pCtx->hFFmpegProcess = 0;
    pCtx->hFFmpegStdin = 0;

    scAudioTrackInfo aTracks[SC_MAX_ACTIVE_RECORDERS];
    uint32_t trackCount = 0;

    if (pCtx->hStderrThread) {
      WaitForSingleObject(pCtx->hStderrThread, 2000);
      CloseHandle(pCtx->hStderrThread);
      pCtx->hStderrThread = NULL;
    }
    if (pCtx->hFFmpegStderrRead) {
      CloseHandle(pCtx->hFFmpegStderrRead);
      pCtx->hFFmpegStderrRead = NULL;
    }

    if (pCtx->bHasAudio) {
      scAudioStopRecording(aTracks, &trackCount);
      if (trackCount > 0) {
        if (!pCtx->bVideoStartFound) {
          scLogWarn("Never saw a frame= line from ffmpeg, falling back to current time for offset calc");
          QueryPerformanceCounter(&pCtx->liVideoStart);
        }
        _scMergeAudioAndVideo(pCtx, aTracks, trackCount);
      }
    }

    scUIOnCaptureSaved(pCtx->wszSavePath);
    scLogInfo("Stopped recording");
  } else {
    scLogError("Failed to stop recording normally");
  }

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

    if (!_scFindExecutable(L"ffmpeg.exe", (wchar_t*)pRecordCtx->wszFFmpegPath, SC_PATH_MAX_LEN)) {
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