#include <stdint.h>
#include <stddef.h>

extern "C" {

// Globals exposed to the kernel and blankUI
volatile uint32_t* framebuffer = NULL;
int screen_width = 1024;
int screen_height = 768;
int pixels_per_scanline = 1024;

extern void init_blankUI_toolkit(void);

// Clear the screen to a specific color
void clear_screen(uint32_t color) {
    if (!framebuffer) return;
    for (int y = 0; y < screen_height; y++) {
        for (int x = 0; x < screen_width; x++) {
            framebuffer[y * pixels_per_scanline + x] = color;
        }
    }
}

// Draw a single pixel with alpha blending
void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha) {
    if (!framebuffer) return;
    if (x >= 0 && x < screen_width && y >= 0 && y < screen_height) {
        if (alpha == 255) {
            framebuffer[y * pixels_per_scanline + x] = color;
        } else if (alpha > 0) {
            uint32_t bg = framebuffer[y * pixels_per_scanline + x];
            uint8_t bg_r = (bg >> 16) & 0xFF;
            uint8_t bg_g = (bg >> 8) & 0xFF;
            uint8_t bg_b = bg & 0xFF;

            uint8_t fg_r = (color >> 16) & 0xFF;
            uint8_t fg_g = (color >> 8) & 0xFF;
            uint8_t fg_b = color & 0xFF;

            uint8_t r = ((fg_r * alpha) + (bg_r * (255 - alpha))) / 255;
            uint8_t g = ((fg_g * alpha) + (bg_g * (255 - alpha))) / 255;
            uint8_t b = ((fg_b * alpha) + (bg_b * (255 - alpha))) / 255;

            framebuffer[y * pixels_per_scanline + x] = (r << 16) | (g << 8) | b;
        }
    }
}

// Draw filled rectangle with optional transparency
void draw_rect_filled(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            put_pixel_alpha(x + dx, y + dy, color, alpha);
        }
    }
}

// Draw circle (e.g. for window controls or icons)
void draw_circle_filled(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                put_pixel_alpha(cx + x, cy + y, color, 255);
            }
        }
    }
}

// Draw a simple modern gradient background
void draw_gradient_background(uint32_t top_color, uint32_t bottom_color) {
    if (!framebuffer) return;
    
    uint8_t r1 = (top_color >> 16) & 0xFF;
    uint8_t g1 = (top_color >> 8) & 0xFF;
    uint8_t b1 = top_color & 0xFF;

    uint8_t r2 = (bottom_color >> 16) & 0xFF;
    uint8_t g2 = (bottom_color >> 8) & 0xFF;
    uint8_t b2 = bottom_color & 0xFF;

    for (int y = 0; y < screen_height; y++) {
        float ratio = (float)y / (float)screen_height;
        uint8_t r = r1 + (int)((r2 - r1) * ratio);
        uint8_t g = g1 + (int)((g2 - g1) * ratio);
        uint8_t b = b1 + (int)((b2 - b1) * ratio);
        uint32_t row_color = (r << 16) | (g << 8) | b;

        for (int x = 0; x < screen_width; x++) {
            framebuffer[y * pixels_per_scanline + x] = row_color;
        }
    }
}

void init_compositor(void) {
    // Render a high-end, premium deep blue/violet gradient desktop background
    draw_gradient_background(0x0f172a, 0x1e1b4b);
    
    // Initialize UI library
    init_blankUI_toolkit();
}

void put_pixel(int x, int y, uint32_t color) {
    put_pixel_alpha(x, y, color, 255);
}

void composite_surface(int x, int y, int width, int height, uint32_t* buffer, int z_index, int has_blur) {
    // Composite overlay logic (stubbed)
}

} // extern "C"
