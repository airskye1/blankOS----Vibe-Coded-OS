#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_rect_filled(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void draw_rect_rounded(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern int screen_width;
    extern int screen_height;
    
    void launch_loading_screen(EFI_SYSTEM_TABLE* SystemTable) {
        // Draw solid black background
        draw_rect_filled(0, 0, screen_width, screen_height, 0x000000, 255);
        swap_buffers();
        
        int text_x = (screen_width - 7 * 8) / 2;
        int text_y = (screen_height) / 2 - 20;
        int bar_w = 200;
        int bar_x = (screen_width - bar_w) / 2;
        int bar_y = text_y + 40;
        
        // Progressively load
        for (int i = 0; i <= 100; i += 2) {
            draw_rect_filled(0, 0, screen_width, screen_height, 0x000000, 255);
            blankUI_draw_text_color(text_x, text_y, (char*)"BlankOS", 0xFFFFFF);
            
            // Progress bar
            draw_rect_rounded(bar_x, bar_y, bar_w, 4, 2, 0x333333, 255);
            draw_rect_rounded(bar_x, bar_y, (int)((bar_w * i) / 100), 4, 2, 0xFFFFFF, 255);
            
            swap_buffers();
            SystemTable->BootServices->Stall(20000); // 20ms delay per frame
        }
    }
}
