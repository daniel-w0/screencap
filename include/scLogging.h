#ifndef SC_LOGGING_H
#define SC_LOGGING_H

#include "scTypes.h"

typedef enum scLogLevel {
  SC_LOG_DEBUG = 0,
  SC_LOG_INFO,
  SC_LOG_WARN,
  SC_LOG_ERROR
} scLogLevel;

#define SC_LOG_STORE_MAX 128
#define SC_LOG_STORE_LEN 1024

typedef void (*scLogSink)(void);

void scLogInit();
void scLog(scLogLevel eLogLevel, const char* sFormat, ...);

void scLogSetSink(scLogSink pfnSink);

s32         scLogStoreCount();
const char* scLogStoreGet(s32 iIndex, scLogLevel* peLevel);

#define scLogDebug(...) scLog(SC_LOG_DEBUG, __VA_ARGS__)
#define scLogInfo(...) scLog(SC_LOG_INFO,  __VA_ARGS__)
#define scLogWarn(...) scLog(SC_LOG_WARN,  __VA_ARGS__)
#define scLogError(...) scLog(SC_LOG_ERROR, __VA_ARGS__)

#endif // SC_LOGGING_H