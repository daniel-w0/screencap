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

//constexpr int HOTKEY_ID_SCREENSHOT = 1;
//constexpr int HOTKEY_ID_SCREENSHOT_CLIPBOARD = 2;
//constexpr int HOTKEY_ID_SCREENSHOT_OCR = 3;
//constexpr int HOTKEY_ID_SCREENSHOT_ACTIVE_WINDOW = 4;
//constexpr int HOTKEY_ID_SCREENSHOT_CURRENT_MONITOR = 5;
//constexpr int HOTKEY_ID_FALLBACK = 6; // Ctrl + Alt + C

std::string get_date_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&now_tm, &now_time_t);
#else
    localtime_r(&now_time_t, &now_tm);
#endif
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &now_tm);
    return std::string(buffer);
}

std::string get_filename_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm now_tm;
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&now_tm, &now_time_t);
#else
    localtime_r(&now_time_t, &now_tm);
#endif
    char buffer[20];
    std::strftime(buffer, sizeof(buffer), "%d-%m-%Y_%H-%M-%S", &now_tm);
    return std::string(buffer);
}

std::string get_save_path() {
    static std::filesystem::path pictures = std::filesystem::path(getenv("USERPROFILE")) / "Pictures";
    if (!std::filesystem::exists(pictures)) {
        pictures = std::filesystem::current_path();
    }
    
    std::filesystem::path base_savepath = pictures / "Screencap";
    std::filesystem::path date_savepath = base_savepath / get_date_string();

    if (!std::filesystem::exists(date_savepath)) {
        std::error_code ec;
        if (!std::filesystem::create_directories(date_savepath, ec)) {
            return (pictures / (get_filename_timestamp() + ".png")).string();
        }
    }

    return (date_savepath / (get_filename_timestamp() + ".png")).string();
}

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
                    std::string savepath = get_save_path();
                    if (sc_save_capture(savepath.c_str(), ci)) {
                        fprintf(stdout, "Saved screenshot to %s\n", savepath.c_str());
                    }
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