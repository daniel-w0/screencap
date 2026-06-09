#include "pch.h"
#include "screencap.h"
#include "locales_data.h"
#include "simpleini.h"

static sc_app g_app;
static std::unordered_map<std::string, std::wstring> language_map;

void _sc_swap_channels(uint32_t* pixels, int totalPixels) {
    for (int i = 0; i < totalPixels; ++i) {
        uint32_t p = pixels[i];
        pixels[i] = 0xFF000000 | (p & 0x0000FF00) | ((p & 0x00FF0000) >> 16) | ((p & 0x000000FF) << 16);
    }
}

std::string sc_get_date_string() {
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

fs::path sc_get_save_path() {
    fs::path save_path(fs::path((g_app.save_path.empty() ? _sc_plat_get_default_save_path() : g_app.save_path)) / sc_get_date_string());
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

std::wstring _sc_utf8_to_wstring(const std::string& str) {
#if defined(_WIN32) || defined(_WIN64)
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size_needed);
    return wstr;
#else
// don't think I need to do this on MacOS
#error "UTF-8 to wstring conversion not implemented for this platform"
#endif
}

sc_internal void _init_languages() {
    language_map.clear();
    g_app.available_languages = { "en" };

    CSimpleIniA ini;
    ini.SetUnicode();

    SI_Error rc = ini.LoadData((const char*)locales_ini, locales_ini_len);
    if (rc < 0) {
        fprintf(stderr, "Failed to load embedded locales.ini: %d\n", rc);
        return;
    }

    std::list<CSimpleIniA::Entry> sections;
    ini.GetAllSections(sections);
    for (auto& section : sections) {
        g_app.available_languages.push_back(section.pItem);
        printf("Found language section: %s\n", section.pItem);
    }

    if (g_app.language_code.empty()) {
        std::string lang;
        if (_sc_get_system_language_impl(lang)) {
            size_t sep = lang.find('-');
            if (sep != std::string::npos) lang = lang.substr(0, sep);
            g_app.language_code = lang;
        } else {
            g_app.language_code = "en";
        }
    }

    auto section = ini.GetSection(g_app.language_code.c_str());
    if (section) {
        for (auto& [key, value] : *section) {
            language_map[key.pItem] = _sc_utf8_to_wstring(value);
        }
    }
}

void sc_set_language(const std::string& language_code) {
    g_app.language_code = language_code;
    _init_languages();
    sc_save_config();
}

void sc_load_or_create_config() {
    CSimpleIniA ini;
    ini.SetUnicode();

    std::string path = _sc_plat_get_config_path();
    if (ini.LoadFile(path.c_str()) < 0) {
        g_app.opt_copy_to_clipboard = true;
        g_app.opt_on_startup_enabled = false;
        g_app.opt_play_sound = true;
        g_app.save_path = _sc_plat_get_default_save_path();
        return;
    }

    g_app.opt_copy_to_clipboard = ini.GetBoolValue("options", "copy_to_clipboard", true);
    g_app.opt_on_startup_enabled = ini.GetBoolValue("options", "run_on_startup", true);
    g_app.opt_play_sound = ini.GetBoolValue("options", "play_sound", true);

    if (const char* save_path = ini.GetValue("options", "save_path")) {
        g_app.save_path = save_path;
    }
    if (const char* language = ini.GetValue("options", "language")) {
        g_app.language_code = language;
    }

    for (auto& hk : g_app.hotkeys) {
        const char* id_str = sc_hotkey_id_strings[hk.id];
        std::string key_name = std::string(id_str) + "_key";
        std::string mod_name = std::string(id_str) + "_modifiers";
        hk.key       = (uint32_t)ini.GetLongValue("hotkeys", key_name.c_str(), (long)hk.key);
        hk.modifiers = (uint32_t)ini.GetLongValue("hotkeys", mod_name.c_str(), (long)hk.modifiers);
    }
}

void sc_save_config() {
    CSimpleIniA ini;
    ini.SetUnicode();

    ini.SetBoolValue("options", "copy_to_clipboard", g_app.opt_copy_to_clipboard);
    ini.SetBoolValue("options", "run_on_startup", g_app.opt_on_startup_enabled);
    ini.SetBoolValue("options", "play_sound", g_app.opt_play_sound);
    ini.SetValue("options", "save_path", g_app.save_path.c_str());
    ini.SetValue("options", "language", g_app.language_code.c_str());

    for (const auto& hk : g_app.hotkeys) {
        const char* id_str = sc_hotkey_id_strings[hk.id];
        std::string key_name = std::string(id_str) + "_key";
        std::string mod_name = std::string(id_str) + "_modifiers";
        ini.SetLongValue("hotkeys", key_name.c_str(), (long)hk.key);
        ini.SetLongValue("hotkeys", mod_name.c_str(), (long)hk.modifiers);
    }

    std::string path = _sc_plat_get_config_path();
    ini.SaveFile(path.c_str());
}

std::wstring& sc_get_localized_string(const std::string& key) {
    if (language_map.find(key) == language_map.end()) {
        language_map[key] = _sc_utf8_to_wstring(key);
    }
    return language_map[key];
}

void sc_initialize() {
    g_app = {};
    g_app.running = true;

    g_app.hotkeys[sc_hotkey_screenshot] = { sc_hotkey_id::sc_hotkey_screenshot, 0, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_clipboard] = { sc_hotkey_id::sc_hotkey_clipboard, MOD_CONTROL | MOD_SHIFT, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_ocr] = { sc_hotkey_id::sc_hotkey_ocr, MOD_CONTROL | MOD_ALT, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_active_window] = { sc_hotkey_id::sc_hotkey_active_window, MOD_ALT, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_current_monitor] = { sc_hotkey_id::sc_hotkey_current_monitor, MOD_CONTROL, VK_SNAPSHOT };
    g_app.hotkeys[sc_hotkey_fallback_screenshot] = { sc_hotkey_id::sc_hotkey_fallback_screenshot, MOD_CONTROL | MOD_ALT, 'C' };

    sc_load_or_create_config();
    _init_languages();
    sc_save_config();
    _sc_set_run_on_startup_impl(g_app.opt_on_startup_enabled);

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

    fs::path saveFile = sc_get_save_path() / (_get_filename_timestamp() + ".png");

    if (!stbi_write_png(saveFile.string().c_str(), ci.width, ci.height, ci.channels, ci.data, ci.width * ci.channels)) {
        fprintf(stderr, "Failed to save capture to %s\n", saveFile.string().c_str());
        return false;
    }
    printf("Saved capture (%dx%d) to %s\n", ci.width, ci.height, saveFile.string().c_str());
    sc_settings_on_capture_saved(saveFile.string());
    return true;
}