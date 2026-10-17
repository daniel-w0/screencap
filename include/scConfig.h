#ifndef SC_CONFIG_H
#define SC_CONFIG_H

#include "scTypes.h"

bool scConfigLoad(void);
void scSaveConfig(void);
void scSetRunOnStartup(bool bEnable, s32 _);

#endif // SC_CONFIG_H
