#pragma once

#include <cstdint>

struct sc_capture_info {
    unsigned char* data;
    uint8_t channels;
    size_t width;
    size_t height;
};

struct sc_capture_options {
    bool include_cursor;
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

#define sc_internal static

void sc_initialize();
bool sc_capture_desktop(uint8_t desktop, sc_capture_info& ci);
bool sc_capture_window(int pid, sc_capture_info& ci);
bool sc_capture_region(sc_rect rect, sc_capture_info& ci);
bool sc_save_capture(const char* filename, const sc_capture_info& ci);
bool sc_capture_auto(sc_capture_info& ci);

void sc_begin_capture();
bool sc_capture_update(sc_capture_info& ci);