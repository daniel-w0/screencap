/*
 * Native screen recorder for Screencap.
 *
 * Replaces the FFmpeg-process implementation with:
 *   DXGI Desktop Duplication (preferred) or GDI/BitBlt fallback -> CPU-readable
 *   BGRA frames -> CPU BGRA-to-NV12 conversion -> Media Foundation Sink Writer
 *   (H.264/MP4). If an encoder rejects a large native frame size, recording
 *   retries at a downscaled <=1920x1080 output while retaining full-size capture.
 *
 * This is intentionally a correctness-first implementation. Frames are copied
 * from the GPU to system memory before being passed to Media Foundation. A
 * later optimisation can use a GPU-backed Media Foundation sample path.
 *
 */

#include "pch.h"
#include "scApp.h"
#include "scLogging.h"
#include "scAssert.h"
#include "scUI.h"

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#ifndef SC_RECORD_DEFAULT_FPS
#define SC_RECORD_DEFAULT_FPS 30
#endif

#define SC_RECORD_HNS_PER_SECOND 10000000LL
#define SC_RECORD_ACQUIRE_TIMEOUT_MS 10
#define SC_RECORD_RECREATE_RETRIES 50
#define SC_RECORD_RECREATE_DELAY_MS 100

typedef struct {
  HANDLE hCaptureThread;
  HANDLE hWriterThread;
  HANDLE hStopEvent;
  HANDLE hCaptureReadyEvent;
  HANDLE hWriterReadyEvent;
  HANDLE hFirstFrameEvent;
  HANDLE hRunEvent;

  wchar_t wszSavePath[SC_PATH_MAX_LEN];
  scRect stRect;
  s32 nWidth;       /* capture dimensions */
  s32 nHeight;
  s32 nEncodeWidth;  /* encoder/output dimensions; may be downscaled */
  s32 nEncodeHeight;
  s32 nFPS;
  scRecordQuality eQuality;
  bool bShowCursor;
  bool bForceSoftware;
  s32 nLocalX;
  s32 nLocalY;
  DXGI_MODE_ROTATION eRotation;

  /* Shared latest-frame slot. Capture publishes into this buffer; the writer
     takes a snapshot for each output frame. Audio callbacks must never use
     this lock; future audio should use its own lock-free/bounded PCM queue. */
  CRITICAL_SECTION csFrame;
  bool bFrameLockInitialized;
  BYTE* pLatestPixels;
  size_t cbPixelsSize;
  bool bHavePixels;
  BYTE* pPointerShape;
  UINT nPointerShapeBytes;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO stPointerShapeInfo;
  POINT stPointerPosition; /* top-left of shape relative to the selected region */
  bool bPointerShapeAvailable;
  bool bPointerVisible;
  ULONGLONG nFrameVersion;
  bool bUseFragmented;
  UINT32 nH264Profile;
  bool bTagColor;

  LARGE_INTEGER qpcFrequency;
  LARGE_INTEGER qpcStart; /* shared A/V timeline origin */
  LARGE_INTEGER qpcStop;  /* user-requested stop time, for final sample duration */
  volatile LONG bQpcStopSet;

  HRESULT hrCaptureStartup;
  HRESULT hrWriterStartup;
  bool bOutputFileMayExist;
  bool bUsingGDI; /* true when DXGI Desktop Duplication is unavailable */
  volatile LONG hrResult; /* first runtime failure; written atomically */
  ULONGLONG nFramesWritten; /* writer-thread owned until both threads join */
  bool bStarted;
} scRecordContext;

static volatile LONG gPendingStops = 0;

static void
_scRecordLogHRESULT(const char* szOperation, HRESULT hr) {
  scLogError("Recording: %s failed (HRESULT 0x%08lX)", szOperation, (unsigned long)hr);
}

static void
_scRecordSetFailure(scRecordContext* pCtx, HRESULT hr) {
  if (FAILED(hr)) {
    InterlockedCompareExchange(&pCtx->hrResult, (LONG)hr, (LONG)S_OK);
  }
}

static LONGLONG
_scFrameTime100ns(LONGLONG frameIndex, s32 fps) {
  /* Derive every timestamp from the frame index instead of accumulating a
     truncated duration, avoiding long-recording drift at rates like 30 FPS. */
  return (frameIndex * SC_RECORD_HNS_PER_SECOND) / fps;
}

static bool
_scRectInsideOutput(const RECT* pOutput, const scRect* pRect, s32 nWidth, s32 nHeight) {
  const int64_t x1 = (int64_t)pRect->x;
  const int64_t y1 = (int64_t)pRect->y;
  const int64_t x2 = x1 + nWidth;
  const int64_t y2 = y1 + nHeight;
  return x1 >= pOutput->left && y1 >= pOutput->top &&
         x2 <= pOutput->right && y2 <= pOutput->bottom;
}

static HRESULT
_scCreateDuplicationForRect(scRecordContext* pCtx,
                            ID3D11Device** ppDevice,
                            ID3D11DeviceContext** ppImmediateContext,
                            IDXGIOutputDuplication** ppDuplication,
                            IDXGIOutput1** ppOutput1) {
  HRESULT hr = S_OK;
  IDXGIFactory1* pFactory = NULL;
  IDXGIAdapter1* pChosenAdapter = NULL;
  IDXGIOutput* pChosenOutput = NULL;
  IDXGIOutput1* pChosenOutput1 = NULL;
  ID3D11Device* pDevice = NULL;
  ID3D11DeviceContext* pImmediateContext = NULL;
  IDXGIOutputDuplication* pDuplication = NULL;
  RECT rcOutput = { 0 };
  bool bFound = false;

  hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&pFactory);
  if (FAILED(hr)) goto cleanup;

  for (UINT adapterIndex = 0; !bFound; ++adapterIndex) {
    IDXGIAdapter1* pAdapter = NULL;
    hr = IDXGIFactory1_EnumAdapters1(pFactory, adapterIndex, &pAdapter);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      hr = DXGI_ERROR_NOT_FOUND;
      break;
    }
    if (FAILED(hr)) break;

    for (UINT outputIndex = 0; ; ++outputIndex) {
      IDXGIOutput* pOutput = NULL;
      DXGI_OUTPUT_DESC outputDesc;
      HRESULT hrOutput = IDXGIAdapter1_EnumOutputs(pAdapter, outputIndex, &pOutput);
      if (hrOutput == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(hrOutput)) {
        hr = hrOutput;
        break;
      }

      ZeroMemory(&outputDesc, sizeof(outputDesc));
      hrOutput = IDXGIOutput_GetDesc(pOutput, &outputDesc);
      if (SUCCEEDED(hrOutput) &&
          _scRectInsideOutput(&outputDesc.DesktopCoordinates, &pCtx->stRect,
                              pCtx->nWidth, pCtx->nHeight)) {
        pChosenAdapter = pAdapter;
        pChosenOutput = pOutput;
        rcOutput = outputDesc.DesktopCoordinates;
        pAdapter = NULL;
        pOutput = NULL;
        bFound = true;
        hr = S_OK;
        break;
      }
      IDXGIOutput_Release(pOutput);
    }

    if (pAdapter) IDXGIAdapter1_Release(pAdapter);
    if (bFound || FAILED(hr)) break;
  }

  if (!bFound) {
    if (SUCCEEDED(hr)) hr = DXGI_ERROR_NOT_FOUND;
    goto cleanup;
  }

  /* Desktop Duplication exposes an unrotated surface on rotated displays.
     Do not silently record the wrong crop until a rotation transform is added. */
  if (rcOutput.right > rcOutput.left && rcOutput.bottom > rcOutput.top) {
    DXGI_OUTPUT_DESC chosenDesc;
    ZeroMemory(&chosenDesc, sizeof(chosenDesc));
    hr = IDXGIOutput_GetDesc(pChosenOutput, &chosenDesc);
    if (FAILED(hr)) goto cleanup;
    pCtx->eRotation = chosenDesc.Rotation;
    if (chosenDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
      scLogInfo("Recording: rotated monitor (rotation %u) is not handled by Desktop Duplication capture",
                (unsigned)chosenDesc.Rotation);
      hr = DXGI_ERROR_UNSUPPORTED;
      goto cleanup;
    }
  }

  pCtx->nLocalX = pCtx->stRect.x - rcOutput.left;
  pCtx->nLocalY = pCtx->stRect.y - rcOutput.top;

  {
    static const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL selectedLevel;
    hr = D3D11CreateDevice((IDXGIAdapter*)pChosenAdapter,
                           D3D_DRIVER_TYPE_UNKNOWN,
                           NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels,
                           ARRAYSIZE(levels),
                           D3D11_SDK_VERSION,
                           &pDevice,
                           &selectedLevel,
                           &pImmediateContext);
    if (hr == E_INVALIDARG) {
      /* Older systems may not accept the 11.1 feature-level enum. */
      hr = D3D11CreateDevice((IDXGIAdapter*)pChosenAdapter,
                             D3D_DRIVER_TYPE_UNKNOWN,
                             NULL,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             levels + 1,
                             ARRAYSIZE(levels) - 1,
                             D3D11_SDK_VERSION,
                             &pDevice,
                             &selectedLevel,
                             &pImmediateContext);
    }
    if (FAILED(hr)) goto cleanup;
  }

  hr = IDXGIOutput_QueryInterface(pChosenOutput, &IID_IDXGIOutput1, (void**)&pChosenOutput1);
  if (FAILED(hr)) goto cleanup;

  hr = IDXGIOutput1_DuplicateOutput(pChosenOutput1, (IUnknown*)pDevice, &pDuplication);
  if (FAILED(hr)) goto cleanup;

  *ppDevice = pDevice;
  pDevice = NULL;
  *ppImmediateContext = pImmediateContext;
  pImmediateContext = NULL;
  *ppDuplication = pDuplication;
  pDuplication = NULL;
  *ppOutput1 = pChosenOutput1;
  pChosenOutput1 = NULL;

cleanup:
  if (pDuplication) IDXGIOutputDuplication_Release(pDuplication);
  if (pImmediateContext) ID3D11DeviceContext_Release(pImmediateContext);
  if (pDevice) ID3D11Device_Release(pDevice);
  if (pChosenOutput1) IDXGIOutput1_Release(pChosenOutput1);
  if (pChosenOutput) IDXGIOutput_Release(pChosenOutput);
  if (pChosenAdapter) IDXGIAdapter1_Release(pChosenAdapter);
  if (pFactory) IDXGIFactory1_Release(pFactory);
  return hr;
}

static HRESULT
_scSetAttributePair(IMFMediaType* pType, const GUID* pKey, UINT32 nHigh, UINT32 nLow) {
  return IMFMediaType_SetUINT64(pType, pKey, ((UINT64)nHigh << 32) | (UINT64)nLow);
}

static HRESULT
_scSetColorAttributes(IMFMediaType* pType, bool bHD) {
  HRESULT hr;
  hr = IMFMediaType_SetUINT32(pType, &MF_MT_YUV_MATRIX,
                              bHD ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601);
  if (FAILED(hr)) return hr;
  hr = IMFMediaType_SetUINT32(pType, &MF_MT_VIDEO_PRIMARIES,
                              bHD ? MFVideoPrimaries_BT709 : MFVideoPrimaries_SMPTE170M);
  if (FAILED(hr)) return hr;
  hr = IMFMediaType_SetUINT32(pType, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  if (FAILED(hr)) return hr;
  return IMFMediaType_SetUINT32(pType, &MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

static UINT32
_scComputeBitrate(const scRecordContext* pCtx) {
  static const double bitsPerPixel[SC_QUALITY_COUNT] = { 0.08, 0.15, 0.30 };
  double bitRate = (double)pCtx->nEncodeWidth * (double)pCtx->nEncodeHeight * 30.0 *
                   bitsPerPixel[pCtx->eQuality] * sqrt((double)pCtx->nFPS / 30.0);
  if (bitRate < 2000000.0) bitRate = 2000000.0;
  if (bitRate > 60000000.0) bitRate = 60000000.0;
  return (UINT32)bitRate;
}

static UINT32
_scEncoderQualityValue(const scRecordContext* pCtx) {
  static const UINT32 values[SC_QUALITY_COUNT] = { 60, 75, 90 };
  return values[pCtx->eQuality];
}

static const GUID scContainerTypeFMPEG4 =
  { 0x9ba876f1, 0x419f, 0x4b77, { 0xa1, 0xe0, 0x35, 0x95, 0x9d, 0x9d, 0x40, 0x04 } };

static UINT32
_scEncoderQualityVsSpeed(const scRecordContext* pCtx) {
  static const UINT32 values[SC_QUALITY_COUNT] = { 33, 66, 100 };
  return values[pCtx->eQuality];
}

static HRESULT
_scCreateSinkWriterForContainer(scRecordContext* pCtx, bool bFragmented,
                                IMFSinkWriter** ppWriter, DWORD* pdwStreamIndex) {
  HRESULT hr = S_OK;
  int attempt;

  *ppWriter = NULL;

  /* Prefer hardware encoding first, but retry using the system software MFT if
     a hardware encoder rejects our input media type. This is particularly
     important on older Windows 7 display drivers, which may expose an encoder
     that does not accept the requested size or format. */
  for (attempt = pCtx->bForceSoftware ? 1 : 0; attempt < 2; ++attempt) {
    const bool bEnableHardware = (attempt == 0);
    const bool bHD = (pCtx->nEncodeWidth >= 1280 || pCtx->nEncodeHeight >= 720);
    const char* szFailedAt = "MFCreateAttributes";
    bool bRetrySoftware = false;
    IMFAttributes* pAttributes = NULL;
    IMFAttributes* pEncoderConfig = NULL;
    IMFMediaType* pOutputType = NULL;
    IMFMediaType* pInputType = NULL;
    IMFSinkWriter* pWriter = NULL;
    UINT32 bitRate;
    UINT32 cbNV12;

    /* Guard the media sample size calculation; dimensions should already be
       even, but avoid wrapping UINT32 on unusually large capture regions. */
    {
      UINT64 cb64 = (UINT64)(UINT32)pCtx->nEncodeWidth * (UINT64)(UINT32)pCtx->nEncodeHeight * 3u / 2u;
      if (cb64 == 0 || cb64 > MAXDWORD) {
        hr = E_OUTOFMEMORY;
        szFailedAt = "NV12 sample size calculation";
        goto attempt_cleanup;
      }
      cbNV12 = (UINT32)cb64;
    }

    szFailedAt = "MFCreateAttributes";
    hr = MFCreateAttributes(&pAttributes, 2);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS";
    hr = IMFAttributes_SetUINT32(pAttributes,
                                 &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                                 bEnableHardware ? TRUE : FALSE);
    if (FAILED(hr)) goto attempt_cleanup;
    if (bFragmented) {
      szFailedAt = "MF_TRANSCODE_CONTAINERTYPE";
      hr = IMFAttributes_SetGUID(pAttributes, &MF_TRANSCODE_CONTAINERTYPE,
                                 &scContainerTypeFMPEG4);
      if (FAILED(hr)) goto attempt_cleanup;
    }

    szFailedAt = "MFCreateSinkWriterFromURL";
    hr = MFCreateSinkWriterFromURL(pCtx->wszSavePath, NULL, pAttributes, &pWriter);
    if (FAILED(hr)) goto attempt_cleanup;

    szFailedAt = "MFCreateMediaType(output)";
    hr = MFCreateMediaType(&pOutputType);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set output major type";
    hr = IMFMediaType_SetGUID(pOutputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set output subtype H264";
    hr = IMFMediaType_SetGUID(pOutputType, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    if (FAILED(hr)) goto attempt_cleanup;

    bitRate = _scComputeBitrate(pCtx);
    szFailedAt = "set output average bitrate";
    hr = IMFMediaType_SetUINT32(pOutputType, &MF_MT_AVG_BITRATE, bitRate);
    if (FAILED(hr)) goto attempt_cleanup;
    if (pCtx->nH264Profile) {
      szFailedAt = "set output profile";
      hr = IMFMediaType_SetUINT32(pOutputType, &MF_MT_MPEG2_PROFILE, pCtx->nH264Profile);
      if (FAILED(hr)) goto attempt_cleanup;
    }
    szFailedAt = "set output frame size";
    hr = _scSetAttributePair(pOutputType, &MF_MT_FRAME_SIZE,
                             (UINT32)pCtx->nEncodeWidth, (UINT32)pCtx->nEncodeHeight);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set output frame rate";
    hr = _scSetAttributePair(pOutputType, &MF_MT_FRAME_RATE, (UINT32)pCtx->nFPS, 1);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set output pixel aspect ratio";
    hr = _scSetAttributePair(pOutputType, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set output interlace mode";
    hr = IMFMediaType_SetUINT32(pOutputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) goto attempt_cleanup;
    if (pCtx->bTagColor) {
      szFailedAt = "set output color attributes";
      hr = _scSetColorAttributes(pOutputType, bHD);
      if (FAILED(hr)) goto attempt_cleanup;
    }

    szFailedAt = "IMFSinkWriter_AddStream(H264)";
    hr = IMFSinkWriter_AddStream(pWriter, pOutputType, pdwStreamIndex);
    if (FAILED(hr)) goto attempt_cleanup;

    szFailedAt = "MFCreateMediaType(input)";
    hr = MFCreateMediaType(&pInputType);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input major type";
    hr = IMFMediaType_SetGUID(pInputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input subtype NV12";
    hr = IMFMediaType_SetGUID(pInputType, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input frame size";
    hr = _scSetAttributePair(pInputType, &MF_MT_FRAME_SIZE,
                             (UINT32)pCtx->nEncodeWidth, (UINT32)pCtx->nEncodeHeight);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input frame rate";
    hr = _scSetAttributePair(pInputType, &MF_MT_FRAME_RATE, (UINT32)pCtx->nFPS, 1);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input pixel aspect ratio";
    hr = _scSetAttributePair(pInputType, &MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input interlace mode";
    hr = IMFMediaType_SetUINT32(pInputType, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) goto attempt_cleanup;
    if (pCtx->bTagColor) {
      szFailedAt = "set input color attributes";
      hr = _scSetColorAttributes(pInputType, bHD);
      if (FAILED(hr)) goto attempt_cleanup;
    }
    /* NV12 is tightly packed here: Y stride is width and the UV stride is
       also width. These are the only layout attributes needed for our buffer. */
    szFailedAt = "set input default stride";
    hr = IMFMediaType_SetUINT32(pInputType, &MF_MT_DEFAULT_STRIDE, (UINT32)pCtx->nEncodeWidth);
    if (FAILED(hr)) goto attempt_cleanup;
    szFailedAt = "set input sample size";
    hr = IMFMediaType_SetUINT32(pInputType, &MF_MT_SAMPLE_SIZE, cbNV12);
    if (FAILED(hr)) goto attempt_cleanup;

    szFailedAt = "IMFSinkWriter_SetInputMediaType(NV12)";
    hr = MFCreateAttributes(&pEncoderConfig, 3);
    if (SUCCEEDED(hr)) {
      IMFAttributes_SetUINT32(pEncoderConfig, &CODECAPI_AVEncCommonQualityVsSpeed,
                              _scEncoderQualityVsSpeed(pCtx));
      IMFAttributes_SetUINT32(pEncoderConfig, &CODECAPI_AVEncCommonRateControlMode,
                              eAVEncCommonRateControlMode_Quality);
      IMFAttributes_SetUINT32(pEncoderConfig, &CODECAPI_AVEncCommonQuality,
                              _scEncoderQualityValue(pCtx));
      hr = IMFSinkWriter_SetInputMediaType(pWriter, *pdwStreamIndex, pInputType, pEncoderConfig);
      if (FAILED(hr)) {
        IMFAttributes_DeleteItem(pEncoderConfig, &CODECAPI_AVEncCommonRateControlMode);
        IMFAttributes_DeleteItem(pEncoderConfig, &CODECAPI_AVEncCommonQuality);
        hr = IMFSinkWriter_SetInputMediaType(pWriter, *pdwStreamIndex, pInputType, pEncoderConfig);
      }
    }
    if (FAILED(hr)) {
      hr = IMFSinkWriter_SetInputMediaType(pWriter, *pdwStreamIndex, pInputType, NULL);
    }
    if (FAILED(hr)) {
      /* Encoder selection and type negotiation occur here. Retrying without
         hardware transforms lets Windows 7 use its inbox software encoder. */
      if (attempt == 0) bRetrySoftware = true;
      goto attempt_cleanup;
    }

    szFailedAt = "IMFSinkWriter_BeginWriting";
    hr = IMFSinkWriter_BeginWriting(pWriter);
    if (FAILED(hr)) {
      if (attempt == 0) bRetrySoftware = true;
      goto attempt_cleanup;
    }

    *ppWriter = pWriter;
    pWriter = NULL;

attempt_cleanup:
    if (pInputType) IMFMediaType_Release(pInputType);
    if (pOutputType) IMFMediaType_Release(pOutputType);
    if (pWriter) IMFSinkWriter_Release(pWriter);
    if (pEncoderConfig) IMFAttributes_Release(pEncoderConfig);
    if (pAttributes) IMFAttributes_Release(pAttributes);

    if (SUCCEEDED(hr)) {
      scLogInfo("Recording: H.264 encoder initialised (%s transforms, %s), %d x %d at %d FPS",
                bEnableHardware ? "hardware-enabled" : "software-only",
                bFragmented ? "fragmented MP4" : "standard MP4",
                pCtx->nEncodeWidth, pCtx->nEncodeHeight, pCtx->nFPS);
      return S_OK;
    }

    if (bRetrySoftware) {
      scLogWarn("Recording: H.264 setup rejected %d x %d NV12 at %d FPS with hardware transforms %s (HRESULT 0x%08lX); retrying software-only",
                 pCtx->nEncodeWidth, pCtx->nEncodeHeight, pCtx->nFPS,
                 bEnableHardware ? "enabled" : "disabled", (unsigned long)hr);
      /* MFCreateSinkWriterFromURL may already have created the output file.
         This path is reached before any samples are written, so remove it
         before retrying the same destination with the software encoder. */
      if (!DeleteFileW(pCtx->wszSavePath) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        scLogError("Recording: unable to remove failed writer output before retry: %lu",
                   GetLastError());
        scLogError("Recording: Sink Writer setup failed at '%s' (HRESULT 0x%08lX)",
                   szFailedAt, (unsigned long)hr);
        return hr;
      }
      continue;
    }

    scLogWarn("Recording: Sink Writer setup failed at '%s' for %d x %d at %d FPS (%s transforms, HRESULT 0x%08lX)",
               szFailedAt, pCtx->nEncodeWidth, pCtx->nEncodeHeight,
               pCtx->nFPS, bEnableHardware ? "hardware-enabled" : "software-only",
               (unsigned long)hr);
    return hr;
  }

  return E_FAIL;
}

static HRESULT
_scCreateSinkWriterForSettings(scRecordContext* pCtx, IMFSinkWriter** ppWriter, DWORD* pdwStreamIndex) {
  HRESULT hr;

  if (pCtx->bUseFragmented) {
    hr = _scCreateSinkWriterForContainer(pCtx, true, ppWriter, pdwStreamIndex);
    if (SUCCEEDED(hr)) return hr;
    scLogInfo("Recording: fragmented MP4 setup failed (HRESULT 0x%08lX); trying standard MP4",
              (unsigned long)hr);
    if (!DeleteFileW(pCtx->wszSavePath) && GetLastError() != ERROR_FILE_NOT_FOUND) {
      scLogError("Recording: unable to remove failed writer output before container retry: %lu",
                 GetLastError());
      return hr;
    }
  }

  hr = _scCreateSinkWriterForContainer(pCtx, false, ppWriter, pdwStreamIndex);
  if (SUCCEEDED(hr)) pCtx->bUseFragmented = false;
  return hr;
}

static HRESULT
_scCreateSinkWriter(scRecordContext* pCtx, IMFSinkWriter** ppWriter, DWORD* pdwStreamIndex) {
  static const struct { UINT32 profile; bool color; } steps[] = {
    { eAVEncH264VProfile_High, true },
    { eAVEncH264VProfile_Main, true },
    { 0, true },
    { 0, false }
  };
  HRESULT hr = E_FAIL;
  size_t i;

  for (i = 0; i < ARRAYSIZE(steps); ++i) {
    pCtx->nH264Profile = steps[i].profile;
    pCtx->bTagColor = steps[i].color;
    hr = _scCreateSinkWriterForSettings(pCtx, ppWriter, pdwStreamIndex);
    if (SUCCEEDED(hr)) {
      scLogInfo("Recording: H.264 profile %u, color tagging %s",
                (unsigned)pCtx->nH264Profile, pCtx->bTagColor ? "on" : "off");
      return hr;
    }
    if (i + 1 < ARRAYSIZE(steps)) {
      scLogInfo("Recording: encoder setup failed (HRESULT 0x%08lX); retrying with more conservative settings",
                (unsigned long)hr);
      if (!DeleteFileW(pCtx->wszSavePath) && GetLastError() != ERROR_FILE_NOT_FOUND) return hr;
    }
  }
  return hr;
}

/* Keep the original capture size where possible. If the installed encoder
   rejects a large frame, retry with a broadly compatible 1080p-class output
   while preserving the selected region's aspect ratio. Both dimensions must
   be even for NV12. */
static void
_scChooseScaledEncoderSize(scRecordContext* pCtx, s32 maxWidth, s32 maxHeight) {
  double sx = (double)maxWidth / (double)pCtx->nWidth;
  double sy = (double)maxHeight / (double)pCtx->nHeight;
  double scale = sx < sy ? sx : sy;
  s32 width;
  s32 height;

  /* This helper is a downscale fallback only: never enlarge a small region. */
  if (scale > 1.0) scale = 1.0;
  width = ((s32)((double)pCtx->nWidth * scale)) & ~1;
  height = ((s32)((double)pCtx->nHeight * scale)) & ~1;

  if (width < 2) width = 2;
  if (height < 2) height = 2;
  if (width > maxWidth) width = maxWidth & ~1;
  if (height > maxHeight) height = maxHeight & ~1;
  pCtx->nEncodeWidth = width;
  pCtx->nEncodeHeight = height;
}

/* Exact area-average BGRA downscale, used for the max resolution option and
   when the encoder rejects the native capture resolution. Each destination
   pixel is the coverage-weighted mean of the source pixels it spans. pTemp
   must hold dstWidth * srcHeight * 3 values. */
static void
_scScaleBGRA(const BYTE* pSrc, s32 srcWidth, s32 srcHeight,
             BYTE* pDst, s32 dstWidth, s32 dstHeight, uint16_t* pTemp) {
  const uint64_t invH = (((uint64_t)16 << 32) + (uint64_t)srcWidth / 2) / (uint64_t)srcWidth;
  const uint64_t invV = ((uint64_t)1 << 32) / ((uint64_t)16 * (uint64_t)srcHeight);
  s32 x, y, i, k;

  for (y = 0; y < srcHeight; ++y) {
    const BYTE* pRow = pSrc + (size_t)y * (size_t)srcWidth * 4u;
    uint16_t* pTempRow = pTemp + (size_t)y * (size_t)dstWidth * 3u;
    for (x = 0; x < dstWidth; ++x) {
      const s32 left = x * srcWidth;
      const s32 right = left + srcWidth;
      uint32_t accB = 0, accG = 0, accR = 0;
      for (i = left / dstWidth; i < srcWidth && i * dstWidth < right; ++i) {
        s32 lo = i * dstWidth;
        s32 hi = lo + dstWidth;
        const BYTE* p = pRow + (size_t)i * 4u;
        uint32_t w;
        if (lo < left) lo = left;
        if (hi > right) hi = right;
        w = (uint32_t)(hi - lo);
        accB += p[0] * w;
        accG += p[1] * w;
        accR += p[2] * w;
      }
      pTempRow[x * 3 + 0] = (uint16_t)(((uint64_t)accB * invH + ((uint64_t)1 << 31)) >> 32);
      pTempRow[x * 3 + 1] = (uint16_t)(((uint64_t)accG * invH + ((uint64_t)1 << 31)) >> 32);
      pTempRow[x * 3 + 2] = (uint16_t)(((uint64_t)accR * invH + ((uint64_t)1 << 31)) >> 32);
    }
  }

  for (y = 0; y < dstHeight; ++y) {
    const s32 top = y * srcHeight;
    const s32 bottom = top + srcHeight;
    const s32 firstRow = top / dstHeight;
    uint32_t weights[64];
    s32 count = 0;
    BYTE* pOut = pDst + (size_t)y * (size_t)dstWidth * 4u;

    for (i = firstRow; i < srcHeight && i * dstHeight < bottom && count < 64; ++i, ++count) {
      s32 lo = i * dstHeight;
      s32 hi = lo + dstHeight;
      if (lo < top) lo = top;
      if (hi > bottom) hi = bottom;
      weights[count] = (uint32_t)(hi - lo);
    }

    for (x = 0; x < dstWidth; ++x) {
      uint32_t accB = 0, accG = 0, accR = 0;
      uint32_t b, g, r;
      for (k = 0; k < count; ++k) {
        const uint16_t* t = pTemp + ((size_t)(firstRow + k) * (size_t)dstWidth + (size_t)x) * 3u;
        accB += t[0] * weights[k];
        accG += t[1] * weights[k];
        accR += t[2] * weights[k];
      }
      b = (uint32_t)(((uint64_t)accB * invV + ((uint64_t)1 << 31)) >> 32);
      g = (uint32_t)(((uint64_t)accG * invV + ((uint64_t)1 << 31)) >> 32);
      r = (uint32_t)(((uint64_t)accR * invV + ((uint64_t)1 << 31)) >> 32);
      pOut[x * 4 + 0] = (BYTE)(b > 255 ? 255 : b);
      pOut[x * 4 + 1] = (BYTE)(g > 255 ? 255 : g);
      pOut[x * 4 + 2] = (BYTE)(r > 255 ? 255 : r);
      pOut[x * 4 + 3] = 255;
    }
  }
}

static BYTE
_scClampByte(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return (BYTE)value;
}

static BYTE
_scLuma(int r, int g, int b, int cr, int cg, int cb) {
  return _scClampByte(((cr * r + cg * g + cb * b + 128) >> 8) + 16);
}

/* Convert packed BGRA (from DXGI or top-down DIB) into encoder-friendly NV12.
   NV12 stores a full-resolution Y plane followed by interleaved U,V samples
   for each 2x2 luma block. Width and height are even by construction. */
static void
_scConvertBGRAtoNV12(const BYTE* pBGRA, s32 width, s32 height, BYTE* pNV12) {
  const size_t yPlaneSize = (size_t)width * (size_t)height;
  BYTE* pY = pNV12;
  BYTE* pUV = pNV12 + yPlaneSize;
  const bool bUse709 = (width >= 1280 || height >= 720);
  const int yr = bUse709 ? 47 : 66;
  const int yg = bUse709 ? 157 : 129;
  const int yb = bUse709 ? 16 : 25;
  const int ur = bUse709 ? -26 : -38;
  const int ug = bUse709 ? -87 : -74;
  const int vg = bUse709 ? -102 : -94;
  const int vb = bUse709 ? -10 : -18;
  s32 y, x;

  for (y = 0; y < height; y += 2) {
    const BYTE* pRow0 = pBGRA + (size_t)y * (size_t)width * 4u;
    const BYTE* pRow1 = pRow0 + (size_t)width * 4u;
    BYTE* pDstY0 = pY + (size_t)y * (size_t)width;
    BYTE* pDstY1 = pDstY0 + (size_t)width;
    BYTE* pDstUV = pUV + (size_t)(y / 2) * (size_t)width;
    for (x = 0; x < width; x += 2) {
      const BYTE* p00 = pRow0 + (size_t)x * 4u;
      const BYTE* p01 = p00 + 4;
      const BYTE* p10 = pRow1 + (size_t)x * 4u;
      const BYTE* p11 = p10 + 4;
      const int sumR = p00[2] + p01[2] + p10[2] + p11[2];
      const int sumG = p00[1] + p01[1] + p10[1] + p11[1];
      const int sumB = p00[0] + p01[0] + p10[0] + p11[0];
      const int u = ((ur * sumR + ug * sumG + 112 * sumB + 512) >> 10) + 128;
      const int v = ((112 * sumR + vg * sumG + vb * sumB + 512) >> 10) + 128;

      pDstY0[x + 0] = _scLuma(p00[2], p00[1], p00[0], yr, yg, yb);
      pDstY0[x + 1] = _scLuma(p01[2], p01[1], p01[0], yr, yg, yb);
      pDstY1[x + 0] = _scLuma(p10[2], p10[1], p10[0], yr, yg, yb);
      pDstY1[x + 1] = _scLuma(p11[2], p11[1], p11[0], yr, yg, yb);
      pDstUV[x + 0] = _scClampByte(u);
      pDstUV[x + 1] = _scClampByte(v);
    }
  }
}

static HRESULT
_scCreateSample(const BYTE* pPixels, DWORD cbPixels, LONGLONG time100ns, IMFSample** ppSample) {
  HRESULT hr;
  IMFMediaBuffer* pBuffer = NULL;
  IMFSample* pSample = NULL;
  BYTE* pDestination = NULL;
  DWORD maxLength = 0;
  DWORD currentLength = 0;

  *ppSample = NULL;

  hr = MFCreateMemoryBuffer(cbPixels, &pBuffer);
  if (FAILED(hr)) goto cleanup;
  hr = IMFMediaBuffer_Lock(pBuffer, &pDestination, &maxLength, &currentLength);
  if (FAILED(hr)) goto cleanup;
  if (maxLength < cbPixels) {
    hr = E_FAIL;
    IMFMediaBuffer_Unlock(pBuffer);
    goto cleanup;
  }
  memcpy(pDestination, pPixels, cbPixels);
  hr = IMFMediaBuffer_Unlock(pBuffer);
  if (FAILED(hr)) goto cleanup;
  hr = IMFMediaBuffer_SetCurrentLength(pBuffer, cbPixels);
  if (FAILED(hr)) goto cleanup;
  hr = MFCreateSample(&pSample);
  if (FAILED(hr)) goto cleanup;
  hr = IMFSample_AddBuffer(pSample, pBuffer);
  if (FAILED(hr)) goto cleanup;
  hr = IMFSample_SetSampleTime(pSample, time100ns);
  if (FAILED(hr)) goto cleanup;

  *ppSample = pSample;
  pSample = NULL;

cleanup:
  if (pSample) IMFSample_Release(pSample);
  if (pBuffer) IMFMediaBuffer_Release(pBuffer);
  return hr;
}

static HRESULT
_scSubmitSample(IMFSinkWriter* pWriter, DWORD dwStreamIndex,
                IMFSample* pSample, LONGLONG duration100ns) {
  HRESULT hr = IMFSample_SetSampleDuration(pSample, duration100ns);
  if (FAILED(hr)) return hr;
  return IMFSinkWriter_WriteSample(pWriter, dwStreamIndex, pSample);
}

static HRESULT
_scCopyRegionToLatest(scRecordContext* pCtx,
                      ID3D11Device* pDevice,
                      ID3D11DeviceContext* pImmediateContext,
                      ID3D11Texture2D* pSource,
                      ID3D11Texture2D** ppStaging) {
  HRESULT hr;
  D3D11_TEXTURE2D_DESC sourceDesc;
  D3D11_TEXTURE2D_DESC stagingDesc;
  ID3D11Texture2D* pStaging = *ppStaging;
  D3D11_BOX box;
  D3D11_MAPPED_SUBRESOURCE mapped;

  ID3D11Texture2D_GetDesc(pSource, &sourceDesc);
  if (pCtx->nLocalX < 0 || pCtx->nLocalY < 0 ||
      (UINT64)(UINT32)pCtx->nLocalX + (UINT64)(UINT32)pCtx->nWidth > sourceDesc.Width ||
      (UINT64)(UINT32)pCtx->nLocalY + (UINT64)(UINT32)pCtx->nHeight > sourceDesc.Height) {
    return DXGI_ERROR_INVALID_CALL;
  }
  if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    scLogError("Recording: unsupported DXGI desktop format %u (expected BGRA8)", (unsigned)sourceDesc.Format);
    return DXGI_ERROR_UNSUPPORTED;
  }

  if (!pStaging) {
    ZeroMemory(&stagingDesc, sizeof(stagingDesc));
    stagingDesc.Width = (UINT)pCtx->nWidth;
    stagingDesc.Height = (UINT)pCtx->nHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = sourceDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(pDevice, &stagingDesc, NULL, &pStaging);
    if (FAILED(hr)) return hr;
    *ppStaging = pStaging;
  }

  ZeroMemory(&box, sizeof(box));
  box.left = (UINT)pCtx->nLocalX;
  box.top = (UINT)pCtx->nLocalY;
  box.front = 0;
  box.right = box.left + (UINT)pCtx->nWidth;
  box.bottom = box.top + (UINT)pCtx->nHeight;
  box.back = 1;
  ID3D11DeviceContext_CopySubresourceRegion(pImmediateContext,
                                             (ID3D11Resource*)pStaging, 0, 0, 0, 0,
                                             (ID3D11Resource*)pSource, 0, &box);
  hr = ID3D11DeviceContext_Map(pImmediateContext, (ID3D11Resource*)pStaging,
                               0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) return hr;

  EnterCriticalSection(&pCtx->csFrame);
  for (s32 y = 0; y < pCtx->nHeight; ++y) {
    memcpy(pCtx->pLatestPixels + (size_t)y * (size_t)pCtx->nWidth * 4u,
           (const BYTE*)mapped.pData + (size_t)y * mapped.RowPitch,
           (size_t)pCtx->nWidth * 4u);
  }
  pCtx->bHavePixels = true;
  ++pCtx->nFrameVersion;
  LeaveCriticalSection(&pCtx->csFrame);
  ID3D11DeviceContext_Unmap(pImmediateContext, (ID3D11Resource*)pStaging, 0);
  SetEvent(pCtx->hFirstFrameEvent);
  return S_OK;
}

static HRESULT
_scRecreateDuplication(ID3D11Device* pDevice, IDXGIOutput1* pOutput1,
                       IDXGIOutputDuplication** ppDuplication) {
  HRESULT hr;
  if (*ppDuplication) {
    IDXGIOutputDuplication_Release(*ppDuplication);
    *ppDuplication = NULL;
  }
  hr = IDXGIOutput1_DuplicateOutput(pOutput1, (IUnknown*)pDevice, ppDuplication);
  return hr;
}

static void
_scPublishLatestFrame(scRecordContext* pCtx, const BYTE* pPixels) {
  EnterCriticalSection(&pCtx->csFrame);
  if (!pCtx->bHavePixels || memcmp(pCtx->pLatestPixels, pPixels, pCtx->cbPixelsSize) != 0) {
    memcpy(pCtx->pLatestPixels, pPixels, pCtx->cbPixelsSize);
    pCtx->bHavePixels = true;
    ++pCtx->nFrameVersion;
  }
  LeaveCriticalSection(&pCtx->csFrame);
  SetEvent(pCtx->hFirstFrameEvent);
}

static void
_scUpdatePointer(scRecordContext* pCtx,
                 IDXGIOutputDuplication* pDuplication,
                 const DXGI_OUTDUPL_FRAME_INFO* pFrameInfo,
                 BYTE** ppShapeScratch,
                 UINT* pnShapeScratchCapacity) {
  if (pFrameInfo->PointerShapeBufferSize > 0) {
    const UINT required = pFrameInfo->PointerShapeBufferSize;
    if (*pnShapeScratchCapacity < required) {
      BYTE* pNew = (BYTE*)realloc(*ppShapeScratch, required);
      if (pNew) {
        *ppShapeScratch = pNew;
        *pnShapeScratchCapacity = required;
      }
    }
    if (*pnShapeScratchCapacity >= required && *ppShapeScratch) {
      UINT actualSize = 0;
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
      HRESULT hrShape;
      ZeroMemory(&shapeInfo, sizeof(shapeInfo));
      hrShape = IDXGIOutputDuplication_GetFramePointerShape(
        pDuplication, *pnShapeScratchCapacity, *ppShapeScratch, &actualSize, &shapeInfo);
      if (SUCCEEDED(hrShape) && actualSize <= *pnShapeScratchCapacity) {
        EnterCriticalSection(&pCtx->csFrame);
        if (pCtx->nPointerShapeBytes < actualSize) {
          BYTE* pNewShape = (BYTE*)realloc(pCtx->pPointerShape, actualSize);
          if (pNewShape) {
            pCtx->pPointerShape = pNewShape;
            pCtx->nPointerShapeBytes = actualSize;
          }
        }
        if (pCtx->pPointerShape && pCtx->nPointerShapeBytes >= actualSize) {
          memcpy(pCtx->pPointerShape, *ppShapeScratch, actualSize);
          pCtx->nPointerShapeBytes = actualSize; /* current shape size, not allocation capacity */
          pCtx->stPointerShapeInfo = shapeInfo;
          pCtx->bPointerShapeAvailable = true;
          ++pCtx->nFrameVersion;
        }
        LeaveCriticalSection(&pCtx->csFrame);
      } else if (FAILED(hrShape)) {
        scLogError("Recording: GetFramePointerShape failed (HRESULT 0x%08lX)",
                   (unsigned long)hrShape);
      }
    } else {
      scLogError("Recording: unable to allocate mouse pointer shape buffer");
    }
  }

  if (pFrameInfo->LastMouseUpdateTime.QuadPart != 0) {
    const LONG x = pFrameInfo->PointerPosition.Position.x - pCtx->nLocalX;
    const LONG y = pFrameInfo->PointerPosition.Position.y - pCtx->nLocalY;
    const bool bVisible = pFrameInfo->PointerPosition.Visible != FALSE;
    EnterCriticalSection(&pCtx->csFrame);
    if (pCtx->stPointerPosition.x != x || pCtx->stPointerPosition.y != y ||
        pCtx->bPointerVisible != bVisible) {
      ++pCtx->nFrameVersion;
    }
    pCtx->stPointerPosition.x = x;
    pCtx->stPointerPosition.y = y;
    pCtx->bPointerVisible = bVisible;
    LeaveCriticalSection(&pCtx->csFrame);
  }
}

static void
_scCompositePointer(scRecordContext* pCtx, BYTE* pPixels) {
  const DXGI_OUTDUPL_POINTER_SHAPE_INFO* pInfo = &pCtx->stPointerShapeInfo;
  const BYTE* pShape = pCtx->pPointerShape;
  s32 width, height;
  s32 startX, startY;

  if (!pCtx->bPointerVisible || !pCtx->bPointerShapeAvailable || !pShape) return;
  if (pInfo->Width == 0 || pInfo->Height == 0 || pInfo->Pitch == 0) return;
  if ((UINT64)pInfo->Height * (UINT64)pInfo->Pitch > (UINT64)pCtx->nPointerShapeBytes) return;

  width = (s32)pInfo->Width;
  height = (s32)pInfo->Height;
  if (pInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
    /* Monochrome shapes contain an AND mask followed by an XOR mask. */
    if ((height & 1) != 0 || pInfo->Pitch < (pInfo->Width + 7u) / 8u) return;
    height /= 2;
  } else if (pInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
             pInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
    if (pInfo->Pitch < pInfo->Width * 4u) return;
  } else {
    return;
  }

  startX = pCtx->stPointerPosition.x;
  startY = pCtx->stPointerPosition.y;
  for (s32 y = 0; y < height; ++y) {
    const s32 dy = startY + y;
    if (dy < 0 || dy >= pCtx->nHeight) continue;
    for (s32 x = 0; x < width; ++x) {
      const s32 dx = startX + x;
      BYTE* pDst;
      if (dx < 0 || dx >= pCtx->nWidth) continue;
      pDst = pPixels + ((size_t)dy * (size_t)pCtx->nWidth + (size_t)dx) * 4u;

      if (pInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        const BYTE* pSrc = pShape + (size_t)y * pInfo->Pitch + (size_t)x * 4u;
        const unsigned alpha = pSrc[3];
        if (alpha == 255) {
          pDst[0] = pSrc[0]; pDst[1] = pSrc[1]; pDst[2] = pSrc[2];
        } else if (alpha != 0) {
          const unsigned inverse = 255u - alpha;
          pDst[0] = (BYTE)((pSrc[0] * alpha + pDst[0] * inverse + 127u) / 255u);
          pDst[1] = (BYTE)((pSrc[1] * alpha + pDst[1] * inverse + 127u) / 255u);
          pDst[2] = (BYTE)((pSrc[2] * alpha + pDst[2] * inverse + 127u) / 255u);
        }
        pDst[3] = 255;
      } else if (pInfo->Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        const BYTE* pSrc = pShape + (size_t)y * pInfo->Pitch + (size_t)x * 4u;
        if (pSrc[3] == 0xFF) {
          pDst[0] ^= pSrc[0]; pDst[1] ^= pSrc[1]; pDst[2] ^= pSrc[2];
        } else {
          pDst[0] = pSrc[0]; pDst[1] = pSrc[1]; pDst[2] = pSrc[2];
        }
        pDst[3] = 255;
      } else {
        const size_t andRowOffset = (size_t)y * pInfo->Pitch;
        const size_t xorRowOffset = (size_t)(y + height) * pInfo->Pitch;
        const BYTE bit = (BYTE)(0x80u >> (x & 7));
        const bool andBit = (pShape[andRowOffset + (size_t)(x >> 3)] & bit) != 0;
        const bool xorBit = (pShape[xorRowOffset + (size_t)(x >> 3)] & bit) != 0;
        for (s32 channel = 0; channel < 3; ++channel) {
          pDst[channel] = (BYTE)(((andBit ? pDst[channel] : 0) ^ (xorBit ? 0xFF : 0)));
        }
        pDst[3] = 255;
      }
    }
  }
}

typedef struct {
  HDC hScreenDC;
  HDC hMemoryDC;
  HBITMAP hBitmap;
  HGDIOBJ hOriginalBitmap;
  BYTE* pBits;
} scGdiCapture;

static HRESULT
_scCreateGDICapture(scRecordContext* pCtx, scGdiCapture* pGDI) {
  BITMAPINFO bmi;
  ZeroMemory(pGDI, sizeof(*pGDI));

  pGDI->hScreenDC = GetDC(NULL);
  if (!pGDI->hScreenDC) {
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY);
  }

  pGDI->hMemoryDC = CreateCompatibleDC(pGDI->hScreenDC);
  if (!pGDI->hMemoryDC) {
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY);
  }

  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = pCtx->nWidth;
  bmi.bmiHeader.biHeight = -pCtx->nHeight; /* top-down DIB */
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  pGDI->hBitmap = CreateDIBSection(pGDI->hScreenDC, &bmi, DIB_RGB_COLORS,
                                  (void**)&pGDI->pBits, NULL, 0);
  if (!pGDI->hBitmap || !pGDI->pBits) {
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_NOT_ENOUGH_MEMORY);
  }

  pGDI->hOriginalBitmap = SelectObject(pGDI->hMemoryDC, pGDI->hBitmap);
  if (!pGDI->hOriginalBitmap || pGDI->hOriginalBitmap == HGDI_ERROR) {
    pGDI->hOriginalBitmap = NULL;
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_HANDLE);
  }
  return S_OK;
}

static void
_scReleaseGDICapture(scGdiCapture* pGDI) {
  if (pGDI->hMemoryDC && pGDI->hOriginalBitmap) {
    SelectObject(pGDI->hMemoryDC, pGDI->hOriginalBitmap);
    pGDI->hOriginalBitmap = NULL;
  }
  if (pGDI->hBitmap) { DeleteObject(pGDI->hBitmap); pGDI->hBitmap = NULL; }
  if (pGDI->hMemoryDC) { DeleteDC(pGDI->hMemoryDC); pGDI->hMemoryDC = NULL; }
  if (pGDI->hScreenDC) { ReleaseDC(NULL, pGDI->hScreenDC); pGDI->hScreenDC = NULL; }
  pGDI->pBits = NULL;
}

static void
_scDrawGDICursor(scRecordContext* pCtx, HDC hdc) {
  CURSORINFO ci;
  ICONINFO ii;
  int x, y;

  ZeroMemory(&ci, sizeof(ci));
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

  ZeroMemory(&ii, sizeof(ii));
  if (!GetIconInfo(ci.hCursor, &ii)) return;

  /* GetCursorInfo returns the cursor hot spot in screen coordinates. */
  x = ci.ptScreenPos.x - (int)ii.xHotspot - pCtx->stRect.x;
  y = ci.ptScreenPos.y - (int)ii.yHotspot - pCtx->stRect.y;
  DrawIconEx(hdc, x, y, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);

  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

static HRESULT
_scCaptureGDIFrame(scRecordContext* pCtx, scGdiCapture* pGDI) {
  if (!BitBlt(pGDI->hMemoryDC, 0, 0, pCtx->nWidth, pCtx->nHeight,
              pGDI->hScreenDC, pCtx->stRect.x, pCtx->stRect.y,
              SRCCOPY | CAPTUREBLT)) {
    DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
  }

  /* The hardware cursor is not consistently included in a GDI screen blit. */
  if (pCtx->bShowCursor) _scDrawGDICursor(pCtx, pGDI->hMemoryDC);
  GdiFlush();
  _scPublishLatestFrame(pCtx, pGDI->pBits);
  return S_OK;
}

static const char*
_scDescribeDXGIFailure(HRESULT hr) {
  if (hr == DXGI_ERROR_NOT_FOUND) return "area is not inside a single output";
  if (hr == DXGI_ERROR_UNSUPPORTED) return "rotated or HDR display, or unsupported format";
  if (hr == E_ACCESSDENIED) return "access denied, e.g. output on another GPU or secure desktop";
  return "Desktop Duplication unavailable";
}

static bool
_scDuplicationIsBGRA8(IDXGIOutputDuplication* pDuplication) {
  DXGI_OUTDUPL_DESC desc;
  ZeroMemory(&desc, sizeof(desc));
  IDXGIOutputDuplication_GetDesc(pDuplication, &desc);
  return desc.ModeDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

static void
_scReleaseDXGICapture(ID3D11Device** ppDevice,
                      ID3D11DeviceContext** ppImmediateContext,
                      IDXGIOutputDuplication** ppDuplication,
                      IDXGIOutput1** ppOutput1,
                      ID3D11Texture2D** ppStaging) {
  if (*ppStaging) { ID3D11Texture2D_Release(*ppStaging); *ppStaging = NULL; }
  if (*ppDuplication) { IDXGIOutputDuplication_Release(*ppDuplication); *ppDuplication = NULL; }
  if (*ppOutput1) { IDXGIOutput1_Release(*ppOutput1); *ppOutput1 = NULL; }
  if (*ppImmediateContext) { ID3D11DeviceContext_Release(*ppImmediateContext); *ppImmediateContext = NULL; }
  if (*ppDevice) { ID3D11Device_Release(*ppDevice); *ppDevice = NULL; }
}

static HRESULT
_scSwitchToGDI(scRecordContext* pCtx, scGdiCapture* pGDI,
               ID3D11Device** ppDevice,
               ID3D11DeviceContext** ppImmediateContext,
               IDXGIOutputDuplication** ppDuplication,
               IDXGIOutput1** ppOutput1,
               ID3D11Texture2D** ppStaging) {
  HRESULT hr;

  _scReleaseDXGICapture(ppDevice, ppImmediateContext, ppDuplication, ppOutput1, ppStaging);
  hr = _scCreateGDICapture(pCtx, pGDI);
  if (FAILED(hr)) {
    _scReleaseGDICapture(pGDI);
    return hr;
  }

  EnterCriticalSection(&pCtx->csFrame);
  pCtx->bPointerShapeAvailable = false;
  pCtx->bPointerVisible = false;
  ++pCtx->nFrameVersion;
  LeaveCriticalSection(&pCtx->csFrame);
  pCtx->bUsingGDI = true;
  return S_OK;
}

static DWORD WINAPI
_scCaptureThreadProc(void* pParam) {
  scRecordContext* pCtx = (scRecordContext*)pParam;
  HRESULT hr = S_OK;
  HRESULT hrRun = S_OK;
  bool bCoInitialized = false;
  bool bReadySignalled = false;
  BYTE* pPointerShapeScratch = NULL;
  UINT nPointerShapeScratchCapacity = 0;
  ID3D11Device* pDevice = NULL;
  ID3D11DeviceContext* pImmediateContext = NULL;
  IDXGIOutputDuplication* pDuplication = NULL;
  IDXGIOutput1* pOutput1 = NULL;
  ID3D11Texture2D* pStaging = NULL;
  scGdiCapture gdi;

  ZeroMemory(&gdi, sizeof(gdi));

  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) goto startup_finished;
  bCoInitialized = true;

  hr = _scCreateDuplicationForRect(pCtx, &pDevice, &pImmediateContext,
                                    &pDuplication, &pOutput1);
  if (SUCCEEDED(hr) && !_scDuplicationIsBGRA8(pDuplication)) hr = DXGI_ERROR_UNSUPPORTED;
  if (SUCCEEDED(hr)) {
    pCtx->bUsingGDI = false;
  } else {
    /* Desktop Duplication requires Windows 8+. Fall back on GDI/BitBlt on
       Windows 7 and whenever the DXGI backend cannot initialise or cannot
       represent the selected area (multiple monitors, rotation, HDR). */
    scLogInfo("Recording: DXGI capture unavailable (%s, HRESULT 0x%08lX); trying GDI fallback",
              _scDescribeDXGIFailure(hr), (unsigned long)hr);
    hr = _scSwitchToGDI(pCtx, &gdi, &pDevice, &pImmediateContext,
                        &pDuplication, &pOutput1, &pStaging);
    if (FAILED(hr)) {
      scLogError("Recording: GDI fallback initialisation failed (HRESULT 0x%08lX)",
                 (unsigned long)hr);
      goto startup_finished;
    }
  }

startup_finished:
  pCtx->hrCaptureStartup = hr;
  SetEvent(pCtx->hCaptureReadyEvent);
  bReadySignalled = true;
  if (FAILED(hr)) {
    hrRun = hr;
    goto cleanup;
  }

  if (pCtx->bUsingGDI) scLogInfo("Recording: using GDI capture backend");

  while (WaitForSingleObject(pCtx->hStopEvent, 0) != WAIT_OBJECT_0) {
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* pDesktopResource = NULL;
    ID3D11Texture2D* pDesktopTexture = NULL;
    HRESULT hrAcquire;
    HRESULT hrFrame = S_OK;

    if (pCtx->bUsingGDI) {
      hr = _scCaptureGDIFrame(pCtx, &gdi);
      if (FAILED(hr)) {
        hrRun = hr;
        break;
      }
      /* Limit the GDI polling rate; the writer independently controls output FPS. */
      {
        DWORD delayMs = (DWORD)(1000 / pCtx->nFPS);
        if (delayMs < 1) delayMs = 1;
        WaitForSingleObject(pCtx->hStopEvent, delayMs);
      }
      continue;
    }

    ZeroMemory(&frameInfo, sizeof(frameInfo));
    hrAcquire = IDXGIOutputDuplication_AcquireNextFrame(pDuplication,
                                                        SC_RECORD_ACQUIRE_TIMEOUT_MS,
                                                        &frameInfo,
                                                        &pDesktopResource);
    if (hrAcquire == S_OK) {
      if (frameInfo.LastPresentTime.QuadPart != 0 || !pCtx->bHavePixels) {
        hrFrame = IDXGIResource_QueryInterface(pDesktopResource, &IID_ID3D11Texture2D,
                                               (void**)&pDesktopTexture);
        if (SUCCEEDED(hrFrame)) {
          hrFrame = _scCopyRegionToLatest(pCtx, pDevice, pImmediateContext,
                                          pDesktopTexture, &pStaging);
        }
      }
      if (SUCCEEDED(hrFrame) && pCtx->bShowCursor) {
        _scUpdatePointer(pCtx, pDuplication, &frameInfo,
                         &pPointerShapeScratch, &nPointerShapeScratchCapacity);
      }
      if (pDesktopTexture) ID3D11Texture2D_Release(pDesktopTexture);
      if (pDesktopResource) IDXGIResource_Release(pDesktopResource);
      {
        HRESULT hrRelease = IDXGIOutputDuplication_ReleaseFrame(pDuplication);
        if (SUCCEEDED(hrFrame) && FAILED(hrRelease)) hrFrame = hrRelease;
      }
      if (FAILED(hrFrame)) {
        scLogInfo("Recording: DXGI frame capture failed (HRESULT 0x%08lX); switching to GDI",
                  (unsigned long)hrFrame);
        hr = _scSwitchToGDI(pCtx, &gdi, &pDevice, &pImmediateContext,
                            &pDuplication, &pOutput1, &pStaging);
        if (FAILED(hr)) {
          hrRun = hrFrame;
          break;
        }
      }
    } else if (hrAcquire == DXGI_ERROR_WAIT_TIMEOUT) {
      continue;
    } else if (hrAcquire == DXGI_ERROR_ACCESS_LOST) {
      /* Mode changes, UAC prompts and the lock screen invalidate the
         duplication; keep retrying for a while instead of ending the recording. */
      HRESULT hrRecreate = _scRecreateDuplication(pDevice, pOutput1, &pDuplication);
      s32 retry;
      for (retry = 0; FAILED(hrRecreate) && retry < SC_RECORD_RECREATE_RETRIES; ++retry) {
        if (WaitForSingleObject(pCtx->hStopEvent, SC_RECORD_RECREATE_DELAY_MS) == WAIT_OBJECT_0) break;
        hrRecreate = _scRecreateDuplication(pDevice, pOutput1, &pDuplication);
      }
      if (WaitForSingleObject(pCtx->hStopEvent, 0) == WAIT_OBJECT_0) break;
      if (SUCCEEDED(hrRecreate) && !_scDuplicationIsBGRA8(pDuplication)) {
        hrRecreate = DXGI_ERROR_UNSUPPORTED;
      }
      if (FAILED(hrRecreate)) {
        scLogInfo("Recording: unable to recover DXGI capture (%s, HRESULT 0x%08lX); switching to GDI",
                  _scDescribeDXGIFailure(hrRecreate), (unsigned long)hrRecreate);
        hr = _scSwitchToGDI(pCtx, &gdi, &pDevice, &pImmediateContext,
                            &pDuplication, &pOutput1, &pStaging);
        if (FAILED(hr)) {
          hrRun = hrRecreate;
          break;
        }
      }
    } else {
      scLogInfo("Recording: DXGI acquire failed (HRESULT 0x%08lX); switching to GDI",
                (unsigned long)hrAcquire);
      hr = _scSwitchToGDI(pCtx, &gdi, &pDevice, &pImmediateContext,
                          &pDuplication, &pOutput1, &pStaging);
      if (FAILED(hr)) {
        hrRun = hrAcquire;
        break;
      }
    }
  }

cleanup:
  if (!bReadySignalled) {
    pCtx->hrCaptureStartup = FAILED(hr) ? hr : E_FAIL;
    SetEvent(pCtx->hCaptureReadyEvent);
  }
  if (FAILED(hrRun)) {
    _scRecordSetFailure(pCtx, hrRun);
    SetEvent(pCtx->hStopEvent);
  }
  _scReleaseDXGICapture(&pDevice, &pImmediateContext, &pDuplication, &pOutput1, &pStaging);
  _scReleaseGDICapture(&gdi);
  free(pPointerShapeScratch);
  if (bCoInitialized) CoUninitialize();
  return 0;
}

static DWORD WINAPI
_scWriterThreadProc(void* pParam) {
  scRecordContext* pCtx = (scRecordContext*)pParam;
  HRESULT hr = S_OK;
  HRESULT hrRun = S_OK;
  bool bCoInitialized = false;
  bool bMFStarted = false;
  bool bReadySignalled = false;
  bool bRunStarted = false;
  BYTE* pFramePixels = NULL;
  BYTE* pScaledPixels = NULL;
  BYTE* pNV12Pixels = NULL;
  uint16_t* pScaleTemp = NULL;
  IMFSinkWriter* pWriter = NULL;
  IMFSample* pPendingSample = NULL;
  DWORD dwStreamIndex = 0;
  LONGLONG lastFrameIndex = -1;
  LONGLONG pendingSampleTime = 0;
  ULONGLONG convertedVersion = 0;
  bool bHaveConverted = false;
  size_t cbNV12Size = 0;
  size_t cbScaledSize = 0;
  DWORD cbNV12 = 0;

  pFramePixels = (BYTE*)malloc(pCtx->cbPixelsSize);
  if (!pFramePixels) {
    hr = E_OUTOFMEMORY;
    goto startup_finished;
  }
  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) goto startup_finished;
  bCoInitialized = true;
  hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(hr)) goto startup_finished;
  bMFStarted = true;
  pCtx->bOutputFileMayExist = true;
  hr = _scCreateSinkWriter(pCtx, &pWriter, &dwStreamIndex);

  /* Encoder limits vary by OS version, MFT and driver. A selected region can
     fail even when it is smaller than the monitor, so retry a sequence of
     smaller output sizes whenever type negotiation fails. Capture remains at
     the original dimensions; only the encoded frames are scaled. */
  if (hr == MF_E_INVALIDMEDIATYPE) {
    static const s32 limits[][2] = {
      { 1920, 1080 },
      { 1280, 720 },
      { 854, 480 },
      { 640, 360 }
    };
    size_t limitIndex;
    for (limitIndex = 0; limitIndex < ARRAYSIZE(limits) &&
                         hr == MF_E_INVALIDMEDIATYPE; ++limitIndex) {
      const s32 previousWidth = pCtx->nEncodeWidth;
      const s32 previousHeight = pCtx->nEncodeHeight;
      _scChooseScaledEncoderSize(pCtx, limits[limitIndex][0], limits[limitIndex][1]);

      /* Skip entries which would not produce a smaller or different frame. */
      if (pCtx->nEncodeWidth >= previousWidth &&
          pCtx->nEncodeHeight >= previousHeight) {
        continue;
      }

      scLogInfo("Recording: encoder rejected %d x %d; retrying encoded output at %d x %d",
                previousWidth, previousHeight,
                pCtx->nEncodeWidth, pCtx->nEncodeHeight);
      if (!DeleteFileW(pCtx->wszSavePath) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        scLogError("Recording: unable to remove failed output before resolution retry: %lu",
                   GetLastError());
        goto startup_finished;
      }
      hr = _scCreateSinkWriter(pCtx, &pWriter, &dwStreamIndex);
    }
  }

  if (SUCCEEDED(hr)) {
    UINT64 cbNV12Size64 = (UINT64)(UINT32)pCtx->nEncodeWidth *
                          (UINT64)(UINT32)pCtx->nEncodeHeight * 3u / 2u;
    UINT64 cbScaledSize64 = (UINT64)(UINT32)pCtx->nEncodeWidth *
                            (UINT64)(UINT32)pCtx->nEncodeHeight * 4u;
    if (cbNV12Size64 == 0 || cbNV12Size64 > MAXDWORD || cbScaledSize64 > SIZE_MAX) {
      hr = E_OUTOFMEMORY;
      goto startup_finished;
    }
    cbNV12Size = (size_t)cbNV12Size64;
    cbNV12 = (DWORD)cbNV12Size;
    pNV12Pixels = (BYTE*)malloc(cbNV12Size);
    if (!pNV12Pixels) {
      hr = E_OUTOFMEMORY;
      goto startup_finished;
    }
    if (pCtx->nEncodeWidth != pCtx->nWidth ||
        pCtx->nEncodeHeight != pCtx->nHeight) {
      const UINT64 cbScaleTemp64 = (UINT64)(UINT32)pCtx->nEncodeWidth *
                                   (UINT64)(UINT32)pCtx->nHeight * 3u * sizeof(uint16_t);
      if (cbScaleTemp64 > SIZE_MAX) {
        hr = E_OUTOFMEMORY;
        goto startup_finished;
      }
      cbScaledSize = (size_t)cbScaledSize64;
      pScaledPixels = (BYTE*)malloc(cbScaledSize);
      pScaleTemp = (uint16_t*)malloc((size_t)cbScaleTemp64);
      if (!pScaledPixels || !pScaleTemp) {
        hr = E_OUTOFMEMORY;
        goto startup_finished;
      }
      scLogInfo("Recording: scaling captured frames from %d x %d to encoded size %d x %d",
                pCtx->nWidth, pCtx->nHeight,
                pCtx->nEncodeWidth, pCtx->nEncodeHeight);
    }
  }

startup_finished:
  pCtx->hrWriterStartup = hr;
  SetEvent(pCtx->hWriterReadyEvent);
  bReadySignalled = true;
  if (FAILED(hr)) {
    hrRun = hr;
    goto cleanup;
  }

  {
    HANDLE waits[2] = { pCtx->hRunEvent, pCtx->hStopEvent };
    DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE);
    if (waitResult != WAIT_OBJECT_0) goto cleanup;
    if (WaitForSingleObject(pCtx->hStopEvent, 0) == WAIT_OBJECT_0) goto cleanup;
    bRunStarted = true;
  }

  while (WaitForSingleObject(pCtx->hStopEvent, 0) != WAIT_OBJECT_0) {
    LARGE_INTEGER qpcNow;
    LONGLONG elapsedQpc;
    LONGLONG dueFrameIndex;

    if (!QueryPerformanceCounter(&qpcNow)) {
      hrRun = HRESULT_FROM_WIN32(GetLastError());
      if (SUCCEEDED(hrRun)) hrRun = E_FAIL;
      break;
    }
    elapsedQpc = qpcNow.QuadPart - pCtx->qpcStart.QuadPart;
    if (elapsedQpc < 0) elapsedQpc = 0;
    dueFrameIndex = (elapsedQpc * (LONGLONG)pCtx->nFPS) / pCtx->qpcFrequency.QuadPart;

    if (dueFrameIndex > lastFrameIndex) {
      /* Drop missed frame deadlines instead of trying to encode every stale
         slot. On slower systems, RGB scaling, NV12 conversion and software
         H.264 encoding may take longer than 1/FPS. Writing all missed frames
         in a catch-up loop compresses wall-clock time and makes the video play
         too fast. Timestamp the newest available frame at its real-time slot,
         and hold it until the next sample's timestamp is known. */
      const LONGLONG sampleIndex = dueFrameIndex;
      const LONGLONG sampleTime = _scFrameTime100ns(sampleIndex, pCtx->nFPS);
      IMFSample* pSample = NULL;
      bool bNeedConvert;

      if (WaitForSingleObject(pCtx->hStopEvent, 0) == WAIT_OBJECT_0) break;
      EnterCriticalSection(&pCtx->csFrame);
      if (!pCtx->bHavePixels) {
        LeaveCriticalSection(&pCtx->csFrame);
        continue;
      }
      bNeedConvert = !bHaveConverted || convertedVersion != pCtx->nFrameVersion;
      if (bNeedConvert) {
        memcpy(pFramePixels, pCtx->pLatestPixels, pCtx->cbPixelsSize);
        if (pCtx->bShowCursor) _scCompositePointer(pCtx, pFramePixels);
        convertedVersion = pCtx->nFrameVersion;
      }
      LeaveCriticalSection(&pCtx->csFrame);

      if (bNeedConvert) {
        if (pScaledPixels) {
          _scScaleBGRA(pFramePixels, pCtx->nWidth, pCtx->nHeight,
                       pScaledPixels, pCtx->nEncodeWidth, pCtx->nEncodeHeight, pScaleTemp);
          _scConvertBGRAtoNV12(pScaledPixels, pCtx->nEncodeWidth,
                               pCtx->nEncodeHeight, pNV12Pixels);
        } else {
          _scConvertBGRAtoNV12(pFramePixels, pCtx->nWidth, pCtx->nHeight, pNV12Pixels);
        }
        bHaveConverted = true;
      }

      hr = _scCreateSample(pNV12Pixels, cbNV12, sampleTime, &pSample);
      if (FAILED(hr)) {
        hrRun = hr;
        break;
      }

      if (pPendingSample) {
        LONGLONG duration = sampleTime - pendingSampleTime;
        if (duration <= 0) duration = SC_RECORD_HNS_PER_SECOND / pCtx->nFPS;
        hr = _scSubmitSample(pWriter, dwStreamIndex, pPendingSample, duration);
        IMFSample_Release(pPendingSample);
        pPendingSample = NULL;
        if (FAILED(hr)) {
          IMFSample_Release(pSample);
          hrRun = hr;
          break;
        }
        ++pCtx->nFramesWritten;
      }

      /* Keep the current sample pending until the next timestamp determines
         its duration. Unchanged frames skip scaling and conversion and reuse
         the previous NV12 image. */
      pPendingSample = pSample;
      pendingSampleTime = sampleTime;
      lastFrameIndex = sampleIndex;
      continue;
    }

    /* Wait until the next absolute frame deadline; recompute from QPC every
       iteration so scheduling delays do not accumulate as timing drift. */
    {
      const LONGLONG nextFrameIndex = lastFrameIndex + 1;
      const LONGLONG targetQpc = pCtx->qpcStart.QuadPart +
        ((nextFrameIndex * pCtx->qpcFrequency.QuadPart + pCtx->nFPS - 1) / pCtx->nFPS);
      LONGLONG remainingQpc = targetQpc - qpcNow.QuadPart;
      DWORD timeoutMs;
      if (remainingQpc <= 0) timeoutMs = 0;
      else {
        LONGLONG ms = (remainingQpc * 1000 + pCtx->qpcFrequency.QuadPart - 1) /
                      pCtx->qpcFrequency.QuadPart;
        if (ms < 1) ms = 1;
        if (ms > 1000) ms = 1000;
        timeoutMs = (DWORD)ms;
      }
      WaitForSingleObject(pCtx->hStopEvent, timeoutMs);
    }
  }

cleanup:
  if (!bReadySignalled) {
    pCtx->hrWriterStartup = FAILED(hr) ? hr : E_FAIL;
    SetEvent(pCtx->hWriterReadyEvent);
  }
  if (pWriter && bRunStarted && SUCCEEDED(hrRun) && pPendingSample) {
    LARGE_INTEGER qpcEnd;
    LONGLONG endTime;
    LONGLONG finalDuration;
    const LONGLONG minimumDuration = SC_RECORD_HNS_PER_SECOND / pCtx->nFPS;

    if (InterlockedCompareExchange(&pCtx->bQpcStopSet, 0, 0) != 0) {
      qpcEnd = pCtx->qpcStop;
    } else if (!QueryPerformanceCounter(&qpcEnd)) {
      qpcEnd.QuadPart = 0;
    }
    if (qpcEnd.QuadPart == 0) {
      endTime = pendingSampleTime + minimumDuration;
    } else {
      LONGLONG elapsed = qpcEnd.QuadPart - pCtx->qpcStart.QuadPart;
      if (elapsed < 0) elapsed = 0;
      endTime = (elapsed / pCtx->qpcFrequency.QuadPart) * SC_RECORD_HNS_PER_SECOND +
                ((elapsed % pCtx->qpcFrequency.QuadPart) * SC_RECORD_HNS_PER_SECOND) /
                  pCtx->qpcFrequency.QuadPart;
    }
    finalDuration = endTime - pendingSampleTime;
    if (finalDuration < minimumDuration) finalDuration = minimumDuration;
    hr = _scSubmitSample(pWriter, dwStreamIndex, pPendingSample, finalDuration);
    if (FAILED(hr)) hrRun = hr;
    else ++pCtx->nFramesWritten;
  }
  if (pPendingSample) IMFSample_Release(pPendingSample);
  if (pWriter) {
    HRESULT hrFinalize = IMFSinkWriter_Finalize(pWriter);
    if (bRunStarted && SUCCEEDED(hrRun) && FAILED(hrFinalize)) hrRun = hrFinalize;
    IMFSinkWriter_Release(pWriter);
  }
  if (bRunStarted && SUCCEEDED(hrRun) && pCtx->nFramesWritten == 0) hrRun = E_FAIL;
  if (bRunStarted && FAILED(hrRun)) {
    _scRecordSetFailure(pCtx, hrRun);
    SetEvent(pCtx->hStopEvent);
  }
  free(pFramePixels);
  free(pScaledPixels);
  free(pScaleTemp);
  free(pNV12Pixels);
  if (bMFStarted) MFShutdown();
  if (bCoInitialized) CoUninitialize();
  return 0;
}

static void
_scCloseRecordingResources(scRecordContext* pCtx) {
  #define _CLOSE_RESOURCE(r) { CloseHandle(r); r = NULL; }
  _CLOSE_RESOURCE(pCtx->hCaptureThread);
  _CLOSE_RESOURCE(pCtx->hWriterThread);
  _CLOSE_RESOURCE(pCtx->hStopEvent);
  _CLOSE_RESOURCE(pCtx->hCaptureReadyEvent);
  _CLOSE_RESOURCE(pCtx->hWriterReadyEvent);
  _CLOSE_RESOURCE(pCtx->hFirstFrameEvent);
  _CLOSE_RESOURCE(pCtx->hRunEvent);

  if (pCtx->bFrameLockInitialized) {
    DeleteCriticalSection(&pCtx->csFrame);
    pCtx->bFrameLockInitialized = false;
  }
  free(pCtx->pLatestPixels);
  pCtx->pLatestPixels = NULL;
  free(pCtx->pPointerShape);
  pCtx->pPointerShape = NULL;
  pCtx->nPointerShapeBytes = 0;
  pCtx->bPointerShapeAvailable = false;
  pCtx->bPointerVisible = false;
  pCtx->cbPixelsSize = 0;
}

static void
_scAbortRecordingStart(scRecordContext* pCtx) {
  if (pCtx->hStopEvent) SetEvent(pCtx->hStopEvent);
  if (pCtx->hRunEvent) SetEvent(pCtx->hRunEvent);
  if (pCtx->hCaptureThread) WaitForSingleObject(pCtx->hCaptureThread, INFINITE);
  if (pCtx->hWriterThread) WaitForSingleObject(pCtx->hWriterThread, INFINITE);
  {
    const bool bDeletePartialOutput = pCtx->bOutputFileMayExist;
    _scCloseRecordingResources(pCtx);
    if (bDeletePartialOutput && pCtx->wszSavePath[0]) DeleteFileW(pCtx->wszSavePath);
  }
}

scInternal bool
_startRecording(scRecordContext* pCtx, scRect rect) {
  s32 width = rect.w & ~1;
  s32 height = rect.h & ~1;
  s32 fps = gApp->config.iFFmpegFramerate;
  scRecordQuality quality = gApp->config.eRecordQuality;
  scRecordMaxRes maxRes = gApp->config.eRecordMaxRes;
  HRESULT hr;
  LARGE_INTEGER qpcStart;
  const DWORD startupTimeoutMs = 15000;

  if (width <= 0 || height <= 0) {
    scLogError("Failed to start recording: invalid capture size %d x %d", rect.w, rect.h);
    return false;
  }
  if (fps < 1 || fps > 120) fps = SC_RECORD_DEFAULT_FPS;
  if ((int)quality < 0 || (int)quality >= SC_QUALITY_COUNT) quality = SC_QUALITY_HIGH;
  if ((int)maxRes < 0 || (int)maxRes >= SC_MAXRES_COUNT) maxRes = SC_MAXRES_NATIVE;

  pCtx->stRect = rect;
  pCtx->nWidth = width;
  pCtx->nHeight = height;
  pCtx->nEncodeWidth = width;
  pCtx->nEncodeHeight = height;
  pCtx->nFPS = fps;
  pCtx->eQuality = quality;
  pCtx->bShowCursor = gApp->config.bRecordShowCursor;
  pCtx->bForceSoftware = gApp->config.bRecordForceSoftware;
  if (maxRes == SC_MAXRES_1080P) _scChooseScaledEncoderSize(pCtx, 1920, 1080);
  else if (maxRes == SC_MAXRES_720P) _scChooseScaledEncoderSize(pCtx, 1280, 720);
  pCtx->hrCaptureStartup = E_PENDING;
  pCtx->hrWriterStartup = E_PENDING;
  pCtx->bOutputFileMayExist = false;
  pCtx->hrResult = S_OK;
  pCtx->nFramesWritten = 0;
  pCtx->qpcStop.QuadPart = 0;
  InterlockedExchange(&pCtx->bQpcStopSet, 0);
  pCtx->bHavePixels = false;
  pCtx->nFrameVersion = 0;
  pCtx->bUseFragmented = true;
  pCtx->eRotation = DXGI_MODE_ROTATION_UNSPECIFIED;
  pCtx->bUsingGDI = false;

  if (!QueryPerformanceFrequency(&pCtx->qpcFrequency) || pCtx->qpcFrequency.QuadPart <= 0) {
    scLogError("Failed to start recording: QueryPerformanceFrequency failed");
    return false;
  }

  if ((size_t)width > SIZE_MAX / 4u / (size_t)height) {
    scLogError("Failed to start recording: capture dimensions overflow the frame buffer size");
    return false;
  }
  pCtx->cbPixelsSize = (size_t)width * (size_t)height * 4u;
  if (pCtx->cbPixelsSize > MAXDWORD) {
    scLogError("Failed to start recording: frame buffer exceeds Media Foundation's buffer limit");
    pCtx->cbPixelsSize = 0;
    return false;
  }
  pCtx->pLatestPixels = (BYTE*)malloc(pCtx->cbPixelsSize);
  if (!pCtx->pLatestPixels) {
    pCtx->cbPixelsSize = 0;
    scLogError("Failed to start recording: unable to allocate frame buffer");
    return false;
  }
  InitializeCriticalSection(&pCtx->csFrame);
  pCtx->bFrameLockInitialized = true;

  { // Get save path
    wchar_t wszDir[MAX_PATH];
    wchar_t wszName[MAX_PATH];
    if (!scGetSavePath(wszDir, MAX_PATH) || !scGetFilename(wszName, MAX_PATH, ".mp4")) {
      scLogError("Failed to start recording: unable to create output filename");
      _scCloseRecordingResources(pCtx);
      return false;
    }
    if (swprintf_s(pCtx->wszSavePath, ARRAYSIZE(pCtx->wszSavePath), L"%ls\\%ls", wszDir, wszName) < 0) {
      scLogError("Failed to start recording: output path is too long");
      _scCloseRecordingResources(pCtx);
      return false;
    }
  }

  pCtx->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  pCtx->hCaptureReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  pCtx->hWriterReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  pCtx->hFirstFrameEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  pCtx->hRunEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!pCtx->hStopEvent || !pCtx->hCaptureReadyEvent || !pCtx->hWriterReadyEvent ||
      !pCtx->hFirstFrameEvent || !pCtx->hRunEvent) {
    scLogError("Failed to start recording: CreateEventW failed: %lu", GetLastError());
    _scAbortRecordingStart(pCtx);
    return false;
  }

  pCtx->hCaptureThread = CreateThread(NULL, 0, _scCaptureThreadProc, pCtx, 0, NULL);
  if (!pCtx->hCaptureThread) {
    scLogError("Failed to start recording: capture CreateThread failed: %lu", GetLastError());
    _scAbortRecordingStart(pCtx);
    return false;
  }
  pCtx->hWriterThread = CreateThread(NULL, 0, _scWriterThreadProc, pCtx, 0, NULL);
  if (!pCtx->hWriterThread) {
    scLogError("Failed to start recording: writer CreateThread failed: %lu", GetLastError());
    _scAbortRecordingStart(pCtx);
    return false;
  }

  if (WaitForSingleObject(pCtx->hCaptureReadyEvent, startupTimeoutMs) != WAIT_OBJECT_0) {
    scLogError("Failed to start recording: timed out initialising screen capture");
    _scAbortRecordingStart(pCtx);
    return false;
  }
  hr = pCtx->hrCaptureStartup;
  if (FAILED(hr)) {
    _scRecordLogHRESULT("screen capture initialisation (DXGI and GDI)", hr);
    _scAbortRecordingStart(pCtx);
    return false;
  }

  if (WaitForSingleObject(pCtx->hWriterReadyEvent, startupTimeoutMs) != WAIT_OBJECT_0) {
    scLogError("Failed to start recording: timed out initialising Media Foundation");
    _scAbortRecordingStart(pCtx);
    return false;
  }
  hr = pCtx->hrWriterStartup;
  if (FAILED(hr)) {
    _scRecordLogHRESULT("Media Foundation writer initialisation", hr);
    _scAbortRecordingStart(pCtx);
    return false;
  }

  {
    HANDLE hWaits[3] = { pCtx->hFirstFrameEvent, pCtx->hCaptureThread, pCtx->hWriterThread };
    DWORD dwWaitResult = WaitForMultipleObjects(ARRAYSIZE(hWaits), hWaits, FALSE, startupTimeoutMs);
    if (dwWaitResult != WAIT_OBJECT_0) {
      scLogError("Failed to start recording: timed out waiting for the first desktop frame");
      _scAbortRecordingStart(pCtx);
      return false;
    }
  }

  if (WaitForSingleObject(pCtx->hStopEvent, 0) == WAIT_OBJECT_0) {
    hr = (HRESULT)InterlockedCompareExchange(&pCtx->hrResult, S_OK, S_OK);
    if (FAILED(hr)) _scRecordLogHRESULT("capture stopped during startup", hr);
    else scLogError("Failed to start recording: capture stopped before the first frame was ready");
    _scAbortRecordingStart(pCtx);
    return false;
  }

  /* The first image is ready. Set the shared A/V clock immediately before the
     writer starts; audio capture can use this exact QPC origin later. */
  if (!QueryPerformanceCounter(&qpcStart)) {
    scLogError("Failed to start recording: QueryPerformanceCounter failed");
    _scAbortRecordingStart(pCtx);
    return false;
  }
  pCtx->qpcStart = qpcStart;
  SetEvent(pCtx->hRunEvent);

  pCtx->bStarted = true;
  scPlaySoundOrSkip(SC_SOUND_SCREENSHOT_QUICK);
  scLogInfo("Started native recording: { %d, %d, %d, %d }, capture %d x %d, encode %d x %d at %d FPS -> %ls",
            rect.x, rect.y, width, height, width, height,
            pCtx->nEncodeWidth, pCtx->nEncodeHeight, fps, pCtx->wszSavePath);
  return true;
}

static bool
_scFinishRecording(scRecordContext* pCtx) {
  HRESULT hr;
  HANDLE threads[2];
  DWORD waitResult;

  threads[0] = pCtx->hCaptureThread;
  threads[1] = pCtx->hWriterThread;
  waitResult = WaitForMultipleObjects(ARRAYSIZE(threads), threads, TRUE, INFINITE);
  if (waitResult != WAIT_OBJECT_0) {
    scLogError("Failed to stop recording: waiting for recorder threads failed: %lu", GetLastError());
    return false;
  }

  hr = (HRESULT)InterlockedCompareExchange(&pCtx->hrResult, S_OK, S_OK);
  if (FAILED(hr)) {
    _scRecordLogHRESULT("capture/encoding/finalisation", hr);
  } else if (pCtx->nFramesWritten == 0) {
    scLogError("Recording stopped without writing any frames");
  } else {
    DWORD attrs = GetFileAttributesW(pCtx->wszSavePath);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      scUIOnCaptureSaved(pCtx->wszSavePath);
      scLogInfo("Saved recording (%llu frames): %ls",
                (unsigned long long)pCtx->nFramesWritten, pCtx->wszSavePath);
    } else {
      scLogError("Recording finalised, but output file was not found: %ls", pCtx->wszSavePath);
    }
  }

  _scCloseRecordingResources(pCtx);
  pCtx->bStarted = false;
  return true;
}

static DWORD WINAPI
_scFinalizeRecordingProc(void* pParam) {
  scRecordContext* pCtx = (scRecordContext*)pParam;
  /* If joining the recorder threads failed, they may still be using the
     context, so leak it rather than free it. */
  if (_scFinishRecording(pCtx)) {
    free(pCtx);
  }
  InterlockedDecrement(&gPendingStops);
  return 0;
}

scInternal bool
_stopRecording(scRecordContext* pCtx) {
  HANDLE hFinalizer;

  if (!pCtx) return false;
  if (!pCtx->hCaptureThread || !pCtx->hWriterThread) {
    scLogError("Failed to stop recording: capture or writer thread was not created");
    return false;
  }

  if (QueryPerformanceCounter(&pCtx->qpcStop)) {
    InterlockedExchange(&pCtx->bQpcStopSet, 1);
  }
  SetEvent(pCtx->hStopEvent);
  scPlaySoundOrSkip(SC_SOUND_SCREENSHOT);

  InterlockedIncrement(&gPendingStops);
  hFinalizer = CreateThread(NULL, 0, _scFinalizeRecordingProc, pCtx, 0, NULL);
  if (!hFinalizer) {
    scLogError("Failed to create recording finalizer thread: %lu; finalising synchronously", GetLastError());
    _scFinalizeRecordingProc(pCtx);
    return true;
  }
  CloseHandle(hFinalizer);
  return true;
}

bool
scRecordWaitForPendingStops(DWORD timeoutMs) {
  const DWORD start = GetTickCount();
  while (InterlockedCompareExchange(&gPendingStops, 0, 0) != 0) {
    if (GetTickCount() - start >= timeoutMs) return false;
    Sleep(10);
  }
  return true;
}

scInternal bool
cbOnHotkeyPressed(scCaptureContext* pCtx) {
  if (pCtx->pUser) {
    scRecordContext* pRecordCtx = (scRecordContext*)pCtx->pUser;
    pCtx->pUser = NULL;
    if (!_stopRecording(pRecordCtx)) {
      free(pRecordCtx);
    }
    return true;
  }

  scRecordContext* pRecordCtx = (scRecordContext*)calloc(1, sizeof(scRecordContext));
  if (!pRecordCtx) {
    scLogError("Unable to allocate recording context");
    return true;
  }

  pCtx->pUser = (void*)pRecordCtx;
  scCtxRequestCaptureArea(pCtx);
  return false;
}

scInternal bool
cbOnAreaSelected(scCaptureContext* pCtx) {
  scRecordContext* pRecordCtx;
  scAssert(pCtx->pUser, "pUser is null!");
  if (!pCtx->pUser) {
    scLogError("Recording context is null in cbOnAreaSelected");
    return true;
  }

  pRecordCtx = (scRecordContext*)pCtx->pUser;
  if (!_startRecording(pRecordCtx, pCtx->stSelectedRect)) {
    pCtx->pUser = NULL;
    free(pRecordCtx);
    return true;
  }
  return false;
}

const scCaptureHandler scRecordHandler = {
  cbOnHotkeyPressed,
  cbOnAreaSelected,
  NULL
};