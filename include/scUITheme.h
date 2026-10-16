#ifndef SC_UI_THEME_H
#define SC_UI_THEME_H

#include "pch.h"
#include "scTypes.h"

typedef struct {
  bool bDark;
  HFONT pFont;
  HFONT pBoldFont;

  COLORREF dwBackground;
  COLORREF dwSidebar;
  COLORREF dwCard;
  COLORREF dwCardHover;
  COLORREF dwCardActive;
  COLORREF dwPopup;
  COLORREF dwStroke;
  COLORREF dwStrokeSoft;
  COLORREF dwText;
  COLORREF dwTextDim;
  COLORREF dwTextFaint;
  COLORREF dwAccent;
  COLORREF dwAccentHover;
  COLORREF dwPillOff;
  COLORREF dwOk;
  COLORREF dwWarn;
  COLORREF dwError;
  COLORREF dwScrollThumb;

  HBRUSH hBackgroundBrush;
  HBRUSH hCardBrush;
} scUITheme;

void scUIThemeGet(scUITheme* pTheme, s32 iFontScale);

f32 _scGetDPIScale();
s32 _scMix(s32 x, s32 y, f32 f);
COLORREF _scBlendColor(COLORREF a, COLORREF b, f32 f);
bool _scColorIsLight(COLORREF c);
bool _scIsWindowsDarkTheme();
COLORREF _scGetSystemAccentColor();

#endif // SC_UI_THEME_H