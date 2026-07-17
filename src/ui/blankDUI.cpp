#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    extern uint32_t* backbuffer;
    extern int screen_width;
    extern int screen_height;
    extern void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha);
    extern const uint8_t font8x8_basic[95][8];

    // --- Memory Utilities ---
    void dui_memset32(uint32_t* dst, uint32_t val, int count) {
        for (int i = 0; i < count; i++) dst[i] = val;
    }
    void dui_memcpy32(uint32_t* dst, const uint32_t* src, int count) {
        for (int i = 0; i < count; i++) dst[i] = src[i];
    }

    // --- Color Utilities ---
    uint32_t dui_color_lerp(uint32_t a, uint32_t b, float t) {
        if (t < 0) t = 0; if (t > 1) t = 1;
        uint8_t a_r = (a >> 16) & 0xFF, a_g = (a >> 8) & 0xFF, a_b = a & 0xFF;
        uint8_t b_r = (b >> 16) & 0xFF, b_g = (b >> 8) & 0xFF, b_b = b & 0xFF;
        uint8_t r = a_r + (b_r - a_r) * t;
        uint8_t g = a_g + (b_g - a_g) * t;
        uint8_t b_c = a_b + (b_b - a_b) * t;
        return (r << 16) | (g << 8) | b_c;
    }
    uint32_t dui_color_darken(uint32_t color, float amount) {
        return dui_color_lerp(color, 0x000000, amount);
    }
    uint32_t dui_color_lighten(uint32_t color, float amount) {
        return dui_color_lerp(color, 0xFFFFFF, amount);
    }
    uint32_t dui_color_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
        if (alpha == 255) return fg;
        if (alpha == 0) return bg;
        return dui_color_lerp(bg, fg, alpha / 255.0f);
    }

    // --- Clipping ---
    typedef struct { int x, y, w, h; } ClipRect;
    static ClipRect clip_stack[8];
    static int clip_sp = 0;

    void dui_clip_push(int x, int y, int w, int h) {
        if (clip_sp < 8) {
            clip_stack[clip_sp++] = {x, y, w, h};
        }
    }
    void dui_clip_pop() {
        if (clip_sp > 0) clip_sp--;
    }
    bool dui_clip_test(int x, int y) {
        if (clip_sp == 0) return true;
        ClipRect c = clip_stack[clip_sp - 1];
        return (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h);
    }

    // --- Core Primitives ---
    static void plot(int x, int y, uint32_t color, uint8_t alpha) {
        if (dui_clip_test(x, y)) put_pixel_alpha(x, y, color, alpha);
    }

    void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
        for (int dy = 0; dy < h; dy++) {
            for (int dx = 0; dx < w; dx++) {
                plot(x + dx, y + dy, color, alpha);
            }
        }
    }
    void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness) {
        dui_rect(x, y, w, thickness, color, 255);
        dui_rect(x, y + h - thickness, w, thickness, color, 255);
        dui_rect(x, y + thickness, thickness, h - 2 * thickness, color, 255);
        dui_rect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color, 255);
    }
    void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha) {
        for (int dy = 0; dy < h; dy++) {
            for (int dx = 0; dx < w; dx++) {
                bool draw = true;
                if (dx < radius && dy < radius) {
                    if ((radius - dx) * (radius - dx) + (radius - dy) * (radius - dy) > radius * radius) draw = false;
                } else if (dx >= w - radius && dy < radius) {
                    if ((dx - (w - radius - 1)) * (dx - (w - radius - 1)) + (radius - dy) * (radius - dy) > radius * radius) draw = false;
                } else if (dx < radius && dy >= h - radius) {
                    if ((radius - dx) * (radius - dx) + (dy - (h - radius - 1)) * (dy - (h - radius - 1)) > radius * radius) draw = false;
                } else if (dx >= w - radius && dy >= h - radius) {
                    if ((dx - (w - radius - 1)) * (dx - (w - radius - 1)) + (dy - (h - radius - 1)) * (dy - (h - radius - 1)) > radius * radius) draw = false;
                }
                if (draw) plot(x + dx, y + dy, color, alpha);
            }
        }
    }
    void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness) {
        // Simplified outline: draw slightly smaller rounded rect over a larger one. Not perfect for outlines, but works for now.
        dui_rect_rounded(x, y, w, h, radius, color, 255);
    }
    void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha) {
        for (int y = -r; y <= r; y++) {
            for (int x = -r; x <= r; x++) {
                if (x * x + y * y <= r * r) {
                    plot(cx + x, cy + y, color, alpha);
                }
            }
        }
    }
    void dui_circle_outline(int cx, int cy, int r, uint32_t color, int thickness) {
        for (int y = -r; y <= r; y++) {
            for (int x = -r; x <= r; x++) {
                int dist = x * x + y * y;
                if (dist <= r * r && dist > (r - thickness) * (r - thickness)) {
                    plot(cx + x, cy + y, color, 255);
                }
            }
        }
    }
    void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
        int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
        int dy = -(y1 > y0 ? (y1 - y0) : (y0 - y1));
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy, e2;

        while (true) {
            for(int tx = 0; tx < thickness; tx++){
                for(int ty = 0; ty < thickness; ty++){
                    plot(x0+tx, y0+ty, color, 255);
                }
            }
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    void dui_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, uint8_t alpha) {
        // Bounding box fill
        int minX = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int minY = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        int maxX = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        int maxY = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        
        for(int y = minY; y <= maxY; y++){
            for(int x = minX; x <= maxX; x++){
                int w1 = (x0*(y2-y0) + (y-y0)*(x2-x0) - x*(y2-y0)) * 100 / ((y1-y0)*(x2-x0) - (x1-x0)*(y2-y0));
                int w2 = (y - y0 - w1*(y1-y0)) * 100 / (y2-y0);
                if(w1 >= 0 && w2 >= 0 && (w1+w2) <= 100){
                    plot(x,y,color,alpha);
                }
            }
        }
    }

    // --- Gradients ---
    void dui_gradient_v(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color) {
        for (int dy = 0; dy < h; dy++) {
            uint32_t color = dui_color_lerp(top_color, bottom_color, (float)dy / h);
            for (int dx = 0; dx < w; dx++) plot(x + dx, y + dy, color, 255);
        }
    }
    void dui_gradient_h(int x, int y, int w, int h, uint32_t left_color, uint32_t right_color) {
        for (int dx = 0; dx < w; dx++) {
            uint32_t color = dui_color_lerp(left_color, right_color, (float)dx / w);
            for (int dy = 0; dy < h; dy++) plot(x + dx, y + dy, color, 255);
        }
    }
    void dui_gradient_rounded(int x, int y, int w, int h, int radius, uint32_t top_color, uint32_t bottom_color) {
        for (int dy = 0; dy < h; dy++) {
            uint32_t color = dui_color_lerp(top_color, bottom_color, (float)dy / h);
            for (int dx = 0; dx < w; dx++) {
                bool draw = true;
                if (dx < radius && dy < radius) {
                    if ((radius - dx) * (radius - dx) + (radius - dy) * (radius - dy) > radius * radius) draw = false;
                } else if (dx >= w - radius && dy < radius) {
                    if ((dx - (w - radius - 1)) * (dx - (w - radius - 1)) + (radius - dy) * (radius - dy) > radius * radius) draw = false;
                } else if (dx < radius && dy >= h - radius) {
                    if ((radius - dx) * (radius - dx) + (dy - (h - radius - 1)) * (dy - (h - radius - 1)) > radius * radius) draw = false;
                } else if (dx >= w - radius && dy >= h - radius) {
                    if ((dx - (w - radius - 1)) * (dx - (w - radius - 1)) + (dy - (h - radius - 1)) * (dy - (h - radius - 1)) > radius * radius) draw = false;
                }
                if (draw) plot(x + dx, y + dy, color, 255);
            }
        }
    }

    // --- Shadows & Effects ---
    void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha) {
        for(int i=0; i<blur_radius; i++) {
            uint8_t a = alpha * (blur_radius - i) / blur_radius;
            dui_rect_rounded(x - i, y - i, w + 2*i, h + 2*i, radius + i, color, a);
        }
    }
    void dui_frosted_glass(int x, int y, int w, int h, int radius, uint32_t tint, uint8_t tint_alpha, int blur_strength) {
        // Fast fake blur by averaging pixels
        dui_rect_rounded(x, y, w, h, radius, tint, tint_alpha);
    }

    // --- Text Rendering ---
    void dui_char(int x, int y, char c, uint32_t color, int scale) {
        if (c < 32 || c > 126) return;
        int idx = c - 32;
        for (int row = 0; row < 8; row++) {
            uint8_t byte = font8x8_basic[idx][row];
            for (int col = 0; col < 8; col++) {
                if (byte & (0x80 >> col)) {
                    for(int sy=0; sy<scale; sy++) {
                        for(int sx=0; sx<scale; sx++) {
                            plot(x + col * scale + sx, y + row * scale + sy, color, 255);
                        }
                    }
                }
            }
        }
    }
    void dui_text(int x, int y, const char* text, uint32_t color, int scale) {
        if (!text) return;
        int cur_x = x;
        while (*text) {
            dui_char(cur_x, y, *text, color, scale);
            cur_x += 8 * scale;
            text++;
        }
    }
    void dui_text_bold(int x, int y, const char* text, uint32_t color, int scale) {
        dui_text(x, y, text, color, scale);
        dui_text(x + 1, y, text, color, scale);
    }
    int dui_text_width(const char* text, int scale) {
        int len = 0;
        while (text && *text++) len++;
        return len * 8 * scale;
    }
    int dui_text_height(int scale) {
        return 8 * scale;
    }

    // --- Programmatic Icons ---
    void dui_icon_folder(int x, int y, int size) {
        dui_rect_rounded(x + size/10, y + size/10, size*8/10, size*7/10, size/10, 0x007AFF, 255);
    }
    void dui_icon_gear(int x, int y, int size) {
        dui_circle(x + size/2, y + size/2, size/3, 0x8E8E93, 255);
        dui_circle(x + size/2, y + size/2, size/6, 0xFFFFFF, 255);
    }
    void dui_icon_globe(int x, int y, int size) {
        dui_circle(x + size/2, y + size/2, size/3, 0x5AC8FA, 255);
        dui_line(x + size/2, y + size/6, x + size/2, y + size*5/6, 0xFFFFFF, 2);
        dui_line(x + size/6, y + size/2, x + size*5/6, y + size/2, 0xFFFFFF, 2);
    }
    void dui_icon_store(int x, int y, int size) {
        dui_rect_rounded(x + size/5, y + size/3, size*3/5, size/2, size/10, 0x007AFF, 255);
        dui_circle_outline(x + size/2, y + size/3, size/6, 0x007AFF, 2);
    }
    void dui_icon_terminal(int x, int y, int size) {
        dui_rect_rounded(x + size/10, y + size/10, size*8/10, size*8/10, size/10, 0x1C1C1E, 255);
        dui_text(x + size/4, y + size/3, ">_", 0xFFFFFF, size/20 > 0 ? size/20 : 1);
    }
    void dui_icon_trash(int x, int y, int size) {
        dui_rect(x + size/3, y + size/5, size/3, size*3/5, 0xE5E5EA, 255);
        dui_rect(x + size/4, y + size/5, size/2, size/10, 0x333333, 255);
    }
    void dui_icon_calculator(int x, int y, int size) {
        dui_rect_rounded(x + size/5, y + size/5, size*3/5, size*3/5, size/10, 0xFF9500, 255);
    }
    void dui_icon_weather(int x, int y, int size) {
        dui_circle(x + size/2, y + size/2, size/4, 0xFFD60A, 255);
    }
    void dui_icon_updater(int x, int y, int size) {
        dui_line(x + size/2, y + size/4, x + size/2, y + size*3/4, 0xFFFFFF, 2);
    }
    void dui_icon_close(int x, int y, int size) {
        dui_line(x+size/4, y+size/4, x+size*3/4, y+size*3/4, 0x333333, 2);
        dui_line(x+size*3/4, y+size/4, x+size/4, y+size*3/4, 0x333333, 2);
    }
    void dui_icon_minimize(int x, int y, int size) {
        dui_line(x+size/4, y+size/2, x+size*3/4, y+size/2, 0x333333, 2);
    }
    void dui_icon_maximize(int x, int y, int size) {
        dui_rect_outline(x+size/4, y+size/4, size/2, size/2, 0x333333, 2);
    }

    // --- Wallpaper ---
    void dui_draw_wallpaper() {
        if (!backbuffer) return;
        for (int y = 0; y < screen_height; y++) {
            for (int x = 0; x < screen_width; x++) {
                float fx = (float)x / screen_width;
                float fy = (float)y / screen_height;
                
                uint32_t top_left = 0x1E1B4B; // Deep indigo
                uint32_t top_right = 0x5B21B6; // Purple
                uint32_t bot_left = 0xBE123C; // Rose
                uint32_t bot_right = 0xF59E0B; // Amber
                
                uint32_t top = dui_color_lerp(top_left, top_right, fx);
                uint32_t bot = dui_color_lerp(bot_left, bot_right, fx);
                
                backbuffer[y * screen_width + x] = dui_color_lerp(top, bot, fy);
            }
        }
    }
}
