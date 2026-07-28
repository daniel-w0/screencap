#include "pch.h"
#include "scFfmpegHelper.h"
#include "scApp.h"
#include "scLogging.h"

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

bool scFFmpegStartRecording(scFFmpegInstance* pFFmpeg, wchar_t wszSavePath[MAX_PATH], scRect rect) {
  wchar_t wszFFmpegPath[SC_PATH_MAX_LEN];
  if (!_scFindExecutable(L"ffmpeg.exe", (wchar_t*)wszFFmpegPath, SC_PATH_MAX_LEN)) {
    scLogError("Unable to locate ffmpeg.exe");
    return true;
  }

  HANDLE hReadPipe, hWritePipe;
  SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
    scLogError("Failed to start recording, CreatePipe failed: %lu", GetLastError());
    return false;
  }

  SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

  const s32 w = rect.w & ~1;
  const s32 h = rect.h & ~1;

  if (w <= 0 || h <= 0) {
    scLogError("Failed to start recording, invalid capture size %d x %d", rect.w, rect.h);
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return false;
  }

  wchar_t wszCmd[1024];
  if (gApp->bIsGeWin10) {
    swprintf(wszCmd, ARRAYSIZE(wszCmd),
             L"\"%ls\" -y -f gdigrab -framerate %d -offset_x %d -offset_y %d "
             L"-video_size %dx%d -i desktop -c:v libx264 -pix_fmt yuv420p \"%ls\"",
             wszFFmpegPath, gApp->config.iFFmpegFramerate, rect.x, rect.y, w, h, wszSavePath);
  } else {
    swprintf(wszCmd, ARRAYSIZE(wszCmd),
             L"\"%ls\" -y -f gdigrab -framerate %d -offset_x %d -offset_y %d "
             L"-video_size %dx%d -i desktop -c:v mpeg4 -qscale:v 4 \"%ls\"",
             wszFFmpegPath, gApp->config.iFFmpegFramerate, rect.x, rect.y, w, h, wszSavePath);
  }

  HANDLE hCon = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  bool   bHaveConsole = (hCon != INVALID_HANDLE_VALUE);
  HANDLE hOut = bHaveConsole ? hCon : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

  STARTUPINFOW si = { sizeof(STARTUPINFOW) };
  si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput   = hReadPipe;
  si.hStdOutput  = hOut;
  si.hStdError   = hOut;

  PROCESS_INFORMATION pi = { 0 };
  DWORD flags = bHaveConsole ? 0 : CREATE_NO_WINDOW;

  bool ok = false;
  if (CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi)) {
    pFFmpeg->hFFmpegProcess = pi.hProcess;
    pFFmpeg->hFFmpegStdin   = hWritePipe;
    CloseHandle(pi.hThread);
    ok = true;
  } else {
    scLogError("Failed to start recording, CreateProcessW failed: %lu", GetLastError());
    CloseHandle(hWritePipe);
  }

  CloseHandle(hReadPipe);
  if (hOut != INVALID_HANDLE_VALUE) {
    CloseHandle(hOut);
  }

  scLogInfo("Started recording: { %d, %d, %d, %d }", rect.x, rect.y, w, h);
  return ok;
}

bool scFFmpegStopRecording(scFFmpegInstance* pFFmpeg) {
  if (pFFmpeg->hFFmpegProcess && pFFmpeg->hFFmpegStdin) {
    DWORD dwCode = STILL_ACTIVE;
    if (GetExitCodeProcess(pFFmpeg->hFFmpegProcess, &dwCode) && dwCode != STILL_ACTIVE) {
      scLogError("Failed to stop recording, it seems ffmpeg has already closed");
    } else {
      DWORD dwWritten = 0;
      if (!WriteFile(pFFmpeg->hFFmpegStdin, "q\n", 2, &dwWritten, NULL)) {
        scLogError("Failed to stop recording, unable to write to stdin: %lu", GetLastError());
      }

      if (WaitForSingleObject(pFFmpeg->hFFmpegProcess, 5000) == WAIT_TIMEOUT) {
        scLogError("Failed to stop recording, WaitForSingleObject failed: %lu", GetLastError());
      }
    }

    CloseHandle(pFFmpeg->hFFmpegProcess);
    CloseHandle(pFFmpeg->hFFmpegStdin);
    pFFmpeg->hFFmpegProcess = 0;
    pFFmpeg->hFFmpegStdin   = 0;
    scLogInfo("Stopped recording");
  } else {
    scLogError("Failed to stop recording normally??????? I have no idea what to do here. Sorry. Maybe force close screencap or any ffmpeg.exe instances");
  }

  return true;
}

bool scFFmpegGetPath(wchar_t* wszOutPath, s32 nPathLen) {
  return _scFindExecutable(L"ffmpeg.exe", wszOutPath, nPathLen);
}