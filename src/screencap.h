#pragma once

#include <cstdint>

#define SC_VERSION_MAJOR 1
#define SC_VERSION_MINOR 2
#define SC_VERSION_PATCH 0

#define SC_STRINGIZE_(x) #x
#define SC_STRINGIZE(x) SC_STRINGIZE_(x)
#define SC_VERSION_STRING SC_STRINGIZE(SC_VERSION_MAJOR) "." SC_STRINGIZE(SC_VERSION_MINOR) "." SC_STRINGIZE(SC_VERSION_PATCH)
#define SC_VERSION_STRING_W L"" SC_VERSION_STRING
#define SC_VERSION_STRING_FULL_W L"v" SC_VERSION_STRING

enum class sc_capture_mode {
    none = -1,
    region,
    window_under_cursor,
    monitor_under_cursor,
    record,
    ocr
};

struct sc_capture_info {
    unsigned char* data;
    bool channels_swapped;
    uint8_t channels;
    int width;
    int height;
    sc_capture_mode captureMode;
    bool shouldSave;
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

enum sc_hotkey_id {
    sc_hotkey_screenshot,
    sc_hotkey_clipboard,
    sc_hotkey_ocr,
    sc_hotkey_active_window,
    sc_hotkey_current_monitor,
    sc_hotkey_fallback_screenshot, // Ctrl + Alt + C
    sc_hotkey_record,
    _sc_hotkey_count
};

static const char* sc_hotkey_id_strings[sc_hotkey_id::_sc_hotkey_count] = {
    "screenshot",
    "clipboard",
    "ocr",
    "active_window",
    "current_monitor",
    "fallback_screenshot",
    "start/stop recording"
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
    bool opt_on_startup_enabled;
    bool opt_play_sound;
    std::array<sc_hotkey, sc_hotkey_id::_sc_hotkey_count> hotkeys;
    std::string save_path;
    std::string language_code;
    std::vector<std::string> available_languages;

    // other
    std::string ffmpeg_path;

    // state
    bool running;
};

#define sc_internal static

void sc_initialize();
bool sc_running();
bool sc_update();
sc_app& sc_get_app();

std::wstring& sc_get_localized_string(const std::string& key);

void sc_set_language(const std::string& lang_code);
void sc_load_config();
void sc_save_config();
bool sc_save_capture(sc_capture_info& ci);
void sc_settings_on_capture_saved(const std::string& path);
void sc_begin_capture(sc_hotkey_id hotkey);
bool sc_capture_update(sc_capture_info& ci);
void sc_cleanup(sc_capture_info& ci);
void sc_shutdown();

std::string sc_get_date_string();
std::string sc_get_filename_timestamp();
fs::path sc_get_save_path();

std::wstring _sc_utf8_to_wstring(const std::string& str);
std::string _sc_wstring_to_utf8(const std::wstring& wstr);

bool _sc_is_win10_or_greater();

void sc_reregister_hotkeys();
void _sc_init_impl();
void _sc_shutdown_impl();
void _sc_cleanup_impl(sc_capture_info& ci);
void _sc_swap_channels(uint32_t* pixels, int totalPixels);
bool _sc_find_executable(const std::string& exeName, std::string& outPath);
bool _sc_get_system_language_impl(std::string& out_lang);
void _sc_set_run_on_startup_impl(bool enable);
std::string _sc_plat_get_config_path();
bool _sc_write_to_clipboard(UINT format, const void* data, size_t size, const void* secondaryData = nullptr, size_t secondarySize = 0);

std::string _sc_plat_get_default_save_path();