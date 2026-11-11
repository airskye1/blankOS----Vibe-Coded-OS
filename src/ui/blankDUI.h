#ifndef BLANKOS_DUI_H
#define BLANKOS_DUI_H

#include <efi.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compositor framebuffer globals (defined in compositor.cpp)
extern uint32_t* framebuffer;
extern uint32_t* backbuffer;
extern uint32_t screen_width;
extern uint32_t screen_height;
extern uint32_t pixels_per_scanline;

void init_compositor(uint32_t* fb, uint32_t w, uint32_t h, uint32_t ppsl);
void swap_buffers(void);
void compositor_clear(uint32_t color);
void put_pixel(int x, int y, uint32_t color);
void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha);
void compositor_set_clip(int x, int y, int w, int h);
void compositor_get_clip(int* x, int* y, int* w, int* h);

// BlankDUI Initialization & Wallpaper Engine
void dui_init(EFI_SYSTEM_TABLE* ST);
void dui_on_resolution_change(void);
void dui_draw_wallpaper(int theme_id);
int dui_get_wallpaper_id(void);
void dui_set_wallpaper_id(int id);

// Core Anti-Aliased & Alpha Primitives
void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
void dui_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2, uint8_t alpha);
void dui_gradient_h(int x, int y, int w, int h, uint32_t c1, uint32_t c2, uint8_t alpha);
void dui_gradient_rounded(int x, int y, int w, int h, int r, uint32_t c1, uint32_t c2, uint8_t alpha);
void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
void dui_circle_outline(int cx, int cy, int r, uint32_t color, uint8_t alpha);
void dui_line(int x0, int y0, int x1, int y1, uint32_t color, uint8_t alpha);
void dui_progress_ring(int cx, int cy, int r, int thickness, float progress, uint32_t bg_color, uint32_t fg_color);

// Horizon x Lira Architectural Surfaces & Elevation
void dui_frosted_glass(int x, int y, int w, int h, int r, uint32_t tint, uint8_t alpha);
void dui_shadow(int x, int y, int w, int h, int radius, int elevation);
void dui_glass_card(int x, int y, int w, int h, int radius, uint32_t tint, uint8_t alpha, bool focused);
void dui_bento_card(int x, int y, int w, int h, int radius, uint32_t tint, uint8_t alpha, bool active);
void dui_dot_grid_overlay(int x, int y, int w, int h, int spacing, uint32_t color, uint8_t alpha);

// Scissor Clip Stack
void dui_push_clip(int x, int y, int w, int h);
void dui_pop_clip(void);
void dui_reset_clip(void);

// Cached TrueType Typography
void dui_text(int x, int y, const char* text, uint32_t color, int scale);
int dui_text_width(const char* text, int scale);
int dui_text_height(int scale);
void dui_text_ellipsis(int x, int y, int max_w, const char* text, uint32_t color, int scale);

// System & App Icon Families
// System icons: 0=search, 1=wifi, 2=volume, 3=battery, 4=gear, 5=folder, 6=globe,
// 7=terminal, 8=download, 9=lock, 10=power, 11=check, 12=close, 13=plus, 14=back,
// 15=forward, 16=refresh, 17=star, 18=bolt, 19=shield
void dui_draw_sys_icon(int x, int y, int size, int sys_icon_id, uint32_t color);
void dui_draw_squircle_icon(int x, int y, int size, int app_id, bool hovered);

// Compatibility alias + STB Image Loader
void dui_draw_icon(int x, int y, int size, int icon_id);
bool dui_draw_image_path(const char* path, int x, int y, int w, int h);

// Cohesive Horizon x Lira Tactile Controls
void dui_set_pointer_down(bool down);
bool dui_button(int x, int y, int w, int h, const char* label, uint32_t accent, bool primary, int mx, int my, bool clicked);
bool dui_button_tactile(int x, int y, int w, int h, const char* label, uint32_t accent, bool primary, int mx, int my, bool down, bool clicked);
bool dui_icon_pill(int x, int y, int w, int h, const char* label, int sys_icon, bool active, int mx, int my, bool clicked);
void dui_status_pill(int x, int y, int w, int h, const char* label, uint32_t dot_color, uint32_t bg, bool active);
void dui_eq_bars(int x, int y, int w, int h, const int* bands, int count, uint32_t accent, bool active);
void dui_toggle_switch(int x, int y, bool on, uint32_t accent);

// Backward-compatible Cursor & Toast entry points (defined in blankUI.cpp)
void blankUI_draw_cursor(int x, int y, int type);
void blankUI_draw_toast(const char* title, const char* message);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_DUI_H
