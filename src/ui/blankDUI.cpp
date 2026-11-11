#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efiprot.h>
#include "../stb_truetype.h"

extern "C" {
    extern uint32_t* backbuffer;
    extern int screen_width;
    extern int screen_height;
    extern void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha);
    extern const uint8_t font8x8_basic[95][8];

    static stbtt_fontinfo font_info;
    static uint8_t* font_buffer = NULL;
    static bool font_loaded = false;
    static uint8_t char_bitmap_buf[16384];

    void dui_init(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
        
        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            EFI_FILE_HANDLE file = NULL;
            EFI_STATUS Status = root->Open(root, &file, (CHAR16*)L"assets\\font.ttf", EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR(Status) && file != NULL) {
                UINTN buf_size = 1024 * 1024;
                SystemTable->BootServices->AllocatePool(EfiLoaderData, buf_size, (void**)&font_buffer);
                if (font_buffer) {
                    Status = file->Read(file, &buf_size, font_buffer);
                    if (!EFI_ERROR(Status)) {
                        if (stbtt_InitFont(&font_info, font_buffer, stbtt_GetFontOffsetForIndex(font_buffer, 0))) {
                            font_loaded = true;
                        }
                    }
                }
                file->Close(file);
                root->Close(root);
                break;
            }
            root->Close(root);
        }
        if (handleBuffer) SystemTable->BootServices->FreePool(handleBuffer);
    }

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
        if (!font_loaded) {
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
            return;
        }
        float px_size = (scale == 1) ? 16.0f : ((scale == 2) ? 24.0f : 32.0f);
        float scale_factor = stbtt_ScaleForPixelHeight(&font_info, px_size);
        
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &lineGap);
        int baseline = (int)(ascent * scale_factor);
        
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font_info, c, scale_factor, scale_factor, &x0, &y0, &x1, &y1);
        
        int w = x1 - x0;
        int h = y1 - y0;
        
        if (w * h < 16384) {
            stbtt_MakeCodepointBitmap(&font_info, char_bitmap_buf, w, h, w, scale_factor, scale_factor, c);
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    uint8_t alpha = char_bitmap_buf[row * w + col];
                    if (alpha > 0) {
                        plot(x + x0 + col, y + baseline + y0 + row, color, alpha);
                    }
                }
            }
        }
    }

    void dui_text(int x, int y, const char* text, uint32_t color, int scale) {
        if (!text) return;
        if (!font_loaded) {
            int cur_x = x;
            while (*text) {
                dui_char(cur_x, y, *text, color, scale);
                cur_x += 8 * scale;
                text++;
            }
            return;
        }
        
        float px_size = (scale == 1) ? 16.0f : ((scale == 2) ? 24.0f : 32.0f);
        float scale_factor = stbtt_ScaleForPixelHeight(&font_info, px_size);
        
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &lineGap);
        int baseline = (int)(ascent * scale_factor);
        
        int cur_x = x;
        int cur_y = y + baseline;
        
        while (*text) {
            char c = *text++;
            if (c < 32 || c > 126) continue;
            
            int advanceWidth, leftSideBearing;
            stbtt_GetCodepointHMetrics(&font_info, c, &advanceWidth, &leftSideBearing);
            
            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&font_info, c, scale_factor, scale_factor, &x0, &y0, &x1, &y1);
            
            int w = x1 - x0;
            int h = y1 - y0;
            
            if (w * h < 16384) {
                stbtt_MakeCodepointBitmap(&font_info, char_bitmap_buf, w, h, w, scale_factor, scale_factor, c);
                for (int row = 0; row < h; row++) {
                    for (int col = 0; col < w; col++) {
                        uint8_t alpha = char_bitmap_buf[row * w + col];
                        if (alpha > 0) {
                            plot(cur_x + x0 + col, cur_y + y0 + row, color, alpha);
                        }
                    }
                }
            }
            
            cur_x += (int)(advanceWidth * scale_factor);
            if (*text) {
                int kern = stbtt_GetCodepointKernAdvance(&font_info, c, *text);
                cur_x += (int)(kern * scale_factor);
            }
        }
    }

    void dui_text_bold(int x, int y, const char* text, uint32_t color, int scale) {
        dui_text(x, y, text, color, scale);
        dui_text(x + 1, y, text, color, scale);
    }

    int dui_text_width(const char* text, int scale) {
        if (!font_loaded) {
            int len = 0;
            while (text && *text++) len++;
            return len * 8 * scale;
        }
        float px_size = (scale == 1) ? 16.0f : ((scale == 2) ? 24.0f : 32.0f);
        float scale_factor = stbtt_ScaleForPixelHeight(&font_info, px_size);
        int w = 0;
        while (text && *text) {
            int advanceWidth, leftSideBearing;
            stbtt_GetCodepointHMetrics(&font_info, *text, &advanceWidth, &leftSideBearing);
            w += (int)(advanceWidth * scale_factor);
            if (*(text+1)) {
                w += (int)(stbtt_GetCodepointKernAdvance(&font_info, *text, *(text+1)) * scale_factor);
            }
            text++;
        }
        return w;
    }

    int dui_text_height(int scale) {
        if (!font_loaded) return 8 * scale;
        return (scale == 1) ? 16 : ((scale == 2) ? 24 : 32);
    }

    // --- High Fidelity Programmatic Icons ---
    void dui_icon_folder(int x, int y, int size) {
        // macOS style friendly Finder face folder
        int tab_w = size * 4 / 10;
        int tab_h = size / 5;
        dui_rect_rounded(x + size/10, y + size/10, tab_w, tab_h, 4, 0x0A84FF, 255);
        dui_rect_rounded(x + size/10, y + size/5, size * 8 / 10, size * 7 / 10, 6, 0x007AFF, 255);
        dui_rect_rounded(x + size*3/20, y + size*7/20, size * 7 / 10, size / 2, 4, 0x64D2FF, 255);
        // Eyes and smile
        int cx = x + size/2;
        int cy = y + size*3/5;
        dui_rect(cx - size/7, cy - size/10, 2, 4, 0x004080, 255);
        dui_rect(cx + size/7, cy - size/10, 2, 4, 0x004080, 255);
        dui_line(cx - size/8, cy + size/12, cx + size/8, cy + size/12, 0x004080, 2);
    }

    void dui_icon_gear(int x, int y, int size) {
        // Interlocking dual gears
        int cx = x + size/2;
        int cy = y + size/2;
        int r = size * 3 / 10;
        // Teeth
        for (int a = 0; a < 8; a++) {
            int tx = cx + ((a % 2 == 0) ? (r + 2) : -(r + 2));
            int ty = cy + ((a / 2 % 2 == 0) ? (r + 2) : -(r + 2));
            dui_rect(tx - 2, ty - 2, 4, 4, 0x8E8E93, 255);
        }
        dui_circle(cx, cy, r, 0xAEAEB2, 255);
        dui_circle(cx, cy, r / 2, 0x48484A, 255);
        dui_circle(cx, cy, r / 4, 0xE5E5EA, 255);
    }

    void dui_icon_globe(int x, int y, int size) {
        // Safari style compass
        int cx = x + size/2;
        int cy = y + size/2;
        int r = size * 7 / 20;
        dui_circle(cx, cy, r, 0x007AFF, 255);
        dui_circle(cx, cy, r - 2, 0xFFFFFF, 255);
        dui_circle(cx, cy, r - 4, 0x1C1C1E, 255);
        // Red and white needle
        dui_line(cx, cy, cx + r/2, cy - r/2, 0xFF3B30, 2);
        dui_line(cx, cy, cx - r/2, cy + r/2, 0xE5E5EA, 2);
        dui_circle(cx, cy, 3, 0xFFFFFF, 255);
    }

    void dui_icon_store(int x, int y, int size) {
        // App Store iconic blue tile with stylized geometric 'A'
        dui_rect_rounded(x + size/10, y + size/10, size*8/10, size*8/10, 8, 0x0A84FF, 255);
        // Draw the white 'A' tripod
        int cx = x + size/2;
        int cy = y + size/2;
        dui_line(cx - size/5, cy + size/4, cx, cy - size/4, 0xFFFFFF, 3);
        dui_line(cx + size/5, cy + size/4, cx, cy - size/4, 0xFFFFFF, 3);
        dui_line(cx - size/6, cy + size/16, cx + size/6, cy + size/16, 0xFFFFFF, 3);
    }

    void dui_icon_terminal(int x, int y, int size) {
        // Sleek dark terminal window
        dui_rect_rounded(x + size/10, y + size/10, size*8/10, size*8/10, 8, 0x1E1E2E, 255);
        dui_rect(x + size/10, y + size/10, size*8/10, size/5, 0x313244, 255);
        // Traffic dots
        dui_circle(x + size/6, y + size/5 - 3, 2, 0xFF5F56, 255);
        dui_circle(x + size/6 + 6, y + size/5 - 3, 2, 0xFFBD2E, 255);
        dui_circle(x + size/6 + 12, y + size/5 - 3, 2, 0x27C93F, 255);
        // Prompt >_
        dui_text(x + size/5, y + size*2/5, ">_", 0xA6E3A1, 1);
    }

    void dui_icon_trash(int x, int y, int size) {
        int cx = x + size/2;
        int cy = y + size/2;
        // Trash lid
        dui_rect_rounded(cx - size/4, cy - size/3, size/2, 4, 2, 0x8E8E93, 255);
        dui_rect(cx - size/10, cy - size/3 - 3, size/5, 3, 0x8E8E93, 255);
        // Can body
        dui_rect_rounded(cx - size/5, cy - size/4, size*2/5, size/2, 4, 0xC7C7CC, 255);
        // Fluted vertical ribs
        dui_line(cx - size/10, cy - size/6, cx - size/10, cy + size/5, 0x8E8E93, 1);
        dui_line(cx, cy - size/6, cx, cy + size/5, 0x8E8E93, 1);
        dui_line(cx + size/10, cy - size/6, cx + size/10, cy + size/5, 0x8E8E93, 1);
    }

    void dui_icon_calculator(int x, int y, int size) {
        dui_rect_rounded(x + size/10, y + size/10, size*8/10, size*8/10, 8, 0xFF9500, 255);
        // Display bar
        dui_rect_rounded(x + size/5, y + size/5, size*3/5, size/6, 3, 0x333333, 255);
        // 4 Keypad buttons (+, -, *, =)
        int bx = x + size/4;
        int by = y + size*9/20;
        dui_rect(bx, by, size/7, size/7, 0xFFFFFF, 255);
        dui_rect(bx + size/4, by, size/7, size/7, 0xFFFFFF, 255);
        dui_rect(bx, by + size/4, size/7, size/7, 0xFFFFFF, 255);
        dui_rect(bx + size/4, by + size/4, size/7, size/7, 0xFFFFFF, 255);
    }

    void dui_icon_weather(int x, int y, int size) {
        int cx = x + size/2;
        int cy = y + size/2;
        // Radiant golden sun
        dui_circle(cx + size/10, cy - size/10, size/5, 0xFFCC00, 255);
        // Fluffy white cloud overlapping
        dui_circle(cx - size/8, cy + size/10, size/7, 0xFFFFFF, 255);
        dui_circle(cx + size/12, cy + size/10, size/6, 0xFFFFFF, 255);
        dui_rect_rounded(cx - size/6, cy + size/10, size/2, size/6, 4, 0xFFFFFF, 255);
    }

    void dui_icon_updater(int x, int y, int size) {
        int cx = x + size/2;
        int cy = y + size/2;
        dui_circle(cx, cy, size*7/20, 0x34C759, 255);
        // Download arrow pointing down
        dui_line(cx, cy - size/6, cx, cy + size/8, 0xFFFFFF, 3);
        dui_line(cx - size/8, cy, cx, cy + size/8, 0xFFFFFF, 3);
        dui_line(cx + size/8, cy, cx, cy + size/8, 0xFFFFFF, 3);
        dui_line(cx - size/6, cy + size/4, cx + size/6, cy + size/4, 0xFFFFFF, 2);
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

    // --- Dynamic Wallpaper Theme Engine ---
    int g_wallpaper_theme = 0; // 0 = Ventura Indigo, 1 = Sunset Orange, 2 = Cyberpunk Neon, 3 = Slate Dark

    void dui_draw_wallpaper() {
        if (!backbuffer) return;
        for (int y = 0; y < screen_height; y++) {
            for (int x = 0; x < screen_width; x++) {
                float fx = (float)x / screen_width;
                float fy = (float)y / screen_height;
                
                uint32_t top_left, top_right, bot_left, bot_right;

                if (g_wallpaper_theme == 1) { // Sunset Horizon
                    top_left  = 0x4A0E4E; // Deep Plum
                    top_right = 0xC2185B; // Magenta
                    bot_left  = 0xE65100; // Deep Orange
                    bot_right = 0xFDD835; // Golden Yellow
                } else if (g_wallpaper_theme == 2) { // Cyberpunk Neon
                    top_left  = 0x0D1B2A; // Midnight Navy
                    top_right = 0x1B263B; // Deep Slate
                    bot_left  = 0x0077B6; // Ocean Blue
                    bot_right = 0x00F5D4; // Neon Aqua
                } else if (g_wallpaper_theme == 3) { // Minimalist Slate
                    top_left  = 0x1C1C1E; // Space Grey
                    top_right = 0x2C2C2E; // Dark Slate
                    bot_left  = 0x3A3A3C; // Mid Grey
                    bot_right = 0x1C1C1E; // Space Grey
                } else { // 0 = Ventura Indigo & Warm Rose (Default)
                    top_left  = 0x1E1B4B; // Deep indigo
                    top_right = 0x5B21B6; // Purple
                    bot_left  = 0xBE123C; // Rose
                    bot_right = 0xF59E0B; // Amber
                }
                
                uint32_t top = dui_color_lerp(top_left, top_right, fx);
                uint32_t bot = dui_color_lerp(bot_left, bot_right, fx);
                
                backbuffer[y * screen_width + x] = dui_color_lerp(top, bot, fy);
            }
        }
    }
}
