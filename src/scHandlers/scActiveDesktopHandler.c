#include "pch.h"
#include "scApp.h"
#include "scLogging.h"

scInternal bool
_copyScreenRectToImage(s32 x, s32 y, s32 w, s32 h, scImage* pOutImage) {
  *pOutImage = (scImage){ 0 };
  if (w <= 0 || h <= 0) {
    return false;
  }

  BITMAPINFO bi = { 0 };
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       =  w;
  bi.bmiHeader.biHeight      = -h;   // negative => top-down rows
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  HDC     hScreenDC = GetDC(NULL);
  void*   pBits     = NULL;
  HBITMAP hDib      = CreateDIBSection(hScreenDC, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
  if (!hDib) {
    ReleaseDC(NULL, hScreenDC);
    return false;
  }

  HDC     hDibDC = CreateCompatibleDC(hScreenDC);
  HBITMAP hOld   = SelectObject(hDibDC, hDib);

  BitBlt(hDibDC, 0, 0, w, h, hScreenDC, x, y, SRCCOPY);
  GdiFlush();

  SelectObject(hDibDC, hOld);
  DeleteDC(hDibDC);
  ReleaseDC(NULL, hScreenDC);

  pOutImage->W       = w;
  pOutImage->H       = h;
  pOutImage->iStride = w * 4;
  pOutImage->pPixels = (u8*)pBits;
  pOutImage->hBitmap = hDib;
  return true;
}

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  POINT pt;
  GetCursorPos(&pt);

  HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = { sizeof(MONITORINFO) };
  if (!GetMonitorInfo(hMon, &mi)) {
    scLogError("Failed getting monitor info: %d", GetLastError());
    return true;
  }

  s32 x = mi.rcMonitor.left;
  s32 y = mi.rcMonitor.top;
  s32 w = mi.rcMonitor.right  - mi.rcMonitor.left;
  s32 h = mi.rcMonitor.bottom - mi.rcMonitor.top;

  scImage stImage;
  if (!_copyScreenRectToImage(x, y, w, h, &stImage)) {
    return true;
  }
  scSaveImage(&stImage, true);
  scPlaySoundOrSkip(SC_SOUND_SCREENSHOT);
  return true;
}

const scCaptureHandler scActiveMonitorHandler = {
  cbOnHotkeyPressed,
  NULL,
  NULL
};