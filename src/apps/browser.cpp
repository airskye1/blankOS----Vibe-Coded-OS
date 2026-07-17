#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_gradient_v(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    
    extern int screen_width;
    extern int screen_height;
    
    void launch_browser(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool done = false;
        
        int win_w = 900;
        int win_h = 600;
        int win_x = (screen_width - win_w) / 2;
        int win_y = (screen_height - win_h) / 2;
        
        char url_buf[128] = "blank://home";
        int url_len = 12;
        int current_page = 0; // 0 = home, 1 = settings, 2 = about
        
        while(!done) {
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) { done = true; } // ESC
                else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126 && url_len < 127) {
                    url_buf[url_len++] = (char)Key.UnicodeChar;
                    url_buf[url_len] = '\0';
                } else if (Key.UnicodeChar == 0x08 && url_len > 0) {
                    url_buf[--url_len] = '\0';
                } else if (Key.UnicodeChar == 0x0D) { // Enter
                    if (url_buf[0] == 'b' && url_buf[8] == 's') current_page = 1; // blank://settings
                    else if (url_buf[0] == 'b' && url_buf[8] == 'a') current_page = 2; // blank://about
                    else current_page = 0; // default to home
                }
            }
            
            bool redraw = false;
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx || dy) {
                        cursor_x += dx; cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                        redraw = true;
                    }
                    if (State.LeftButton) {
                        int cx = win_x + 16, cy = win_y + 16;
                        if ((cursor_x - cx)*(cursor_x - cx) + (cursor_y - cy)*(cursor_y - cy) <= 64) done = true;
                        redraw = true;
                    }
                }
            }
            
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            
            // Window Drop Shadow & Base
            dui_shadow(win_x, win_y, win_w, win_h, 12, 12, 0x000000, 60);
            dui_rect_rounded(win_x, win_y, win_w, win_h, 12, 0xFFFFFF, 255);
            
            // Tab Bar
            dui_rect_rounded(win_x, win_y, win_w, 40, 12, 0xE5E5EA, 255);
            dui_rect(win_x, win_y+20, win_w, 20, 0xE5E5EA, 255); // square bottom
            
            // Traffic lights
            dui_circle(win_x + 16, win_y + 16, 6, 0xFF5F56, 255);
            dui_circle(win_x + 36, win_y + 16, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 56, win_y + 16, 6, 0x27C93F, 255);
            
            // Tab
            dui_rect_rounded(win_x + 80, win_y + 8, 200, 32, 8, 0xFFFFFF, 255);
            dui_rect(win_x + 80, win_y + 24, 200, 16, 0xFFFFFF, 255); // square bottom
            dui_text(win_x + 100, win_y + 16, "New Tab", 0x333333, 1);
            
            // Toolbar
            dui_rect(win_x, win_y + 40, win_w, 50, 0xFFFFFF, 255);
            dui_rect(win_x, win_y + 89, win_w, 1, 0xDDDDDD, 255);
            
            // Back/Forward
            dui_triangle(win_x+25, win_y+65, win_x+35, win_y+55, win_x+35, win_y+75, 0x999999, 255);
            dui_triangle(win_x+65, win_y+65, win_x+55, win_y+55, win_x+55, win_y+75, 0x999999, 255);
            
            // URL Bar
            dui_rect_rounded(win_x + 90, win_y + 50, win_w - 180, 30, 6, 0xF2F2F7, 255);
            dui_text(win_x + 100, win_y + 60, url_buf, 0x333333, 1);
            
            // Content Area
            int content_y = win_y + 90;
            int content_h = win_h - 90;
            
            if (current_page == 0) {
                // Home page
                dui_text(win_x + win_w/2 - 120, content_y + 100, "BlankOS Browser", 0x000000, 2);
                
                // Search bar
                dui_rect_rounded(win_x + win_w/2 - 250, content_y + 160, 500, 48, 24, 0xFFFFFF, 255);
                dui_rect_rounded_outline(win_x + win_w/2 - 250, content_y + 160, 500, 48, 24, 0xDDDDDD, 1);
                dui_text(win_x + win_w/2 - 230, content_y + 176, "Search the web...", 0x999999, 1);
                
                // Tiles
                for (int i=0; i<4; i++) {
                    int tx = win_x + win_w/2 - 240 + i*130;
                    dui_rect_rounded(tx, content_y + 260, 90, 90, 16, 0xF2F2F7, 255);
                }
                dui_text(win_x + win_w/2 - 225, content_y + 360, "Settings", 0x666666, 1);
                dui_text(win_x + win_w/2 - 95, content_y + 360, "Store", 0x666666, 1);
                dui_text(win_x + win_w/2 + 35, content_y + 360, "About", 0x666666, 1);
                dui_text(win_x + win_w/2 + 155, content_y + 360, "SysInfo", 0x666666, 1);
                
            } else if (current_page == 1) {
                // Settings page
                dui_text(win_x + 50, content_y + 50, "Browser Settings", 0x000000, 2);
                dui_text(win_x + 50, content_y + 100, "Privacy and Security", 0x333333, 1);
                dui_text(win_x + 50, content_y + 130, "Clear browsing data", 0x007AFF, 1);
            } else if (current_page == 2) {
                // About page
                dui_text(win_x + 50, content_y + 50, "About BlankOS Browser", 0x000000, 2);
                dui_text(win_x + 50, content_y + 100, "Version 1.0 (64-bit)", 0x666666, 1);
            }
            
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
        }
    }
}
