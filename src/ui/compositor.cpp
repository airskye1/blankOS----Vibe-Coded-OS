#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>

extern "C" {

volatile uint32_t* framebuffer = NULL;
uint32_t* backbuffer = NULL;

int screen_width = 1024;
int screen_height = 768;
int pixels_per_scanline = 1024;

extern bool svga_enabled;
extern void svga_update(int x, int y, int w, int h);
extern void svga_fill_rect(int x, int y, int w, int h, uint32_t color);

extern void init_blankUI_toolkit(void);

void swap_buffers() {
    if (!framebuffer || !backbuffer) return;
    
    // Fast 64-bit aligned CPU copy (optimized software fallback)
    uint64_t* dst = (uint64_t*)framebuffer;
    uint64_t* src = (uint64_t*)backbuffer;
    int count = (screen_width * screen_height * 4) / 8;
    for (int i = 0; i < count; i++) {
        dst[i] = src[i];
    }
    
    // Hardware accelerated presentation
    if (svga_enabled) {
        svga_update(0, 0, screen_width, screen_height);
    }
}

void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha) {
    if (!backbuffer || x < 0 || x >= screen_width || y < 0 || y >= screen_height) return;
    if (alpha == 255) {
        backbuffer[y * screen_width + x] = color;
    } else if (alpha > 0) {
        uint32_t bg = backbuffer[y * screen_width + x];
        uint8_t bg_r = (bg >> 16) & 0xFF;
        uint8_t bg_g = (bg >> 8) & 0xFF;
        uint8_t bg_b = bg & 0xFF;

        uint8_t fg_r = (color >> 16) & 0xFF;
        uint8_t fg_g = (color >> 8) & 0xFF;
        uint8_t fg_b = color & 0xFF;

        uint8_t r = ((fg_r * alpha) + (bg_r * (255 - alpha))) / 255;
        uint8_t g = ((fg_g * alpha) + (bg_g * (255 - alpha))) / 255;
        uint8_t b = ((fg_b * alpha) + (bg_b * (255 - alpha))) / 255;

        backbuffer[y * screen_width + x] = (r << 16) | (g << 8) | b;
    }
}

void draw_rect_filled(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            put_pixel_alpha(x + dx, y + dy, color, alpha);
        }
    }
}

void draw_rect_rounded(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            bool draw = true;
            if (dx < r && dy < r) {
                if ((r - dx) * (r - dx) + (r - dy) * (r - dy) > r * r) draw = false;
            } else if (dx > w - r - 1 && dy < r) {
                if ((dx - (w - r - 1)) * (dx - (w - r - 1)) + (r - dy) * (r - dy) > r * r) draw = false;
            } else if (dx < r && dy > h - r - 1) {
                if ((r - dx) * (r - dx) + (dy - (h - r - 1)) * (dy - (h - r - 1)) > r * r) draw = false;
            } else if (dx > w - r - 1 && dy > h - r - 1) {
                if ((dx - (w - r - 1)) * (dx - (w - r - 1)) + (dy - (h - r - 1)) * (dy - (h - r - 1)) > r * r) draw = false;
            }
            if (draw) {
                put_pixel_alpha(x + dx, y + dy, color, alpha);
            }
        }
    }
}

void draw_macos_wallpaper() {
    if (!backbuffer) return;
    for (int y = 0; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            float fx = (float)x / screen_width;
            float fy = (float)y / screen_height;
            
            // Creamy beige/white base
            uint8_t r = 245;
            uint8_t g = 240;
            uint8_t b = 230;
            
            // Wavy light blue overlay
            float wave = fy - 0.5f * fx * fx - 0.2f;
            if (wave > 0 && wave < 0.3f) {
                float intensity = wave / 0.3f;
                r = 245 - (int)(50 * intensity);
                g = 240 - (int)(40 * intensity);
                b = 230 + (int)(25 * intensity);
            } else if (wave >= 0.3f) {
                r = 195; g = 200; b = 255;
            }
            
            float wave2 = fx + fy - 1.2f;
            if (wave2 > 0) {
                float intensity = wave2;
                if (intensity > 1.0f) intensity = 1.0f;
                r = r - (int)((r - 150) * intensity);
                g = g - (int)((g - 160) * intensity);
                b = b - (int)((b - 180) * intensity);
            }
            
            backbuffer[y * screen_width + x] = (r << 16) | (g << 8) | b;
        }
    }
}

void draw_frosted_glass_rounded(int x, int y, int w, int h, int r, uint32_t tint_color, uint8_t tint_alpha) {
    draw_rect_rounded(x, y, w, h, r, tint_color, tint_alpha);
}

void init_compositor(EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->BootServices->AllocatePool(EfiLoaderData, screen_width * screen_height * 4, (void**)&backbuffer);
    draw_macos_wallpaper();
    init_blankUI_toolkit();
    swap_buffers();
}

void put_pixel(int x, int y, uint32_t color) {
    put_pixel_alpha(x, y, color, 255);
}

void composite_surface(int x, int y, int width, int height, uint32_t* buffer, int z_index, int has_blur) {
    // Stub
}

void draw_gradient_background(uint32_t top_color, uint32_t bottom_color) {
    draw_macos_wallpaper();
}

void draw_circle_filled(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                put_pixel_alpha(cx + x, cy + y, color, 255);
            }
        }
    }
}

} // extern (char*)(char*)(char*)"C"
