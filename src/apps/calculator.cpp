#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    extern void blankUI_set_cursor_type(int type);
    extern void play_system_sound(const char* event_name);

    extern int screen_width;
    extern int screen_height;

    static int c_strlen(const char* s) {
        int n = 0;
        while (s && s[n]) n++;
        return n;
    }

    static void c_strcpy(char* dst, const char* src, int max_len) {
        int i = 0;
        while (src && src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }

    static void c_strcat(char* dst, const char* src, int max_len) {
        int len = c_strlen(dst);
        int i = 0;
        while (src && src[i] && (len + i) < max_len - 1) {
            dst[len + i] = src[i];
            i++;
        }
        dst[len + i] = '\0';
    }

    static void int64_to_str(int64_t val, char* buf) {
        if (val == 0) {
            buf[0] = '0';
            buf[1] = '\0';
            return;
        }
        int i = 0;
        bool is_neg = false;
        uint64_t uval = 0;
        if (val < 0) {
            is_neg = true;
            uval = (uint64_t)(-val);
        } else {
            uval = (uint64_t)val;
        }
        while (uval != 0) {
            buf[i++] = (char)((uval % 10) + '0');
            uval /= 10;
        }
        if (is_neg) buf[i++] = '-';
        buf[i] = '\0';

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

    // Format double with up to 4 decimal places (trimming trailing zeros)
    static void double_to_str(double val, char* buf) {
        if (val < 0.0) {
            *buf++ = '-';
            val = -val;
        }
        int64_t ipart = (int64_t)val;
        double frac = val - (double)ipart;
        int64_t fpart = (int64_t)(frac * 10000.0 + 0.5);
        if (fpart >= 10000) {
            ipart += 1;
            fpart -= 10000;
        }
        char ibuf[32];
        int64_to_str(ipart, ibuf);
        int i = 0;
        while (ibuf[i]) {
            *buf++ = ibuf[i++];
        }
        if (fpart > 0) {
            *buf++ = '.';
            char fbuf[8];
            fbuf[0] = (char)('0' + ((fpart / 1000) % 10));
            fbuf[1] = (char)('0' + ((fpart / 100) % 10));
            fbuf[2] = (char)('0' + ((fpart / 10) % 10));
            fbuf[3] = (char)('0' + (fpart % 10));
            int last = 3;
            while (last > 0 && fbuf[last] == '0') last--;
            for (int k = 0; k <= last; k++) {
                *buf++ = fbuf[k];
            }
        }
        *buf = '\0';
    }

    static double str_to_double(const char* s) {
        if (!s || !s[0]) return 0.0;
        int i = 0;
        double sign = 1.0;
        if (s[i] == '-') {
            sign = -1.0;
            i++;
        }
        double res = 0.0;
        while (s[i] >= '0' && s[i] <= '9') {
            res = res * 10.0 + (double)(s[i] - '0');
            i++;
        }
        if (s[i] == '.') {
            i++;
            double place = 0.1;
            while (s[i] >= '0' && s[i] <= '9') {
                res += (double)(s[i] - '0') * place;
                place *= 0.1;
                i++;
            }
        }
        return res * sign;
    }

    static void uint32_to_hex_str(uint32_t v, char* out) {
        const char* hex = "0123456789ABCDEF";
        out[0] = '0';
        out[1] = 'x';
        for (int i = 7; i >= 0; i--) {
            out[2 + (7 - i)] = hex[(v >> (i * 4)) & 0xF];
        }
        out[10] = '\0';
    }

    static void uint16_to_bin_str(uint16_t v, char* out) {
        int idx = 0;
        for (int i = 15; i >= 0; i--) {
            out[idx++] = ((v >> i) & 1) ? '1' : '0';
            if (i > 0 && (i % 4 == 0)) {
                out[idx++] = ' ';
            }
        }
        out[idx] = '\0';
    }

    // Save Calculation History Tape to real FAT32 file \CALCTAPE.TXT
    static bool save_tape_to_fat32(EFI_SYSTEM_TABLE* SystemTable, const char tape[8][48], int count) {
        if (!SystemTable || !SystemTable->BootServices) return false;
        EFI_GUID sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
        if (SystemTable->BootServices->LocateProtocol(&sfsGuid, NULL, (void**)&Sfs) != EFI_SUCCESS || !Sfs) return false;
        EFI_FILE_HANDLE Root = NULL;
        if (Sfs->OpenVolume(Sfs, &Root) != EFI_SUCCESS || !Root) return false;

        EFI_FILE_HANDLE File = NULL;
        EFI_STATUS st = Root->Open(Root, &File, (CHAR16*)L"\\CALCTAPE.TXT",
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (st != EFI_SUCCESS || !File) {
            Root->Close(Root);
            return false;
        }

        char export_buf[512] = "=== BlankOS Sequoia Calculator History Tape ===\r\n";
        for (int i = 0; i < count; i++) {
            c_strcat(export_buf, tape[i], 512);
            c_strcat(export_buf, "\r\n", 512);
        }
        UINTN wlen = (UINTN)c_strlen(export_buf);
        st = File->Write(File, &wlen, export_buf);
        File->Flush(File);
        File->Close(File);
        Root->Close(Root);
        return (st == EFI_SUCCESS);
    }

    // History Tape state
    static char history_tape[8][48];
    static int history_count = 0;
    static char status_banner[48] = "Ready  |  64-Bit FP + Bitwise Inspector";

    static void push_history_line(const char* line) {
        if (history_count < 8) {
            c_strcpy(history_tape[history_count], line, 48);
            history_count++;
        } else {
            for (int i = 0; i < 7; i++) {
                c_strcpy(history_tape[i], history_tape[i + 1], 48);
            }
            c_strcpy(history_tape[7], line, 48);
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

        int win_w = 600;
        int win_h = 540;

        char input_buf[32] = "0";
        double current_val = 0.0;
        double stored_val = 0.0;
        // op codes: 0=none, 1=+, 2=-, 3=*, 4=/, 5=AND, 6=OR, 7=XOR, 8=SHL, 9=SHR
        int current_op = 0;
        bool new_number = true;
        bool has_decimal = false;
        bool prev_left = false;

        const char* btn_labels[20] = {
            "AC", "+/-", "%", "/",
            "7", "8", "9", "*",
            "4", "5", "6", "-",
            "1", "2", "3", "+",
            "0", "0", ".", "="
        };

        uint32_t btn_colors[20] = {
            0x5C5C5E, 0x5C5C5E, 0x5C5C5E, 0xFF9F0A,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9F0A,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9F0A,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9F0A,
            0x3A3A3C, 0x3A3A3C, 0x3A3A3C, 0xFF9F0A
        };

        const char* bit_labels[6] = { "AND", "OR", "XOR", "NOT", "<<", ">>" };

        while (!done) {
            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);

            // Helper lambda-equivalent state transitions via key/button action
            char action_cmd = 0;
            int bit_cmd = -1;

            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) {
                    done = true;
                } else if (Key.UnicodeChar >= '0' && Key.UnicodeChar <= '9') {
                    action_cmd = (char)Key.UnicodeChar;
                } else if (Key.UnicodeChar == '.') {
                    action_cmd = '.';
                } else if (Key.UnicodeChar == '+' || Key.UnicodeChar == '-' ||
                           Key.UnicodeChar == '*' || Key.UnicodeChar == '/') {
                    action_cmd = (char)Key.UnicodeChar;
                } else if (Key.UnicodeChar == '=' || Key.UnicodeChar == 0x0D) {
                    action_cmd = '=';
                } else if (Key.UnicodeChar == 'c' || Key.UnicodeChar == 'C') {
                    action_cmd = 'C';
                } else if (Key.UnicodeChar == '%') {
                    action_cmd = '%';
                } else if (Key.UnicodeChar == 0x08) {
                    action_cmd = 'B'; // Backspace
                }
            }

            bool mouse_click = false;
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx || dy) {
                        cursor_x += dx;
                        cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                    }
                    blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                    if (State.LeftButton && !prev_left) {
                        mouse_click = true;
                    }
                    prev_left = State.LeftButton;
                }
            }

            int start_y = win_y + 198;
            int btn_w = 72;
            int btn_h = 54;
            int gap = 10;

            if (mouse_click) {
                if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                    done = true;
                }

                // Check Bitwise Programmer Bar clicks (6 pills at y = win_y + 154)
                int bit_y = win_y + 154;
                for (int b = 0; b < 6; b++) {
                    int bx = win_x + 18 + b * 54;
                    if (cursor_x >= bx && cursor_x <= bx + 48 && cursor_y >= bit_y && cursor_y <= bit_y + 30) {
                        bit_cmd = b;
                    }
                }

                // Check Standard 5x4 Button Grid
                for (int r = 0; r < 5; r++) {
                    for (int c = 0; c < 4; c++) {
                        if (r == 4 && c == 1) continue;
                        int bx = win_x + 18 + c * (btn_w + gap);
                        int by = start_y + r * (btn_h + gap);
                        int bw = (r == 4 && c == 0) ? (btn_w * 2 + gap) : btn_w;

                        if (cursor_x >= bx && cursor_x <= bx + bw && cursor_y >= by && cursor_y <= by + btn_h) {
                            int idx = r * 4 + c;
                            if (idx == 0) action_cmd = 'C';
                            else if (idx == 1) action_cmd = 'N'; // +/-
                            else if (idx == 2) action_cmd = '%';
                            else action_cmd = btn_labels[idx][0];
                        }
                    }
                }

                // Check Right History Panel "Save \CALCTAPE.TXT" button & "Clear Tape" button
                int hpanel_x = win_x + 356;
                int save_by = win_y + win_h - 52;
                if (cursor_x >= hpanel_x && cursor_x <= hpanel_x + 148 &&
                    cursor_y >= save_by && cursor_y <= save_by + 32) {
                    if (save_tape_to_fat32(SystemTable, history_tape, history_count)) {
                        c_strcpy(status_banner, "Saved tape to FAT32 \\CALCTAPE.TXT!", 48);
                        play_system_sound((char*)"update");
                    } else {
                        c_strcpy(status_banner, "FAT32 Write Error", 48);
                        play_system_sound((char*)"error");
                    }
                }
                if (cursor_x >= hpanel_x + 156 && cursor_x <= hpanel_x + 226 &&
                    cursor_y >= save_by && cursor_y <= save_by + 32) {
                    history_count = 0;
                    c_strcpy(status_banner, "History tape cleared", 48);
                    play_system_sound((char*)"click");
                }

                // Clicking any bit in the 16-bit binary inspector toggles that bit!
                int bin_y = win_y + 122;
                int bin_x0 = win_x + 58;
                if (cursor_y >= bin_y - 4 && cursor_y <= bin_y + 16 && cursor_x >= bin_x0 && cursor_x <= bin_x0 + 260) {
                    int rel_x = cursor_x - bin_x0;
                    int char_idx = rel_x / 8;
                    // Map char_idx in "XXXX XXXX XXXX XXXX" to bit 15..0
                    int bit_pos = -1;
                    int cur_c = 0;
                    for (int b = 15; b >= 0; b--) {
                        if (char_idx == cur_c) {
                            bit_pos = b;
                            break;
                        }
                        cur_c++;
                        if (b > 0 && (b % 4 == 0)) cur_c++;
                    }
                    if (bit_pos >= 0) {
                        int64_t iv = (int64_t)current_val;
                        iv ^= (1LL << bit_pos);
                        current_val = (double)iv;
                        double_to_str(current_val, input_buf);
                        new_number = true;
                        play_system_sound((char*)"click");
                    }
                }
            }

            // Execute Bitwise command if triggered
            if (bit_cmd >= 0) {
                play_system_sound((char*)"click");
                if (bit_cmd == 3) {
                    // NOT (~)
                    int64_t iv = (int64_t)current_val;
                    iv = (~iv) & 0xFFFF;
                    current_val = (double)iv;
                    double_to_str(current_val, input_buf);
                    new_number = true;
                } else {
                    stored_val = current_val;
                    if (bit_cmd == 0) current_op = 5; // AND
                    if (bit_cmd == 1) current_op = 6; // OR
                    if (bit_cmd == 2) current_op = 7; // XOR
                    if (bit_cmd == 4) current_op = 8; // <<
                    if (bit_cmd == 5) current_op = 9; // >>
                    new_number = true;
                    has_decimal = false;
                }
            }

            // Execute standard calculator command if triggered
            if (action_cmd != 0) {
                play_system_sound((char*)"click");
                if (action_cmd >= '0' && action_cmd <= '9') {
                    if (new_number) {
                        input_buf[0] = action_cmd;
                        input_buf[1] = '\0';
                        new_number = false;
                        has_decimal = false;
                    } else {
                        int len = c_strlen(input_buf);
                        if (len == 1 && input_buf[0] == '0') {
                            input_buf[0] = action_cmd;
                        } else if (len < 16) {
                            input_buf[len] = action_cmd;
                            input_buf[len + 1] = '\0';
                        }
                    }
                    current_val = str_to_double(input_buf);
                } else if (action_cmd == '.') {
                    if (new_number) {
                        c_strcpy(input_buf, "0.", 32);
                        new_number = false;
                        has_decimal = true;
                    } else if (!has_decimal) {
                        c_strcat(input_buf, ".", 32);
                        has_decimal = true;
                    }
                } else if (action_cmd == 'B') {
                    int len = c_strlen(input_buf);
                    if (len > 1) {
                        if (input_buf[len - 1] == '.') has_decimal = false;
                        input_buf[len - 1] = '\0';
                    } else {
                        c_strcpy(input_buf, "0", 32);
                        new_number = true;
                    }
                    current_val = str_to_double(input_buf);
                } else if (action_cmd == 'C') {
                    current_val = 0.0;
                    stored_val = 0.0;
                    current_op = 0;
                    c_strcpy(input_buf, "0", 32);
                    new_number = true;
                    has_decimal = false;
                } else if (action_cmd == 'N') {
                    current_val = -current_val;
                    double_to_str(current_val, input_buf);
                } else if (action_cmd == '%') {
                    current_val = current_val * 0.01;
                    double_to_str(current_val, input_buf);
                    new_number = true;
                } else if (action_cmd == '+' || action_cmd == '-' || action_cmd == '*' || action_cmd == '/') {
                    stored_val = current_val;
                    if (action_cmd == '+') current_op = 1;
                    if (action_cmd == '-') current_op = 2;
                    if (action_cmd == '*') current_op = 3;
                    if (action_cmd == '/') current_op = 4;
                    new_number = true;
                    has_decimal = false;
                } else if (action_cmd == '=') {
                    if (current_op != 0) {
                        double rhs = current_val;
                        double res = current_val;
                        const char* op_sym = "?";
                        if (current_op == 1) { res = stored_val + rhs; op_sym = " + "; }
                        else if (current_op == 2) { res = stored_val - rhs; op_sym = " - "; }
                        else if (current_op == 3) { res = stored_val * rhs; op_sym = " * "; }
                        else if (current_op == 4) {
                            op_sym = " / ";
                            if (rhs != 0.0) res = stored_val / rhs;
                            else {
                                c_strcpy(status_banner, "Division by zero guarded", 48);
                                res = 0.0;
                            }
                        } else if (current_op == 5) {
                            res = (double)(((int64_t)stored_val) & ((int64_t)rhs));
                            op_sym = " & ";
                        } else if (current_op == 6) {
                            res = (double)(((int64_t)stored_val) | ((int64_t)rhs));
                            op_sym = " | ";
                        } else if (current_op == 7) {
                            res = (double)(((int64_t)stored_val) ^ ((int64_t)rhs));
                            op_sym = " ^ ";
                        } else if (current_op == 8) {
                            res = (double)(((int64_t)stored_val) << (((int64_t)rhs) & 31));
                            op_sym = " << ";
                        } else if (current_op == 9) {
                            res = (double)(((int64_t)stored_val) >> (((int64_t)rhs) & 31));
                            op_sym = " >> ";
                        }

                        char lhs_s[20], rhs_s[20], res_s[20], tape_line[48];
                        double_to_str(stored_val, lhs_s);
                        double_to_str(rhs, rhs_s);
                        double_to_str(res, res_s);
                        tape_line[0] = '\0';
                        c_strcat(tape_line, lhs_s, 48);
                        c_strcat(tape_line, op_sym, 48);
                        c_strcat(tape_line, rhs_s, 48);
                        c_strcat(tape_line, " = ", 48);
                        c_strcat(tape_line, res_s, 48);
                        push_history_line(tape_line);

                        current_val = res;
                        double_to_str(current_val, input_buf);
                        current_op = 0;
                        new_number = true;
                        has_decimal = false;
                    }
                }
            }

            // Render Desktop & Window
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();

            dui_shadow(win_x, win_y, win_w, win_h, 18, 20, 0x000000, 140);
            dui_rect_rounded(win_x, win_y, win_w, win_h, 18, 0x1C1C1E, 250);
            dui_rect_rounded_outline(win_x, win_y, win_w, win_h, 18, 0x38383A, 1);

            // Traffic lights & Title
            dui_circle(win_x + 18, win_y + 18, 6, 0xFF5F56, 255);
            dui_circle(win_x + 38, win_y + 18, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 58, win_y + 18, 6, 0x27C93F, 255);
            dui_text(win_x + 82, win_y + 14, "Sequoia Calculator + Programmer Inspector", 0x8E8E93, 1);

            // Main Numeric Display Card
            dui_rect_rounded(win_x + 18, win_y + 38, 320, 108, 14, 0x111113, 255);
            dui_rect_rounded_outline(win_x + 18, win_y + 38, 320, 108, 14, 0x2C2C2E, 1);

            // Active operator indicator badge
            const char* op_names[10] = { "", "+", "-", "*", "/", "AND", "OR", "XOR", "SHL", "SHR" };
            if (current_op > 0 && current_op < 10) {
                dui_rect_rounded(win_x + 28, win_y + 46, 36, 18, 5, 0xFF9F0A, 255);
                dui_text(win_x + 34, win_y + 50, op_names[current_op], 0x111113, 1);
            }

            // Primary value text (right-aligned)
            int val_scale = (c_strlen(input_buf) > 10) ? 2 : 3;
            int txt_w = dui_text_width(input_buf, val_scale);
            dui_text(win_x + 326 - txt_w, win_y + 52, input_buf, 0xFFFFFF, val_scale);

            // Live Programmer Telemetry: HEX & Interactive 16-bit BIN
            uint32_t u32_val = (uint32_t)((int64_t)current_val);
            char hex_str[16];
            uint32_to_hex_str(u32_val, hex_str);
            dui_text(win_x + 28, win_y + 98, "HEX:", 0x636366, 1);
            dui_text(win_x + 62, win_y + 98, hex_str, 0x38BDF8, 1);

            char oct_buf[24];
            int64_to_str((int64_t)current_val, oct_buf);
            dui_text(win_x + 178, win_y + 98, "INT64:", 0x636366, 1);
            dui_text(win_x + 226, win_y + 98, oct_buf, 0x34D399, 1);

            char bin_str[32];
            uint16_to_bin_str((uint16_t)(u32_val & 0xFFFF), bin_str);
            dui_text(win_x + 28, win_y + 122, "BIN:", 0x636366, 1);
            dui_text(win_x + 58, win_y + 122, bin_str, 0xFBBF24, 1);

            // Bitwise Operator Row (6 buttons)
            int bit_y = win_y + 154;
            for (int b = 0; b < 6; b++) {
                int bx = win_x + 18 + b * 54;
                bool active_b = ((b == 0 && current_op == 5) || (b == 1 && current_op == 6) ||
                                 (b == 2 && current_op == 7) || (b == 4 && current_op == 8) ||
                                 (b == 5 && current_op == 9));
                dui_rect_rounded(bx, bit_y, 48, 30, 8, active_b ? 0x0A84FF : 0x2C2C2E, 255);
                int btw = dui_text_width(bit_labels[b], 1);
                dui_text(bx + 24 - btw / 2, bit_y + 10, bit_labels[b], 0xFFFFFF, 1);
            }

            // Standard 5x4 macOS Sequoia Button Grid
            for (int r = 0; r < 5; r++) {
                for (int c = 0; c < 4; c++) {
                    if (r == 4 && c == 1) continue;
                    int idx = r * 4 + c;
                    int bx = win_x + 18 + c * (btn_w + gap);
                    int by = start_y + r * (btn_h + gap);
                    int bw = (r == 4 && c == 0) ? (btn_w * 2 + gap) : btn_w;

                    uint32_t color = btn_colors[idx];
                    bool hovered = (cursor_x >= bx && cursor_x <= bx + bw && cursor_y >= by && cursor_y <= by + btn_h);
                    if (hovered) {
                        // Subtle highlight on hover
                        color = (color == 0xFF9F0A) ? 0xFFB340 : 0x48484A;
                    }
                    dui_rect_rounded(bx, by, bw, btn_h, 16, color, 255);

                    const char* lbl = (idx == 0 && !new_number) ? "C" : btn_labels[idx];
                    int tw = dui_text_width(lbl, 2);
                    dui_text(bx + bw / 2 - tw / 2, by + btn_h / 2 - 8, lbl, 0xFFFFFF, 2);
                }
            }

            // Right Panel: Live Calculation History Tape & FAT32 Export
            int hpanel_x = win_x + 356;
            int hpanel_y = win_y + 38;
            int hpanel_w = win_w - 374;
            int hpanel_h = win_h - 100;

            dui_rect_rounded(hpanel_x, hpanel_y, hpanel_w, hpanel_h, 14, 0x141416, 255);
            dui_rect_rounded_outline(hpanel_x, hpanel_y, hpanel_w, hpanel_h, 14, 0x2C2C2E, 1);
            dui_text(hpanel_x + 14, hpanel_y + 14, "CALCULATION TAPE", 0x8E8E93, 1);
            dui_line(hpanel_x + 12, hpanel_y + 34, hpanel_x + hpanel_w - 12, hpanel_y + 34, 0x2C2C2E, 1);

            if (history_count == 0) {
                dui_text(hpanel_x + 14, hpanel_y + 54, "No calculations yet.", 0x48484A, 1);
                dui_text(hpanel_x + 14, hpanel_y + 76, "Click BIN bits to toggle!", 0x48484A, 1);
            } else {
                for (int i = 0; i < history_count; i++) {
                    int hy = hpanel_y + 46 + i * 44;
                    dui_rect_rounded(hpanel_x + 10, hy, hpanel_w - 20, 36, 8, 0x1C1C1E, 255);
                    dui_text(hpanel_x + 16, hy + 12, history_tape[i], 0xE5E5EA, 1);
                }
            }

            // Save to FAT32 & Clear buttons
            int save_by = win_y + win_h - 52;
            dui_rect_rounded(hpanel_x, save_by, 148, 32, 8, 0x059669, 255);
            dui_text(hpanel_x + 12, save_by + 11, "Save \\CALCTAPE.TXT", 0xFFFFFF, 1);

            dui_rect_rounded(hpanel_x + 156, save_by, 70, 32, 8, 0x2C2C2E, 255);
            dui_text(hpanel_x + 174, save_by + 11, "Clear", 0xF87171, 1);

            // Bottom status bar
            dui_text(win_x + 20, win_y + win_h - 16, status_banner, 0x636366, 1);

            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }
    }
}
