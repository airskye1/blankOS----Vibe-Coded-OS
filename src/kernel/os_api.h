#ifndef OS_API_H
#define OS_API_H

#include <stdint.h>
#include <efi.h>

// ---------------------------------------------------------
// BlankOS API (System Calls for External Apps)
// This struct is passed to all loaded .ELF apps, providing
// them access to the kernel's graphical and system functions.
// ---------------------------------------------------------
typedef struct {
    void (*swap_buffers)();
    void (*draw_macos_wallpaper)();
    void (*blankUI_draw_menubar)();
    void (*blankUI_draw_dock)();
    void (*blankUI_draw_cursor)(int x, int y);
    void (*blankUI_draw_window)(int width, int height, char* title);
    void (*blankUI_draw_text_color)(int x, int y, char* text, uint32_t color);
    void (*blankUI_draw_button)(int x, int y, int width, int height, char* text);
    int  (*blankUI_hit_test_window_close)(int cursor_x, int cursor_y, int width, int height);
    EFI_SYSTEM_TABLE *SystemTable; // For keyboard/mouse input and memory allocation
} OS_API;

#endif
