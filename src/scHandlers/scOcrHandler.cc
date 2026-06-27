#include "pch.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>

extern "C" {
#include "scApp.h"
#include "scLogging.h"
}

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage::Streams;

//------------------------------------------------------------------------
// State
//------------------------------------------------------------------------
struct scOcrLine {
  scRect rect;
  std::vector<scRect> chars;
};

static std::mutex            gOcrMutex;
static std::vector<scOcrLine> gOcrLines;
static std::atomic<uint32_t> gOcrGeneration{ 0 };

//------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------
static bool
_scRectsIntersect(const scRect& a, const scRect& b) {
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
           a.y + a.h <= b.y || b.y + b.h <= a.y);
}

static uint8_t*
_scGrabScaled(HDC hFrozenDC, int srcX, int srcY, int srcW, int srcH, int dstW, int dstH) {
  HDC     hMemDC = CreateCompatibleDC(hFrozenDC);
  HBITMAP hBitmap = CreateCompatibleBitmap(hFrozenDC, dstW, dstH);
  SelectObject(hMemDC, hBitmap);
  SetStretchBltMode(hMemDC, HALFTONE);
  StretchBlt(hMemDC, 0, 0, dstW, dstH, hFrozenDC, srcX, srcY, srcW, srcH, SRCCOPY);

  BITMAPINFOHEADER bih = {};
  bih.biSize        = sizeof(BITMAPINFOHEADER);
  bih.biWidth       =  dstW;
  bih.biHeight      = -dstH;
  bih.biPlanes      = 1;
  bih.biBitCount    = 32;
  bih.biCompression = BI_RGB;

  uint8_t* pBuf = (uint8_t*)malloc((size_t)dstW * dstH * 4);
  if (pBuf) {
    GetDIBits(hMemDC, hBitmap, 0, dstH, pBuf, (BITMAPINFO*)&bih, DIB_RGB_COLORS);
  }

  DeleteObject(hBitmap);
  DeleteDC(hMemDC);
  return pBuf;
}

static OcrResult
_scOcrRecognize(const uint8_t* pBgra, int w, int h) {
  DataWriter writer;
  writer.WriteBytes(array_view<const uint8_t>(pBgra, pBgra + (size_t)w * h * 4));

  SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
    writer.DetachBuffer(), BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);

  OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
  if (!engine) {
    return nullptr;
  }
  return engine.RecognizeAsync(bitmap).get();
}

static void
_scOcrSetClipboardText(const std::wstring& text) {
  if (text.empty() || !OpenClipboard(NULL)) {
    return;
  }
  EmptyClipboard();

  size_t  nBytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL hMem   = GlobalAlloc(GMEM_MOVEABLE, nBytes);
  if (hMem) {
    memcpy(GlobalLock(hMem), text.c_str(), nBytes);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
  }
  CloseClipboard();
}

//------------------------------------------------------------------------
// Screen scan (for snapping)
//------------------------------------------------------------------------
static void
_scOcrBeginScan(scCaptureContext* pCtx) {
  if (!pCtx->hFrozenDC) {
    return;
  }

  int vx = pCtx->vCaptureRegion.x;
  int vy = pCtx->vCaptureRegion.y;
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  const int kMaxDim = 4096;
  int scale = 2;
  while (scale > 1 && (vw * scale > kMaxDim || vh * scale > kMaxDim)) {
    --scale;
  }

  int sw = vw * scale;
  int sh = vh * scale;
  uint8_t* pBits = _scGrabScaled(pCtx->hFrozenDC, 0, 0, vw, vh, sw, sh);
  if (!pBits) {
    return;
  }

  uint32_t generation = ++gOcrGeneration;
  {
    std::lock_guard<std::mutex> lock(gOcrMutex);
    gOcrLines.clear();
  }

  std::thread([pBits, sw, sh, scale, vx, vy, generation]() {
    init_apartment(apartment_type::multi_threaded);
    try {
      if (OcrResult result = _scOcrRecognize(pBits, sw, sh)) {
        std::vector<scOcrLine> lines;
        for (auto const& line : result.Lines()) {
          scOcrLine ol;
          bool first = true;
          s32 minx = 0, miny = 0, maxx = 0, maxy = 0;

          for (auto const& word : line.Words()) {
            auto wr = word.BoundingRect();
            scRect box = {
              (int)(wr.X / scale) + vx,
              (int)(wr.Y / scale) + vy,
              (int)(wr.Width  / scale),
              (int)(wr.Height / scale)
            };

            int n = std::max(1, (int)word.Text().size());
            for (int c = 0; c < n; ++c) {
              int x0 = box.x + box.w * c / n;
              int cw = (box.x + box.w * (c + 1) / n) - x0;
              ol.chars.push_back(scRect{ x0, box.y, cw, box.h });
            }

            if (first) {
              minx = box.x; miny = box.y; maxx = box.x + box.w; maxy = box.y + box.h;
              first = false;
            } else {
              minx = std::min((int)minx, (int)box.x); miny = std::min((int)miny, (int)box.y);
              maxx = std::max(maxx, box.x + box.w); maxy = std::max(maxy, box.y + box.h);
            }
          }

          if (!first) {
            ol.rect = scRect{ minx, miny, maxx - minx, maxy - miny };
            lines.push_back(std::move(ol));
          }
        }

        std::lock_guard<std::mutex> lock(gOcrMutex);
        if (generation == gOcrGeneration.load()) {
          gOcrLines = std::move(lines);
        }
      }
    } catch (...) {
      scLogError("OCR screen scan failed");
    }
    free(pBits);
  }).detach();
}

//------------------------------------------------------------------------
// Handler callbacks
//------------------------------------------------------------------------
extern "C" bool
_scOcrOnHotkeyPressed(scCaptureContext* pCtx) {
  scCtxRequestCaptureArea(pCtx);
  if (gApp->pCaptureContext == pCtx) {
    _scOcrBeginScan(pCtx);
  }
  return false;
}

extern "C" bool
_scOcrOnAreaSelected(scCaptureContext* pCtx) {
  scRect r = pCtx->stSelectedRect;
  if (r.w <= 0 || r.h <= 0 || !pCtx->hFrozenDC) {
    return true;
  }

  int scale = 2;
  int dw = r.w * scale;
  int dh = r.h * scale;
  while (dw < 40 || dh < 40) {
    scale *= 2;
    dw = r.w * scale;
    dh = r.h * scale;
  }

  uint8_t* pScaled = _scGrabScaled(pCtx->hFrozenDC,
                                   r.x - pCtx->vCaptureRegion.x,
                                   r.y - pCtx->vCaptureRegion.y,
                                   r.w, r.h, dw, dh);
  if (!pScaled) {
    return true;
  }

  const int pad = 24;
  int pw = dw + pad * 2;
  int ph = dh + pad * 2;
  uint8_t* pPadded = (uint8_t*)malloc((size_t)pw * ph * 4);
  if (!pPadded) {
    free(pScaled);
    return true;
  }
  for (int i = 0; i < pw * ph; ++i) {
    pPadded[i * 4 + 0] = pScaled[0];
    pPadded[i * 4 + 1] = pScaled[1];
    pPadded[i * 4 + 2] = pScaled[2];
    pPadded[i * 4 + 3] = 255;
  }
  for (int y = 0; y < dh; ++y) {
    memcpy(&pPadded[((size_t)(y + pad) * pw + pad) * 4], &pScaled[(size_t)y * dw * 4], (size_t)dw * 4);
  }
  free(pScaled);

  std::thread([pPadded, pw, ph]() {
    init_apartment(apartment_type::multi_threaded);
    try {
      if (OcrResult result = _scOcrRecognize(pPadded, pw, ph)) {
        std::wstring text;
        for (auto const& line : result.Lines()) {
          if (!text.empty()) {
            text += L"\n";
          }
          text += line.Text().c_str();
        }
        _scOcrSetClipboardText(text);
      }
    } catch (...) {
      scLogError("OCR text extraction failed");
    }
    free(pPadded);
  }).detach();

  return true;
}

extern "C" HCURSOR
_scOcrOverlayCursor(scCaptureContext* pCtx) {
  (void)pCtx;
  return LoadCursorA(NULL, (LPCSTR)IDC_IBEAM);
}

extern "C" bool
_scOcrSnapSelection(scCaptureContext* pCtx, scRect rcInput, bool bDragging, scRect* pOut) {
  (void)pCtx;
  std::lock_guard<std::mutex> lock(gOcrMutex);

  if (bDragging) {
    bool found = false;
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (auto const& line : gOcrLines) {
      for (auto const& ch : line.chars) {
        if (!_scRectsIntersect(rcInput, ch)) {
          continue;
        }
        if (!found) {
          minx = ch.x; miny = ch.y; maxx = ch.x + ch.w; maxy = ch.y + ch.h;
          found = true;
        } else {
          minx = std::min((int)minx, (int)ch.x); miny = std::min((int)miny, (int)ch.y);
          maxx = std::max((int)maxx, (int)(ch.x + ch.w)); maxy = std::max((int)maxy, (int)(ch.y + ch.h));
        }
      }
    }
    if (found) {
      *pOut = scRect{ minx, miny, maxx - minx, maxy - miny };
    }
    return found;
  }

  for (auto const& line : gOcrLines) {
    if (rcInput.x >= line.rect.x && rcInput.x < line.rect.x + line.rect.w &&
        rcInput.y >= line.rect.y && rcInput.y < line.rect.y + line.rect.h) {
      *pOut = line.rect;
      return true;
    }
  }
  return false;
}

extern "C" scCaptureHandler scOcrHandler = {
  _scOcrOnHotkeyPressed,
  _scOcrOnAreaSelected,
  nullptr,
  _scOcrOverlayCursor,
  _scOcrSnapSelection
};
