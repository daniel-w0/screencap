#include "pch.h"

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include <string>
#include <vector>
#include <thread>

extern "C" {
#include "scUI.h"
#include "scNotification.h"
#include "scLogging.h"

unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int reqComp);
void           stbi_image_free(void* p);
int            stbi_write_png_to_func(void (*func)(void*, void*, int), void* context, int w, int h, int comp, const void* data, int strideBytes);
}

using namespace winrt;
using namespace winrt::Windows::Data::Xml::Dom;
using namespace winrt::Windows::UI::Notifications;

#define SC_AUMID L"Screencap.App"

//------------------------------------------------------------------------
// File / image helpers
//------------------------------------------------------------------------
static void
_scByteSink(void* pContext, void* pData, int nSize) {
  std::vector<uint8_t>* pOut = (std::vector<uint8_t>*)pContext;
  pOut->insert(pOut->end(), (uint8_t*)pData, (uint8_t*)pData + nSize);
}

static std::vector<uint8_t>
_scReadFile(const std::wstring& path) {
  std::vector<uint8_t> out;
  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    return out;
  }
  LARGE_INTEGER liSize;
  if (GetFileSizeEx(hFile, &liSize) && liSize.QuadPart > 0 && liSize.QuadPart < (1 << 30)) {
    out.resize((size_t)liSize.QuadPart);
    DWORD dwRead = 0;
    if (!ReadFile(hFile, out.data(), (DWORD)out.size(), &dwRead, nullptr) || dwRead != out.size()) {
      out.clear();
    }
  }
  CloseHandle(hFile);
  return out;
}

static bool
_scWriteFile(const std::wstring& path, const void* pData, size_t nSize) {
  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD dwWritten = 0;
  bool ok = WriteFile(hFile, pData, (DWORD)nSize, &dwWritten, nullptr) && dwWritten == nSize;
  CloseHandle(hFile);
  return ok;
}

static std::wstring
_scAppDataDir() {
  std::wstring dir;
  PWSTR pszPath = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath))) {
    dir = std::wstring(pszPath) + L"\\Screencap";
    CoTaskMemFree(pszPath);
  }
  return dir;
}

static std::wstring
_scXmlEscape(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    switch (c) {
      case L'&':  out += L"&amp;";  break;
      case L'<':  out += L"&lt;";   break;
      case L'>':  out += L"&gt;";   break;
      case L'"':  out += L"&quot;"; break;
      case L'\'': out += L"&apos;"; break;
      default:    out += c;         break;
    }
  }
  return out;
}

static std::wstring
_scFileUri(const std::wstring& path) {
  std::wstring out = L"file:///";
  for (wchar_t c : path) {
    switch (c) {
      case L'\\': out += L'/';   break;
      case L' ':  out += L"%20"; break;
      case L'#':  out += L"%23"; break;
      case L'?':  out += L"%3F"; break;
      case L'%':  out += L"%25"; break;
      default:    out += c;      break;
    }
  }
  return out;
}

//------------------------------------------------------------------------
// App icon -> PNG (for the toast's IconUri)
//------------------------------------------------------------------------
static bool
_scWriteAppIcon(const std::wstring& path) {
  HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
  if (!hIcon) {
    return false;
  }

  bool ok = false;
  ICONINFO ii = {};
  if (GetIconInfo(hIcon, &ii)) {
    BITMAP bm = {};
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) {
      int w = bm.bmWidth;
      int h = bm.bmHeight;

      BITMAPINFO bmi = {};
      bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
      bmi.bmiHeader.biWidth       =  w;
      bmi.bmiHeader.biHeight      = -h;
      bmi.bmiHeader.biPlanes      = 1;
      bmi.bmiHeader.biBitCount    = 32;
      bmi.bmiHeader.biCompression = BI_RGB;

      std::vector<uint8_t> px((size_t)w * h * 4);
      HDC hDC = GetDC(nullptr);
      if (GetDIBits(hDC, ii.hbmColor, 0, h, px.data(), &bmi, DIB_RGB_COLORS)) {
        bool bHasAlpha = false;
        for (int i = 0; i < w * h && !bHasAlpha; ++i) {
          bHasAlpha = px[i * 4 + 3] != 0;
        }
        for (int i = 0; i < w * h; ++i) {
          uint8_t b = px[i * 4 + 0];
          px[i * 4 + 0] = px[i * 4 + 2]; // BGRA -> RGBA
          px[i * 4 + 2] = b;
          if (!bHasAlpha) {
            px[i * 4 + 3] = 255;
          }
        }
        std::vector<uint8_t> png;
        if (stbi_write_png_to_func(_scByteSink, &png, w, h, 4, px.data(), w * 4)) {
          ok = _scWriteFile(path, png.data(), png.size());
        }
      }
      ReleaseDC(nullptr, hDC);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
  }

  DestroyIcon(hIcon);
  return ok;
}

//------------------------------------------------------------------------
// Downscaled capture preview (keeps the toast image under the size limit)
//------------------------------------------------------------------------
static std::wstring
_scMakePreview(const std::wstring& srcPath, const std::wstring& outPath) {
  std::vector<uint8_t> fileBytes = _scReadFile(srcPath);
  if (fileBytes.empty()) {
    return L"";
  }

  int w, h, comp;
  unsigned char* pData = stbi_load_from_memory(fileBytes.data(), (int)fileBytes.size(), &w, &h, &comp, 4);
  if (!pData) {
    return L"";
  }

  const int kMaxDim = 1456;
  float fScale = (float)kMaxDim / (w > h ? w : h);
  if (fScale > 1.0f) fScale = 1.0f;
  int tw = (int)(w * fScale); if (tw < 1) tw = 1;
  int th = (int)(h * fScale); if (th < 1) th = 1;

  std::vector<uint8_t> out((size_t)tw * th * 4);
  for (int ty = 0; ty < th; ++ty) {
    int sy0 = ty * h / th;
    int sy1 = (ty + 1) * h / th; if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int tx = 0; tx < tw; ++tx) {
      int sx0 = tx * w / tw;
      int sx1 = (tx + 1) * w / tw; if (sx1 <= sx0) sx1 = sx0 + 1;

      uint32_t acc[4] = { 0, 0, 0, 0 };
      for (int sy = sy0; sy < sy1; ++sy) {
        for (int sx = sx0; sx < sx1; ++sx) {
          const uint8_t* p = &pData[((size_t)sy * w + sx) * 4];
          acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
        }
      }
      uint32_t n = (uint32_t)((sy1 - sy0) * (sx1 - sx0));
      uint8_t* d = &out[((size_t)ty * tw + tx) * 4];
      d[0] = (uint8_t)(acc[0] / n);
      d[1] = (uint8_t)(acc[1] / n);
      d[2] = (uint8_t)(acc[2] / n);
      d[3] = (uint8_t)(acc[3] / n);
    }
  }
  stbi_image_free(pData);

  std::vector<uint8_t> png;
  if (!stbi_write_png_to_func(_scByteSink, &png, tw, th, 4, out.data(), tw * 4)) {
    return L"";
  }
  if (!_scWriteFile(outPath, png.data(), png.size())) {
    return L"";
  }
  return outPath;
}

//------------------------------------------------------------------------
// API
//------------------------------------------------------------------------
extern "C" void
scNotificationInit() {
  SetCurrentProcessExplicitAppUserModelID(SC_AUMID);

  std::wstring iconPath = _scAppDataDir() + L"\\app_icon.png";
  bool bHaveIcon = _scWriteAppIcon(iconPath);

  HKEY hKey;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\AppUserModelId\\" SC_AUMID,
                      0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
    const wchar_t* szDisplayName = L"Screencap";
    RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)szDisplayName,
                   (DWORD)((wcslen(szDisplayName) + 1) * sizeof(wchar_t)));
    if (bHaveIcon) {
      RegSetValueExW(hKey, L"IconUri", 0, REG_SZ, (const BYTE*)iconPath.c_str(),
                     (DWORD)((iconPath.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(hKey);
  }
}

extern "C" void
scNotificationShowCapture(const wchar_t* wszPath, const wchar_t* wszTitle) {
  std::wstring path  = wszPath;
  std::wstring title = wszTitle ? wszTitle : L"Screenshot saved";

  std::thread([path, title]() {
    init_apartment(apartment_type::multi_threaded);
    try {
      const wchar_t* pSlash = wcsrchr(path.c_str(), L'\\');
      std::wstring filename = pSlash ? std::wstring(pSlash + 1) : path;
      std::wstring launchUri = _scXmlEscape(_scFileUri(path));

      // Use the capture directly when small, otherwise a downscaled preview so
      // the image stays within the toast's size limit.
      std::wstring imageUri = launchUri;
      WIN32_FILE_ATTRIBUTE_DATA fad;
      if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
          fad.nFileSizeHigh == 0 && fad.nFileSizeLow >= 2 * 1024 * 1024) {
        std::wstring preview = _scMakePreview(path, _scAppDataDir() + L"\\toast_preview.png");
        imageUri = preview.empty() ? L"" : _scXmlEscape(_scFileUri(preview));
      }

      std::wstring xml =
        L"<toast activationType=\"protocol\" launch=\"" + launchUri + L"\">"
        L"<visual><binding template=\"ToastGeneric\">"
        L"<text>" + _scXmlEscape(title) + L"</text>"
        L"<text>" + _scXmlEscape(filename) + L"</text>";

      std::wstring iconPath = _scAppDataDir() + L"\\app_icon.png";
      if (GetFileAttributesW(iconPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        xml += L"<image placement=\"appLogoOverride\" src=\"" + _scXmlEscape(_scFileUri(iconPath)) + L"\"/>";
      }
      if (!imageUri.empty()) {
        xml += L"<image placement=\"hero\" src=\"" + imageUri + L"\"/>";
      }
      xml +=
        L"</binding></visual>"
        L"<audio silent=\"true\"/>"
        L"</toast>";

      XmlDocument doc;
      doc.LoadXml(xml);

      ToastNotification toast(doc);
      ToastNotificationManager::CreateToastNotifier(SC_AUMID).Show(toast);
    } catch (...) {
      scLogError("Failed to show capture notification");
    }
  }).detach();
}
