#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void draw_frosted_glass_rounded(int x, int y, int w, int h, int r, uint32_t tint_color, uint8_t tint_alpha);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_cursor(int x, int y);
    extern void draw_rect_rounded(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha);
    extern void draw_rect_filled(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern int screen_width;
    extern int screen_height;
    
    void launch_login_screen(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        EFI_INPUT_KEY Key;
        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool logged_in = false;
        
        int win_w = 300;
        int win_h = 200;
        int win_x = (screen_width - win_w) / 2;
        int win_y = (screen_height - win_h) / 2;
        
        while (!logged_in) {
            bool redraw = false;
            
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') {
                    logged_in = true;
                }
            }
            
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx != 0 || dy != 0) {
                        cursor_x += dx; cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0; if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0; if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                        redraw = true;
                    }
                    if (State.LeftButton) {
                        // Click Login (win_x + 100, win_y + 120, 100, 32)
                        if (cursor_x >= win_x + 100 && cursor_x <= win_x + 200 &&
                            cursor_y >= win_y + 120 && cursor_y <= win_y + 152) {
                            logged_in = true;
                        }
                    }
                }
            }
            
            if (redraw) {
                draw_macos_wallpaper();
                draw_frosted_glass_rounded(win_x, win_y, win_w, win_h, 16, 0xFFFFFF, 180);
                
                blankUI_draw_text_color(win_x + (win_w - 7 * 8)/2, win_y + 40, (char*)"BlankOS", 0x000000);
                
                // Mock password field
                draw_rect_rounded(win_x + 50, win_y + 70, 200, 30, 8, 0xFFFFFF, 255);
                blankUI_draw_text_color(win_x + 60, win_y + 80, (char*)"**********", 0x000000);
                
                // Login Button
                draw_rect_rounded(win_x + 100, win_y + 120, 100, 32, 16, 0x007AFF, 255);
                blankUI_draw_text_color(win_x + 130, win_y + 130, (char*)"Login", 0xFFFFFF);
                
                blankUI_draw_cursor(cursor_x, cursor_y);
                swap_buffers();
            }
        }
        
        // Final fade out or transition
        for (int i = 0; i <= 255; i += 25) {
            draw_rect_filled(0, 0, screen_width, screen_height, 0x000000, i);
            swap_buffers();
        }
    }
}
