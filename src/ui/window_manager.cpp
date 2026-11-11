#include <stdint.h>
#include <stdbool.h>
#include "window_manager.h"
#include "blankDUI.h"
#include "theme.h"
#include "../kernel/audio.h"
#include <stddef.h>

extern "C" {
    extern void uefi_memset(void* dst, int val, UINTN n);
}

static EFI_SYSTEM_TABLE* g_wm_st = NULL;

typedef struct {
    bool registered;
    char title[64];
    int def_w;
    int def_h;
    uint32_t accent;
    WinRenderFn render_fn;
    WinCloseFn close_fn;
} AppSpec;

static AppSpec g_app_specs[APP_COUNT];
static BlankWindow g_windows[APP_COUNT];
static int g_z_order[APP_COUNT];
static int g_z_count = 0;

static char g_pending_file[256] = {0};
static bool g_has_pending_file = false;
static char g_pending_url[512] = {0};
static bool g_has_pending_url = false;

static void copy_str_wm(char* dst, const char* src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool pt_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void remove_from_z(int idx) {
    int pos = -1;
    for (int i = 0; i < g_z_count; i++) {
        if (g_z_order[i] == idx) { pos = i; break; }
    }
    if (pos >= 0) {
        for (int i = pos; i < g_z_count - 1; i++) {
            g_z_order[i] = g_z_order[i + 1];
        }
        g_z_count--;
    }
}

static void push_top_z(int idx) {
    remove_from_z(idx);
    if (g_z_count < APP_COUNT) {
        g_z_order[g_z_count++] = idx;
    }
}

extern "C" {

void wm_init(EFI_SYSTEM_TABLE* st) {
    g_wm_st = st;
    uefi_memset(g_app_specs, 0, sizeof(g_app_specs));
    uefi_memset(g_windows, 0, sizeof(g_windows));
    g_z_count = 0;
    g_has_pending_file = false;
    g_has_pending_url = false;
    for (int i = 0; i < APP_COUNT; i++) {
        g_windows[i].id = i;
        g_windows[i].app_id = (AppId)i;
    }
}

void wm_register_app(AppId id, const char* title, int def_w, int def_h, uint32_t accent,
                     WinRenderFn render_fn, WinCloseFn close_fn) {
    if ((int)id < 0 || id >= APP_COUNT) return;
    AppSpec* spec = &g_app_specs[id];
    spec->registered = true;
    copy_str_wm(spec->title, title ? title : "App", 64);
    spec->def_w = def_w;
    spec->def_h = def_h;
    spec->accent = accent;
    spec->render_fn = render_fn;
    spec->close_fn = close_fn;
}

BlankWindow* wm_open_app(AppId id) {
    if ((int)id < 0 || id >= APP_COUNT) return NULL;
    AppSpec* spec = &g_app_specs[id];
    if (!spec->registered) return NULL;

    BlankWindow* w = &g_windows[id];
    if (w->open) {
        w->minimized = false;
        w->visible = true;
        wm_focus_window(w);
        return w;
    }

    int sw = (screen_width > 0) ? (int)screen_width : 1024;
    int sh = (screen_height > 0) ? (int)screen_height : 768;

    int win_w = spec->def_w;
    int win_h = spec->def_h;
    if (win_w > sw - 48) win_w = sw - 48;
    if (win_h > sh - (THEME_TOPBAR_H + THEME_DOCK_H + 36)) {
        win_h = sh - (THEME_TOPBAR_H + THEME_DOCK_H + 36);
    }

    // Stagger newly opened windows slightly so stacked apps don't overlap identically
    int stagger = (g_z_count % 5) * 22;
    int win_x = (sw - win_w) / 2 + stagger - 30;
    int win_y = THEME_TOPBAR_H + 26 + stagger;
    if (win_x < 16) win_x = 16;
    if (win_x + win_w > sw - 16) win_x = sw - win_w - 16;
    if (win_y + win_h > sh - THEME_DOCK_H - 18) win_y = THEME_TOPBAR_H + 22;

    copy_str_wm(w->title, spec->title, 64);
    w->x = win_x;
    w->y = win_y;
    w->w = win_w;
    w->h = win_h;
    w->saved_x = win_x;
    w->saved_y = win_y;
    w->saved_w = win_w;
    w->saved_h = win_h;
    w->min_w = 340;
    w->min_h = 260;
    w->open = true;
    w->visible = true;
    w->minimized = false;
    w->maximized = false;
    w->resizable = (id != APP_CALCULATOR);
    w->dragging = false;
    w->resizing = false;
    w->anim_open = 0.05f;
    w->anim_min = 0.0f;
    w->accent_color = spec->accent;
    w->on_frame = spec->render_fn;
    w->on_close = spec->close_fn;

    push_top_z((int)id);
    play_system_sound((char*)"open");
    return w;
}

void wm_close_window(BlankWindow* win) {
    if (!win || !win->open) return;
    if (win->on_close) win->on_close(win);
    win->open = false;
    win->visible = false;
    win->dragging = false;
    win->resizing = false;
    remove_from_z(win->id);
    play_system_sound((char*)"close");
}

void wm_minimize_window(BlankWindow* win) {
    if (!win || !win->open) return;
    win->minimized = true;
    win->dragging = false;
    win->resizing = false;
    play_system_sound((char*)"minimize");
}

void wm_toggle_maximize(BlankWindow* win) {
    if (!win || !win->open || !win->resizable) return;
    int sw = (int)screen_width;
    int sh = (int)screen_height;
    if (!win->maximized) {
        win->saved_x = win->x;
        win->saved_y = win->y;
        win->saved_w = win->w;
        win->saved_h = win->h;
        win->x = 14;
        win->y = THEME_TOPBAR_H + 14;
        win->w = sw - 28;
        win->h = sh - THEME_TOPBAR_H - THEME_DOCK_H - 30;
        win->maximized = true;
    } else {
        win->x = win->saved_x;
        win->y = win->saved_y;
        win->w = win->saved_w;
        win->h = win->saved_h;
        win->maximized = false;
    }
}

void wm_focus_window(BlankWindow* win) {
    if (!win || !win->open) return;
    win->minimized = false;
    win->visible = true;
    push_top_z(win->id);
}

BlankWindow* wm_get_window(AppId id) {
    if ((int)id < 0 || id >= APP_COUNT) return NULL;
    return &g_windows[id];
}

BlankWindow* wm_get_focused_window(void) {
    for (int i = g_z_count - 1; i >= 0; i--) {
        BlankWindow* w = &g_windows[g_z_order[i]];
        if (w->open && !w->minimized) return w;
    }
    return NULL;
}

bool wm_is_app_open(AppId id) {
    if ((int)id < 0 || id >= APP_COUNT) return false;
    return g_windows[id].open;
}

int wm_get_z_count(void) {
    return g_z_count;
}

BlankWindow* wm_get_z_window(int z_idx) {
    if (z_idx < 0 || z_idx >= g_z_count) return NULL;
    return &g_windows[g_z_order[z_idx]];
}

void wm_tick_and_render(const BlankInputState* raw_in) {
    if (!raw_in) return;

    BlankInputState in = *raw_in;
    dui_set_pointer_down(in.left_down);
    int sw = (int)screen_width;
    int sh = (int)screen_height;
    int work_top = THEME_TOPBAR_H + 14;
    int work_bot = sh - THEME_DOCK_H - 16;

    // 1. Determine which visible top-most window owns mouse click focus
    if (in.left_pressed) {
        for (int i = g_z_count - 1; i >= 0; i--) {
            BlankWindow* w = &g_windows[g_z_order[i]];
            if (!w->open || w->minimized) continue;
            if (pt_in_rect(in.mx, in.my, w->x, w->y, w->w + 6, w->h + 6)) {
                if (i != g_z_count - 1) {
                    wm_focus_window(w);
                }
                break;
            }
        }
    }

    // 2. Process active drag or resize on topmost focused window
    BlankWindow* top = wm_get_focused_window();
    if (top) {
        int tb_h = THEME_TITLEBAR_H;
        // Close / Maximize / Minimize Horizon pill buttons (top right inside titlebar)
        int btn_y = top->y + 9;
        int close_x = top->x + top->w - 28;
        int max_x   = top->x + top->w - 52;
        int min_x   = top->x + top->w - 76;

        if (in.left_pressed) {
            if (pt_in_rect(in.mx, in.my, close_x, btn_y, 20, 20)) {
                wm_close_window(top);
                input_consume_mouse(&in);
                top = wm_get_focused_window();
            } else if (top->resizable && pt_in_rect(in.mx, in.my, max_x, btn_y, 20, 20)) {
                wm_toggle_maximize(top);
                input_consume_mouse(&in);
            } else if (pt_in_rect(in.mx, in.my, min_x, btn_y, 20, 20)) {
                wm_minimize_window(top);
                input_consume_mouse(&in);
                top = wm_get_focused_window();
            } else if (top->resizable &&
                       pt_in_rect(in.mx, in.my, top->x + top->w - 14, top->y + top->h - 14, 20, 20)) {
                top->resizing = true;
                top->resize_mode = 3;
                input_consume_mouse(&in);
            } else if (top->resizable &&
                       pt_in_rect(in.mx, in.my, top->x + top->w - 6, top->y + tb_h, 12, top->h - tb_h - 14)) {
                top->resizing = true;
                top->resize_mode = 1;
                input_consume_mouse(&in);
            } else if (top->resizable &&
                       pt_in_rect(in.mx, in.my, top->x + 10, top->y + top->h - 6, top->w - 24, 12)) {
                top->resizing = true;
                top->resize_mode = 2;
                input_consume_mouse(&in);
            } else if (pt_in_rect(in.mx, in.my, top->x, top->y, top->w - 82, tb_h)) {
                if (in.left_double_click && top->resizable) {
                    wm_toggle_maximize(top);
                } else {
                    top->dragging = true;
                    top->drag_ox = in.mx - top->x;
                    top->drag_oy = in.my - top->y;
                    if (top->maximized) {
                        top->maximized = false;
                        top->w = top->saved_w;
                        top->h = top->saved_h;
                        top->drag_ox = top->w / 2;
                    }
                }
                input_consume_mouse(&in);
            }
        }
    }

    // Update ongoing drag / resize
    for (int i = 0; i < g_z_count; i++) {
        BlankWindow* w = &g_windows[g_z_order[i]];
        if (!w->open) continue;

        if (w->dragging) {
            if (!in.left_down) {
                w->dragging = false;
                // Edge snap to left or right half when released near edge
                if (w->resizable) {
                    if (in.mx <= 12) {
                        w->x = 12; w->y = work_top + 4;
                        w->w = (sw / 2) - 18; w->h = work_bot - work_top - 8;
                    } else if (in.mx >= sw - 12) {
                        w->x = (sw / 2) + 6; w->y = work_top + 4;
                        w->w = (sw / 2) - 18; w->h = work_bot - work_top - 8;
                    }
                }
            } else {
                w->x = in.mx - w->drag_ox;
                w->y = in.my - w->drag_oy;
                if (w->y < work_top) w->y = work_top;
                if (w->y > work_bot - 40) w->y = work_bot - 40;
                if (w->x < -w->w + 120) w->x = -w->w + 120;
                if (w->x > sw - 120) w->x = sw - 120;

                // Show translucent snap zone preview when cursor touches screen edge
                if (w->resizable && in.mx <= 12) {
                    dui_rect_rounded(12, work_top + 4, (sw / 2) - 18, work_bot - work_top - 8, THEME_RADIUS_LG, THEME_ACCENT_CYAN, 40);
                    dui_rect_rounded_outline(12, work_top + 4, (sw / 2) - 18, work_bot - work_top - 8, THEME_RADIUS_LG, THEME_ACCENT_CYAN, 180);
                } else if (w->resizable && in.mx >= sw - 12) {
                    dui_rect_rounded((sw / 2) + 6, work_top + 4, (sw / 2) - 18, work_bot - work_top - 8, THEME_RADIUS_LG, THEME_ACCENT_CYAN, 40);
                    dui_rect_rounded_outline((sw / 2) + 6, work_top + 4, (sw / 2) - 18, work_bot - work_top - 8, THEME_RADIUS_LG, THEME_ACCENT_CYAN, 180);
                }
            }
        }

        if (w->resizing) {
            if (!in.left_down) {
                w->resizing = false;
            } else {
                if (w->resize_mode == 1 || w->resize_mode == 3) {
                    int nw = in.mx - w->x;
                    if (nw < w->min_w) nw = w->min_w;
                    if (w->x + nw > sw - 8) nw = sw - 8 - w->x;
                    w->w = nw;
                }
                if (w->resize_mode == 2 || w->resize_mode == 3) {
                    int nh = in.my - w->y;
                    if (nh < w->min_h) nh = w->min_h;
                    if (w->y + nh > sh - 12) nh = sh - 12 - w->y;
                    w->h = nh;
                }
            }
        }
    }

    // 3. Render windows bottom-to-top
    BlankWindow* active = wm_get_focused_window();
    for (int i = 0; i < g_z_count; i++) {
        BlankWindow* w = &g_windows[g_z_order[i]];
        if (!w->open || w->minimized) continue;

        // Damped Lira spring open animation (never from scale 0: 0.94..1.0)
        w->anim_open = horizon_spring_step(w->anim_open, 1.0f, 0.34f);
        float scale = horizon_enter_scale(w->anim_open);
        int lift_y = horizon_enter_offset_y(w->anim_open, 12);

        bool is_focused = (w == active);
        int draw_y = w->y + lift_y;
        int draw_h = (int)((float)w->h * scale);
        if (draw_h < 60) draw_h = 60;

        // Elevation shadow + generous Lira squircle glass shell (20px radius, zero glow)
        dui_shadow(w->x, draw_y, w->w, draw_h, THEME_RADIUS_XL, is_focused ? 10 : 5);
        dui_glass_card(w->x, draw_y, w->w, draw_h, THEME_RADIUS_XL, THEME_SURFACE_BASE, is_focused ? 238 : 216, is_focused);

        // Clean titlebar divider hairline
        dui_rect(w->x + 14, draw_y + THEME_TITLEBAR_H - 1, w->w - 28, 1, THEME_BORDER_SUBTLE, is_focused ? 190 : 120);

        // Crisp focus dot + squircle app badge + title
        uint32_t dot_col = w->accent_color ? w->accent_color : THEME_ACCENT_CYAN;
        dui_circle(w->x + 16, draw_y + THEME_TITLEBAR_H / 2, is_focused ? 4 : 3, is_focused ? dot_col : THEME_TEXT_MUTED, is_focused ? 245 : 140);
        dui_draw_squircle_icon(w->x + 26, draw_y + 9, 20, (int)w->app_id, false);
        dui_text_ellipsis(w->x + 54, draw_y + 12, w->w - 150, w->title,
                          is_focused ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);

        // Lira Tactile Pill Window Controls: Minimize, Maximize, Close (with immediate press detent)
        int btn_y = draw_y + 8;
        int min_x   = w->x + w->w - 82;
        int max_x   = w->x + w->w - 56;
        int close_x = w->x + w->w - 30;

        bool hov_min   = pt_in_rect(in.mx, in.my, min_x, btn_y, 22, 22);
        bool hov_max   = w->resizable && pt_in_rect(in.mx, in.my, max_x, btn_y, 22, 22);
        bool hov_close = pt_in_rect(in.mx, in.my, close_x, btn_y, 22, 22);

        int dy_min   = (hov_min && in.left_down) ? 1 : 0;
        int dy_max   = (hov_max && in.left_down) ? 1 : 0;
        int dy_close = (hov_close && in.left_down) ? 1 : 0;

        dui_rect_rounded(min_x, btn_y + dy_min, 22, 22, 11, hov_min ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 220);
        dui_rect_rounded_outline(min_x, btn_y + dy_min, 22, 22, 11, hov_min ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE, 180);
        dui_rect(min_x + 6, btn_y + dy_min + 10, 10, 2, THEME_TEXT_SECONDARY, 255);

        if (w->resizable) {
            dui_rect_rounded(max_x, btn_y + dy_max, 22, 22, 11, hov_max ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 220);
            dui_rect_rounded_outline(max_x, btn_y + dy_max, 22, 22, 11, hov_max ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE, 180);
            dui_rect_rounded_outline(max_x + 6, btn_y + dy_max + 6, 10, 10, 2, THEME_TEXT_SECONDARY, 230);
        }

        dui_rect_rounded(close_x, btn_y + dy_close, 22, 22, 11, hov_close ? THEME_ACCENT_RED : THEME_SURFACE_CARD, hov_close ? 240 : 220);
        dui_rect_rounded_outline(close_x, btn_y + dy_close, 22, 22, 11, hov_close ? THEME_ACCENT_ROSE : THEME_BORDER_SUBTLE, 190);
        dui_draw_sys_icon(close_x + 4, btn_y + dy_close + 4, 14, 12, hov_close ? 0xFFFFFF : THEME_TEXT_SECONDARY);

        // Client area coordinates + scissor clip
        int cx = w->x + 1;
        int cy = draw_y + THEME_TITLEBAR_H;
        int cw = w->w - 2;
        int ch = draw_h - THEME_TITLEBAR_H - 1;

        if (cw > 20 && ch > 20 && w->on_frame) {
            dui_push_clip(cx, cy, cw, ch);

            // Only route active keyboard/mouse input to the focused window (hovered scroll still works if focused)
            BlankInputState win_in = in;
            if (!is_focused) {
                win_in.has_key = false;
                win_in.key_char = 0;
                win_in.scan_code = 0;
                win_in.left_pressed = false;
                win_in.left_double_click = false;
                win_in.right_pressed = false;
                win_in.scroll_y = 0;
            }
            w->on_frame(w, cx, cy, cw, ch, &win_in);
            dui_pop_clip();
        }

        // Subtle SE resize grip indicator on resizable windows
        if (w->resizable) {
            int gx = w->x + w->w - 11;
            int gy = draw_y + draw_h - 11;
            dui_line(gx, gy + 7, gx + 7, gy, THEME_BORDER_SPECULAR, 180);
            dui_line(gx + 3, gy + 7, gx + 7, gy + 3, THEME_BORDER_SPECULAR, 180);
        }
    }
}

void wm_request_open_file(const char* path) {
    if (!path || !path[0]) return;
    copy_str_wm(g_pending_file, path, 256);
    g_has_pending_file = true;
    wm_open_app(APP_FILES);
}

const char* wm_consume_pending_file(void) {
    if (!g_has_pending_file) return NULL;
    g_has_pending_file = false;
    return g_pending_file;
}

void wm_request_open_url(const char* url) {
    if (!url || !url[0]) return;
    copy_str_wm(g_pending_url, url, 512);
    g_has_pending_url = true;
    wm_open_app(APP_BROWSER);
}

const char* wm_consume_pending_url(void) {
    if (!g_has_pending_url) return NULL;
    g_has_pending_url = false;
    return g_pending_url;
}

} // extern "C"
