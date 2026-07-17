#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_progress_bar(int x, int y, int width, float percentage);
    extern void blankUI_draw_cursor(int x, int y);
    
    
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
        
        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) {
            Mouse->Reset(Mouse, TRUE);
        }

        EFI_INPUT_KEY Key;
        int cursor_x = 512;
        int cursor_y = 384;
        bool done = false;
        
        while (!done) {
            bool redraw = false;
            
            EFI_STATUS Status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);
            if (Status == EFI_SUCCESS) {
                if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n' || Key.UnicodeChar == ' ') {
                    done = true;
                }
            }
            
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                Status = Mouse->GetState(Mouse, &State);
                if (Status == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    
                    if (dx != 0 || dy != 0) {
                        cursor_x += dx;
                        cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > 1023) cursor_x = 1023;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > 767) cursor_y = 767;
                        redraw = true;
                    }
                    
                    if (State.LeftButton) {
                        // Click on "Done" button (win_x + 190, win_y + 170, 100, 32)
                        if (cursor_x >= win_x + 190 && cursor_x <= win_x + 290 &&
                            cursor_y >= win_y + 170 && cursor_y <= win_y + 202) {
                            done = true;
                        }
                    }
                }
            }
            
            if (redraw) {
                draw_macos_wallpaper();
                blankUI_draw_menubar();
                blankUI_draw_dock();
                blankUI_draw_window(win_w, win_h, (char*)"Software Update");
                blankUI_draw_text_color(win_x + 40, win_y + 80, (char*)"BlankOS is up to date.", 0x000000);
                blankUI_draw_text_color(win_x + 40, win_y + 110, (char*)"Version 1.2.8 is installed.", 0x666666);
                blankUI_draw_button(win_x + 190, win_y + 170, 100, 32, (char*)"Done (or Enter)");
                blankUI_draw_cursor(cursor_x, cursor_y);
                swap_buffers();
            }
        }
    }
}
