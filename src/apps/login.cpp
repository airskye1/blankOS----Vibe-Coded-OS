#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void blankUI_draw_cursor(int x, int y);
    extern void play_system_sound(char* sound_name);
    extern void sha256_hash(char* input_string, uint8_t* output_hash);
    extern void os_restart(void);
    extern void os_shutdown(void);
    extern int screen_width;
    extern int screen_height;

    struct RTC_Time {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint32_t year;
    };
    extern void get_rtc_time(RTC_Time *time);

    void launch_login_screen(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool logged_in = false;

        char pass_buf[32];
        int pass_len = 0;
        pass_buf[0] = '\0';
        uint8_t pass_hash[32];

        int card_w = 340;
        int card_h = 250;

        while (!logged_in) {
            int card_x = (screen_width - card_w) / 2;
            int card_y = screen_height / 2 - 40;

            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') {
                    sha256_hash(pass_buf, pass_hash);
                    play_system_sound((char*)"click");
                    logged_in = true;
                } else if (Key.UnicodeChar == 0x08) {
                    if (pass_len > 0) {
                        pass_len--;
                        pass_buf[pass_len] = '\0';
                    }
                } else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                    if (pass_len < 28) {
                        pass_buf[pass_len++] = (char)Key.UnicodeChar;
                        pass_buf[pass_len] = '\0';
                    }
                }
            }

            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx != 0 || dy != 0) {
                        cursor_x += dx; cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                    }
                    if (State.LeftButton) {
                        // Unlock button
                        int btn_x = card_x + 90;
                        int btn_y = card_y + 184;
                        if (cursor_x >= btn_x && cursor_x <= btn_x + 160 &&
                            cursor_y >= btn_y && cursor_y <= btn_y + 38) {
                            sha256_hash(pass_buf, pass_hash);
                            play_system_sound((char*)"click");
                            logged_in = true;
                        }
                        // Bottom Restart button
                        int rx = screen_width / 2 - 110;
                        int ry = screen_height - 64;
                        if (cursor_x >= rx && cursor_x <= rx + 96 && cursor_y >= ry && cursor_y <= ry + 32) {
                            os_restart();
                        }
                        // Bottom Shut Down button
                        int sx = screen_width / 2 + 14;
                        if (cursor_x >= sx && cursor_x <= sx + 106 && cursor_y >= ry && cursor_y <= ry + 32) {
                            os_shutdown();
                        }
                    }
                }
            }

            dui_draw_wallpaper();

            // macOS Sonoma / Sequoia Lock Screen Giant Clock at Top
            RTC_Time rt;
            get_rtc_time(&rt);
            char clock_str[12];
            clock_str[0] = '0' + (rt.hour / 10);
            clock_str[1] = '0' + (rt.hour % 10);
            clock_str[2] = ':';
            clock_str[3] = '0' + (rt.minute / 10);
            clock_str[4] = '0' + (rt.minute % 10);
            clock_str[5] = ':';
            clock_str[6] = '0' + (rt.second / 10);
            clock_str[7] = '0' + (rt.second % 10);
            clock_str[8] = '\0';

            int cw = dui_text_width(clock_str, 3);
            dui_text((screen_width - cw) / 2, 72, clock_str, 0xFFFFFF, 3);

            const char* sub_hdr = "BlankOS Sequoia  |  Secure UEFI Session";
            int sw = dui_text_width(sub_hdr, 1);
            dui_text((screen_width - sw) / 2, 118, sub_hdr, 0xE5E5EA, 1);

            // Frosted Glass Lock Card
            dui_shadow(card_x, card_y, card_w, card_h, 20, 12, 0x000000, 90);
            dui_rect_rounded(card_x, card_y, card_w, card_h, 20, 0x1E1E2E, 215);
            dui_rect_rounded_outline(card_x, card_y, card_w, card_h, 20, 0x585B70, 1);

            // Avatar Ring + User Initial
            int av_x = card_x + card_w / 2;
            int av_y = card_y + 52;
            dui_circle(av_x, av_y, 30, 0x007AFF, 255);
            dui_circle(av_x, av_y, 26, 0x11111B, 255);
            dui_text(av_x - 8, av_y - 10, "E", 0x64D2FF, 2);

            const char* uname = "Admin (Administrator)";
            int uw = dui_text_width(uname, 1);
            dui_text(card_x + (card_w - uw) / 2, card_y + 94, uname, 0xFFFFFF, 1);

            // Password Input Field
            int inp_x = card_x + 40;
            int inp_y = card_y + 128;
            int inp_w = card_w - 80;
            dui_rect_rounded(inp_x, inp_y, inp_w, 36, 10, 0x11111B, 240);
            dui_rect_rounded_outline(inp_x, inp_y, inp_w, 36, 10, 0x007AFF, 1);

            if (pass_len == 0) {
                dui_text(inp_x + 14, inp_y + 11, "Enter Passcode or Press Enter...", 0x6C7086, 1);
            } else {
                char mask[32];
                for (int i = 0; i < pass_len; i++) mask[i] = '*';
                mask[pass_len] = '\0';
                dui_text(inp_x + 14, inp_y + 11, mask, 0xFFFFFF, 1);
            }

            // Unlock Pill Button
            int btn_x = card_x + 90;
            int btn_y = card_y + 184;
            dui_rect_rounded(btn_x, btn_y, 160, 38, 19, 0x0A84FF, 255);
            const char* ul_txt = "Unlock Desktop ->";
            dui_text(btn_x + (160 - dui_text_width(ul_txt, 1)) / 2, btn_y + 11, ul_txt, 0xFFFFFF, 1);

            // Bottom Power Pills
            int ry = screen_height - 64;
            int rx = screen_width / 2 - 110;
            int sx = screen_width / 2 + 14;
            dui_rect_rounded(rx, ry, 96, 32, 16, 0x1E1E2E, 200);
            dui_text(rx + 20, ry + 9, "Restart", 0xCDD6F4, 1);

            dui_rect_rounded(sx, ry, 106, 32, 16, 0x1E1E2E, 200);
            dui_text(sx + 16, ry + 9, "Shut Down", 0xF38BA8, 1);

            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }
    }
}
