#include "pch.h"
#include "scTypes.h"
#include "scGdiPlus.h"
#include "scGui.h"
#include "scApp.h"
#include "scLogging.h"

#include <math.h>

//------------------------------------------------------------------------
// Types
//------------------------------------------------------------------------
typedef enum {
  SC_NODE_VSTACK,
  SC_NODE_HSTACK,
  SC_NODE_SPACER,
  SC_NODE_DIVIDER,
  SC_NODE_LABEL,
  SC_NODE_FIELD,
  SC_NODE_BUTTON,
  SC_NODE_LINK,
  SC_NODE_TAB,
  SC_NODE_TOGGLE,
  SC_NODE_SLIDER,
  SC_NODE_DROPDOWN,
  SC_NODE_CUSTOM
} scNodeType;

struct scNode {
  scNodeType eType;
  scNode*    pParent;
  scNode*    pFirst;
  scNode*    pLast;
  scNode*    pNext;
  scGui*     pGui;

  scLength width;
  scLength height;
  s32      aPadding[4];
  s32      iSpacing;
  scAlign  eAlign;
  bool     bWrap;
  bool     bScrollable;
  bool     bStickBottom;
  bool     bSurface;
  bool     bEllipsis;
  f32      fCardRadius;
  f32      fOutlineRadius;
  scTextRole eTextRole;
  struct {
    COLORREF dwBackground;
    COLORREF dwHoverBackground;
    COLORREF dwActiveBackground;
    COLORREF dwText;
    COLORREF dwHoverText;
    COLORREF dwBorder;
    COLORREF dwHoverBorder;
    scFontRole eFontRole;
    f32 fCornerRadius;
    f32 fBorderWidth;
  } style;

  RECT rc;
  s32  aSize[2];
  s32  iScrollY;
  s32  iMaxScroll;
  s32  iContentH;

  wchar_t* wszText;
  void*    pUser;
  union {
    struct { bool bBold; } label;
    struct { void (*pfnClick)(s32 iParam); s32 iParam; bool bActive; } button;
    struct { bool* pValue; s32 iParam; void (*pfnChange)(bool bValue, s32 iParam); } toggle;
    struct { f32* pValue; f32 fMin; f32 fMax; f32 fStep; s32 iParam; void (*pfnChange)(f32 fValue, s32 iParam); } slider;
    struct {
      const char* const* aOptions;
      s32                nOptionCount;
      s32                iSelected;
      s32                iParam;
      void (*pfnSelect)(s32 iIndex, s32 iParam);
    } dropdown;
    scCustomOps custom;
  } u;
};

typedef struct {
  bool bVisible;
  RECT rcTrack;
  RECT rcThumb;
  s32  iTrackRange;
} scScrollGeom;

typedef struct {
  RECT         rcBox;
  s32          iItemH;
  s32          nVisible;
  bool         bHasScroll;
  s32          iMaxScroll;
  scScrollGeom scroll;
} scPopupGeom;

struct scGui {
  scGuiHost  host;
  scGuiTheme theme;
  f32        fScale;
  s32        iWidth;
  s32        iHeight;
  scNode*    pRoot;
  bool       bLayoutDirty;
  HDC        hMeasureDC;

  scNode* pHover;
  scNode* pPressed;
  POINT   ptPress;
  scNode* pHotScroll;

  scNode*  pSliding;
  scNode*  pEditing;
  wchar_t  wszEdit[32];
  s32      nEdit;
  bool     bEditFresh;
  scNode*  pLastClick;
  DWORD    dwLastClickTime;
  POINT    ptLastClick;

  scNode* pExpanded;
  s32     iPopupHover;
  s32     iPopupScroll;
  bool    bPopupScrollHot;

  struct {
    bool bActive;
    s32* pValue;
    s32  iStartY;
    s32  iStartScroll;
    s32  iTrackRange;
    s32  iMaxScroll;
  } drag;
};

scInternal void _scFlow(scGui* pGui, scNode* pNode, s32 iW, s32 iH, s32 iX, s32 iY, bool bPlace, s32* pUsedW, s32* pUsedH);
scInternal void _scPlace(scGui* pGui, scNode* pNode, s32 iX, s32 iY, s32 iW, s32 iH);

//------------------------------------------------------------------------
// Utility
//------------------------------------------------------------------------
scInternal s32 _scPx(scGui* pGui, f32 fValue) { return (s32)(fValue * pGui->fScale); }
scInternal s32 _scMax(s32 a, s32 b) { return a > b ? a : b; }
scInternal s32 _scMin(s32 a, s32 b) { return a < b ? a : b; }
scInternal s32 _scClamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
scInternal f32 _scRoundRad(scGui* pGui) { return 6.0f * pGui->fScale; }

scInternal bool
_scIsContainer(scNode* pNode) {
  return pNode->eType == SC_NODE_VSTACK || pNode->eType == SC_NODE_HSTACK;
}

scInternal bool
_scIsClickable(scNode* pNode) {
  switch (pNode->eType) {
    case SC_NODE_BUTTON:
    case SC_NODE_LINK:
    case SC_NODE_TAB:
    case SC_NODE_TOGGLE:
    case SC_NODE_SLIDER:
    case SC_NODE_DROPDOWN:
      return true;
    case SC_NODE_CUSTOM:
      return pNode->u.custom.pfnClick != NULL;
    default:
      return false;
  }
}

scInternal bool
_scIsInteractive(scNode* pNode) {
  switch (pNode->eType) {
    case SC_NODE_BUTTON:
    case SC_NODE_LINK:
    case SC_NODE_TAB:
    case SC_NODE_TOGGLE:
    case SC_NODE_SLIDER:
    case SC_NODE_DROPDOWN:
      return true;
    case SC_NODE_CUSTOM:
      return pNode->u.custom.pfnClick || pNode->u.custom.pfnDrag || pNode->u.custom.pfnContext;
    default:
      return false;
  }
}

scInternal RECT
_scScreenRect(scNode* pNode) {
  RECT r = pNode->rc;
  for (scNode* p = pNode->pParent; p; p = p->pParent) {
    if (p->bScrollable) {
      OffsetRect(&r, 0, -p->iScrollY);
    }
  }
  return r;
}

scInternal void
_scInvalidate(scGui* pGui, const RECT* pRect) {
  if (pGui->host.pfnInvalidate) {
    pGui->host.pfnInvalidate(pGui->host.pUser, pRect);
  }
}

scInternal void
_scInvalidateNode(scGui* pGui, scNode* pNode) {
  if (!pNode) {
    return;
  }
  RECT r = _scScreenRect(pNode);
  InflateRect(&r, 2, 2);
  _scInvalidate(pGui, &r);
}

scInternal void
_scCapture(scGui* pGui, bool bCapture) {
  if (pGui->host.pfnCapture) {
    pGui->host.pfnCapture(pGui->host.pUser, bCapture);
  }
}

scInternal void
_scMarkDirty(scNode* pNode, bool bLayout) {
  scNode* pTop = pNode;
  while (pTop->pParent) {
    pTop = pTop->pParent;
  }
  scGui* pGui = pTop->pGui;
  if (!pGui) {
    return;
  }
  if (bLayout) {
    pGui->bLayoutDirty = true;
    _scInvalidate(pGui, NULL);
  } else {
    _scInvalidateNode(pGui, pNode);
  }
}

void
scGuiThemeDestroy(scGuiTheme* pTheme) {
  if (!pTheme) return;
  if (pTheme->hFont) DeleteObject(pTheme->hFont);
  if (pTheme->hSmallFont) DeleteObject(pTheme->hSmallFont);
  if (pTheme->hBoldFont) DeleteObject(pTheme->hBoldFont);
  if (pTheme->hHeadingFont) DeleteObject(pTheme->hHeadingFont);
  if (pTheme->hMonoFont) DeleteObject(pTheme->hMonoFont);
  if (pTheme->hPreviewFont) DeleteObject(pTheme->hPreviewFont);
  if (pTheme->hBackgroundBrush) DeleteObject(pTheme->hBackgroundBrush);
  if (pTheme->hCardBrush) DeleteObject(pTheme->hCardBrush);
  memset(pTheme, 0, sizeof(*pTheme));
}

COLORREF
scBlendColor(COLORREF a, COLORREF b, f32 f) {
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  return RGB(
    (s32)(GetRValue(a) + ((s32)GetRValue(b) - (s32)GetRValue(a)) * f + 0.5f),
    (s32)(GetGValue(a) + ((s32)GetGValue(b) - (s32)GetGValue(a)) * f + 0.5f),
    (s32)(GetBValue(a) + ((s32)GetBValue(b) - (s32)GetBValue(a)) * f + 0.5f)
  );
}

bool
scColorIsLight(COLORREF c) {
  return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000 > 150;
}

bool
scIsWindowsDarkTheme(void) {
  if (gApp && gApp->bIsGeWin10) {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
      return value == 0;
    }
  }
  return false;
}

COLORREF
scGetSystemAccentColor(void) {
  const COLORREF defaultAccent = RGB(0, 120, 215);
  if (gApp && gApp->bIsGeWin10) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
      return RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
    }
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor",
                     RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
      return RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    }
  }
  return defaultAccent;
}

f32
scGetDPIScale(HWND hWnd) {
  HDC hDC = GetDC(hWnd);
  if (!hDC) return 1.0f;
  s32 dpi = GetDeviceCaps(hDC, LOGPIXELSY);
  ReleaseDC(hWnd, hDC);
  return dpi > 0 ? dpi / 96.0f : 1.0f;
}

void
scGuiThemeInit(scGuiTheme* pTheme, s32 iFontSize) {
  if (!pTheme) return;
  scGuiThemeDestroy(pTheme);

  if (iFontSize < 1) iFontSize = 1;
  s32 smallFontSize = (s32)(iFontSize * 0.86f + 0.5f);
  s32 headingFontSize = (s32)(iFontSize * 1.25f + 0.5f);
  s32 previewFontSize = (s32)(iFontSize * 1.5f + 0.5f);
  if (smallFontSize < 1) smallFontSize = 1;
  if (headingFontSize < 1) headingFontSize = 1;
  if (previewFontSize < 1) previewFontSize = 1;

  pTheme->hFont = CreateFontW(iFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  pTheme->hSmallFont = CreateFontW(smallFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  pTheme->hBoldFont = CreateFontW(iFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  pTheme->hHeadingFont = CreateFontW(headingFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  pTheme->hMonoFont = CreateFontW(iFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  FIXED_PITCH | FF_MODERN, L"Consolas");
  pTheme->hPreviewFont = CreateFontW(previewFontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     FIXED_PITCH | FF_MODERN, L"Consolas");

  pTheme->bDark = scIsWindowsDarkTheme();
  pTheme->dwAccent = scGetSystemAccentColor();
  if (pTheme->bDark) {
    pTheme->dwBackground = RGB(28, 28, 28);
    pTheme->dwPanel = RGB(35, 35, 35);
    pTheme->dwCard = RGB(43, 43, 43);
    pTheme->dwCardHover = RGB(52, 52, 52);
    pTheme->dwCardActive = RGB(60, 60, 60);
    pTheme->dwPopup = RGB(45, 45, 45);
    pTheme->dwStroke = RGB(70, 70, 70);
    pTheme->dwStrokeSoft = RGB(50, 50, 50);
    pTheme->dwText = RGB(243, 243, 243);
    pTheme->dwTextDim = RGB(176, 176, 176);
    pTheme->dwTextFaint = RGB(122, 122, 122);
    pTheme->dwPillOff = RGB(158, 158, 158);
    pTheme->dwOk = RGB(108, 203, 95);
    pTheme->dwWarn = RGB(230, 180, 80);
    pTheme->dwError = RGB(255, 99, 97);
    pTheme->dwScrollThumb = RGB(170, 170, 170);
    pTheme->dwAccentHover = scBlendColor(pTheme->dwAccent, RGB(255, 255, 255), 0.15f);
  } else {
    pTheme->dwBackground = RGB(243, 243, 243);
    pTheme->dwPanel = RGB(236, 236, 236);
    pTheme->dwCard = RGB(251, 251, 251);
    pTheme->dwCardHover = RGB(245, 245, 245);
    pTheme->dwCardActive = RGB(238, 238, 238);
    pTheme->dwPopup = RGB(252, 252, 252);
    pTheme->dwStroke = RGB(208, 208, 208);
    pTheme->dwStrokeSoft = RGB(224, 224, 224);
    pTheme->dwText = RGB(27, 27, 27);
    pTheme->dwTextDim = RGB(96, 96, 96);
    pTheme->dwTextFaint = RGB(142, 142, 142);
    pTheme->dwPillOff = RGB(134, 134, 134);
    pTheme->dwOk = RGB(15, 123, 15);
    pTheme->dwWarn = RGB(176, 124, 0);
    pTheme->dwError = RGB(196, 43, 28);
    pTheme->dwScrollThumb = RGB(120, 120, 120);
    pTheme->dwAccentHover = scBlendColor(pTheme->dwAccent, RGB(0, 0, 0), 0.15f);
  }

  pTheme->dwAccentText = scColorIsLight(pTheme->dwAccent) ? RGB(0, 0, 0) : RGB(255, 255, 255);
  pTheme->hBackgroundBrush = CreateSolidBrush(pTheme->dwBackground);
  pTheme->hCardBrush = CreateSolidBrush(pTheme->dwCard);
  pTheme->fControlRadius = 4.0f;
  pTheme->fPanelRadius = 6.0f;
  pTheme->fBorderWidth = 1.0f;
  pTheme->fFocusBorderWidth = 1.5f;
  scLogDebug("Created GUI theme, dark mode: %s", pTheme->bDark ? "true" : "false");
}

//------------------------------------------------------------------------
// Nodes
//------------------------------------------------------------------------
scLength scLengthAuto()                 { return (scLength){ SC_LENGTH_AUTO,    0.0f }; }
scLength scLengthPixels(f32 fPixels)    { return (scLength){ SC_LENGTH_PIXELS,  fPixels }; }
scLength scLengthDips(f32 fDips)         { return (scLength){ SC_LENGTH_PIXELS,  fDips }; }
scLength scLengthPercent(f32 fFraction) { return (scLength){ SC_LENGTH_PERCENT, fFraction }; }
scLength scLengthStretch(f32 fWeight)   { return (scLength){ SC_LENGTH_STRETCH, fWeight }; }

scInternal scNode*
_scNewNode(scNodeType eType, scLength width, scLength height, const wchar_t* wszText) {
  scNode* pNode = (scNode*)calloc(1, sizeof(scNode));
  if (!pNode) return NULL;
  pNode->eType   = eType;
  pNode->width   = width;
  pNode->height  = height;
  pNode->wszText = _wcsdup(wszText ? wszText : L"");
  if (!pNode->wszText) {
    free(pNode);
    return NULL;
  }
  pNode->style.dwBackground = CLR_INVALID;
  pNode->style.dwHoverBackground = CLR_INVALID;
  pNode->style.dwActiveBackground = CLR_INVALID;
  pNode->style.dwText = CLR_INVALID;
  pNode->style.dwHoverText = CLR_INVALID;
  pNode->style.dwBorder = CLR_INVALID;
  pNode->style.dwHoverBorder = CLR_INVALID;
  pNode->style.eFontRole = SC_FONT_INHERIT;
  pNode->style.fCornerRadius = -1.0f;
  pNode->style.fBorderWidth = -1.0f;
  return pNode;
}

scInternal void
_scFreeNode(scNode* pNode) {
  scNode* pChild = pNode->pFirst;
  while (pChild) {
    scNode* pNext = pChild->pNext;
    _scFreeNode(pChild);
    pChild = pNext;
  }
  free(pNode->wszText);
  free(pNode->pUser);
  free(pNode);
}

scNode* scCreateVStack(scLength width, scLength height) {
  return _scNewNode(SC_NODE_VSTACK, width, height, NULL);
}

scNode* scCreateHStack(scLength width, scLength height) {
  return _scNewNode(SC_NODE_HSTACK, width, height, NULL);
}

scNode* scCreateSpacer(scLength width, scLength height) {
  return _scNewNode(SC_NODE_SPACER, width, height, NULL);
}

scNode* scCreateDivider() {
  return _scNewNode(SC_NODE_DIVIDER, scLengthStretch(1.0f), scLengthPixels(1), NULL);
}

scNode* scCreateLabel(const wchar_t* wszText, bool bBold) {
  scNode* pNode = _scNewNode(SC_NODE_LABEL, scLengthAuto(), scLengthAuto(), wszText);
  if (!pNode) return NULL;
  pNode->u.label.bBold = bBold;
  return pNode;
}

scNode* scCreateField(const wchar_t* wszText) {
  return _scNewNode(SC_NODE_FIELD, scLengthStretch(1.0f), scLengthPixels(30), wszText);
}

scNode* scCreateButton(const wchar_t* wszText, void (*pfnClick)(s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_BUTTON, scLengthAuto(), scLengthAuto(), wszText);
  if (!pNode) return NULL;
  pNode->u.button.pfnClick = pfnClick;
  pNode->u.button.iParam   = iParam;
  return pNode;
}

scNode* scCreateLink(const wchar_t* wszText, void (*pfnClick)(s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_LINK, scLengthStretch(1.0f), scLengthPixels(30), wszText);
  if (!pNode) return NULL;
  pNode->u.button.pfnClick = pfnClick;
  pNode->u.button.iParam   = iParam;
  return pNode;
}

scNode* scCreateTab(const wchar_t* wszText, bool bActive, void (*pfnClick)(s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_TAB, scLengthStretch(1.0f), scLengthPixels(30), wszText);
  if (!pNode) return NULL;
  pNode->u.button.pfnClick = pfnClick;
  pNode->u.button.iParam   = iParam;
  pNode->u.button.bActive  = bActive;
  return pNode;
}

scNode* scCreateToggle(const wchar_t* wszText, bool* pValue, s32 iParam, void (*pfnChange)(bool bValue, s32 iParam)) {
  scNode* pNode = _scNewNode(SC_NODE_TOGGLE, scLengthStretch(1.0f), scLengthPixels(40), wszText);
  if (!pNode) return NULL;
  pNode->u.toggle.pValue    = pValue;
  pNode->u.toggle.iParam    = iParam;
  pNode->u.toggle.pfnChange = pfnChange;
  return pNode;
}

scNode* scCreateSlider(const wchar_t* wszText, f32* pValue, f32 fMin, f32 fMax, f32 fStep, s32 iParam, void (*pfnChange)(f32 fValue, s32 iParam)) {
  scNode* pNode = _scNewNode(SC_NODE_SLIDER, scLengthStretch(1.0f), scLengthPixels(40), wszText);
  if (!pNode) return NULL;
  pNode->u.slider.pValue    = pValue;
  pNode->u.slider.iParam    = iParam;
  pNode->u.slider.pfnChange = pfnChange;
  scSetSliderRange(pNode, fMin, fMax, fStep);
  return pNode;
}

scNode* scCreateDropdown(const wchar_t* wszLabel, const char* const* aOptions, s32 nOptionCount, s32 iSelected, void (*pfnSelect)(s32 iIndex, s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_DROPDOWN, scLengthStretch(1.0f), scLengthPixels(40), wszLabel);
  if (!pNode) return NULL;
  pNode->u.dropdown.aOptions     = aOptions;
  pNode->u.dropdown.nOptionCount = nOptionCount;
  pNode->u.dropdown.iSelected    = iSelected;
  pNode->u.dropdown.iParam       = iParam;
  pNode->u.dropdown.pfnSelect    = pfnSelect;
  return pNode;
}

scNode* scCreateCustom(scLength width, scLength height, const scCustomOps* pOps, const void* pUser, s32 nUserSize) {
  scNode* pNode = _scNewNode(SC_NODE_CUSTOM, width, height, NULL);
  if (!pNode) return NULL;
  if (pOps) {
    pNode->u.custom = *pOps;
  }
  if (pUser && nUserSize > 0) {
    pNode->pUser = malloc(nUserSize);
    if (!pNode->pUser) {
      _scFreeNode(pNode);
      return NULL;
    }
    memcpy(pNode->pUser, pUser, nUserSize);
  }
  return pNode;
}

void scAddChild(scNode* pParent, scNode* pChild) {
  if (!pParent || !pChild) {
    return;
  }
  pChild->pParent = pParent;
  pChild->pNext   = NULL;
  if (pParent->pLast) {
    pParent->pLast->pNext = pChild;
  } else {
    pParent->pFirst = pChild;
  }
  pParent->pLast = pChild;
  _scMarkDirty(pParent, true);
}

void* scGetUserData(scNode* pNode) {
  return pNode ? pNode->pUser : NULL;
}

void scSetSize(scNode* pNode, scLength width, scLength height) {
  pNode->width  = width;
  pNode->height = height;
  _scMarkDirty(pNode, true);
}

void scSetPadding(scNode* pNode, s32 iPadding) {
  scSetPaddingEx(pNode, iPadding, iPadding, iPadding, iPadding);
}

void scSetPaddingEx(scNode* pNode, s32 iLeft, s32 iTop, s32 iRight, s32 iBottom) {
  pNode->aPadding[0] = iLeft;
  pNode->aPadding[1] = iTop;
  pNode->aPadding[2] = iRight;
  pNode->aPadding[3] = iBottom;
  _scMarkDirty(pNode, true);
}

void scSetSpacing(scNode* pNode, s32 iSpacing) {
  pNode->iSpacing = iSpacing;
  _scMarkDirty(pNode, true);
}

void scSetAlignment(scNode* pNode, scAlign eAlign) {
  pNode->eAlign = eAlign;
  _scMarkDirty(pNode, true);
}

void scSetWrap(scNode* pNode, bool bWrap) {
  pNode->bWrap = bWrap;
  _scMarkDirty(pNode, true);
}

void scSetScrollable(scNode* pNode, bool bScrollable) {
  pNode->bScrollable = bScrollable;
  _scMarkDirty(pNode, true);
}

void scSetStickBottom(scNode* pNode, bool bStickBottom) {
  pNode->bStickBottom = bStickBottom;
}

void scSetSurface(scNode* pNode, bool bSurface) {
  pNode->bSurface = bSurface;
  _scMarkDirty(pNode, false);
}

void scSetText(scNode* pNode, const wchar_t* wszText) {
  free(pNode->wszText);
  pNode->wszText = _wcsdup(wszText ? wszText : L"");
  _scMarkDirty(pNode, true);
}

void scSetTextRole(scNode* pNode, scTextRole eRole) {
  pNode->eTextRole = eRole;
  _scMarkDirty(pNode, false);
}

void scSetEllipsis(scNode* pNode, bool bEllipsis) {
  pNode->bEllipsis = bEllipsis;
  _scMarkDirty(pNode, false);
}

void scSetCard(scNode* pNode, f32 fRadius) {
  pNode->fCardRadius = fRadius;
  _scMarkDirty(pNode, false);
}

void scSetOutline(scNode* pNode, f32 fRadius) {
  pNode->fOutlineRadius = fRadius;
  _scMarkDirty(pNode, false);
}

void scSetNodeBackgroundColors(scNode* pNode, COLORREF normal, COLORREF hover, COLORREF active) {
  if (!pNode) return;
  pNode->style.dwBackground = normal;
  pNode->style.dwHoverBackground = hover;
  pNode->style.dwActiveBackground = active;
  _scMarkDirty(pNode, false);
}

void scSetNodeTextColors(scNode* pNode, COLORREF normal, COLORREF hover) {
  if (!pNode) return;
  pNode->style.dwText = normal;
  pNode->style.dwHoverText = hover;
  _scMarkDirty(pNode, false);
}

void scSetNodeBorderColors(scNode* pNode, COLORREF normal, COLORREF hover) {
  if (!pNode) return;
  pNode->style.dwBorder = normal;
  pNode->style.dwHoverBorder = hover;
  _scMarkDirty(pNode, false);
}

void scSetNodeCornerRadius(scNode* pNode, f32 fRadius) {
  if (!pNode) return;
  pNode->style.fCornerRadius = fRadius < 0.0f ? -1.0f : fRadius;
  _scMarkDirty(pNode, false);
}

void scSetNodeBorderWidth(scNode* pNode, f32 fWidth) {
  if (!pNode) return;
  pNode->style.fBorderWidth = fWidth < 0.0f ? -1.0f : fWidth;
  _scMarkDirty(pNode, false);
}

void scSetNodeFontRole(scNode* pNode, scFontRole eRole) {
  if (!pNode) return;
  if (eRole < SC_FONT_INHERIT || eRole > SC_FONT_PREVIEW) eRole = SC_FONT_INHERIT;
  pNode->style.eFontRole = eRole;
  _scMarkDirty(pNode, true);
}

void scResetNodeStyle(scNode* pNode) {
  if (!pNode) return;
  pNode->style.dwBackground = CLR_INVALID;
  pNode->style.dwHoverBackground = CLR_INVALID;
  pNode->style.dwActiveBackground = CLR_INVALID;
  pNode->style.dwText = CLR_INVALID;
  pNode->style.dwHoverText = CLR_INVALID;
  pNode->style.dwBorder = CLR_INVALID;
  pNode->style.dwHoverBorder = CLR_INVALID;
  pNode->style.eFontRole = SC_FONT_INHERIT;
  pNode->style.fCornerRadius = -1.0f;
  pNode->style.fBorderWidth = -1.0f;
  _scMarkDirty(pNode, true);
}

scInternal scGui*
_scNodeGui(scNode* pNode) {
  if (!pNode) return NULL;
  while (pNode->pParent) pNode = pNode->pParent;
  return pNode->pGui;
}

scInternal HFONT
_scFontForRole(scGui* pGui, scFontRole eRole) {
  if (!pGui) return NULL;
  switch (eRole) {
    case SC_FONT_SMALL:     return pGui->theme.hSmallFont ? pGui->theme.hSmallFont : pGui->theme.hFont;
    case SC_FONT_BOLD:      return pGui->theme.hBoldFont ? pGui->theme.hBoldFont : pGui->theme.hFont;
    case SC_FONT_HEADING:   return pGui->theme.hHeadingFont ? pGui->theme.hHeadingFont : (pGui->theme.hBoldFont ? pGui->theme.hBoldFont : pGui->theme.hFont);
    case SC_FONT_MONOSPACE: return pGui->theme.hMonoFont ? pGui->theme.hMonoFont : pGui->theme.hFont;
    case SC_FONT_PREVIEW:   return pGui->theme.hPreviewFont ? pGui->theme.hPreviewFont : (pGui->theme.hMonoFont ? pGui->theme.hMonoFont : pGui->theme.hFont);
    default:               return pGui->theme.hFont;
  }
}

HFONT scGetNodeFont(scNode* pNode, scFontRole eFallback) {
  scGui* pGui = _scNodeGui(pNode);
  if (!pGui) return NULL;
  scFontRole eRole = pNode->style.eFontRole != SC_FONT_INHERIT ? pNode->style.eFontRole : eFallback;
  return _scFontForRole(pGui, eRole);
}

COLORREF scGetNodeBackground(scNode* pNode, scNodeState eState) {
  scGui* pGui = _scNodeGui(pNode);
  if (!pGui) return CLR_INVALID;
  scGuiTheme* t = &pGui->theme;
  COLORREF c = CLR_INVALID;
  if (eState == SC_NODE_STATE_HOVER) c = pNode->style.dwHoverBackground;
  else if (eState == SC_NODE_STATE_ACTIVE) c = pNode->style.dwActiveBackground;
  else c = pNode->style.dwBackground;
  if (c != CLR_INVALID) return c;
  if (pNode->style.dwBackground != CLR_INVALID) return pNode->style.dwBackground;
  if (eState == SC_NODE_STATE_HOVER) return t->dwCardHover;
  if (eState == SC_NODE_STATE_ACTIVE) return t->dwCardActive;
  return t->dwCard;
}

bool scNodeHasBackgroundOverride(scNode* pNode, scNodeState eState) {
  if (!pNode) return false;
  if (eState == SC_NODE_STATE_HOVER && pNode->style.dwHoverBackground != CLR_INVALID) return true;
  if (eState == SC_NODE_STATE_ACTIVE && pNode->style.dwActiveBackground != CLR_INVALID) return true;
  return pNode->style.dwBackground != CLR_INVALID;
}

COLORREF scGetNodeTextColor(scNode* pNode, scNodeState eState) {
  scGui* pGui = _scNodeGui(pNode);
  if (!pGui) return CLR_INVALID;
  scGuiTheme* t = &pGui->theme;
  if (eState == SC_NODE_STATE_HOVER && pNode->style.dwHoverText != CLR_INVALID)
    return pNode->style.dwHoverText;
  if (pNode->style.dwText != CLR_INVALID) return pNode->style.dwText;
  if (pNode->eType == SC_NODE_LINK && eState == SC_NODE_STATE_HOVER) return t->dwAccent;
  if (pNode->eTextRole == SC_TEXT_DIM) return t->dwTextDim;
  if (pNode->eTextRole == SC_TEXT_FAINT) return t->dwTextFaint;
  return t->dwText;
}

COLORREF scGetNodeBorderColor(scNode* pNode, bool bHovered) {
  scGui* pGui = _scNodeGui(pNode);
  if (!pGui) return CLR_INVALID;
  if (bHovered && pNode->style.dwHoverBorder != CLR_INVALID) return pNode->style.dwHoverBorder;
  if (pNode->style.dwBorder != CLR_INVALID) return pNode->style.dwBorder;
  return bHovered ? pGui->theme.dwAccent : pGui->theme.dwStroke;
}

f32 scGetNodeCornerRadius(scNode* pNode, f32 fFallback) {
  return pNode && pNode->style.fCornerRadius >= 0.0f ? pNode->style.fCornerRadius : fFallback;
}

f32 scGetNodeBorderWidth(scNode* pNode, f32 fFallback) {
  return pNode && pNode->style.fBorderWidth >= 0.0f ? pNode->style.fBorderWidth : fFallback;
}

void scSetSliderRange(scNode* pNode, f32 fMin, f32 fMax, f32 fStep) {
  if (!pNode || pNode->eType != SC_NODE_SLIDER) {
    return;
  }
  pNode->u.slider.fMin  = fMin < fMax ? fMin : fMax;
  pNode->u.slider.fMax  = fMin < fMax ? fMax : fMin;
  pNode->u.slider.fStep = fStep;
  _scMarkDirty(pNode, false);
}

void scSetScrollY(scNode* pNode, s32 iScrollY) {
  pNode->iScrollY = iScrollY;
  _scMarkDirty(pNode, true);
}

s32 scGetScrollY(scNode* pNode) {
  return pNode->iScrollY;
}

//------------------------------------------------------------------------
// Layout
//------------------------------------------------------------------------
scInternal SIZE
_scTextSize(scGui* pGui, HFONT hFont, const wchar_t* wszText, bool bWrap, s32 iMaxW) {
  SIZE sz = { 0, 0 };
  HGDIOBJ hOldFont = SelectObject(pGui->hMeasureDC, hFont);
  if (bWrap) {
    RECT rc = { 0, 0, _scMax(iMaxW, 1), 0 };
    DrawTextW(pGui->hMeasureDC, wszText, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    sz.cx = rc.right;
    sz.cy = rc.bottom;
  } else {
    GetTextExtentPoint32W(pGui->hMeasureDC, wszText, lstrlenW(wszText), &sz);
  }
  SelectObject(pGui->hMeasureDC, hOldFont);
  return sz;
}

scInternal SIZE
_scIntrinsic(scGui* pGui, scNode* pNode, s32 iAvailW, s32 iAvailH) {
  SIZE sz = { 0, 0 };
  scGuiTheme* t = &pGui->theme;
  switch (pNode->eType) {
    case SC_NODE_VSTACK:
    case SC_NODE_HSTACK: {
      s32 iUsedW = 0, iUsedH = 0;
      _scFlow(pGui, pNode, iAvailW, iAvailH, 0, 0, false, &iUsedW, &iUsedH);
      sz.cx = iUsedW + _scPx(pGui, (f32)(pNode->aPadding[0] + pNode->aPadding[2]));
      sz.cy = iUsedH + _scPx(pGui, (f32)(pNode->aPadding[1] + pNode->aPadding[3]));
      break;
    }
    case SC_NODE_SPACER:
    case SC_NODE_DIVIDER:
      break;
    case SC_NODE_CUSTOM: {
      if (pNode->u.custom.pfnMeasure) {
        sz.cx = iAvailW;
        sz.cy = pNode->u.custom.pfnMeasure(pNode, pGui->hMeasureDC, iAvailW);
      }
      break;
    }
    case SC_NODE_LABEL: {
      HFONT hFont = _scFontForRole(pGui, pNode->style.eFontRole != SC_FONT_INHERIT ? pNode->style.eFontRole : (pNode->u.label.bBold ? SC_FONT_BOLD : SC_FONT_BODY));
      sz = _scTextSize(pGui, hFont, pNode->wszText, pNode->bWrap, iAvailW);
      break;
    }
    default: {
      sz = _scTextSize(pGui, _scFontForRole(pGui, pNode->style.eFontRole != SC_FONT_INHERIT ? pNode->style.eFontRole : SC_FONT_BODY), pNode->wszText, false, 0);
      sz.cx += _scPx(pGui, 30.0f);
      sz.cy  = _scPx(pGui, 30.0f);
      break;
    }
  }
  return sz;
}

scInternal void
_scPreferred(scGui* pGui, scNode* pNode, s32 iBaseW, s32 iBaseH, const bool* aIntrinsic) {
  scLength aLen[2]  = { pNode->width, pNode->height };
  s32      aBase[2] = { iBaseW, iBaseH };

  bool bMeasure = false;
  for (s32 i = 0; i < 2; ++i) {
    if (aLen[i].eType == SC_LENGTH_AUTO || (aLen[i].eType == SC_LENGTH_STRETCH && aIntrinsic[i])) {
      bMeasure = true;
    }
  }

  SIZE sz = { 0, 0 };
  if (bMeasure) {
    sz = _scIntrinsic(pGui, pNode, iBaseW, iBaseH);
  }
  s32 aAuto[2] = { sz.cx, sz.cy };

  for (s32 i = 0; i < 2; ++i) {
    switch (aLen[i].eType) {
      case SC_LENGTH_PIXELS:  pNode->aSize[i] = _scPx(pGui, aLen[i].fValue);        break;
      case SC_LENGTH_PERCENT: pNode->aSize[i] = (s32)(aLen[i].fValue * aBase[i]);   break;
      case SC_LENGTH_STRETCH: pNode->aSize[i] = aIntrinsic[i] ? aAuto[i] : 0;       break;
      default:                pNode->aSize[i] = aAuto[i];                           break;
    }
  }
}

scInternal void
_scRefreshHeight(scGui* pGui, scNode* pNode, s32 iBaseH) {
  bool bSensitive = _scIsContainer(pNode) || (pNode->eType == SC_NODE_LABEL && pNode->bWrap) ||
                    (pNode->eType == SC_NODE_CUSTOM && pNode->u.custom.pfnMeasure);
  if (pNode->height.eType == SC_LENGTH_AUTO && bSensitive) {
    pNode->aSize[1] = _scIntrinsic(pGui, pNode, pNode->aSize[0], iBaseH).cy;
  }
}

scInternal void
_scFlow(scGui* pGui, scNode* pNode, s32 iW, s32 iH, s32 iX, s32 iY, bool bPlace, s32* pUsedW, s32* pUsedH) {
  bool bVert = pNode->eType == SC_NODE_VSTACK;
  s32  m     = bVert ? 1 : 0;
  s32  c     = 1 - m;

  s32 iPadL = _scPx(pGui, (f32)pNode->aPadding[0]);
  s32 iPadT = _scPx(pGui, (f32)pNode->aPadding[1]);
  s32 iPadR = _scPx(pGui, (f32)pNode->aPadding[2]);
  s32 iPadB = _scPx(pGui, (f32)pNode->aPadding[3]);

  s32 aBox[2]    = { _scMax(0, iW - iPadL - iPadR), _scMax(0, iH - iPadT - iPadB) };
  s32 aOrigin[2] = { iX + iPadL, iY + iPadT };
  s32 iSpacing   = _scPx(pGui, (f32)pNode->iSpacing);

  bool aDefinite[2] = {
    pNode->width.eType  != SC_LENGTH_AUTO,
    pNode->height.eType != SC_LENGTH_AUTO
  };
  if (bVert && pNode->bScrollable) {
    aDefinite[1] = false;
  }
  bool aIntrinsic[2] = { !aDefinite[0], !aDefinite[1] };

  s32 nChildren = 0;
  f32 fWeight   = 0.0f;
  for (scNode* p = pNode->pFirst; p; p = p->pNext) {
    _scPreferred(pGui, p, aBox[0], aBox[1], aIntrinsic);
    scLength lenMain = m == 0 ? p->width : p->height;
    if (lenMain.eType == SC_LENGTH_STRETCH) {
      fWeight += lenMain.fValue;
    }
    ++nChildren;
  }
  s32 nGaps = nChildren > 1 ? nChildren - 1 : 0;

  if (bVert) {
    s32 iFill = aBox[0];
    if (!aDefinite[0]) {
      iFill = 0;
      for (scNode* p = pNode->pFirst; p; p = p->pNext) {
        iFill = _scMax(iFill, p->aSize[0]);
      }
    }
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      if (p->width.eType == SC_LENGTH_STRETCH) {
        p->aSize[0] = iFill;
      }
      _scRefreshHeight(pGui, p, aBox[1]);
    }
  }

  if (!pNode->bWrap && aDefinite[m] && fWeight > 0.0f) {
    s32 iFixed = nGaps * iSpacing;
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      scLength lenMain = m == 0 ? p->width : p->height;
      if (lenMain.eType != SC_LENGTH_STRETCH) {
        iFixed += p->aSize[m];
      }
    }
    s32 iRemain = _scMax(0, aBox[m] - iFixed);
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      scLength lenMain = m == 0 ? p->width : p->height;
      if (lenMain.eType == SC_LENGTH_STRETCH) {
        p->aSize[m] = (s32)(iRemain * lenMain.fValue / fWeight);
      }
    }
  }

  if (!bVert) {
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      _scRefreshHeight(pGui, p, aBox[1]);
    }
    s32 iFill = aBox[1];
    if (!aDefinite[1]) {
      iFill = 0;
      for (scNode* p = pNode->pFirst; p; p = p->pNext) {
        iFill = _scMax(iFill, p->aSize[1]);
      }
    }
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      if (p->height.eType == SC_LENGTH_STRETCH) {
        p->aSize[1] = iFill;
      }
    }
  }

  s32 iCrossExtent = aBox[c];
  if (!aDefinite[c]) {
    iCrossExtent = 0;
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      iCrossExtent = _scMax(iCrossExtent, p->aSize[c]);
    }
  }

  s32 aPos[2]     = { 0, 0 };
  s32 iLineCross  = 0;
  s32 iUsedMain   = 0;
  for (scNode* p = pNode->pFirst; p; p = p->pNext) {
    if (pNode->bWrap && aPos[m] > 0 && aPos[m] + p->aSize[m] > aBox[m]) {
      aPos[c] += iLineCross + iSpacing;
      aPos[m]  = 0;
      iLineCross = 0;
    }

    s32 iAlign = 0;
    if (!pNode->bWrap) {
      s32 iSlack = _scMax(0, iCrossExtent - p->aSize[c]);
      iAlign = pNode->eAlign == SC_ALIGN_MIDDLE ? iSlack / 2 : (pNode->eAlign == SC_ALIGN_END ? iSlack : 0);
    }

    if (bPlace) {
      s32 aChild[2];
      aChild[m] = aOrigin[m] + aPos[m];
      aChild[c] = aOrigin[c] + aPos[c] + iAlign;
      _scPlace(pGui, p, aChild[0], aChild[1], p->aSize[0], p->aSize[1]);
    }

    aPos[m]   += p->aSize[m] + iSpacing;
    iUsedMain  = _scMax(iUsedMain, aPos[m] - iSpacing);
    iLineCross = _scMax(iLineCross, p->aSize[c]);
  }

  s32 aUsed[2];
  aUsed[m] = iUsedMain;
  aUsed[c] = aPos[c] + iLineCross;
  *pUsedW = aUsed[0];
  *pUsedH = aUsed[1];
}

scInternal void
_scPlace(scGui* pGui, scNode* pNode, s32 iX, s32 iY, s32 iW, s32 iH) {
  pNode->rc = (RECT){ iX, iY, iX + iW, iY + iH };
  if (!_scIsContainer(pNode)) {
    return;
  }

  s32 iUsedW = 0, iUsedH = 0;
  _scFlow(pGui, pNode, iW, iH, iX, iY, true, &iUsedW, &iUsedH);

  if (pNode->bScrollable) {
    bool bAtBottom = pNode->iScrollY >= pNode->iMaxScroll;
    pNode->iContentH  = iUsedH + _scPx(pGui, (f32)(pNode->aPadding[1] + pNode->aPadding[3]));
    pNode->iMaxScroll = _scMax(0, pNode->iContentH - iH);
    pNode->iScrollY   = (pNode->bStickBottom && bAtBottom) ? pNode->iMaxScroll : _scClamp(pNode->iScrollY, 0, pNode->iMaxScroll);
  }
}

scInternal void
_scEnsureLayout(scGui* pGui) {
  if (!pGui->bLayoutDirty || !pGui->pRoot) {
    return;
  }
  pGui->hMeasureDC = CreateCompatibleDC(NULL);
  _scPlace(pGui, pGui->pRoot, 0, 0, pGui->iWidth, pGui->iHeight);
  DeleteDC(pGui->hMeasureDC);
  pGui->hMeasureDC   = NULL;
  pGui->bLayoutDirty = false;
}

//------------------------------------------------------------------------
// Hit testing
//------------------------------------------------------------------------
scInternal scNode*
_scHit(scNode* pNode, POINT pt, s32 iOffY) {
  RECT r = pNode->rc;
  OffsetRect(&r, 0, -iOffY);

  if (_scIsContainer(pNode)) {
    if (pNode->bScrollable && !PtInRect(&r, pt)) {
      return NULL;
    }
    s32 iChildOff = iOffY + (pNode->bScrollable ? pNode->iScrollY : 0);
    for (scNode* p = pNode->pFirst; p; p = p->pNext) {
      scNode* pHit = _scHit(p, pt, iChildOff);
      if (pHit) {
        return pHit;
      }
    }
    return NULL;
  }

  return (_scIsInteractive(pNode) && PtInRect(&r, pt)) ? pNode : NULL;
}

scInternal scNode*
_scScrollerAt(scNode* pNode, POINT pt, s32 iOffY) {
  if (!_scIsContainer(pNode)) {
    return NULL;
  }
  RECT r = pNode->rc;
  OffsetRect(&r, 0, -iOffY);
  if (pNode->bScrollable && !PtInRect(&r, pt)) {
    return NULL;
  }
  s32 iChildOff = iOffY + (pNode->bScrollable ? pNode->iScrollY : 0);
  for (scNode* p = pNode->pFirst; p; p = p->pNext) {
    scNode* pFound = _scScrollerAt(p, pt, iChildOff);
    if (pFound) {
      return pFound;
    }
  }
  return pNode->bScrollable ? pNode : NULL;
}

//------------------------------------------------------------------------
// Scrolling
//------------------------------------------------------------------------
scInternal scScrollGeom
_scComputeScrollGeom(scGui* pGui, RECT rcTrack, s32 iContentH, s32 iViewportH, s32 iScroll, s32 iMaxScroll) {
  scScrollGeom sg = { 0 };
  if (iMaxScroll <= 0 || iContentH <= iViewportH) {
    return sg;
  }
  sg.bVisible = true;
  sg.rcTrack  = rcTrack;

  s32 iTrackH = rcTrack.bottom - rcTrack.top;
  s32 iThumbH = _scClamp((s32)((s64)iTrackH * iViewportH / iContentH), _scPx(pGui, 24.0f), iTrackH);
  sg.iTrackRange = iTrackH - iThumbH;

  s32 iY = rcTrack.top + (s32)((s64)sg.iTrackRange * _scClamp(iScroll, 0, iMaxScroll) / iMaxScroll);
  sg.rcThumb = (RECT){ rcTrack.left, iY, rcTrack.right, iY + iThumbH };
  return sg;
}

scInternal scScrollGeom
_scNodeScrollGeom(scGui* pGui, scNode* pNode) {
  if (!pNode->bScrollable || pNode->iMaxScroll <= 0) {
    return (scScrollGeom){ 0 };
  }
  RECT r = _scScreenRect(pNode);
  RECT rcTrack = { r.right - _scPx(pGui, 14.0f), r.top + _scPx(pGui, 8.0f), r.right - _scPx(pGui, 6.0f), r.bottom - _scPx(pGui, 8.0f) };
  return _scComputeScrollGeom(pGui, rcTrack, pNode->iContentH, r.bottom - r.top, pNode->iScrollY, pNode->iMaxScroll);
}

scInternal bool
_scScrollbarHit(scGui* pGui, scScrollGeom sg, POINT pt) {
  if (!sg.bVisible) {
    return false;
  }
  RECT rcHit = sg.rcTrack;
  InflateRect(&rcHit, _scPx(pGui, 3.0f), 0);
  return PtInRect(&rcHit, pt);
}

scInternal void
_scInvalidateScrollbar(scGui* pGui, scNode* pNode) {
  if (!pNode) {
    return;
  }
  scScrollGeom sg = _scNodeScrollGeom(pGui, pNode);
  if (sg.bVisible) {
    RECT r = sg.rcTrack;
    InflateRect(&r, _scPx(pGui, 3.0f), 0);
    _scInvalidate(pGui, &r);
  }
}

scInternal void
_scBeginDrag(scGui* pGui, s32* pValue, scScrollGeom sg, s32 iMaxScroll, POINT pt) {
  if (!PtInRect(&sg.rcThumb, pt)) {
    s32 iThumbH = sg.rcThumb.bottom - sg.rcThumb.top;
    s32 iRel    = pt.y - sg.rcTrack.top - iThumbH / 2;
    *pValue = sg.iTrackRange > 0 ? _scClamp((s32)((s64)iRel * iMaxScroll / sg.iTrackRange), 0, iMaxScroll) : 0;
  }
  pGui->drag.bActive      = true;
  pGui->drag.pValue       = pValue;
  pGui->drag.iStartY      = pt.y;
  pGui->drag.iStartScroll = *pValue;
  pGui->drag.iTrackRange  = sg.iTrackRange;
  pGui->drag.iMaxScroll   = iMaxScroll;
  _scCapture(pGui, true);
  _scInvalidate(pGui, NULL);
}

//------------------------------------------------------------------------
// Dropdown popup
//------------------------------------------------------------------------
scInternal RECT
_scDropdownBox(scGui* pGui, scNode* pNode, RECT r) {
  bool bCompact = pNode->wszText[0] == 0;
  s32  iWidth   = bCompact ? (r.right - r.left) : _scPx(pGui, 150.0f);
  s32  iInset   = bCompact ? 0 : _scPx(pGui, 15.0f);
  return (RECT){ r.right - iInset - iWidth, r.top + _scPx(pGui, 6.0f),
                 r.right - iInset,          r.bottom - _scPx(pGui, 6.0f) };
}

scInternal scPopupGeom
_scPopupGeom(scGui* pGui, scNode* pNode) {
  scPopupGeom pg = { 0 };
  s32 nOptions = pNode->u.dropdown.nOptionCount;
  pg.iItemH = _scPx(pGui, 28.0f);

  RECT r = _scScreenRect(pNode);
  s32 iMaxVisible = (pGui->iHeight - r.bottom - _scPx(pGui, 20.0f)) / pg.iItemH;
  if (iMaxVisible < 1) iMaxVisible = 1;
  pg.nVisible = _scMin(nOptions, iMaxVisible);

  RECT rcBox = _scDropdownBox(pGui, pNode, r);
  pg.rcBox = (RECT){ rcBox.left, rcBox.bottom, rcBox.right, rcBox.bottom + pg.nVisible * pg.iItemH };

  pg.bHasScroll = nOptions > pg.nVisible;
  pg.iMaxScroll = pg.bHasScroll ? (nOptions - pg.nVisible) * pg.iItemH : 0;
  if (pg.bHasScroll) {
    RECT rcTrack = { pg.rcBox.right - _scPx(pGui, 10.0f), pg.rcBox.top + _scPx(pGui, 4.0f),
                     pg.rcBox.right - _scPx(pGui, 4.0f),  pg.rcBox.bottom - _scPx(pGui, 4.0f) };
    pg.scroll = _scComputeScrollGeom(pGui, rcTrack, nOptions * pg.iItemH, pg.nVisible * pg.iItemH, pGui->iPopupScroll, pg.iMaxScroll);
  }
  return pg;
}

//------------------------------------------------------------------------
// Slider
//------------------------------------------------------------------------
scInternal s32
_scSliderDecimals(scNode* pNode) {
  f32 fStep = pNode->u.slider.fStep;
  if (fStep <= 0.0f) {
    return 2;
  }
  s32 nDecimals = 0;
  while (nDecimals < 6 && fabsf(fStep - floorf(fStep + 0.5f)) > 0.0001f) {
    fStep *= 10.0f;
    ++nDecimals;
  }
  return nDecimals;
}

scInternal f32
_scSliderSnap(scNode* pNode, f32 fValue) {
  f32 fMin  = pNode->u.slider.fMin;
  f32 fMax  = pNode->u.slider.fMax;
  f32 fStep = pNode->u.slider.fStep;

  fValue = fValue < fMin ? fMin : (fValue > fMax ? fMax : fValue);
  if (fStep > 0.0f) {
    fValue = fMin + floorf((fValue - fMin) / fStep + 0.5f) * fStep;
    f32 fScale = powf(10.0f, (f32)_scSliderDecimals(pNode));
    fValue = floorf(fValue * fScale + 0.5f) / fScale;
    fValue = fValue > fMax ? fMax : fValue;
  }
  return fValue;
}

scInternal void
_scSliderRects(scGui* pGui, scNode* pNode, RECT r, RECT* pTrack, RECT* pValue) {
  s32 iValueW = _scPx(pGui, 64.0f);
  s32 iGap    = _scPx(pGui, 12.0f);
  *pValue = (RECT){ r.right - _scPx(pGui, 15.0f) - iValueW, r.top + _scPx(pGui, 6.0f),
                    r.right - _scPx(pGui, 15.0f),           r.bottom - _scPx(pGui, 6.0f) };

  s32 iAvail  = pValue->left - iGap - (r.left + _scPx(pGui, 15.0f));
  s32 iTrackW = _scMax(_scPx(pGui, 40.0f), _scMin(_scPx(pGui, 150.0f), iAvail / 2));
  s32 iCy     = (r.top + r.bottom) / 2;
  *pTrack = (RECT){ pValue->left - iGap - iTrackW, iCy - _scPx(pGui, 2.0f),
                    pValue->left - iGap,           iCy + _scPx(pGui, 2.0f) };
}

scInternal void
_scSliderApply(scGui* pGui, scNode* pNode, f32 fRaw) {
  f32* pValue = pNode->u.slider.pValue;
  if (!pValue) {
    return;
  }
  f32 fNew = _scSliderSnap(pNode, fRaw);
  if (fNew == *pValue) {
    return;
  }
  *pValue = fNew;

  void (*pfnChange)(f32 fValue, s32 iParam) = pNode->u.slider.pfnChange;
  s32 iParam = pNode->u.slider.iParam;
  _scInvalidateNode(pGui, pNode);
  if (pfnChange) {
    pfnChange(fNew, iParam);
  }
}

scInternal void
_scSliderApplyFromX(scGui* pGui, scNode* pNode, s32 iX) {
  RECT rcTrack, rcValue;
  _scSliderRects(pGui, pNode, _scScreenRect(pNode), &rcTrack, &rcValue);
  f32 fWidth = (f32)(rcTrack.right - rcTrack.left);
  f32 fFrac  = fWidth > 0.0f ? (f32)(iX - rcTrack.left) / fWidth : 0.0f;
  fFrac = fFrac < 0.0f ? 0.0f : (fFrac > 1.0f ? 1.0f : fFrac);
  _scSliderApply(pGui, pNode, pNode->u.slider.fMin + fFrac * (pNode->u.slider.fMax - pNode->u.slider.fMin));
}

scInternal void
_scBeginEdit(scGui* pGui, scNode* pNode) {
  f32 fValue = pNode->u.slider.pValue ? *pNode->u.slider.pValue : pNode->u.slider.fMin;
  pGui->pEditing = pNode;
  swprintf_s(pGui->wszEdit, ARRAYSIZE(pGui->wszEdit), L"%.*f", _scSliderDecimals(pNode), fValue);
  pGui->nEdit      = lstrlenW(pGui->wszEdit);
  pGui->bEditFresh = true;
  _scInvalidateNode(pGui, pNode);
}

scInternal void
_scEndEdit(scGui* pGui, bool bCommit) {
  scNode* pNode = pGui->pEditing;
  if (!pNode) {
    return;
  }
  pGui->pEditing = NULL;
  _scInvalidateNode(pGui, pNode);

  if (!bCommit || pGui->nEdit == 0) {
    return;
  }
  wchar_t* pEnd = NULL;
  double dValue = wcstod(pGui->wszEdit, &pEnd);
  if (pEnd == pGui->wszEdit) {
    return;
  }
  _scSliderApply(pGui, pNode, (f32)dValue);
  if (pGui->host.pfnValueChanged) {
    pGui->host.pfnValueChanged(pGui->host.pUser);
  }
}

scInternal void
_scSliderMouseDown(scGui* pGui, scNode* pNode, POINT pt) {
  bool  bCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  DWORD dwNow = GetTickCount();
  bool  bDouble = pGui->pLastClick == pNode &&
                  dwNow - pGui->dwLastClickTime <= GetDoubleClickTime() &&
                  abs(pt.x - pGui->ptLastClick.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                  abs(pt.y - pGui->ptLastClick.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;

  pGui->pLastClick      = bDouble ? NULL : pNode;
  pGui->dwLastClickTime = dwNow;
  pGui->ptLastClick     = pt;

  if (bCtrl || bDouble) {
    _scBeginEdit(pGui, pNode);
    return;
  }

  RECT rcTrack, rcValue;
  _scSliderRects(pGui, pNode, _scScreenRect(pNode), &rcTrack, &rcValue);
  RECT rcHit = { rcTrack.left - _scPx(pGui, 8.0f), _scScreenRect(pNode).top, rcTrack.right + _scPx(pGui, 8.0f), _scScreenRect(pNode).bottom };
  if (PtInRect(&rcHit, pt)) {
    pGui->pSliding = pNode;
    _scCapture(pGui, true);
    _scSliderApplyFromX(pGui, pNode, pt.x);
  }
}

//------------------------------------------------------------------------
// Drawing
//------------------------------------------------------------------------
scInternal void
_scDrawLabel(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  HFONT hFont = scGetNodeFont(pNode, pNode->u.label.bBold ? SC_FONT_BOLD : SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : pGui->theme.hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL));
  if (pNode->bEllipsis) {
    DrawTextW(hDC, pNode->wszText, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  } else if (pNode->bWrap) {
    DrawTextW(hDC, pNode->wszText, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  } else {
    DrawTextW(hDC, pNode->wszText, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
}

scInternal void
_scDrawField(scGui* pGui, HDC hDC, scNode* pNode, RECT r) {
  scGuiTheme* t = &pGui->theme;
  f32 fRad = scGetNodeCornerRadius(pNode, t->fControlRadius > 0.0f ? t->fControlRadius : 4.0f) * pGui->fScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(scGetNodeBackground(pNode, SC_NODE_STATE_NORMAL), 255));
  gpStrokeRound(pGraphics, r, fRad, gpColor(scGetNodeBorderColor(pNode, false), 255), scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);
  gpGraphicsEnd(pGraphics);

  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, SC_NODE_STATE_NORMAL));
  RECT rText = { r.left + _scPx(pGui, 10.0f), r.top, r.right - _scPx(pGui, 10.0f), r.bottom };
  DrawTextW(hDC, pNode->wszText, -1, &rText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
}

scInternal void
_scDrawButton(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bActive = pGui->pPressed == pNode;
  scNodeState eState = bActive ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
  f32 fRad = scGetNodeCornerRadius(pNode, t->fControlRadius > 0.0f ? t->fControlRadius : 4.0f) * pGui->fScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(scGetNodeBackground(pNode, eState), 255));
  gpStrokeRound(pGraphics, r, fRad, gpColor(scGetNodeBorderColor(pNode, bHovered || bActive), 255), scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);
  gpGraphicsEnd(pGraphics);

  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, eState));
  RECT rText = { r.left + _scPx(pGui, 8.0f), r.top, r.right - _scPx(pGui, 8.0f), r.bottom };
  DrawTextW(hDC, pNode->wszText, -1, &rText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

scInternal void
_scDrawLink(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL));
  DrawTextW(hDC, pNode->wszText, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

scInternal void
_scDrawTab(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bActive = pNode->u.button.bActive;

  if (bActive || bHovered || scNodeHasBackgroundOverride(pNode, SC_NODE_STATE_NORMAL) ||
      pNode->style.dwBorder != CLR_INVALID || pNode->style.dwHoverBorder != CLR_INVALID) {
    GpGraphics* pGraphics = gpGraphicsBegin(hDC);
    f32 fRadius = scGetNodeCornerRadius(pNode, t->fControlRadius > 0.0f ? t->fControlRadius : 5.0f) * pGui->fScale;
    scNodeState eState = bActive ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
    if (bActive || bHovered || scNodeHasBackgroundOverride(pNode, SC_NODE_STATE_NORMAL)) {
      gpFillRound(pGraphics, r, fRadius, gpColor(scGetNodeBackground(pNode, eState), 255));
    }
    if (pNode->style.dwBorder != CLR_INVALID || pNode->style.dwHoverBorder != CLR_INVALID) {
      gpStrokeRound(pGraphics, r, fRadius, gpColor(scGetNodeBorderColor(pNode, bHovered), 255),
                    scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);
    }
    if (bActive) {
      s32 iCenterY = (r.top + r.bottom) / 2;
      RECT rcInd = { r.left + _scPx(pGui, 3.0f), iCenterY - _scPx(pGui, 8.0f), r.left + _scPx(pGui, 6.0f), iCenterY + _scPx(pGui, 8.0f) };
      gpFillRound(pGraphics, rcInd, 1.5f * pGui->fScale, gpColor(t->dwAccent, 255));
    }
    gpGraphicsEnd(pGraphics);
  }

  HFONT hFont = scGetNodeFont(pNode, bActive ? SC_FONT_BOLD : SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : (bActive ? t->hBoldFont : t->hFont));
  COLORREF dwText = pNode->style.dwText != CLR_INVALID ? pNode->style.dwText : (bActive ? t->dwText : t->dwTextDim);
  if (bHovered && pNode->style.dwHoverText != CLR_INVALID) dwText = pNode->style.dwHoverText;
  SetTextColor(hDC, dwText);
  RECT rText = { r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 7.0f), r.right - _scPx(pGui, 8.0f), r.bottom };
  DrawTextW(hDC, pNode->wszText, -1, &rText, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

scInternal void
_scDrawToggle(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bOn = pNode->u.toggle.pValue && *pNode->u.toggle.pValue;

  bool bActive = pGui->pPressed == pNode;
  scNodeState eState = bActive ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
  f32 fRad = scGetNodeCornerRadius(pNode, t->fControlRadius > 0.0f ? t->fControlRadius : 6.0f) * pGui->fScale;
  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(scGetNodeBackground(pNode, eState), 255));
  gpStrokeRound(pGraphics, r, fRad, gpColor(scGetNodeBorderColor(pNode, bHovered), 255), scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);

  RECT pill = { r.right - _scPx(pGui, 55.0f), r.top + _scPx(pGui, 10.0f), r.right - _scPx(pGui, 15.0f), r.top + _scPx(pGui, 30.0f) };
  f32 fPillRad = (pill.bottom - pill.top) / 2.0f;
  if (bOn) {
    gpFillRound(pGraphics, pill, fPillRad, gpColor(bHovered ? t->dwAccentHover : t->dwAccent, 255));
  } else {
    gpFillRound(pGraphics, pill, fPillRad, gpColor(t->dwCardActive, 255));
    gpStrokeRound(pGraphics, pill, fPillRad, gpColor(t->dwPillOff, 255), 1.0f);
  }

  f32 fInset  = 3.0f * pGui->fScale;
  f32 fThumb  = (pill.bottom - pill.top) - fInset * 2.0f;
  f32 fThumbX = bOn ? (pill.right - fInset - fThumb) : (pill.left + fInset);
  COLORREF dwThumb = bOn ? t->dwAccentText : t->dwPillOff;
  gpFillEllipse(pGraphics, fThumbX, pill.top + fInset, fThumb, fThumb, gpColor(dwThumb, 255));
  gpGraphicsEnd(pGraphics);

  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, eState));
  RECT rText = { r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 10.0f), pill.left - _scPx(pGui, 10.0f), r.bottom };
  DrawTextW(hDC, pNode->wszText, -1, &rText, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

scInternal void
_scDrawDropdown(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bExpanded = pGui->pExpanded == pNode;
  bool bCompact  = pNode->wszText[0] == 0;
  RECT box = _scDropdownBox(pGui, pNode, r);

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  if (!bCompact) {
    scNodeState eState = bExpanded ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
    f32 fRad = scGetNodeCornerRadius(pNode, t->fControlRadius > 0.0f ? t->fControlRadius : 6.0f) * pGui->fScale;
    gpFillRound(pGraphics, r, fRad, gpColor(scGetNodeBackground(pNode, eState), 255));
    gpStrokeRound(pGraphics, r, fRad, gpColor(scGetNodeBorderColor(pNode, bHovered), 255), scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);
  }

  f32 fBoxRad = 4.0f * pGui->fScale;
  COLORREF cBox = scNodeHasBackgroundOverride(pNode, bExpanded ? SC_NODE_STATE_ACTIVE
    : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL))
    ? scGetNodeBackground(pNode, bExpanded ? SC_NODE_STATE_ACTIVE
      : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL)) : t->dwBackground;
  gpFillRound(pGraphics, box, fBoxRad, gpColor(cBox, 255));
  gpStrokeRound(pGraphics, box, fBoxRad, gpColor(scGetNodeBorderColor(pNode, bExpanded || bHovered), 255),
                (bExpanded ? (t->fFocusBorderWidth > 0.0f ? t->fFocusBorderWidth : 1.5f) : scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f)) * pGui->fScale);
  gpGraphicsEnd(pGraphics);

  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, bExpanded ? SC_NODE_STATE_ACTIVE
    : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL)));
  if (!bCompact) {
    RECT rLabel = { r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 10.0f), box.left - _scPx(pGui, 10.0f), r.bottom };
    DrawTextW(hDC, pNode->wszText, -1, &rLabel, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  }

  s32 iSelected = pNode->u.dropdown.iSelected;
  if (iSelected >= 0 && iSelected < pNode->u.dropdown.nOptionCount) {
    wchar_t wszValue[128] = { 0 };
    if (MultiByteToWideChar(CP_UTF8, 0, pNode->u.dropdown.aOptions[iSelected], -1, wszValue, ARRAYSIZE(wszValue)) > 0) {
      RECT rcValue = { box.left + _scPx(pGui, 10.0f), box.top + _scPx(pGui, 2.0f), box.right - _scPx(pGui, 24.0f), box.bottom - _scPx(pGui, 2.0f) };
      DrawTextW(hDC, wszValue, -1, &rcValue, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
  }

  SetTextColor(hDC, t->dwTextDim);
  TextOutW(hDC, box.right - _scPx(pGui, 20.0f), box.top + _scPx(pGui, 5.0f), L"\x25BC", 1);
}

scInternal void
_scDrawSlider(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bEditing = pGui->pEditing == pNode;
  bool bSliding = pGui->pSliding == pNode;

  RECT rcTrack, rcValue;
  _scSliderRects(pGui, pNode, r, &rcTrack, &rcValue);

  f32 fMin   = pNode->u.slider.fMin;
  f32 fRange = pNode->u.slider.fMax - fMin;
  f32 fValue = pNode->u.slider.pValue ? *pNode->u.slider.pValue : fMin;
  f32 fFrac  = fRange > 0.0f ? (fValue - fMin) / fRange : 0.0f;
  fFrac = fFrac < 0.0f ? 0.0f : (fFrac > 1.0f ? 1.0f : fFrac);

  wchar_t wszValue[32];
  if (bEditing) {
    wcscpy_s(wszValue, ARRAYSIZE(wszValue), pGui->wszEdit);
  } else {
    swprintf_s(wszValue, ARRAYSIZE(wszValue), L"%.*f", _scSliderDecimals(pNode), fValue);
  }

  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SIZE szText = { 0, 0 };
  GetTextExtentPoint32W(hDC, wszValue, lstrlenW(wszValue), &szText);
  s32 iTextRight = rcValue.right - _scPx(pGui, 6.0f);

  s32 iThumbX  = rcTrack.left + (s32)(fFrac * (rcTrack.right - rcTrack.left));
  f32 fTrackR  = (rcTrack.bottom - rcTrack.top) / 2.0f;
  f32 fThumb   = 16.0f * pGui->fScale;
  f32 fThumbY  = (r.top + r.bottom) / 2.0f - fThumb / 2.0f;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  scNodeState eState = bSliding ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
  f32 fRad = scGetNodeCornerRadius(pNode, t->fControlRadius > 0.0f ? t->fControlRadius : 6.0f) * pGui->fScale;
  gpFillRound(pGraphics, r, fRad, gpColor(scGetNodeBackground(pNode, eState), 255));
  gpStrokeRound(pGraphics, r, fRad, gpColor(scGetNodeBorderColor(pNode, bHovered || bSliding), 255), scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);

  gpFillRound(pGraphics, rcTrack, fTrackR, gpColor(t->dwPillOff, 255));
  if (iThumbX > rcTrack.left) {
    RECT rcFill = { rcTrack.left, rcTrack.top, iThumbX, rcTrack.bottom };
    gpFillRound(pGraphics, rcFill, fTrackR, gpColor(t->dwAccent, 255));
  }
  gpFillEllipse(pGraphics, iThumbX - fThumb / 2.0f, fThumbY, fThumb, fThumb,
                gpColor(bSliding || bHovered ? t->dwAccentHover : t->dwAccent, 255));

  if (bEditing) {
    f32 fBoxRad = 4.0f * pGui->fScale;
    gpFillRound(pGraphics, rcValue, fBoxRad, gpColor(t->dwBackground, 255));
    gpStrokeRound(pGraphics, rcValue, fBoxRad, gpColor(t->dwAccent, 255), 1.5f);
    if (pGui->bEditFresh && szText.cx > 0) {
      RECT rcSel = { iTextRight - szText.cx - _scPx(pGui, 2.0f), rcValue.top + _scPx(pGui, 4.0f),
                     iTextRight + _scPx(pGui, 2.0f),             rcValue.bottom - _scPx(pGui, 4.0f) };
      gpFillRound(pGraphics, rcSel, 2.0f * pGui->fScale, gpColor(t->dwAccent, 110));
    }
  }
  gpGraphicsEnd(pGraphics);

  SetTextColor(hDC, scGetNodeTextColor(pNode, eState));
  RECT rLabel = { r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 10.0f), rcTrack.left - _scPx(pGui, 10.0f), r.bottom };
  DrawTextW(hDC, pNode->wszText, -1, &rLabel, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

  RECT rValue = { rcValue.left, rcValue.top, iTextRight, rcValue.bottom };
  DrawTextW(hDC, wszValue, -1, &rValue, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  if (bEditing && !pGui->bEditFresh) {
    RECT rcCaret = { iTextRight + _scPx(pGui, 1.0f), rcValue.top + _scPx(pGui, 5.0f),
                     iTextRight + _scPx(pGui, 2.0f) + 1,  rcValue.bottom - _scPx(pGui, 5.0f) };
    HBRUSH hBrush = CreateSolidBrush(t->dwAccent);
    FillRect(hDC, &rcCaret, hBrush);
    DeleteObject(hBrush);
  }
}

scInternal void
_scDrawScrollbar(scGui* pGui, HDC hDC, scScrollGeom sg, bool bHot) {
  if (!sg.bVisible) {
    return;
  }
  f32 fRad = (sg.rcTrack.right - sg.rcTrack.left) / 2.0f;
  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, sg.rcTrack, fRad, gpColor(pGui->theme.dwScrollThumb, 32));
  gpFillRound(pGraphics, sg.rcThumb, fRad, gpColor(pGui->theme.dwScrollThumb, bHot ? 230 : 150));
  gpGraphicsEnd(pGraphics);
}

scInternal void
_scDrawNodeStyleSurface(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scNodeState eState = pGui->pPressed == pNode ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
  COLORREF cBackground = eState == SC_NODE_STATE_ACTIVE ? pNode->style.dwActiveBackground
    : (eState == SC_NODE_STATE_HOVER ? pNode->style.dwHoverBackground : pNode->style.dwBackground);
  COLORREF cBorder = bHovered && pNode->style.dwHoverBorder != CLR_INVALID
    ? pNode->style.dwHoverBorder : pNode->style.dwBorder;
  bool bBackground = cBackground != CLR_INVALID || pNode->style.dwBackground != CLR_INVALID;
  bool bBorder = cBorder != CLR_INVALID;
  if (!bBackground && !bBorder) return;

  f32 fFallbackRadius = pGui->theme.fControlRadius > 0.0f ? pGui->theme.fControlRadius : 4.0f;
  f32 fRadius = scGetNodeCornerRadius(pNode, fFallbackRadius) * pGui->fScale;
  if (bBackground) {
    GpGraphics* pGraphics = gpGraphicsBegin(hDC);
    gpFillRound(pGraphics, r, fRadius, gpColor(scGetNodeBackground(pNode, eState), 255));
    gpGraphicsEnd(pGraphics);
  }
  if (bBorder) {
    GpGraphics* pGraphics = gpGraphicsBegin(hDC);
    gpStrokeRound(pGraphics, r, fRadius, gpColor(scGetNodeBorderColor(pNode, bHovered), 255),
                  scGetNodeBorderWidth(pNode, pGui->theme.fBorderWidth > 0.0f ? pGui->theme.fBorderWidth : 1.0f) * pGui->fScale);
    gpGraphicsEnd(pGraphics);
  }
}

scInternal void
_scPaintNode(scGui* pGui, HDC hDC, scNode* pNode, s32 iOffY, const RECT* pClip) {
  RECT r = pNode->rc;
  OffsetRect(&r, 0, -iOffY);

  RECT rcVisible;
  bool bVisible = IntersectRect(&rcVisible, &r, pClip) != 0;
  bool bHovered = pNode == pGui->pHover;

  switch (pNode->eType) {
    case SC_NODE_VSTACK:
    case SC_NODE_HSTACK: {
      if (pNode->bScrollable && !bVisible) {
        return;
      }
      scNodeState eState = pGui->pPressed == pNode ? SC_NODE_STATE_ACTIVE : (bHovered ? SC_NODE_STATE_HOVER : SC_NODE_STATE_NORMAL);
      bool bHasSurface = pNode->bSurface || scNodeHasBackgroundOverride(pNode, eState);
      bool bRounded = pNode->fCardRadius > 0.0f || pNode->style.fCornerRadius >= 0.0f;
      COLORREF cSurface = pNode->bSurface && !scNodeHasBackgroundOverride(pNode, eState)
        ? pGui->theme.dwPanel : scGetNodeBackground(pNode, eState);
      if (bHasSurface && bVisible && !bRounded) {
        HBRUSH hBrush = CreateSolidBrush(cSurface);
        if (hBrush) {
          FillRect(hDC, &r, hBrush);
          DeleteObject(hBrush);
        }
      }
      if (bRounded && bVisible && (bHasSurface || pNode->fCardRadius > 0.0f ||
          pNode->style.dwBorder != CLR_INVALID || pNode->style.dwHoverBorder != CLR_INVALID)) {
        GpGraphics* pGraphics = gpGraphicsBegin(hDC);
        f32 fBaseRadius = pNode->fCardRadius > 0.0f ? pNode->fCardRadius : 4.0f;
        f32 fRad = scGetNodeCornerRadius(pNode, fBaseRadius) * pGui->fScale;
        if (bHasSurface || pNode->fCardRadius > 0.0f) gpFillRound(pGraphics, r, fRad, gpColor(cSurface, 255));
        if (pNode->fCardRadius > 0.0f || pNode->style.dwBorder != CLR_INVALID || pNode->style.dwHoverBorder != CLR_INVALID) {
          gpStrokeRound(pGraphics, r, fRad, gpColor(scGetNodeBorderColor(pNode, bHovered), 255), scGetNodeBorderWidth(pNode, pGui->theme.fBorderWidth > 0.0f ? pGui->theme.fBorderWidth : 1.0f) * pGui->fScale);
        }
        gpGraphicsEnd(pGraphics);
      }

      s32 iSaved    = 0;
      s32 iChildOff = iOffY;
      if (pNode->bScrollable) {
        iSaved     = SaveDC(hDC);
        iChildOff += pNode->iScrollY;
        IntersectClipRect(hDC, r.left, r.top, r.right, r.bottom);
      }
      for (scNode* p = pNode->pFirst; p; p = p->pNext) {
        _scPaintNode(pGui, hDC, p, iChildOff, pClip);
      }
      if (pNode->bScrollable) {
        RestoreDC(hDC, iSaved);
        bool bHot = pGui->pHotScroll == pNode || (pGui->drag.bActive && pGui->drag.pValue == &pNode->iScrollY);
        _scDrawScrollbar(pGui, hDC, _scNodeScrollGeom(pGui, pNode), bHot);
      }
      if (pNode->fOutlineRadius > 0.0f && bVisible) {
        GpGraphics* pGraphics = gpGraphicsBegin(hDC);
        gpStrokeRound(pGraphics, r, pNode->fOutlineRadius * pGui->fScale, gpColor(scGetNodeBorderColor(pNode, false), 255), scGetNodeBorderWidth(pNode, pGui->theme.fBorderWidth > 0.0f ? pGui->theme.fBorderWidth : 1.0f) * pGui->fScale);
        gpGraphicsEnd(pGraphics);
      }
      return;
    }
    default:
      break;
  }

  if (!bVisible) {
    return;
  }

  switch (pNode->eType) {
    case SC_NODE_DIVIDER: {
      HBRUSH hBrush = CreateSolidBrush(pNode->style.dwBackground != CLR_INVALID
        ? scGetNodeBackground(pNode, SC_NODE_STATE_NORMAL) : pGui->theme.dwStrokeSoft);
      FillRect(hDC, &r, hBrush);
      DeleteObject(hBrush);
      break;
    }
    case SC_NODE_LABEL:
      _scDrawNodeStyleSurface(pGui, hDC, pNode, r, bHovered);
      _scDrawLabel(pGui, hDC, pNode, r, bHovered);
      break;
    case SC_NODE_FIELD:    _scDrawField   (pGui, hDC, pNode, r);           break;
    case SC_NODE_BUTTON:   _scDrawButton  (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_LINK:
      _scDrawNodeStyleSurface(pGui, hDC, pNode, r, bHovered);
      _scDrawLink(pGui, hDC, pNode, r, bHovered);
      break;
    case SC_NODE_TAB:      _scDrawTab     (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_TOGGLE:   _scDrawToggle  (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_SLIDER:   _scDrawSlider  (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_DROPDOWN: _scDrawDropdown(pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_CUSTOM: {
      _scDrawNodeStyleSurface(pGui, hDC, pNode, r, bHovered);
      if (pNode->u.custom.pfnDraw) {
        pNode->u.custom.pfnDraw(pNode, hDC, r, rcVisible, bHovered);
      }
      break;
    }
    default:
      break;
  }
}

scInternal void
_scPaintPopup(scGui* pGui, HDC hDC) {
  scNode* pNode = pGui->pExpanded;
  if (!pNode) {
    return;
  }

  scGuiTheme* t = &pGui->theme;
  scPopupGeom pg = _scPopupGeom(pGui, pNode);
  f32 fRad = scGetNodeCornerRadius(pNode, t->fPanelRadius > 0.0f ? t->fPanelRadius : 6.0f) * pGui->fScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, pg.rcBox, fRad, gpColor(t->dwPopup, 255));
  gpStrokeRound(pGraphics, pg.rcBox, fRad, gpColor(scGetNodeBorderColor(pNode, false), 255), scGetNodeBorderWidth(pNode, t->fBorderWidth > 0.0f ? t->fBorderWidth : 1.0f) * pGui->fScale);
  gpGraphicsEnd(pGraphics);

  s32 iSaved = SaveDC(hDC);
  HRGN hRgn = CreateRoundRectRgn(pg.rcBox.left, pg.rcBox.top, pg.rcBox.right + 1, pg.rcBox.bottom + 1, (int)(fRad * 2), (int)(fRad * 2));
  ExtSelectClipRgn(hDC, hRgn, RGN_AND);

  pGraphics = gpGraphicsBegin(hDC);
  for (s32 i = 0; i < pNode->u.dropdown.nOptionCount; ++i) {
    s32 iTop    = pg.rcBox.top + i * pg.iItemH - pGui->iPopupScroll;
    s32 iBottom = iTop + pg.iItemH;
    if (iBottom <= pg.rcBox.top || iTop >= pg.rcBox.bottom) {
      continue;
    }
    if (pGui->iPopupHover == i) {
      RECT rcItem = { pg.rcBox.left + _scPx(pGui, 3.0f), iTop + _scPx(pGui, 1.0f), pg.rcBox.right - _scPx(pGui, pg.bHasScroll ? 11.0f : 3.0f), iBottom - _scPx(pGui, 1.0f) };
      gpFillRound(pGraphics, rcItem, (t->fControlRadius > 0.0f ? t->fControlRadius : 4.0f) * pGui->fScale,
                  gpColor(scGetNodeBackground(pNode, SC_NODE_STATE_HOVER), 255));
    }
    if (pNode->u.dropdown.iSelected == i) {
      s32 iCy = (iTop + iBottom) / 2;
      RECT rcInd = { pg.rcBox.left + _scPx(pGui, 3.0f), iCy - _scPx(pGui, 7.0f), pg.rcBox.left + _scPx(pGui, 6.0f), iCy + _scPx(pGui, 7.0f) };
      gpFillRound(pGraphics, rcInd, 1.5f * pGui->fScale, gpColor(t->dwAccent, 255));
    }
  }
  if (pg.bHasScroll && pg.scroll.bVisible) {
    bool bHot = pGui->bPopupScrollHot || pGui->drag.bActive;
    f32 fScrollRad = (pg.scroll.rcTrack.right - pg.scroll.rcTrack.left) / 2.0f;
    gpFillRound(pGraphics, pg.scroll.rcTrack, fScrollRad, gpColor(t->dwScrollThumb, 32));
    gpFillRound(pGraphics, pg.scroll.rcThumb, fScrollRad, gpColor(t->dwScrollThumb, bHot ? 230 : 150));
  }
  gpGraphicsEnd(pGraphics);

  HFONT hFont = scGetNodeFont(pNode, SC_FONT_BODY);
  SelectObject(hDC, hFont ? hFont : t->hFont);
  SetTextColor(hDC, scGetNodeTextColor(pNode, SC_NODE_STATE_NORMAL));
  for (s32 i = 0; i < pNode->u.dropdown.nOptionCount; ++i) {
    s32 iTop    = pg.rcBox.top + i * pg.iItemH - pGui->iPopupScroll;
    s32 iBottom = iTop + pg.iItemH;
    if (iBottom <= pg.rcBox.top || iTop >= pg.rcBox.bottom) {
      continue;
    }
    wchar_t wszOption[128] = { 0 };
    if (MultiByteToWideChar(CP_UTF8, 0, pNode->u.dropdown.aOptions[i], -1, wszOption, ARRAYSIZE(wszOption)) > 0) {
      RECT rcOption = { pg.rcBox.left + _scPx(pGui, 12.0f), iTop + _scPx(pGui, 2.0f), pg.rcBox.right - _scPx(pGui, pg.bHasScroll ? 12.0f : 5.0f), iBottom - _scPx(pGui, 2.0f) };
      DrawTextW(hDC, wszOption, -1, &rcOption, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
  }

  RestoreDC(hDC, iSaved);
  DeleteObject(hRgn);
}

void scGuiPaint(scGui* pGui, HDC hDC, const RECT* pClip) {
  if (!pGui || !pGui->pRoot) {
    return;
  }
  _scEnsureLayout(pGui);
  SetBkMode(hDC, TRANSPARENT);
  _scPaintNode(pGui, hDC, pGui->pRoot, 0, pClip);
  _scPaintPopup(pGui, hDC);
}

//------------------------------------------------------------------------
// Input
//------------------------------------------------------------------------
scInternal void
_scActivate(scGui* pGui, scNode* pNode, POINT pt) {
  switch (pNode->eType) {
    case SC_NODE_BUTTON:
    case SC_NODE_LINK:
    case SC_NODE_TAB: {
      void (*pfnClick)(s32 iParam) = pNode->u.button.pfnClick;
      s32 iParam = pNode->u.button.iParam;
      if (pfnClick) {
        pfnClick(iParam);
      }
      break;
    }
    case SC_NODE_TOGGLE: {
      bool* pValue = pNode->u.toggle.pValue;
      if (!pValue) {
        break;
      }
      *pValue = !*pValue;
      bool bValue = *pValue;
      void (*pfnChange)(bool bValue, s32 iParam) = pNode->u.toggle.pfnChange;
      s32 iParam = pNode->u.toggle.iParam;
      _scInvalidateNode(pGui, pNode);
      if (pfnChange) {
        pfnChange(bValue, iParam);
      }
      if (pGui->host.pfnValueChanged) {
        pGui->host.pfnValueChanged(pGui->host.pUser);
      }
      break;
    }
    case SC_NODE_DROPDOWN: {
      RECT box = _scDropdownBox(pGui, pNode, _scScreenRect(pNode));
      if (PtInRect(&box, pt) && pNode->u.dropdown.nOptionCount > 0) {
        pGui->pExpanded       = pNode;
        pGui->iPopupScroll    = 0;
        pGui->iPopupHover     = -1;
        pGui->bPopupScrollHot = false;
        _scInvalidate(pGui, NULL);
      }
      break;
    }
    case SC_NODE_CUSTOM: {
      void (*pfnClick)(scNode* pNode) = pNode->u.custom.pfnClick;
      if (pfnClick) {
        pfnClick(pNode);
      }
      break;
    }
    default:
      break;
  }
}

void scGuiMouseMove(scGui* pGui, POINT pt) {
  if (!pGui || !pGui->pRoot) {
    return;
  }
  _scEnsureLayout(pGui);

  if (pGui->drag.bActive) {
    s32 iDelta  = pt.y - pGui->drag.iStartY;
    s32 iScroll = pGui->drag.iStartScroll + (pGui->drag.iTrackRange > 0 ? (s32)((s64)iDelta * pGui->drag.iMaxScroll / pGui->drag.iTrackRange) : 0);
    iScroll = _scClamp(iScroll, 0, pGui->drag.iMaxScroll);
    if (iScroll != *pGui->drag.pValue) {
      *pGui->drag.pValue = iScroll;
      _scInvalidate(pGui, NULL);
    }
    return;
  }

  if (pGui->pSliding) {
    _scSliderApplyFromX(pGui, pGui->pSliding, pt.x);
    return;
  }

  scNode* pPressed = pGui->pPressed;
  if (pPressed && pPressed->eType == SC_NODE_CUSTOM && pPressed->u.custom.pfnDrag &&
      (abs(pt.x - pGui->ptPress.x) > GetSystemMetrics(SM_CXDRAG) ||
       abs(pt.y - pGui->ptPress.y) > GetSystemMetrics(SM_CYDRAG))) {
    pGui->pPressed = NULL;
    _scCapture(pGui, false);
    pPressed->u.custom.pfnDrag(pPressed);
    return;
  }

  if (pGui->pExpanded) {
    scPopupGeom pg = _scPopupGeom(pGui, pGui->pExpanded);
    s32  iPrevHover = pGui->iPopupHover;
    bool bPrevHot   = pGui->bPopupScrollHot;

    pGui->bPopupScrollHot = pg.bHasScroll && PtInRect(&pg.scroll.rcThumb, pt);
    pGui->iPopupHover     = -1;
    if (!pGui->bPopupScrollHot && PtInRect(&pg.rcBox, pt)) {
      s32 iIndex = (pt.y - pg.rcBox.top + pGui->iPopupScroll) / pg.iItemH;
      if (iIndex >= 0 && iIndex < pGui->pExpanded->u.dropdown.nOptionCount) {
        pGui->iPopupHover = iIndex;
      }
    }
    if (pGui->iPopupHover != iPrevHover || pGui->bPopupScrollHot != bPrevHot) {
      _scInvalidate(pGui, &pg.rcBox);
    }
    return;
  }

  scNode* pScroller  = _scScrollerAt(pGui->pRoot, pt, 0);
  scNode* pHotScroll = NULL;
  if (pScroller) {
    scScrollGeom sg = _scNodeScrollGeom(pGui, pScroller);
    if (sg.bVisible && PtInRect(&sg.rcThumb, pt)) {
      pHotScroll = pScroller;
    }
  }
  if (pHotScroll != pGui->pHotScroll) {
    _scInvalidateScrollbar(pGui, pGui->pHotScroll);
    _scInvalidateScrollbar(pGui, pHotScroll);
    pGui->pHotScroll = pHotScroll;
  }

  scNode* pHit = pHotScroll ? NULL : _scHit(pGui->pRoot, pt, 0);
  if (pHit != pGui->pHover) {
    _scInvalidateNode(pGui, pGui->pHover);
    _scInvalidateNode(pGui, pHit);
    pGui->pHover = pHit;
  }
}

void scGuiMouseLeave(scGui* pGui) {
  if (!pGui) {
    return;
  }
  _scInvalidateNode(pGui, pGui->pHover);
  _scInvalidateScrollbar(pGui, pGui->pHotScroll);
  pGui->pHover    = NULL;
  pGui->pHotScroll = NULL;
}

void scGuiMouseDown(scGui* pGui, POINT pt) {
  if (!pGui || !pGui->pRoot) {
    return;
  }
  _scEnsureLayout(pGui);

  if (pGui->pEditing) {
    RECT rcTrack, rcValue;
    _scSliderRects(pGui, pGui->pEditing, _scScreenRect(pGui->pEditing), &rcTrack, &rcValue);
    if (PtInRect(&rcValue, pt)) {
      return;
    }
    _scEndEdit(pGui, true);
    if (!pGui->pRoot) {
      return;
    }
    _scEnsureLayout(pGui);
  }

  if (pGui->pExpanded) {
    scNode* pNode = pGui->pExpanded;
    scPopupGeom pg = _scPopupGeom(pGui, pNode);

    if (pg.bHasScroll && _scScrollbarHit(pGui, pg.scroll, pt)) {
      _scBeginDrag(pGui, &pGui->iPopupScroll, pg.scroll, pg.iMaxScroll, pt);
      return;
    }

    s32 iIndex = -1;
    if (PtInRect(&pg.rcBox, pt)) {
      iIndex = (pt.y - pg.rcBox.top + pGui->iPopupScroll) / pg.iItemH;
      if (iIndex < 0 || iIndex >= pNode->u.dropdown.nOptionCount) {
        iIndex = -1;
      }
    }

    void (*pfnSelect)(s32 iIndex, s32 iParam) = pNode->u.dropdown.pfnSelect;
    s32 iParam = pNode->u.dropdown.iParam;
    if (iIndex >= 0) {
      pNode->u.dropdown.iSelected = iIndex;
    }

    pGui->pExpanded       = NULL;
    pGui->iPopupHover     = -1;
    pGui->bPopupScrollHot = false;
    _scInvalidate(pGui, NULL);

    if (iIndex >= 0 && pfnSelect) {
      pfnSelect(iIndex, iParam);
    }
    return;
  }

  scNode* pScroller = _scScrollerAt(pGui->pRoot, pt, 0);
  if (pScroller) {
    scScrollGeom sg = _scNodeScrollGeom(pGui, pScroller);
    if (_scScrollbarHit(pGui, sg, pt)) {
      _scBeginDrag(pGui, &pScroller->iScrollY, sg, pScroller->iMaxScroll, pt);
      return;
    }
  }

  scNode* pHit = _scHit(pGui->pRoot, pt, 0);
  if (pHit && pHit->eType == SC_NODE_SLIDER) {
    _scSliderMouseDown(pGui, pHit, pt);
    return;
  }
  if (pHit) {
    pGui->pPressed = pHit;
    pGui->ptPress  = pt;
    _scCapture(pGui, true);
  }
}

void scGuiMouseUp(scGui* pGui, POINT pt) {
  if (!pGui) {
    return;
  }

  if (pGui->drag.bActive) {
    pGui->drag.bActive = false;
    _scCapture(pGui, false);
    _scInvalidate(pGui, NULL);
    return;
  }

  if (pGui->pSliding) {
    pGui->pSliding = NULL;
    _scCapture(pGui, false);
    if (pGui->host.pfnValueChanged) {
      pGui->host.pfnValueChanged(pGui->host.pUser);
    }
    return;
  }

  scNode* pPressed = pGui->pPressed;
  if (!pPressed) {
    return;
  }
  pGui->pPressed = NULL;
  _scCapture(pGui, false);

  _scEnsureLayout(pGui);
  if (pGui->pRoot && _scHit(pGui->pRoot, pt, 0) == pPressed) {
    _scActivate(pGui, pPressed, pt);
  }
}

void scGuiMouseRightUp(scGui* pGui, POINT pt) {
  if (!pGui || !pGui->pRoot || pGui->pExpanded) {
    return;
  }
  _scEnsureLayout(pGui);
  scNode* pHit = _scHit(pGui->pRoot, pt, 0);
  if (pHit && pHit->eType == SC_NODE_CUSTOM && pHit->u.custom.pfnContext) {
    pHit->u.custom.pfnContext(pHit, pt);
  }
}

void scGuiMouseWheel(scGui* pGui, POINT pt, s32 iNotches) {
  if (!pGui || !pGui->pRoot) {
    return;
  }
  _scEnsureLayout(pGui);

  if (pGui->pExpanded) {
    scPopupGeom pg = _scPopupGeom(pGui, pGui->pExpanded);
    pGui->iPopupScroll = _scClamp(pGui->iPopupScroll - iNotches * pg.iItemH, 0, pg.iMaxScroll);
    _scInvalidate(pGui, &pg.rcBox);
    return;
  }

  scNode* pScroller = _scScrollerAt(pGui->pRoot, pt, 0);
  if (!pScroller || pScroller->iMaxScroll <= 0) {
    return;
  }
  pScroller->iScrollY = _scClamp(pScroller->iScrollY - iNotches * _scPx(pGui, 40.0f), 0, pScroller->iMaxScroll);
  _scInvalidateNode(pGui, pScroller);
}

bool scGuiChar(scGui* pGui, wchar_t wc) {
  if (!pGui || !pGui->pEditing) {
    return false;
  }
  if (wc == L',') {
    wc = L'.';
  }
  bool bAllowed = (wc >= L'0' && wc <= L'9') || wc == L'.' || wc == L'-';
  if (!bAllowed) {
    return true;
  }
  if (pGui->bEditFresh) {
    pGui->nEdit      = 0;
    pGui->bEditFresh = false;
  }
  if (pGui->nEdit < (s32)ARRAYSIZE(pGui->wszEdit) - 1) {
    pGui->wszEdit[pGui->nEdit++] = wc;
    pGui->wszEdit[pGui->nEdit]   = 0;
  }
  _scInvalidateNode(pGui, pGui->pEditing);
  return true;
}

bool scGuiKeyDown(scGui* pGui, u32 uVk) {
  if (!pGui || !pGui->pEditing) {
    return false;
  }
  switch (uVk) {
    case VK_RETURN:
      _scEndEdit(pGui, true);
      return true;
    case VK_ESCAPE:
      _scEndEdit(pGui, false);
      return true;
    case VK_BACK:
      if (pGui->bEditFresh) {
        pGui->nEdit      = 0;
        pGui->bEditFresh = false;
      } else if (pGui->nEdit > 0) {
        --pGui->nEdit;
      }
      pGui->wszEdit[pGui->nEdit] = 0;
      _scInvalidateNode(pGui, pGui->pEditing);
      return true;
    case VK_DELETE:
      pGui->nEdit      = 0;
      pGui->bEditFresh = false;
      pGui->wszEdit[0] = 0;
      _scInvalidateNode(pGui, pGui->pEditing);
      return true;
    default:
      return false;
  }
}

void scGuiFocusLost(scGui* pGui) {
  if (pGui) {
    _scEndEdit(pGui, true);
  }
}

bool scGuiHitClickable(scGui* pGui, POINT pt) {
  if (!pGui || !pGui->pRoot || pGui->pExpanded) {
    return false;
  }
  _scEnsureLayout(pGui);
  scNode* pHit = _scHit(pGui->pRoot, pt, 0);
  return pHit && _scIsClickable(pHit);
}

void scGuiCaptureLost(scGui* pGui) {
  if (!pGui) {
    return;
  }
  pGui->drag.bActive = false;
  pGui->pSliding     = NULL;
  pGui->pPressed     = NULL;
}

//------------------------------------------------------------------------
// Gui
//------------------------------------------------------------------------
scGui* scGuiCreate(const scGuiHost* pHost) {
  scGui* pGui = (scGui*)calloc(1, sizeof(scGui));
  if (!pGui) return NULL;
  if (pHost) {
    pGui->host = *pHost;
  }
  pGui->fScale       = 1.0f;
  pGui->iPopupHover  = -1;
  pGui->bLayoutDirty = true;
  return pGui;
}

void scGuiDestroy(scGui* pGui) {
  if (!pGui) {
    return;
  }
  if (pGui->pRoot) {
    _scFreeNode(pGui->pRoot);
  }
  free(pGui);
}

void scGuiSetTheme(scGui* pGui, const scGuiTheme* pTheme) {
  if (!pGui || !pTheme) return;
  pGui->theme = *pTheme;
  scGuiInvalidateLayout(pGui);
}

void scGuiSetScale(scGui* pGui, f32 fScale) {
  if (pGui && fScale > 0.0f && pGui->fScale != fScale) {
    pGui->fScale = fScale;
    scGuiInvalidateLayout(pGui);
  }
}

void scGuiSetRoot(scGui* pGui, scNode* pRoot) {
  if (!pGui) return;
  if (pGui->drag.bActive) {
    pGui->drag.bActive = false;
    _scCapture(pGui, false);
  }
  if (pGui->pSliding) {
    pGui->pSliding = NULL;
    _scCapture(pGui, false);
  }
  pGui->pEditing        = NULL;
  pGui->pLastClick      = NULL;
  pGui->pPressed        = NULL;
  pGui->pHover          = NULL;
  pGui->pHotScroll      = NULL;
  pGui->pExpanded       = NULL;
  pGui->iPopupHover     = -1;
  pGui->bPopupScrollHot = false;

  scNode* pOld = pGui->pRoot;
  pGui->pRoot = pRoot;
  if (pRoot) {
    pRoot->pGui = pGui;
  }
  if (pOld && pOld != pRoot) {
    _scFreeNode(pOld);
  }
  scGuiInvalidateLayout(pGui);
}

void scGuiResize(scGui* pGui, s32 iWidth, s32 iHeight) {
  if (!pGui || (pGui->iWidth == iWidth && pGui->iHeight == iHeight)) {
    return;
  }
  pGui->iWidth    = iWidth;
  pGui->iHeight   = iHeight;
  pGui->pPressed  = NULL;
  pGui->pExpanded = NULL;
  pGui->iPopupHover = -1;
  scGuiInvalidateLayout(pGui);
}

void scGuiInvalidate(scGui* pGui) {
  if (pGui) _scInvalidate(pGui, NULL);
}

void scGuiInvalidateLayout(scGui* pGui) {
  if (!pGui) return;
  pGui->bLayoutDirty = true;
  _scInvalidate(pGui, NULL);
}
