#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    
    extern int screen_width;
    extern int screen_height;
    
    static void int_to_str(long val, char* buf) {
        if (val == 0) {
            buf[0] = '0';
            buf[1] = '\0';
            return;
        }
        int i = 0;
        bool is_neg = false;
        if (val < 0) {
            is_neg = true;
            val = -val;
        }
        while (val != 0) {
            buf[i++] = (val % 10) + '0';
            val /= 10;
        }
        if (is_neg) buf[i++] = '-';
        buf[i] = '\0';
        
        // Reverse
        int start = 0;
        int end = i - 1;
        while (start < end) {
            char temp = buf[start];
            buf[start] = buf[end];
            buf[end] = temp;
            start++;
            end--;
        }
    }
    
    void launch_calculator(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool done = false;
        
        int win_w = 340;
        int win_h = 480;
        int win_x = (screen_width - win_w) / 2;
        int win_y = (screen_height - win_h) / 2;
        
        long current_val = 0;
        long stored_val = 0;
        int current_op = 0; // 0=none, 1=+, 2=-, 3=*, 4=/
        bool new_number = true;
        
        const char* btn_labels[20] = {
            (char*)(char*)(char*)"C", "+/-", "%", "/",
            "7", "8", "9", "*",
            "4", "5", "6", "-",
            "1", "2", "3", "+",
            "0", "0", ".", "=" // Note: double 0 to span two columns
        };
        
        uint32_t btn_colors[20] = {
            0xFF3B30, 0x8E8E93, 0x8E8E93, 0xFF9500,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9500,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9500,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9500,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9500 // 0 spanning, = orange
        };

        while(!done) {
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) { done = true; } // ESC
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
                        // Close button
                        int cx = win_x + 16, cy = win_y + 16;
                        if ((cursor_x - cx)*(cursor_x - cx) + (cursor_y - cy)*(cursor_y - cy) <= 64) done = true;
                        
                        // Hit test buttons
                        int start_y = win_y + 120;
                        int btn_w = 70;
                        int btn_h = 60;
                        int gap = 12;
                        
                        for (int r = 0; r < 5; r++) {
                            for (int c = 0; c < 4; c++) {
                                if (r == 4 && c == 1) continue; // Skip second part of 0
                                int bx = win_x + 14 + c * (btn_w + gap);
                                int by = start_y + r * (btn_h + gap);
                                int bw = (r == 4 && c == 0) ? (btn_w * 2 + gap) : btn_w;
                                
                                if (cursor_x >= bx && cursor_x <= bx + bw && cursor_y >= by && cursor_y <= by + btn_h) {
                                    int idx = r * 4 + c;
                                    char label = btn_labels[idx][0];
                                    
                                    if (label >= '0' && label <= '9') {
                                        if (new_number) {
                                            current_val = label - '0';
                                            new_number = false;
                                        } else {
                                            current_val = current_val * 10 + (label - '0');
                                        }
                                    } else if (label == 'C') {
                                        current_val = 0;
                                        stored_val = 0;
                                        current_op = 0;
                                        new_number = true;
                                    } else if (label == '+' || label == '-' || label == '*' || label == '/') {
                                        stored_val = current_val;
                                        if (label == '+') current_op = 1;
                                        if (label == '-') current_op = 2;
                                        if (label == '*') current_op = 3;
                                        if (label == '/') current_op = 4;
                                        new_number = true;
                                    } else if (label == '=') {
                                        if (current_op == 1) current_val = stored_val + current_val;
                                        if (current_op == 2) current_val = stored_val - current_val;
                                        if (current_op == 3) current_val = stored_val * current_val;
                                        if (current_op == 4 && current_val != 0) current_val = stored_val / current_val;
                                        current_op = 0;
                                        new_number = true;
                                    }
                                }
                            }
                        }
                        
                        SystemTable->BootServices->Stall(200000); // Debounce
                        redraw = true;
                    }
                }
            }
            
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            
            // Calc window
            dui_shadow(win_x, win_y, win_w, win_h, 16, 12, 0x000000, 80);
            dui_rect_rounded(win_x, win_y, win_w, win_h, 16, 0x1C1C1E, 255);
            
            // Title controls
            dui_circle(win_x + 16, win_y + 16, 6, 0xFF5F56, 255);
            dui_circle(win_x + 36, win_y + 16, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 56, win_y + 16, 6, 0x27C93F, 255);
            
            // Display area
            char val_str[32];
            int_to_str(current_val, val_str);
            int txt_w = dui_text_width(val_str, 3);
            dui_text(win_x + win_w - txt_w - 20, win_y + 70, val_str, 0xFFFFFF, 3);
            
            // Buttons
            int start_y = win_y + 120;
            int btn_w = 70;
            int btn_h = 60;
            int gap = 12;
            
            for (int r = 0; r < 5; r++) {
                for (int c = 0; c < 4; c++) {
                    if (r == 4 && c == 1) continue; // Skip second part of 0
                    
                    int idx = r * 4 + c;
                    int bx = win_x + 14 + c * (btn_w + gap);
                    int by = start_y + r * (btn_h + gap);
                    int bw = (r == 4 && c == 0) ? (btn_w * 2 + gap) : btn_w;
                    
                    uint32_t color = btn_colors[idx];
                    dui_rect_rounded(bx, by, bw, btn_h, 16, color, 255);
                    
                    int tw = dui_text_width(btn_labels[idx], 2);
                    dui_text(bx + bw/2 - tw/2, by + btn_h/2 - 8, btn_labels[idx], 0xFFFFFF, 2);
                }
            }
            
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
        }
    }
}
