#ifndef SC_LOCALE_H
#define SC_LOCALE_H

#include "scTypes.h"

void scLocaleInit();
void scLocaleSet(const char* szCode);

const wchar_t* scLocaleGet(const char* szKey);

s32                scLocaleCount();
const char*        scLocaleCode(s32 iIndex);
const char*        scLocaleCurrent();
const char* const* scLocaleCodes();

#endif // SC_LOCALE_H
