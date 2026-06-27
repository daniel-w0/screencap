#include "pch.h"

HRESULT WINAPI
CoIncrementMTAUsage(PVOID* pCookie) {
  typedef HRESULT(WINAPI* CoIncrementMTAUsageFn)(PVOID*);
  static CoIncrementMTAUsageFn pfn = NULL;
  static bool bChecked = false;

  if (!bChecked) {
    HMODULE hOle32 = GetModuleHandleW(L"ole32.dll");
    if (hOle32) {
      pfn = (CoIncrementMTAUsageFn)GetProcAddress(hOle32, "CoIncrementMTAUsage");
    }
    bChecked = true;
  }

  if (pfn) {
    return pfn(pCookie);
  }
  if (pCookie) {
    *pCookie = (PVOID)1;
  }
  return S_OK;
}
