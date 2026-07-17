#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_draw_search_bar(int x, int y, int width);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    
    void launch_blankbrowser(EFI_SYSTEM_TABLE *SystemTable) {
        int win_w = 800;
        int win_h = 560;
        int win_x = (1024 - win_w) / 2;
        int win_y = (768 - win_h) / 2;
        
        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = 512;
        int cursor_y = 384;
        bool done = false;
        bool redraw = true;
        
        while (!done) {
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
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
                        if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                            done = true;
                        }
                    }
                }
            }
            
            if (redraw) {
                draw_macos_wallpaper();
                blankUI_draw_menubar();
                blankUI_draw_dock();
                
                blankUI_draw_window(win_w, win_h, (char*)"blankBrowser 1.2.8");
                blankUI_draw_search_bar(win_x + 40, win_y + 50, 600);
                
                blankUI_draw_text_color(win_x + 40, win_y + 110, (char*)"Recommended Videos:", 0x000000);
                
                blankUI_draw_text_color(win_x + 40, win_y + 150, (char*)"[1] How to build an OS in C++", 0x333333);
                blankUI_draw_button(win_x + 40, win_y + 175, 120, 25, (char*)"Watch");
                
                blankUI_draw_text_color(win_x + 300, win_y + 150, (char*)"[2] Lofi Beats to Code To", 0x333333);
                blankUI_draw_button(win_x + 300, win_y + 175, 120, 25, (char*)"Watch");
                
                blankUI_draw_text_color(win_x + 560, win_y + 150, (char*)"[3] Vibe Coding 101", 0x333333);
                blankUI_draw_button(win_x + 560, win_y + 175, 120, 25, (char*)"Watch");
                
                blankUI_draw_cursor(cursor_x, cursor_y);
                swap_buffers();
                redraw = false;
            }
        }
    }
}
