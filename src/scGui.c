#include "pch.h"
#include "scTypes.h"
#include "scGdiPlus.h"
#include "scGui.h"

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

//------------------------------------------------------------------------
// Nodes
//------------------------------------------------------------------------
scLength scLengthAuto()                 { return (scLength){ SC_LENGTH_AUTO,    0.0f }; }
scLength scLengthPixels(f32 fPixels)    { return (scLength){ SC_LENGTH_PIXELS,  fPixels }; }
scLength scLengthPercent(f32 fFraction) { return (scLength){ SC_LENGTH_PERCENT, fFraction }; }
scLength scLengthStretch(f32 fWeight)   { return (scLength){ SC_LENGTH_STRETCH, fWeight }; }

scInternal scNode*
_scNewNode(scNodeType eType, scLength width, scLength height, const wchar_t* wszText) {
  scNode* pNode = (scNode*)calloc(1, sizeof(scNode));
  pNode->eType   = eType;
  pNode->width   = width;
  pNode->height  = height;
  pNode->wszText = _wcsdup(wszText ? wszText : L"");
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
  pNode->u.label.bBold = bBold;
  return pNode;
}

scNode* scCreateField(const wchar_t* wszText) {
  return _scNewNode(SC_NODE_FIELD, scLengthStretch(1.0f), scLengthPixels(30), wszText);
}

scNode* scCreateButton(const wchar_t* wszText, void (*pfnClick)(s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_BUTTON, scLengthAuto(), scLengthAuto(), wszText);
  pNode->u.button.pfnClick = pfnClick;
  pNode->u.button.iParam   = iParam;
  return pNode;
}

scNode* scCreateLink(const wchar_t* wszText, void (*pfnClick)(s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_LINK, scLengthStretch(1.0f), scLengthPixels(30), wszText);
  pNode->u.button.pfnClick = pfnClick;
  pNode->u.button.iParam   = iParam;
  return pNode;
}

scNode* scCreateTab(const wchar_t* wszText, bool bActive, void (*pfnClick)(s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_TAB, scLengthStretch(1.0f), scLengthPixels(30), wszText);
  pNode->u.button.pfnClick = pfnClick;
  pNode->u.button.iParam   = iParam;
  pNode->u.button.bActive  = bActive;
  return pNode;
}

scNode* scCreateToggle(const wchar_t* wszText, bool* pValue, s32 iParam, void (*pfnChange)(bool bValue, s32 iParam)) {
  scNode* pNode = _scNewNode(SC_NODE_TOGGLE, scLengthStretch(1.0f), scLengthPixels(40), wszText);
  pNode->u.toggle.pValue    = pValue;
  pNode->u.toggle.iParam    = iParam;
  pNode->u.toggle.pfnChange = pfnChange;
  return pNode;
}

scNode* scCreateDropdown(const wchar_t* wszLabel, const char* const* aOptions, s32 nOptionCount, s32 iSelected, void (*pfnSelect)(s32 iIndex, s32 iParam), s32 iParam) {
  scNode* pNode = _scNewNode(SC_NODE_DROPDOWN, scLengthStretch(1.0f), scLengthPixels(40), wszLabel);
  pNode->u.dropdown.aOptions     = aOptions;
  pNode->u.dropdown.nOptionCount = nOptionCount;
  pNode->u.dropdown.iSelected    = iSelected;
  pNode->u.dropdown.iParam       = iParam;
  pNode->u.dropdown.pfnSelect    = pfnSelect;
  return pNode;
}

scNode* scCreateCustom(scLength width, scLength height, const scCustomOps* pOps, const void* pUser, s32 nUserSize) {
  scNode* pNode = _scNewNode(SC_NODE_CUSTOM, width, height, NULL);
  if (pOps) {
    pNode->u.custom = *pOps;
  }
  if (pUser && nUserSize > 0) {
    pNode->pUser = malloc(nUserSize);
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
  return pNode->pUser;
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
      HFONT hFont = pNode->u.label.bBold ? t->hBoldFont : t->hFont;
      sz = _scTextSize(pGui, hFont, pNode->wszText, pNode->bWrap, iAvailW);
      break;
    }
    default: {
      sz = _scTextSize(pGui, t->hFont, pNode->wszText, false, 0);
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
// Drawing
//------------------------------------------------------------------------
scInternal void
_scDrawLabel(scGui* pGui, HDC hDC, scNode* pNode, RECT r) {
  scGuiTheme* t = &pGui->theme;
  COLORREF dwColor = t->dwText;
  if (pNode->eTextRole == SC_TEXT_DIM) {
    dwColor = t->dwTextDim;
  } else if (pNode->eTextRole == SC_TEXT_FAINT) {
    dwColor = t->dwTextFaint;
  }
  SelectObject(hDC, pNode->u.label.bBold ? t->hBoldFont : t->hFont);
  SetTextColor(hDC, dwColor);
  if (pNode->bEllipsis) {
    DrawTextW(hDC, pNode->wszText, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  } else if (pNode->bWrap) {
    DrawTextW(hDC, pNode->wszText, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  } else {
    TextOutW(hDC, r.left, r.top, pNode->wszText, lstrlenW(pNode->wszText));
  }
}

scInternal void
_scDrawField(scGui* pGui, HDC hDC, scNode* pNode, RECT r) {
  scGuiTheme* t = &pGui->theme;
  f32 fRad = 4.0f * pGui->fScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(t->dwCard, 255));
  gpStrokeRound(pGraphics, r, fRad, gpColor(t->dwStroke, 255), 1.0f);
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->hFont);
  SetTextColor(hDC, t->dwText);
  RECT rText = { r.left + _scPx(pGui, 10.0f), r.top, r.right - _scPx(pGui, 10.0f), r.bottom };
  DrawTextW(hDC, pNode->wszText, -1, &rText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
}

scInternal void
_scDrawButton(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  f32 fRad = 4.0f * pGui->fScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, fRad, gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  gpStrokeRound(pGraphics, r, fRad, gpColor(bHovered ? t->dwAccent : t->dwStroke, 255), 1.0f);
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->hFont);
  SetTextColor(hDC, t->dwText);
  DrawTextW(hDC, pNode->wszText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

scInternal void
_scDrawLink(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  SelectObject(hDC, t->hFont);
  SetTextColor(hDC, bHovered ? t->dwAccent : t->dwTextFaint);
  DrawTextW(hDC, pNode->wszText, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

scInternal void
_scDrawTab(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bActive = pNode->u.button.bActive;

  if (bActive || bHovered) {
    GpGraphics* pGraphics = gpGraphicsBegin(hDC);
    gpFillRound(pGraphics, r, 5.0f * pGui->fScale, gpColor(bActive ? t->dwCardHover : t->dwCard, 255));
    if (bActive) {
      s32 iCenterY = (r.top + r.bottom) / 2;
      RECT rcInd = { r.left + _scPx(pGui, 3.0f), iCenterY - _scPx(pGui, 8.0f), r.left + _scPx(pGui, 6.0f), iCenterY + _scPx(pGui, 8.0f) };
      gpFillRound(pGraphics, rcInd, 1.5f * pGui->fScale, gpColor(t->dwAccent, 255));
    }
    gpGraphicsEnd(pGraphics);
  }

  SelectObject(hDC, bActive ? t->hBoldFont : t->hFont);
  SetTextColor(hDC, bActive ? t->dwText : t->dwTextDim);
  TextOutW(hDC, r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 7.0f), pNode->wszText, lstrlenW(pNode->wszText));
}

scInternal void
_scDrawToggle(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bOn = pNode->u.toggle.pValue && *pNode->u.toggle.pValue;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, r, _scRoundRad(pGui), gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
  gpStrokeRound(pGraphics, r, _scRoundRad(pGui), gpColor(t->dwStrokeSoft, 255), 1.0f);

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

  SelectObject(hDC, t->hFont);
  SetTextColor(hDC, t->dwText);
  TextOutW(hDC, r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 10.0f), pNode->wszText, lstrlenW(pNode->wszText));
}

scInternal void
_scDrawDropdown(scGui* pGui, HDC hDC, scNode* pNode, RECT r, bool bHovered) {
  scGuiTheme* t = &pGui->theme;
  bool bExpanded = pGui->pExpanded == pNode;
  bool bCompact  = pNode->wszText[0] == 0;
  RECT box = _scDropdownBox(pGui, pNode, r);

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  if (!bCompact) {
    gpFillRound(pGraphics, r, _scRoundRad(pGui), gpColor(bHovered ? t->dwCardHover : t->dwCard, 255));
    gpStrokeRound(pGraphics, r, _scRoundRad(pGui), gpColor(t->dwStrokeSoft, 255), 1.0f);
  }

  f32 fBoxRad = 4.0f * pGui->fScale;
  gpFillRound(pGraphics, box, fBoxRad, gpColor(t->dwBackground, 255));
  gpStrokeRound(pGraphics, box, fBoxRad, gpColor((bExpanded || bHovered) ? t->dwAccent : t->dwStroke, 255), bExpanded ? 1.5f : 1.0f);
  gpGraphicsEnd(pGraphics);

  SelectObject(hDC, t->hFont);
  SetTextColor(hDC, t->dwText);
  if (!bCompact) {
    TextOutW(hDC, r.left + _scPx(pGui, 15.0f), r.top + _scPx(pGui, 10.0f), pNode->wszText, lstrlenW(pNode->wszText));
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
      if (pNode->bSurface && bVisible) {
        HBRUSH hBrush = CreateSolidBrush(pGui->theme.dwPanel);
        FillRect(hDC, &r, hBrush);
        DeleteObject(hBrush);
      }
      if (pNode->fCardRadius > 0.0f && bVisible) {
        GpGraphics* pGraphics = gpGraphicsBegin(hDC);
        gpFillRound(pGraphics, r, pNode->fCardRadius * pGui->fScale, gpColor(pGui->theme.dwCard, 255));
        gpStrokeRound(pGraphics, r, pNode->fCardRadius * pGui->fScale, gpColor(pGui->theme.dwStrokeSoft, 255), 1.0f);
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
        gpStrokeRound(pGraphics, r, pNode->fOutlineRadius * pGui->fScale, gpColor(pGui->theme.dwStroke, 255), 1.0f);
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
      HBRUSH hBrush = CreateSolidBrush(pGui->theme.dwStrokeSoft);
      FillRect(hDC, &r, hBrush);
      DeleteObject(hBrush);
      break;
    }
    case SC_NODE_LABEL:    _scDrawLabel   (pGui, hDC, pNode, r);           break;
    case SC_NODE_FIELD:    _scDrawField   (pGui, hDC, pNode, r);           break;
    case SC_NODE_BUTTON:   _scDrawButton  (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_LINK:     _scDrawLink    (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_TAB:      _scDrawTab     (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_TOGGLE:   _scDrawToggle  (pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_DROPDOWN: _scDrawDropdown(pGui, hDC, pNode, r, bHovered); break;
    case SC_NODE_CUSTOM: {
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
  f32 fRad = 6.0f * pGui->fScale;

  GpGraphics* pGraphics = gpGraphicsBegin(hDC);
  gpFillRound(pGraphics, pg.rcBox, fRad, gpColor(t->dwPopup, 255));
  gpStrokeRound(pGraphics, pg.rcBox, fRad, gpColor(t->dwStroke, 255), 1.0f);
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
      gpFillRound(pGraphics, rcItem, 4.0f * pGui->fScale, gpColor(t->dwCardHover, 255));
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

  SelectObject(hDC, t->hFont);
  SetTextColor(hDC, t->dwText);
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
  pGui->pPressed     = NULL;
}

//------------------------------------------------------------------------
// Gui
//------------------------------------------------------------------------
scGui* scGuiCreate(const scGuiHost* pHost) {
  scGui* pGui = (scGui*)calloc(1, sizeof(scGui));
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
  pGui->theme = *pTheme;
  scGuiInvalidateLayout(pGui);
}

void scGuiSetScale(scGui* pGui, f32 fScale) {
  if (pGui->fScale != fScale) {
    pGui->fScale = fScale;
    scGuiInvalidateLayout(pGui);
  }
}

void scGuiSetRoot(scGui* pGui, scNode* pRoot) {
  if (pGui->drag.bActive) {
    pGui->drag.bActive = false;
    _scCapture(pGui, false);
  }
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
  if (pGui) {
    _scInvalidate(pGui, NULL);
  }
}

void scGuiInvalidateLayout(scGui* pGui) {
  pGui->bLayoutDirty = true;
  _scInvalidate(pGui, NULL);
}
