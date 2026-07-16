#include <stdint.h>
#include <efi.h>
#include <efilib.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_progress_bar(int x, int y, int width, float percentage);
    
    void launch_updater(EFI_SYSTEM_TABLE *SystemTable) {
        int win_w = 480;
        int win_h = 240;
        int win_x = (1024 - win_w) / 2;
        int win_y = (768 - win_h) / 2;
        
        for (int i = 0; i <= 100; i += 2) {
            draw_macos_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            
            blankUI_draw_window(win_w, win_h, (char*)"Software Update");
            blankUI_draw_text_color(win_x + 40, win_y + 80, (char*)"Downloading macOS-Inspired Update...", 0x000000);
            blankUI_draw_progress_bar(win_x + 40, win_y + 120, 400, (float)i / 100.0f);
            
            swap_buffers();
            for (volatile int d = 0; d < 1000000; d++);
        }
        
        draw_macos_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        blankUI_draw_window(win_w, win_h, (char*)"Software Update");
        blankUI_draw_text_color(win_x + 40, win_y + 80, (char*)"BlankOS is up to date.", 0x000000);
        blankUI_draw_text_color(win_x + 40, win_y + 110, (char*)"Version 1.2.8 is installed.", 0x666666);
        blankUI_draw_button(win_x + 190, win_y + 170, 100, 32, (char*)"Done");
        swap_buffers();
        
        for (volatile int d = 0; d < 80000000; d++);
    }
}
