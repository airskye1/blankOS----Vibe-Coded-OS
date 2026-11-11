#ifndef BLANKOS_WINDOW_MANAGER_H
#define BLANKOS_WINDOW_MANAGER_H

#include "../kernel/input.h"
#include <efi.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Built-in Application IDs
typedef enum {
    APP_FILES = 0,
    APP_BROWSER,
    APP_SETTINGS,
    APP_STORE,
    APP_TERMINAL,
    APP_MONITOR,
    APP_CALCULATOR,
    APP_WEATHER,
    APP_REGEDIT,
    APP_UPDATER,
    APP_COUNT
} AppId;

struct BlankWindow;

typedef void (*WinRenderFn)(struct BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in);
typedef void (*WinCloseFn)(struct BlankWindow* win);

// Managed Desktop Window with Spring Transition State
typedef struct BlankWindow {
    int id;
    AppId app_id;
    char title[64];
    int x, y, w, h;
    int saved_x, saved_y, saved_w, saved_h; // Restore rect when un-maximizing
    int min_w, min_h;

    bool open;
    bool visible;
    bool minimized;
    bool maximized;
    bool resizable;
    bool dragging;
    bool resizing;
    int drag_ox, drag_oy;
    int resize_mode;         // 0=none, 1=right, 2=bottom, 3=corner SE

    // Smooth visual interpolation (0.0 -> 1.0 open/minimize animation)
    float anim_open;
    float anim_min;
    float target_x, target_y, target_w, target_h;
    uint32_t accent_color;

    void* user_data;
    WinRenderFn on_frame;    // Renders client area & handles input when focused/hovered
    WinCloseFn on_close;     // Optional cleanup callback
} BlankWindow;

// Window Manager Core API
void wm_init(EFI_SYSTEM_TABLE* st);
void wm_register_app(AppId id, const char* title, int def_w, int def_h, uint32_t accent,
                     WinRenderFn render_fn, WinCloseFn close_fn);

// Open or focus an app window; optionally pass an initial file path or URL
BlankWindow* wm_open_app(AppId id);
void wm_close_window(BlankWindow* win);
void wm_minimize_window(BlankWindow* win);
void wm_toggle_maximize(BlankWindow* win);
void wm_focus_window(BlankWindow* win);
BlankWindow* wm_get_window(AppId id);
BlankWindow* wm_get_focused_window(void);
bool wm_is_app_open(AppId id);
int wm_get_z_count(void);
BlankWindow* wm_get_z_window(int z_idx); // 0 = bottom, count-1 = top

// Update dragging/resizing/snapping/animations & render all visible windows bottom-to-top
void wm_tick_and_render(const BlankInputState* in);

// Cross-app launch helpers (e.g. Browser opening a downloaded text/html file in Files/BlankPad)
void wm_request_open_file(const char* path);
const char* wm_consume_pending_file(void);
void wm_request_open_url(const char* url);
const char* wm_consume_pending_url(void);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_WINDOW_MANAGER_H
