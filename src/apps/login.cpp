#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <efi.h>

#include "../ui/blankDUI.h"
#include "../ui/theme.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"

extern "C" {
    typedef struct {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint32_t year;
    } RTC_Time;

    extern void get_rtc_time(RTC_Time* time);
    extern void sha256_hash(char* input_string, uint8_t* output_hash);
    extern void os_restart(void);
    extern void os_shutdown(void);
    extern char* blankReg_get_string(const char* key, const char* default_val);
    extern void blankReg_set_string(const char* key, const char* val);
}

static void login_str_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int login_str_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static bool login_str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static void login_format_clock(const RTC_Time* rt, char* clock_out, char* date_out) {
    clock_out[0] = (char)('0' + ((rt->hour / 10) % 10));
    clock_out[1] = (char)('0' + (rt->hour % 10));
    clock_out[2] = ':';
    clock_out[3] = (char)('0' + ((rt->minute / 10) % 10));
    clock_out[4] = (char)('0' + (rt->minute % 10));
    clock_out[5] = ':';
    clock_out[6] = (char)('0' + ((rt->second / 10) % 10));
    clock_out[7] = (char)('0' + (rt->second % 10));
    clock_out[8] = '\0';

    uint32_t y = (rt->year >= 2000 && rt->year <= 9999) ? rt->year : 2026;
    date_out[0] = (char)('0' + ((y / 1000) % 10));
    date_out[1] = (char)('0' + ((y / 100) % 10));
    date_out[2] = (char)('0' + ((y / 10) % 10));
    date_out[3] = (char)('0' + (y % 10));
    date_out[4] = '-';
    date_out[5] = (char)('0' + ((rt->month / 10) % 10));
    date_out[6] = (char)('0' + (rt->month % 10));
    date_out[7] = '-';
    date_out[8] = (char)('0' + ((rt->day / 10) % 10));
    date_out[9] = (char)('0' + (rt->day % 10));
    date_out[10] = '\0';
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return (px >= x && px < x + w && py >= y && py < y + h);
}

extern "C" void launch_login_screen(EFI_SYSTEM_TABLE* SystemTable) {
    const char* saved_user = blankReg_get_string("User/Name", "Admin");
    char initial_user[32];
    login_str_copy(initial_user, (saved_user && saved_user[0]) ? saved_user : "Admin", sizeof(initial_user));

    char user_buf[32];
    login_str_copy(user_buf, initial_user, sizeof(user_buf));
    int user_len = login_str_len(user_buf);

    char pass_buf[32];
    pass_buf[0] = '\0';
    int pass_len = 0;

    int active_field = 1; // 0 = Username, 1 = Passcode
    bool logged_in = false;
    uint32_t frame_cnt = 0;
    uint8_t pass_hash[32];
    BlankInputState in;

    while (!logged_in) {
        frame_cnt++;
        input_poll(&in);
        audio_tick();
        dui_set_pointer_down(in.left_down);

        int sw = (int)(screen_width > 0 ? screen_width : 1024);
        int sh = (int)(screen_height > 0 ? screen_height : 768);

        int card_w = 408;
        int card_h = 348;
        int card_x = (sw - card_w) / 2;
        int card_y = (sh - card_h) / 2 + 28;

        int inp_x = card_x + 32;
        int inp_w = card_w - 64;
        int user_y = card_y + 128;
        int pass_y = card_y + 192;
        int inp_h = 40;

        int btn_w = card_w - 64;
        int btn_h = 44;
        int btn_x = card_x + 32;
        int btn_y = card_y + 258;

        // Handle field focus clicks
        if (in.left_pressed) {
            if (point_in_rect(in.mx, in.my, inp_x, user_y, inp_w, inp_h)) {
                active_field = 0;
                play_system_sound((char*)"tool");
            } else if (point_in_rect(in.mx, in.my, inp_x, pass_y, inp_w, inp_h)) {
                active_field = 1;
                play_system_sound((char*)"tool");
            }
        }

        // Handle keyboard input
        bool submit_requested = false;
        if (in.has_key) {
            if (in.key_char == '\t' || in.scan_code == BLANK_SCAN_UP || in.scan_code == BLANK_SCAN_DOWN) {
                active_field = (active_field == 0) ? 1 : 0;
            } else if (in.key_char == '\r' || in.key_char == '\n') {
                submit_requested = true;
            } else if (in.key_char == '\b' || (uint8_t)in.key_char == 0x08) {
                if (active_field == 0 && user_len > 0) {
                    user_buf[--user_len] = '\0';
                } else if (active_field == 1 && pass_len > 0) {
                    pass_buf[--pass_len] = '\0';
                }
            } else if (in.key_char >= 32 && in.key_char <= 126) {
                if (active_field == 0 && user_len < 24) {
                    user_buf[user_len++] = in.key_char;
                    user_buf[user_len] = '\0';
                } else if (active_field == 1 && pass_len < 28) {
                    pass_buf[pass_len++] = in.key_char;
                    pass_buf[pass_len] = '\0';
                }
            }
        }

        // Render fullscreen cached Aurora wallpaper
        dui_draw_wallpaper(dui_get_wallpaper_id());
        dui_rect(0, 0, sw, sh, THEME_BG_VOID, 44);

        // Giant Horizon Clock & Date Header
        RTC_Time rt;
        get_rtc_time(&rt);
        char clock_str[12];
        char date_str[16];
        login_format_clock(&rt, clock_str, date_str);

        int clock_y = (card_y > 140) ? (card_y - 104) : 28;
        int cw = dui_text_width(clock_str, 3);
        dui_text((sw - cw) / 2, clock_y, clock_str, THEME_TEXT_PRIMARY, 3);

        char sub_hdr[48];
        login_str_copy(sub_hdr, date_str, sizeof(sub_hdr));
        int slen = login_str_len(sub_hdr);
        login_str_copy(sub_hdr + slen, "  |  BlankOS Horizon", (int)sizeof(sub_hdr) - slen);
        int sw_txt = dui_text_width(sub_hdr, 1);
        dui_text((sw - sw_txt) / 2, clock_y + 48, sub_hdr, THEME_TEXT_SECONDARY, 1);

        // Centered Lira Bento Login Card (24px radius, dot grid accent, crisp hairlines)
        dui_shadow(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, 12);
        dui_bento_card(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 232, true);
        dui_dot_grid_overlay(card_x + 16, card_y + 14, card_w - 32, 74, 14, THEME_TEXT_PRIMARY, 12);

        // Tactile Monogram Seal (zero neon halo)
        int av_x = card_x + card_w / 2;
        int av_y = card_y + 54;
        dui_circle(av_x, av_y, 28, THEME_SURFACE_CARD, 255);
        dui_circle_outline(av_x, av_y, 28, THEME_BORDER_SPECULAR, 210);
        dui_circle_outline(av_x, av_y, 33, THEME_BORDER_SUBTLE, 130);

        char initial[2] = { 'H', '\0' };
        if (user_len > 0) {
            char ch = user_buf[0];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            initial[0] = ch;
        }
        int iw = dui_text_width(initial, 2);
        dui_text(av_x - iw / 2, av_y - 12, initial, THEME_ACCENT_CYAN, 2);

        // Username label + pill input
        dui_text(inp_x + 4, user_y - 20, "Account", THEME_TEXT_SECONDARY, 1);
        dui_rect_rounded(inp_x, user_y, inp_w, inp_h, THEME_RADIUS_PILL, THEME_SURFACE_INPUT, 235);
        dui_rect_rounded_outline(inp_x, user_y, inp_w, inp_h, THEME_RADIUS_PILL,
                                 (active_field == 0) ? THEME_BORDER_FOCUS : THEME_BORDER_SUBTLE, 220);

        bool blink_on = ((frame_cnt / 24) % 2) == 0;
        if (user_len == 0) {
            dui_text(inp_x + 16, user_y + 11, (active_field == 0 && blink_on) ? "|" : "Username", THEME_TEXT_MUTED, 1);
        } else {
            char disp_user[36];
            login_str_copy(disp_user, user_buf, sizeof(disp_user));
            if (active_field == 0 && blink_on) {
                int dl = login_str_len(disp_user);
                if (dl < 34) { disp_user[dl] = '|'; disp_user[dl + 1] = '\0'; }
            }
            dui_text_ellipsis(inp_x + 16, user_y + 11, inp_w - 32, disp_user, THEME_TEXT_PRIMARY, 1);
        }

        // Passcode label + pill input
        dui_text(inp_x + 4, pass_y - 20, "Passcode", THEME_TEXT_SECONDARY, 1);
        dui_rect_rounded(inp_x, pass_y, inp_w, inp_h, THEME_RADIUS_PILL, THEME_SURFACE_INPUT, 235);
        dui_rect_rounded_outline(inp_x, pass_y, inp_w, inp_h, THEME_RADIUS_PILL,
                                 (active_field == 1) ? THEME_BORDER_FOCUS : THEME_BORDER_SUBTLE, 220);

        if (pass_len == 0) {
            dui_text(inp_x + 16, pass_y + 11,
                     (active_field == 1 && blink_on) ? "|  Enter passcode or press Enter" : "Enter passcode or press Enter",
                     THEME_TEXT_MUTED, 1);
        } else {
            char mask[36];
            for (int i = 0; i < pass_len; i++) mask[i] = '*';
            mask[pass_len] = '\0';
            if (active_field == 1 && blink_on && pass_len < 34) {
                mask[pass_len] = '|';
                mask[pass_len + 1] = '\0';
            }
            dui_text_ellipsis(inp_x + 16, pass_y + 11, inp_w - 32, mask, THEME_TEXT_PRIMARY, 1);
        }

        // Unlock Horizon Cyan Pill Button
        if (dui_button(btn_x, btn_y, btn_w, btn_h, "Unlock Horizon",
                       THEME_ACCENT_CYAN, true, in.mx, in.my, in.left_pressed)) {
            submit_requested = true;
        }

        if (submit_requested) {
            sha256_hash(pass_buf, pass_hash);
            if (user_len > 0 && !login_str_eq(user_buf, initial_user)) {
                blankReg_set_string("User/Name", user_buf);
            }
            play_system_sound((char*)"open");
            logged_in = true;
        }

        // Subtle bottom-right Restart / Power Off pills
        int py = sh - 54;
        int pw_off_w = 114;
        int restart_w = 104;
        int pw_off_x = sw - 24 - pw_off_w;
        int restart_x = pw_off_x - 10 - restart_w;

        if (dui_icon_pill(restart_x, py, restart_w, 34, "Restart", 16, false, in.mx, in.my, in.left_pressed)) {
            play_system_sound((char*)"close");
            os_restart();
        }
        if (dui_icon_pill(pw_off_x, py, pw_off_w, 34, "Power Off", 10, false, in.mx, in.my, in.left_pressed)) {
            play_system_sound((char*)"close");
            os_shutdown();
        }

        blankUI_draw_cursor(in.mx, in.my, 0);
        swap_buffers();

        if (SystemTable && SystemTable->BootServices) {
            SystemTable->BootServices->Stall(16000);
        }
    }
}
