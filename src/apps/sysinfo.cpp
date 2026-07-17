#include <stdbool.h>
#include <stdint.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_cursor(int x, int y);
    
    // Stub registry functions for setting resolution
    extern void blankReg_set_string(char* key, char* value);

    void launch_sysinfo(void) {
        int win_w = 700;
        int win_h = 500;
        int win_x = (1024 - win_w) / 2;
        int win_y = (768 - win_h) / 2;
        
        bool done = false;
        int active_tab = 0; // 0 = Display, 1 = GPU Rendering, 2 = About
        
        // Wait for user interaction
        while(!done) {
            draw_macos_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"System Settings");
            
            // Sidebar
            blankUI_draw_button(win_x + 10, win_y + 40, 150, 40, (char*)"Displays");
            blankUI_draw_button(win_x + 10, win_y + 90, 150, 40, (char*)"GPU Rendering");
            blankUI_draw_button(win_x + 10, win_y + 140, 150, 40, (char*)"About OS");
            blankUI_draw_button(win_x + 10, win_y + win_h - 50, 150, 40, (char*)"Close Settings");
            
            if (active_tab == 0) {
                blankUI_draw_text_color(win_x + 180, win_y + 50, (char*)"Display Resolution", 0x000000);
                blankUI_draw_text_color(win_x + 180, win_y + 80, (char*)"Select a resolution. A reboot is required to apply.", 0x666666);
                blankUI_draw_button(win_x + 180, win_y + 110, 120, 32, (char*)"1920 x 1080");
                blankUI_draw_button(win_x + 320, win_y + 110, 120, 32, (char*)"1024 x 768");
                blankUI_draw_button(win_x + 460, win_y + 110, 120, 32, (char*)"800 x 600");
                
                blankUI_draw_text_color(win_x + 180, win_y + 180, (char*)"Refresh Rate", 0x000000);
                blankUI_draw_button(win_x + 180, win_y + 210, 120, 32, (char*)"60 Hz");
                blankUI_draw_button(win_x + 320, win_y + 210, 120, 32, (char*)"144 Hz");
                
            } else if (active_tab == 1) {
                blankUI_draw_text_color(win_x + 180, win_y + 50, (char*)"GPU Hardware Acceleration", 0x000000);
                blankUI_draw_text_color(win_x + 180, win_y + 80, (char*)"Driver: BDRM (BlankOS Direct Rendering Manager)", 0x666666);
                blankUI_draw_text_color(win_x + 180, win_y + 100, (char*)"Status: Hardware Blitting Active", 0x008800);
                
                blankUI_draw_button(win_x + 180, win_y + 140, 200, 32, (char*)"Enable V-Sync (Double Buffer)");
                blankUI_draw_button(win_x + 180, win_y + 180, 200, 32, (char*)"Enable Anti-Aliasing (MSAA)");
                
            } else {
                blankUI_draw_text_color(win_x + 180, win_y + 50, (char*)"BlankOS - Vibe Coded Edition", 0x000000);
                blankUI_draw_text_color(win_x + 180, win_y + 80, (char*)"Kernel: Custom Monolithic UEFI", 0x666666);
                blankUI_draw_text_color(win_x + 180, win_y + 110, (char*)"Architecture: x86_64 Bare Metal", 0x666666);
            }
            
            swap_buffers();
            
            // Simple delay to let the user see it, then auto-close for now
            // To make it fully interactive, we'd port the mouse event loop here.
            // For the sake of demonstration, we show it for 4 seconds then close.
            for (volatile int d = 0; d < 80000000; d++);
            active_tab++;
            if (active_tab > 2) done = true;
        }
    }
}
