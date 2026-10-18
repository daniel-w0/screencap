#ifndef SC_GUI_H
#define SC_GUI_H

#include <windows.h>
#include "scTypes.h"

typedef struct scGui  scGui;
typedef struct scNode scNode;

typedef enum {
  SC_LENGTH_AUTO,
  SC_LENGTH_PIXELS,
  SC_LENGTH_PERCENT,
  SC_LENGTH_STRETCH
} scLengthType;

typedef struct {
  scLengthType eType;
  f32          fValue;
} scLength;

typedef enum {
  SC_ALIGN_START,
  SC_ALIGN_MIDDLE,
  SC_ALIGN_END
} scAlign;

typedef enum {
  SC_TEXT_NORMAL,
  SC_TEXT_DIM,
  SC_TEXT_FAINT
} scTextRole;

typedef struct {
  COLORREF dwText;
  COLORREF dwTextDim;
  COLORREF dwTextFaint;
  COLORREF dwBackground;
  COLORREF dwPanel;
  COLORREF dwPopup;
  COLORREF dwCard;
  COLORREF dwCardHover;
  COLORREF dwCardActive;
  COLORREF dwStroke;
  COLORREF dwStrokeSoft;
  COLORREF dwAccent;
  COLORREF dwAccentHover;
  COLORREF dwAccentText;
  COLORREF dwPillOff;
  COLORREF dwScrollThumb;
  HFONT    hFont;
  HFONT    hBoldFont;
} scGuiTheme;

typedef struct {
  void* pUser;
  void (*pfnInvalidate)(void* pUser, const RECT* pRect);
  void (*pfnCapture)(void* pUser, bool bCapture);
  void (*pfnValueChanged)(void* pUser);
} scGuiHost;

typedef struct {
  void (*pfnDraw)(scNode* pNode, HDC hDC, RECT rcNode, RECT rcVisible, bool bHovered);
  void (*pfnClick)(scNode* pNode);
  void (*pfnDrag)(scNode* pNode);
  void (*pfnContext)(scNode* pNode, POINT ptClient);
  s32  (*pfnMeasure)(scNode* pNode, HDC hMeasureDC, s32 iWidth);
} scCustomOps;

scLength scLengthAuto();
scLength scLengthPixels(f32 fPixels);
scLength scLengthPercent(f32 fFraction);
scLength scLengthStretch(f32 fWeight);

scNode* scCreateVStack(scLength width, scLength height);
scNode* scCreateHStack(scLength width, scLength height);
scNode* scCreateSpacer(scLength width, scLength height);
scNode* scCreateDivider();
scNode* scCreateLabel(const wchar_t* wszText, bool bBold);
scNode* scCreateField(const wchar_t* wszText);
scNode* scCreateButton(const wchar_t* wszText, void (*pfnClick)(s32 iParam), s32 iParam);
scNode* scCreateLink(const wchar_t* wszText, void (*pfnClick)(s32 iParam), s32 iParam);
scNode* scCreateTab(const wchar_t* wszText, bool bActive, void (*pfnClick)(s32 iParam), s32 iParam);
scNode* scCreateToggle(const wchar_t* wszText, bool* pValue, s32 iParam, void (*pfnChange)(bool bValue, s32 iParam));
scNode* scCreateDropdown(const wchar_t* wszLabel, const char* const* aOptions, s32 nOptionCount, s32 iSelected, void (*pfnSelect)(s32 iIndex, s32 iParam), s32 iParam);
scNode* scCreateCustom(scLength width, scLength height, const scCustomOps* pOps, const void* pUser, s32 nUserSize);

void  scAddChild(scNode* pParent, scNode* pChild);
void* scGetUserData(scNode* pNode);

void scSetSize(scNode* pNode, scLength width, scLength height);
void scSetPadding(scNode* pNode, s32 iPadding);
void scSetPaddingEx(scNode* pNode, s32 iLeft, s32 iTop, s32 iRight, s32 iBottom);
void scSetSpacing(scNode* pNode, s32 iSpacing);
void scSetAlignment(scNode* pNode, scAlign eAlign);
void scSetWrap(scNode* pNode, bool bWrap);
void scSetScrollable(scNode* pNode, bool bScrollable);
void scSetStickBottom(scNode* pNode, bool bStickBottom);
void scSetSurface(scNode* pNode, bool bSurface);
void scSetText(scNode* pNode, const wchar_t* wszText);
void scSetTextRole(scNode* pNode, scTextRole eRole);
void scSetEllipsis(scNode* pNode, bool bEllipsis);
void scSetCard(scNode* pNode, f32 fRadius);
void scSetOutline(scNode* pNode, f32 fRadius);
void scSetScrollY(scNode* pNode, s32 iScrollY);
s32  scGetScrollY(scNode* pNode);

scGui* scGuiCreate(const scGuiHost* pHost);
void   scGuiDestroy(scGui* pGui);
void   scGuiSetTheme(scGui* pGui, const scGuiTheme* pTheme);
void   scGuiSetScale(scGui* pGui, f32 fScale);
void   scGuiSetRoot(scGui* pGui, scNode* pRoot);
void   scGuiResize(scGui* pGui, s32 iWidth, s32 iHeight);
void   scGuiInvalidate(scGui* pGui);
void   scGuiInvalidateLayout(scGui* pGui);
void   scGuiPaint(scGui* pGui, HDC hDC, const RECT* pClip);

void scGuiMouseMove(scGui* pGui, POINT pt);
void scGuiMouseLeave(scGui* pGui);
void scGuiMouseDown(scGui* pGui, POINT pt);
void scGuiMouseUp(scGui* pGui, POINT pt);
void scGuiMouseRightUp(scGui* pGui, POINT pt);
void scGuiMouseWheel(scGui* pGui, POINT pt, s32 iNotches);
bool scGuiHitClickable(scGui* pGui, POINT pt);
void scGuiCaptureLost(scGui* pGui);

#endif // SC_GUI_H
