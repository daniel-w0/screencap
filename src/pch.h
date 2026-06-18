#pragma once


#if defined(SC_PLATFORM_WINDOWS) && defined(SC_DEBUG)
#  define _CRTDBG_MAP_ALLOC
#  include <crtdbg.h>
#endif

#include <algorithm>
#include <cstdlib>

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <mutex>
#include <thread>
#include <memory>
#include <iostream>
#include <string>
#include <chrono>
#include <filesystem>
#include <array>
#include <cctype>

namespace fs = std::filesystem;

#if defined(SC_PLATFORM_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX

#  include <Windows.h>
#  include <WinUser.h>
#  include <wingdi.h>
#  include <dwmapi.h>
#  include <shellapi.h>

#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.Foundation.Collections.h>
#  include <winrt/Windows.Graphics.Imaging.h>
#  include <winrt/Windows.Media.Ocr.h>
#  include <winrt/Windows.Storage.Streams.h>

using DataWriter = winrt::Windows::Storage::Streams::DataWriter;
using IBuffer = winrt::Windows::Storage::Streams::IBuffer;
using SoftwareBitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using OcrEngine = winrt::Windows::Media::Ocr::OcrEngine;
using BitmapPixelFormat = winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using BitmapAlphaMode = winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using OcrResult = winrt::Windows::Media::Ocr::OcrResult;

#  ifndef PW_RENDERFULLCONTENT
#    define PW_RENDERFULLCONTENT 2
#  endif
#endif

#pragma warning(push, 0)
#include "stb_image.h"
#include "stb_image_write.h"
#pragma warning(pop)