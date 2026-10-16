#ifndef SC_TIMER_H
#define SC_TIMER_H

#include "scTypes.h"

typedef struct {
  s64  lFrequency;
  s64  lStartTime;
  s64  lElapsed;
  bool bRunning;
} scTimer;

void scTimerInit(scTimer* pTimer);
void scTimerStart(scTimer* pTimer);
void scTimerStop(scTimer* pTimer);
void scTimerReset(scTimer* pTimer);
f64  scTimerElapsedSeconds(const scTimer* pTimer);
f64  scTimerElapsedMS(const scTimer* pTimer);
s64  scTimerElapsedNS(const scTimer* pTimer);

#endif // SC_TIMER_H