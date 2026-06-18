#include "pch.h"
#if defined(SC_PLATFORM_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <Windows.h>
#endif

#include "screencap.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int entry(int argc, char** argv) {
    sc_initialize();

    while (sc_running()) {
        if (sc_update()) {
            sc_capture_info ci = {};
            if (sc_capture_update(ci)) {
                if (ci.captureMode != sc_capture_mode::ocr) {
                    sc_save_capture(ci);
                }
                sc_cleanup(ci);
            }
        } else {
            break;
        }
    }

    return 0;
}

#if defined(SC_PLATFORM_WINDOWS)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
#if defined(SC_DEBUG)
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
#endif
    }

#if defined(SC_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    HANDLE mutex = CreateMutexA(nullptr, TRUE, "screencap_single_instance_mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    return entry(__argc, __argv);
}
#endif