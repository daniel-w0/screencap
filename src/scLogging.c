#include "pch.h"
#include "scLogging.h"

static const char* gLevelNames[] = {
  "Debug", "Info", "Warn", "Error"
};

typedef struct {
  scLogLevel eLevel;
  char       szText[SC_LOG_STORE_LEN];
} scLogEntry;

static scLogEntry*      gStore = NULL;
static s32              gStoreHead = 0;
static s32              gStoreCount = 0;
static CRITICAL_SECTION gStoreLock;
static scLogSink        gSink = NULL;

void scLogInit() {
  if (gStore) {
    return;
  }
  InitializeCriticalSection(&gStoreLock);
  gStore = (scLogEntry*)malloc(SC_LOG_STORE_MAX * sizeof(scLogEntry));
  gStoreHead  = 0;
  gStoreCount = 0;
}

void scLogSetSink(scLogSink pfnSink) {
  gSink = pfnSink;
}

void scLog(scLogLevel eLogLevel, const char* sFormat, ...) {
  time_t now = time(NULL);
  struct tm timeInfo;
  localtime_s(&timeInfo, &now);

  char timeBuffer[12];
  strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &timeInfo);

  char messageBuffer[1024];
  va_list args;
  va_start(args, sFormat);
  vsnprintf(messageBuffer, sizeof(messageBuffer), sFormat, args);
  va_end(args);

  char finalLogBuffer[2048];
  snprintf(finalLogBuffer, sizeof(finalLogBuffer), "[%s] %s: %s\n", gLevelNames[eLogLevel], timeBuffer, messageBuffer);

  fprintf(eLogLevel == SC_LOG_ERROR ? stderr : stdout, "%s", finalLogBuffer);
  OutputDebugStringA(finalLogBuffer);

  if (gStore) {
    EnterCriticalSection(&gStoreLock);
    scLogEntry* pEntry = &gStore[gStoreHead];
    pEntry->eLevel = eLogLevel;
    snprintf(pEntry->szText, SC_LOG_STORE_LEN, "%s  %s", timeBuffer, messageBuffer);
    gStoreHead = (gStoreHead + 1) % SC_LOG_STORE_MAX;
    if (gStoreCount < SC_LOG_STORE_MAX) {
      ++gStoreCount;
    }
    LeaveCriticalSection(&gStoreLock);
  }

  if (gSink) {
    gSink();
  }
}

s32 scLogStoreCount() {
  return gStoreCount;
}

const char* scLogStoreGet(s32 iIndex, scLogLevel* peLevel) {
  if (!gStore || iIndex < 0 || iIndex >= gStoreCount) {
    return NULL;
  }
  s32 iOldest = (gStoreCount < SC_LOG_STORE_MAX) ? 0 : gStoreHead;
  s32 iSlot   = (iOldest + iIndex) % SC_LOG_STORE_MAX;
  if (peLevel) {
    *peLevel = gStore[iSlot].eLevel;
  }
  return gStore[iSlot].szText;
}
