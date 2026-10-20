#ifndef SC_CONFIG_H
#define SC_CONFIG_H

#include "scTypes.h"

bool scConfigLoad(void);
void scSaveConfig(void);
void scWriteWindowConfig(int iX, int iY, int iWidth, int iHeight);
bool scReadWindowConfig(int* iX, int* iY, int* iWidth, int* iHeight);
void scSetRunOnStartup(bool bEnable, s32 _);

#endif // SC_CONFIG_H
