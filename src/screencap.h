#pragma once

#include <cstdint>

struct sc_capture_info {
    unsigned char* data;
    bool channels_swapped;
    uint8_t channels;
    int width;
    int height;
};

struct sc_rect {
    int x;
    int y;
    int width;
    int height;
};

struct sc_monitor_info {
    uint8_t id;
    char name[256];
    sc_rect rect;
};

enum class sc_capture_mode {
    interactive = 0,
    window_under_cursor = 1,
    monitor_under_cursor = 2,
    ocr = 3
};

struct sc_capture_options {
    //bool copy_to_clipboard;
    bool extract_text;
    sc_capture_mode mode;
};

enum sc_hotkey_id {
    sc_hotkey_screenshot,
    sc_hotkey_clipboard,
    sc_hotkey_ocr,
    sc_hotkey_active_window,
    sc_hotkey_current_monitor,
    sc_hotkey_fallback_screenshot, // Ctrl + Alt + C
    _sc_hotkey_count
};

static const char* sc_hotkey_id_strings[sc_hotkey_id::_sc_hotkey_count] = {
    "screenshot",
    "clipboard",
    "ocr",
    "active_window",
    "current_monitor",
    "fallback_screenshot"
};

struct sc_hotkey {
    sc_hotkey_id id;
    uint32_t modifiers;
    uint32_t key;
    bool registered;
};

struct sc_app {
    // options
    bool opt_copy_to_clipboard;
    std::array<sc_hotkey, sc_hotkey_id::_sc_hotkey_count> hotkeys;
    std::string save_path;

    // state
    bool running;
};

#define sc_internal static

void sc_initialize();
bool sc_running();
bool sc_update(sc_capture_options& active_options);

// -1 to auto-detect desktop/window under cursor
bool sc_capture_desktop(int8_t desktop, sc_capture_info& ci);
// -1 to auto-detect window under cursor
bool sc_capture_window(int pid, sc_capture_info& ci);
bool sc_capture_region(sc_rect rect, sc_capture_info& ci);
bool sc_save_capture(sc_capture_info& ci);
void sc_begin_capture(sc_capture_options options);
bool sc_capture_update(sc_capture_info& ci);
void sc_cleanup(sc_capture_info& ci);

int sc_get_fastest_refresh_rate();