#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "blankDUI.h"
#include "theme.h"

extern "C" {

uint32_t* framebuffer = NULL;
uint32_t* backbuffer = NULL;
uint32_t screen_width = 1024;
uint32_t screen_height = 768;
uint32_t pixels_per_scanline = 1024;

extern EFI_SYSTEM_TABLE* gSystemTable;
extern void* uefi_malloc(UINTN size);
extern void uefi_free(void* ptr);
extern bool svga_enabled;
extern void svga_update(int x, int y, int w, int h);

static uint32_t backbuffer_cap_pixels = 0;
static int clip_x = 0;
static int clip_y = 0;
static int clip_w = 1024;
static int clip_h = 768;
static bool clip_active = false;

static inline uint32_t blend_pixel_fast(uint32_t bg, uint32_t fg, uint8_t alpha) {
    uint32_t inv = 255U - (uint32_t)alpha;
    uint32_t rb = (fg & 0x00FF00FFU) * (uint32_t)alpha + (bg & 0x00FF00FFU) * inv + 0x00800080U;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFU)) >> 8) & 0x00FF00FFU;

    uint32_t g = (fg & 0x0000FF00U) * (uint32_t)alpha + (bg & 0x0000FF00U) * inv + 0x00008000U;
    g = ((g + ((g >> 8) & 0x0000FF00U)) >> 8) & 0x0000FF00U;

    return rb | g;
}

void compositor_set_clip(int x, int y, int w, int h) {
    int sw = (int)screen_width;
    int sh = (int)screen_height;
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;

    if (x1 <= x0 || y1 <= y0) {
        clip_x = 0;
        clip_y = 0;
        clip_w = 0;
        clip_h = 0;
    } else {
        clip_x = x0;
        clip_y = y0;
        clip_w = x1 - x0;
        clip_h = y1 - y0;
    }
    clip_active = true;
}

void compositor_get_clip(int* x, int* y, int* w, int* h) {
    if (!clip_active) {
        clip_x = 0;
        clip_y = 0;
        clip_w = (int)screen_width;
        clip_h = (int)screen_height;
        clip_active = true;
    }
    if (x) *x = clip_x;
    if (y) *y = clip_y;
    if (w) *w = clip_w;
    if (h) *h = clip_h;
}

void compositor_clear(uint32_t color) {
    if (!backbuffer || screen_width == 0 || screen_height == 0) return;
    uint32_t c = color & 0x00FFFFFFU;
    uint64_t c64 = ((uint64_t)c << 32) | (uint64_t)c;
    uint64_t total = (uint64_t)screen_width * (uint64_t)screen_height;
    uint64_t qwords = total >> 1;
    uint64_t* dst64 = (uint64_t*)backbuffer;
    for (uint64_t i = 0; i < qwords; i++) {
        dst64[i] = c64;
    }
    if (total & 1ULL) {
        backbuffer[total - 1] = c;
    }
}

void init_compositor(uint32_t* fb, uint32_t w, uint32_t h, uint32_t ppsl) {
    if (w == 0 || h == 0) return;

    framebuffer = fb;
    screen_width = w;
    screen_height = h;
    pixels_per_scanline = (ppsl >= w) ? ppsl : w;

    uint32_t needed_pixels = w * h;
    if (!backbuffer || needed_pixels > backbuffer_cap_pixels) {
        uint32_t* new_buf = (uint32_t*)uefi_malloc((UINTN)needed_pixels * 4U);
        if (!new_buf && gSystemTable && gSystemTable->BootServices) {
            void* pool_buf = NULL;
            EFI_STATUS st = gSystemTable->BootServices->AllocatePool(EfiLoaderData, (UINTN)needed_pixels * 4U, &pool_buf);
            if (!EFI_ERROR(st) && pool_buf) {
                new_buf = (uint32_t*)pool_buf;
            }
        }
        if (new_buf) {
            if (backbuffer) {
                uefi_free(backbuffer);
            }
            backbuffer = new_buf;
            backbuffer_cap_pixels = needed_pixels;
        }
    }

    compositor_set_clip(0, 0, (int)w, (int)h);
    compositor_clear(THEME_BG_VOID);
}

void swap_buffers(void) {
    if (!framebuffer || !backbuffer) return;
    if (screen_width == 0 || screen_height == 0) return;

    if (pixels_per_scanline == screen_width) {
        uint64_t total_pixels = (uint64_t)screen_width * (uint64_t)screen_height;
        uint64_t qwords = total_pixels >> 1;
        volatile uint64_t* dst64 = (volatile uint64_t*)framebuffer;
        const uint64_t* src64 = (const uint64_t*)backbuffer;
        for (uint64_t i = 0; i < qwords; i++) {
            dst64[i] = src64[i];
        }
        if (total_pixels & 1ULL) {
            ((volatile uint32_t*)framebuffer)[total_pixels - 1] = backbuffer[total_pixels - 1];
        }
    } else {
        uint32_t qwords_per_row = screen_width >> 1;
        bool odd_w = (screen_width & 1U) != 0;
        for (uint32_t y = 0; y < screen_height; y++) {
            volatile uint32_t* dst_row = (volatile uint32_t*)framebuffer + (uint64_t)y * pixels_per_scanline;
            const uint32_t* src_row = backbuffer + (uint64_t)y * screen_width;
            volatile uint64_t* dst64 = (volatile uint64_t*)dst_row;
            const uint64_t* src64 = (const uint64_t*)src_row;
            for (uint32_t i = 0; i < qwords_per_row; i++) {
                dst64[i] = src64[i];
            }
            if (odd_w) {
                dst_row[screen_width - 1] = src_row[screen_width - 1];
            }
        }
    }

    if (svga_enabled) {
        svga_update(0, 0, (int)screen_width, (int)screen_height);
    }
}

void put_pixel(int x, int y, uint32_t color) {
    if (!backbuffer) return;
    if (clip_active) {
        if (x < clip_x || x >= clip_x + clip_w || y < clip_y || y >= clip_y + clip_h) return;
    }
    if (x < 0 || x >= (int)screen_width || y < 0 || y >= (int)screen_height) return;
    backbuffer[(uint32_t)y * screen_width + (uint32_t)x] = color & 0x00FFFFFFU;
}

void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha) {
    if (!backbuffer || alpha == 0) return;
    if (clip_active) {
        if (x < clip_x || x >= clip_x + clip_w || y < clip_y || y >= clip_y + clip_h) return;
    }
    if (x < 0 || x >= (int)screen_width || y < 0 || y >= (int)screen_height) return;

    uint32_t idx = (uint32_t)y * screen_width + (uint32_t)x;
    uint32_t fg = color & 0x00FFFFFFU;
    if (alpha == 255) {
        backbuffer[idx] = fg;
    } else {
        backbuffer[idx] = blend_pixel_fast(backbuffer[idx], fg, alpha);
    }
}

// Fast span helper for blankDUI and primitives (respects active clip rect)
void compositor_fill_span(int x0, int x1, int y, uint32_t color, uint8_t alpha) {
    if (!backbuffer || alpha == 0) return;
    int cx = 0, cy = 0, cw = (int)screen_width, ch = (int)screen_height;
    if (clip_active) {
        cx = clip_x; cy = clip_y; cw = clip_w; ch = clip_h;
    }
    if (y < cy || y >= cy + ch || y < 0 || y >= (int)screen_height) return;
    if (x0 < cx) x0 = cx;
    if (x0 < 0) x0 = 0;
    if (x1 > cx + cw) x1 = cx + cw;
    if (x1 > (int)screen_width) x1 = (int)screen_width;
    if (x0 >= x1) return;

    uint32_t* row = backbuffer + (uint32_t)y * screen_width;
    uint32_t fg = color & 0x00FFFFFFU;
    if (alpha == 255) {
        for (int x = x0; x < x1; x++) {
            row[x] = fg;
        }
    } else {
        for (int x = x0; x < x1; x++) {
            row[x] = blend_pixel_fast(row[x], fg, alpha);
        }
    }
}

// Legacy compatibility wrappers for non-migrated modules
void draw_rect_filled(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    dui_rect(x, y, w, h, color, alpha);
}

void draw_rect_rounded(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha) {
    dui_rect_rounded(x, y, w, h, r, color, alpha);
}

void draw_circle_filled(int cx, int cy, int r, uint32_t color) {
    dui_circle(cx, cy, r, color, 255);
}

void draw_frosted_glass_rounded(int x, int y, int w, int h, int r, uint32_t tint_color, uint8_t tint_alpha) {
    dui_frosted_glass(x, y, w, h, r, tint_color, tint_alpha);
}

void draw_gradient_background(uint32_t top_color, uint32_t bottom_color) {
    (void)top_color;
    (void)bottom_color;
    dui_draw_wallpaper(dui_get_wallpaper_id());
}

void composite_surface(int x, int y, int width, int height, uint32_t* buffer, int z_index, int has_blur) {
    (void)z_index;
    (void)has_blur;
    if (!backbuffer || !buffer || width <= 0 || height <= 0) return;
    for (int dy = 0; dy < height; dy++) {
        for (int dx = 0; dx < width; dx++) {
            uint32_t px = buffer[dy * width + dx];
            uint8_t a = (uint8_t)((px >> 24) & 0xFF);
            if (a == 0) a = 255;
            put_pixel_alpha(x + dx, y + dy, px & 0x00FFFFFFU, a);
        }
    }
}

} // extern "C"
