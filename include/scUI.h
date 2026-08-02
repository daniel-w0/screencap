#ifndef SC_UI_H
#define SC_UI_H

#include "scTypes.h"

#define IDI_APP_ICON 101

typedef enum {
  SC_PAGE_GENERAL,
  SC_PAGE_SETTINGS,
  SC_PAGE_GALLERY,
  SC_PAGE_LOGS,
  _SC_PAGE_COUNT,
  SC_PAGE_NONE
} scPageID;

void scUIOpenWindow();
void scUICloseWindow();
void scUISetCurrentPage(scPageID ePageID);
void scUIOnCaptureSaved(const wchar_t* wszPath);
COLORREF scGetSystemAccentColor();
bool scIsWindowsDarkTheme();
COLORREF scBlendColor(COLORREF a, COLORREF b, f32 f);

#endif // SC_UI_H