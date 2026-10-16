#include "pch.h"
#include "scTimer.h"

scInternal s64 _scTimerNow() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return li.QuadPart;
}

void scTimerInit(scTimer* pTimer) {
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  pTimer->lFrequency = freq.QuadPart;
  scTimerReset(pTimer);
}

void scTimerStart(scTimer* pTimer) {
  if (!pTimer->bRunning) {
    pTimer->lStartTime = _scTimerNow();
    pTimer->bRunning   = true;
  }
}

void scTimerStop(scTimer* pTimer) {
  if (pTimer->bRunning) {
    pTimer->lElapsed += _scTimerNow() - pTimer->lStartTime;
    pTimer->bRunning = false;
  }
}

void scTimerReset(scTimer* pTimer) {
  pTimer->lStartTime = 0;
  pTimer->lElapsed = 0;
  pTimer->bRunning = false;
}

s64 scTimerElapsedNS(const scTimer* pTimer) {
  s64 nTicks = pTimer->lElapsed;
  if (pTimer->bRunning) {
    nTicks += _scTimerNow() - pTimer->lStartTime;
  }

  s64 nSeconds   = nTicks / pTimer->lFrequency;
  s64 nRemainder = nTicks % pTimer->lFrequency;

  return nSeconds * 1000000000LL + (nRemainder * 1000000000LL) / pTimer->lFrequency;
}

f64 scTimerElapsedMS(const scTimer* pTimer) {
  return (f64)scTimerElapsedNS(pTimer) / 1000000.0;
}

f64 scTimerElapsedSeconds(const scTimer* pTimer) {
  return (f64)scTimerElapsedNS(pTimer) / 1000000000.0;
}