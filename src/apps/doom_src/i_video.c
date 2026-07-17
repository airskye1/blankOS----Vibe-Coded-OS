#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "d_main.h"
#include "d_event.h"

extern "C" {
    // Screen buffer pointer from compositor
    extern uint32_t* backbuffer;
    extern int screen_width;
    extern int screen_height;
    
    // Window coordinate position
    int doom_win_x = 100;
    int doom_win_y = 100;
    int doom_win_w = 640 + 16;
    int doom_win_h = 400 + 44;
    
    extern EFI_SYSTEM_TABLE* global_ST;
    extern void dui_draw_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_cursor(int x, int y);
    extern void swap_buffers();
    
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    
    // Current palette (256 RGB triples)
    static byte active_palette[768];
    
    // Mouse coords
    static int cursor_x = 512;
    static int cursor_y = 384;
    
    void I_InitGraphics(void) {
        screens[0] = (byte*)malloc(SCREENWIDTH * SCREENHEIGHT);
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    }

    void I_ShutdownGraphics(void) {
        if (screens[0]) {
            free(screens[0]);
            screens[0] = NULL;
        }
    }

    void I_StartFrame(void) {
        // No-op
    }

    void I_UpdateNoBlit(void) {
        // No-op
    }

    // Blits the 320x200 8-bit Doom frame into the 32-bit window backbuffer (2x scale)
    void I_FinishUpdate(void) {
        if (!screens[0] || !backbuffer) return;
        
        // Redraw Desktop Compositor underneath Doom Window
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        blankUI_draw_window(doom_win_w, doom_win_h, (char*)"DOOM - Shareware v1.10");
        
        int wx = doom_win_x + 8; // Offset for window borders
        int wy = doom_win_y + 36;
        
        for (int sy = 0; sy < SCREENHEIGHT; sy++) {
            for (int sx = 0; sx < SCREENWIDTH; sx++) {
                byte idx = screens[0][sy * SCREENWIDTH + sx];
                byte r = active_palette[idx * 3 + 0];
                byte g = active_palette[idx * 3 + 1];
                byte b = active_palette[idx * 3 + 2];
                uint32_t rgb = (r << 16) | (g << 8) | b;
                
                // Write 2x2 scaled pixels
                int dx = wx + sx * 2;
                int dy = wy + sy * 2;
                
                if (dx >= 0 && dx < screen_width - 1 && dy >= 0 && dy < screen_height - 1) {
                    uint32_t* row0 = &backbuffer[dy * screen_width + dx];
                    uint32_t* row1 = &backbuffer[(dy + 1) * screen_width + dx];
                    row0[0] = rgb;
                    row0[1] = rgb;
                    row1[0] = rgb;
                    row1[1] = rgb;
                }
            }
        }
        
        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();
    }

    void I_SetPalette(byte* palette) {
        memcpy(active_palette, palette, 768);
    }

    void I_ReadScreen(byte* scr) {
        if (screens[0]) memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
    }

    // Process input events dynamically inside Doom game loop tick
    void I_StartTic(void) {
        if (!global_ST) return;
        
        // 1. Keyboard event poll
        EFI_INPUT_KEY Key;
        if (global_ST->ConIn->ReadKeyStroke(global_ST->ConIn, &Key) == EFI_SUCCESS) {
            event_t ev;
            
            // Check window close shortcut ESC
            if (Key.ScanCode == 0x17) {
                I_Quit();
            }
            
            // Map standard keys
            ev.type = ev_keydown;
            if (Key.ScanCode == 0x01) ev.data1 = KEY_UPARROW;
            else if (Key.ScanCode == 0x02) ev.data1 = KEY_DOWNARROW;
            else if (Key.ScanCode == 0x03) ev.data1 = KEY_LEFTARROW;
            else if (Key.ScanCode == 0x04) ev.data1 = KEY_RIGHTARROW;
            else if (Key.UnicodeChar == ' ') ev.data1 = ' ';
            else if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') ev.data1 = KEY_ENTER;
            else if (Key.UnicodeChar == '\t') ev.data1 = KEY_TAB;
            else if (Key.UnicodeChar == 'e' || Key.UnicodeChar == 'E') ev.data1 = 'e';
            else if (Key.UnicodeChar == 'w' || Key.UnicodeChar == 'W') ev.data1 = KEY_UPARROW;
            else if (Key.UnicodeChar == 's' || Key.UnicodeChar == 'S') ev.data1 = KEY_DOWNARROW;
            else if (Key.UnicodeChar == 'a' || Key.UnicodeChar == 'A') ev.data1 = KEY_LEFTARROW;
            else if (Key.UnicodeChar == 'd' || Key.UnicodeChar == 'D') ev.data1 = KEY_RIGHTARROW;
            else ev.data1 = Key.UnicodeChar;
            
            D_PostEvent(&ev);
        }
        
        // 2. Mouse event poll
        static EFI_SIMPLE_POINTER_PROTOCOL* Mouse = NULL;
        if (!Mouse) {
            EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
            global_ST->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        }
        
        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;
                cursor_x += dx;
                cursor_y += dy;
                if (cursor_x < 0) cursor_x = 0;
                if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                if (cursor_y < 0) cursor_y = 0;
                if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                
                // Track window dragging coordinates
                blankUI_get_window_pos(&doom_win_x, &doom_win_y, doom_win_w, doom_win_h);
                blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, doom_win_w, doom_win_h);
                
                if (State.LeftButton) {
                    // Check close button click
                    if (blankUI_hit_test_window_close(cursor_x, cursor_y, doom_win_w, doom_win_h)) {
                        I_Quit();
                    }
                    
                    // Post mouse fire click to Doom (data1: buttons mask)
                    event_t ev;
                    ev.type = ev_keydown;
                    ev.data1 = KEY_RCTRL; // Shoot weapon!
                    D_PostEvent(&ev);
                }
                
                // Post mouse turn event
                if (dx != 0) {
                    event_t ev;
                    ev.type = ev_mouse;
                    ev.data1 = State.LeftButton ? 1 : 0;
                    ev.data2 = dx * 3;
                    ev.data3 = 0;
                    D_PostEvent(&ev);
                }
            }
        }
    }
}
