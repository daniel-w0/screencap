#include "pch.h"
#include "scLogging.h"

static const char* gLevelNames[] = {
  "Debug", "Info", "Warn", "Error"
};

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
}