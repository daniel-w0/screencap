#include "pch.h"
#if defined(SC_PLATFORM_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <Windows.h>
#endif

#include "platform/platform_screencap.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int entry(int argc, char** argv) {
    sc_initialize();

    sc_capture_options active_options = { 0 };

    while (sc_running()) {
        if (sc_update(active_options)) {
            sc_capture_info ci = {};
            if (sc_capture_update(ci)) {
                if (active_options.extract_text) {
                    fprintf(stdout, "Extracted text to clipboard\n");
                } else {
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
#if defined(SC_DEBUG)
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
    }
#endif

#if defined(SC_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    return entry(__argc, __argv);
}
#endif