#ifndef BLANKOS_SDK_H
#define BLANKOS_SDK_H

#include <stdint.h>
#include <stdbool.h>
#include <efi.h>

// ---------------------------------------------------------
// BlankOS Cursor Types
// ---------------------------------------------------------
typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_POINTER = 1,
    CURSOR_TEXT = 2,
    CURSOR_MOVE = 3,
    CURSOR_CROSSHAIR = 4,
    CURSOR_RESIZE = 5
} SDK_CursorType;

// ---------------------------------------------------------
// BlankOS API (System Calls for Developers)
// This structure is passed to dynamically loaded apps
// to communicate with the kernel compositor and systems.
// ---------------------------------------------------------
typedef struct {
    // Composition & Display
    void (*swap_buffers)();
    void (*draw_wallpaper)();
    void (*draw_menubar)();
    void (*draw_dock)();
    
    // Mouse & Cursor State
    void (*set_cursor_type)(SDK_CursorType type);
    void (*draw_cursor)(int x, int y);
    
    // Basic Drawing Primitives
    void (*draw_rect)(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    void (*draw_rect_rounded)(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    void (*draw_rect_outline)(int x, int y, int w, int h, uint32_t color, int thickness);
    void (*draw_circle)(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    void (*draw_line)(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    void (*draw_text)(int x, int y, const char* text, uint32_t color, int scale);
    int  (*get_text_width)(const char* text, int scale);
    int  (*get_text_height)(int scale);
    
    // Window Widgets
    void (*draw_window)(int x, int y, int w, int h, const char* title);
    bool (*draw_button)(int x, int y, int w, int h, const char* text, int mouse_x, int mouse_y, bool mouse_click);
    
    // File I/O (Wrapper over Simple File System)
    bool (*open_file)(const char* path, void** buffer, uint64_t* size);
    bool (*write_file)(const char* path, void* buffer, uint64_t size);
    
    // Sound & System Metrics
    void (*play_sound)(const char* sound_name);
    void (*get_time)(int* hour, int* minute, int* second);
    void (*panic)(const char* error_code, const char* details);
    
    // UEFI context
    EFI_SYSTEM_TABLE *SystemTable;
} OS_API;

#endif // BLANKOS_SDK_H
