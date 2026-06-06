#include "pch.h"
#include "screencap.h"

static sc_app g_app;

void _sc_swap_channels(uint32_t* pixels, int totalPixels) {
    for (int i = 0; i < totalPixels; ++i) {
        uint32_t p = pixels[i];
        pixels[i] = 0xFF000000 | (p & 0x0000FF00) | ((p & 0x00FF0000) >> 16) | ((p & 0x000000FF) << 16);
    }
}

sc_internal std::string _get_date_string() {
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

sc_internal std::string _get_filename_timestamp() {
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

sc_internal fs::path _get_save_path() {
    fs::path save_path(fs::path((g_app.save_path.empty() ? _sc_plat_get_default_save_path() : g_app.save_path)) / _get_date_string());
    if (!fs::exists(save_path)) {
        std::error_code ec;
        if (!fs::create_directories(save_path, ec)) {
            return _sc_plat_get_default_save_path();
        }
    }
    return save_path.string();
}

sc_app& sc_get_app() {
    return g_app;
}

void sc_initialize() {
    g_app = {};
    g_app.running = true;
    g_app.opt_copy_to_clipboard = true;
    g_app.save_path = _sc_plat_get_default_save_path();

    g_app.hotkeys[sc_hotkey_screenshot] = { sc_hotkey_id::sc_hotkey_screenshot, 0, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_clipboard] = { sc_hotkey_id::sc_hotkey_clipboard, MOD_CONTROL | MOD_SHIFT, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_ocr] = { sc_hotkey_id::sc_hotkey_ocr, MOD_CONTROL | MOD_ALT, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_active_window] = { sc_hotkey_id::sc_hotkey_active_window, MOD_ALT, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_current_monitor] = { sc_hotkey_id::sc_hotkey_current_monitor, MOD_CONTROL, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_fallback_screenshot] = { sc_hotkey_id::sc_hotkey_fallback_screenshot, MOD_CONTROL | MOD_ALT, 'C' };

    _sc_init_impl();
}

void sc_shutdown() {
    g_app.running = false;
    _sc_shutdown_impl();
}

void sc_cleanup(sc_capture_info& ci) {
    if (ci.data) {
        delete[] ci.data;
        ci.data = nullptr;
    }
    ci.width = 0;
    ci.height = 0;
    ci.channels = 0;
    ci.channels_swapped = false;

    _sc_cleanup_impl(ci);
}

bool sc_running() {
    return g_app.running;
}

bool sc_save_capture(sc_capture_info& ci) {
    if (!ci.data || ci.width <= 0 || ci.height <= 0 || ci.channels <= 0) {
        fprintf(stderr, "Invalid capture info\n");
        return false;
    }

    if (g_app.save_path[0] == '\0') {
        fprintf(stderr, "No save path configured\n");
        return false;
    }

    if (!ci.channels_swapped) {
        _sc_swap_channels((uint32_t*)ci.data, ci.width * ci.height);
        ci.channels_swapped = true;
    }

    fs::path saveFile = _get_save_path() / (_get_filename_timestamp() + ".png");

    if (!stbi_write_png(saveFile.string().c_str(), ci.width, ci.height, ci.channels, ci.data, ci.width * ci.channels)) {
        fprintf(stderr, "Failed to save capture to %s\n", saveFile.string().c_str());
        return false;
    }
    printf("Saved capture (%dx%d) to %s\n", ci.width, ci.height, saveFile.string().c_str());
    return true;
}