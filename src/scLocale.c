#include "pch.h"
#include "scLocale.h"
#include "scApp.h"
#include "scLogging.h"
#include "embed/locales_data.h"

#include <string.h>

#define SC_LOC_MAX_LANGS   64
#define SC_LOC_MAX_ENTRIES 256
#define SC_LOC_CODE_LEN    16
#define SC_LOC_KEY_LEN     128
#define SC_LOC_VALUE_LEN   256

typedef struct {
  char    szKey[SC_LOC_KEY_LEN];
  wchar_t wszValue[SC_LOC_VALUE_LEN];
} scLocEntry;

static struct {
  char        aLangs[SC_LOC_MAX_LANGS][SC_LOC_CODE_LEN];
  const char* aLangPtrs[SC_LOC_MAX_LANGS];
  s32         nLangCount;

  scLocEntry  aEntries[SC_LOC_MAX_ENTRIES];
  s32         nEntryCount;
} gLoc;

//------------------------------------------------------------------------
// Loading
//------------------------------------------------------------------------
scInternal void
_scLocAddLang(const char* szCode) {
  for (s32 i = 0; i < gLoc.nLangCount; ++i) {
    if (strcmp(gLoc.aLangs[i], szCode) == 0) {
      return;
    }
  }
  if (gLoc.nLangCount < SC_LOC_MAX_LANGS) {
    strcpy_s(gLoc.aLangs[gLoc.nLangCount], SC_LOC_CODE_LEN, szCode);
    gLoc.aLangPtrs[gLoc.nLangCount] = gLoc.aLangs[gLoc.nLangCount];
    ++gLoc.nLangCount;
  }
}

scInternal void
_scLocLoad() {
  gLoc.nLangCount  = 0;
  gLoc.nEntryCount = 0;
  _scLocAddLang("en"); // english is the fallback; keys are their own english text

  const char* pData   = (const char*)locales_ini;
  s32         nLen    = (s32)locales_ini_len;
  const char* pTarget = gApp->config.sLanguageCode;

  char szSection[SC_LOC_CODE_LEN] = { 0 };
  bool bInTarget = false;

  s32 i = 0;
  while (i < nLen) {
    s32 iStart = i;
    while (i < nLen && pData[i] != '\n') {
      ++i;
    }
    s32 iEnd = i;
    if (i < nLen) {
      ++i;
    }

    while (iEnd > iStart && (pData[iEnd - 1] == '\r' || pData[iEnd - 1] == ' ' || pData[iEnd - 1] == '\t')) {
      --iEnd;
    }
    while (iStart < iEnd && (pData[iStart] == ' ' || pData[iStart] == '\t')) {
      ++iStart;
    }

    s32 nLine = iEnd - iStart;
    if (nLine <= 0) {
      continue;
    }

    const char* p = pData + iStart;
    if (p[0] == '[' && p[nLine - 1] == ']') {
      s32 nName = nLine - 2;
      if (nName > 0 && nName < SC_LOC_CODE_LEN) {
        memcpy(szSection, p + 1, nName);
        szSection[nName] = '\0';
        _scLocAddLang(szSection);
        bInTarget = (strcmp(szSection, pTarget) == 0);
      }
    } else if (bInTarget) {
      const char* pEq = (const char*)memchr(p, '=', nLine);
      if (pEq) {
        s32 nKey = (s32)(pEq - p);
        s32 nVal = nLine - nKey - 1;
        if (nKey > 0 && nKey < SC_LOC_KEY_LEN && gLoc.nEntryCount < SC_LOC_MAX_ENTRIES) {
          scLocEntry* pEntry = &gLoc.aEntries[gLoc.nEntryCount++];
          memcpy(pEntry->szKey, p, nKey);
          pEntry->szKey[nKey] = '\0';
          s32 nWide = MultiByteToWideChar(CP_UTF8, 0, pEq + 1, nVal, pEntry->wszValue, SC_LOC_VALUE_LEN - 1);
          pEntry->wszValue[nWide >= 0 ? nWide : 0] = L'\0';
        }
      }
    }
  }
}

//------------------------------------------------------------------------
// API
//------------------------------------------------------------------------
void scLocaleInit() {
  _scLocLoad();
  scLogDebug("Loaded locale '%s' with %d languages available", gApp->config.sLanguageCode, gLoc.nLangCount);
}

void scLocaleSet(const char* szCode) {
  strcpy_s(gApp->config.sLanguageCode, sizeof(gApp->config.sLanguageCode), szCode);
  _scLocLoad();
  scSaveConfig();
}

const wchar_t* scLocaleGet(const char* szKey) {
  for (s32 i = 0; i < gLoc.nEntryCount; ++i) {
    if (strcmp(gLoc.aEntries[i].szKey, szKey) == 0) {
      return gLoc.aEntries[i].wszValue;
    }
  }

  if (gLoc.nEntryCount < SC_LOC_MAX_ENTRIES) {
    scLocEntry* pEntry = &gLoc.aEntries[gLoc.nEntryCount++];
    strcpy_s(pEntry->szKey, SC_LOC_KEY_LEN, szKey);
    MultiByteToWideChar(CP_UTF8, 0, szKey, -1, pEntry->wszValue, SC_LOC_VALUE_LEN);
    return pEntry->wszValue;
  }

  static wchar_t wszFallback[SC_LOC_VALUE_LEN];
  MultiByteToWideChar(CP_UTF8, 0, szKey, -1, wszFallback, SC_LOC_VALUE_LEN);
  return wszFallback;
}

s32 scLocaleCount() {
  return gLoc.nLangCount;
}

const char* scLocaleCode(s32 iIndex) {
  return (iIndex >= 0 && iIndex < gLoc.nLangCount) ? gLoc.aLangs[iIndex] : "en";
}

const char* scLocaleCurrent() {
  return gApp->config.sLanguageCode;
}

const char* const* scLocaleCodes() {
  return gLoc.aLangPtrs;
}
