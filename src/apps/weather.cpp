#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_gradient_rounded(int x, int y, int w, int h, int radius, uint32_t top_color, uint32_t bottom_color);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    extern void blankUI_set_cursor_type(int type);
    extern void play_system_sound(const char* event_name);

    extern void blankReg_set_int(const char* key, int val);
    extern int blankReg_get_int(const char* key, int default_val);
    extern void blankReg_set_string(const char* key, const char* val);
    extern char* blankReg_get_string(const char* key, const char* default_val);
    struct RTC_Time {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint32_t year;
    };
    extern void get_rtc_time(RTC_Time *time);

    extern int screen_width;
    extern int screen_height;

    // Integer-only sine/cosine approximation (-1000..1000)
    static int w_sin1000(int deg) {
        deg = deg % 360;
        if (deg < 0) deg += 360;
        int sign = 1;
        if (deg >= 180) { deg -= 180; sign = -1; }
        if (deg > 90) deg = 180 - deg;
        int num = 4 * deg * (180 - deg);
        int den = 40500 - deg * (180 - deg);
        if (den == 0) den = 1;
        return sign * (num * 1000 / den);
    }
    static int w_cos1000(int deg) { return w_sin1000(deg + 90); }

    static int w_strlen(const char* s) {
        int n = 0;
        while (s && s[n]) n++;
        return n;
    }

    static void w_strcpy(char* dst, const char* src, int max_len) {
        int i = 0;
        while (src && src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }

    static void w_strcat(char* dst, const char* src, int max_len) {
        int len = w_strlen(dst);
        int i = 0;
        while (src && src[i] && (len + i) < max_len - 1) {
            dst[len + i] = src[i];
            i++;
        }
        dst[len + i] = '\0';
    }

    static void w_int_to_str(int val, char* buf) {
        if (val == 0) {
            buf[0] = '0';
            buf[1] = '\0';
            return;
        }
        int i = 0;
        bool neg = false;
        if (val < 0) {
            neg = true;
            val = -val;
        }
        while (val > 0) {
            buf[i++] = (char)((val % 10) + '0');
            val /= 10;
        }
        if (neg) buf[i++] = '-';
        buf[i] = '\0';
        int s = 0, e = i - 1;
        while (s < e) {
            char t = buf[s];
            buf[s] = buf[e];
            buf[e] = t;
            s++;
            e--;
        }
    }

    static int f_to_c(int f) {
        return (f - 32) * 5 / 9;
    }

    static void format_temp(int temp_f, bool use_celsius, char* out) {
        int v = use_celsius ? f_to_c(temp_f) : temp_f;
        w_int_to_str(v, out);
        w_strcat(out, use_celsius ? " C" : " F", 16);
    }

    static void draw_sun_icon(int cx, int cy, int body_r, int ray_len, uint32_t color) {
        dui_circle(cx, cy, body_r + 3, 0xFDE68A, 80);
        dui_circle(cx, cy, body_r, color, 255);
        for (int i = 0; i < 8; i++) {
            int angle = i * 45;
            int inner_r = body_r + 4;
            int outer_r = body_r + ray_len;
            int x0 = cx + (w_cos1000(angle) * inner_r) / 1000;
            int y0 = cy - (w_sin1000(angle) * inner_r) / 1000;
            int x1 = cx + (w_cos1000(angle) * outer_r) / 1000;
            int y1 = cy - (w_sin1000(angle) * outer_r) / 1000;
            dui_line(x0, y0, x1, y1, color, 2);
        }
    }

    static void draw_small_sun(int cx, int cy) {
        dui_circle(cx, cy, 6, 0xFBBF24, 255);
        for (int i = 0; i < 8; i++) {
            int angle = i * 45;
            int x0 = cx + (w_cos1000(angle) * 8) / 1000;
            int y0 = cy - (w_sin1000(angle) * 8) / 1000;
            int x1 = cx + (w_cos1000(angle) * 12) / 1000;
            int y1 = cy - (w_sin1000(angle) * 12) / 1000;
            dui_line(x0, y0, x1, y1, 0xFBBF24, 1);
        }
    }

    static void draw_small_cloud(int cx, int cy) {
        dui_circle(cx - 5, cy + 2, 6, 0xCBD5E1, 230);
        dui_circle(cx + 5, cy + 2, 6, 0xCBD5E1, 230);
        dui_circle(cx, cy - 2, 7, 0xF1F5F9, 245);
    }

    static void draw_small_rain(int cx, int cy) {
        draw_small_cloud(cx, cy - 2);
        dui_line(cx - 4, cy + 6, cx - 6, cy + 11, 0x38BDF8, 1);
        dui_line(cx + 2, cy + 6, cx, cy + 11, 0x38BDF8, 1);
    }

    static void draw_text_centered(int cx, int y, const char* text, uint32_t color, int scale) {
        int tw = dui_text_width(text, scale);
        dui_text(cx - tw / 2, y, text, color, scale);
    }

    typedef struct {
        const char* name;
        const char* region;
        const char* condition;
        int tz_offset;     // Hours offset from UTC/RTC base
        int base_temp_f;
        int high_f;
        int low_f;
        int uv_index;
        int wind_mph;
        int wind_deg;
        const char* wind_dir;
        int humidity;
        int pressure_hpa;
        uint32_t day_top;
        uint32_t day_bot;
        uint32_t night_top;
        uint32_t night_bot;
        int daily_lo[7];
        int daily_hi[7];
        int daily_icon[7]; // 0=sun, 1=cloud, 2=rain
    } CityWeather;

    static const CityWeather cities[6] = {
        {
            "Cupertino", "California, USA", "Mostly Sunny", -7,
            72, 76, 55, 7, 9, 315, "NW", 48, 1016,
            0x0284C7, 0x1E3A8A, 0x090D16, 0x1E1B4B,
            { 55, 54, 57, 58, 56, 53, 55 },
            { 76, 74, 79, 81, 75, 72, 77 },
            { 0, 0, 0, 1, 0, 1, 0 }
        },
        {
            "Tokyo", "Kanto, Japan", "Clear Sky", 9,
            66, 71, 56, 6, 11, 60, "ENE", 54, 1018,
            0x1D4ED8, 0x0F172A, 0x090D16, 0x172554,
            { 56, 58, 55, 53, 57, 59, 58 },
            { 71, 73, 68, 69, 74, 75, 72 },
            { 0, 1, 2, 1, 0, 0, 1 }
        },
        {
            "New York", "New York, USA", "Partly Cloudy", -4,
            64, 68, 52, 5, 14, 225, "SW", 62, 1012,
            0x2563EB, 0x1E293B, 0x0B0F19, 0x1E293B,
            { 52, 50, 49, 54, 56, 51, 50 },
            { 68, 65, 63, 70, 72, 66, 64 },
            { 1, 1, 2, 0, 0, 1, 2 }
        },
        {
            "London", "United Kingdom", "Light Rain", 1,
            57, 61, 48, 3, 18, 270, "W", 82, 1004,
            0x334155, 0x0F172A, 0x090D16, 0x1E293B,
            { 48, 47, 46, 49, 50, 48, 47 },
            { 61, 59, 58, 63, 64, 60, 59 },
            { 2, 2, 1, 1, 0, 2, 1 }
        },
        {
            "Reykjavik", "Iceland", "Subpolar Crisp", 0,
            43, 47, 34, 2, 21, 30, "NNE", 74, 998,
            0x0E7490, 0x0F172A, 0x042F2E, 0x090D16,
            { 34, 32, 35, 36, 33, 31, 34 },
            { 47, 44, 46, 48, 45, 42, 46 },
            { 1, 2, 1, 0, 2, 1, 0 }
        },
        {
            "Cairo", "Egypt", "Arid & Sunny", 3,
            91, 95, 73, 10, 12, 15, "NNE", 24, 1014,
            0xD97706, 0x7C2D12, 0x1C1917, 0x431407,
            { 73, 75, 74, 76, 74, 72, 73 },
            { 95, 97, 94, 98, 96, 93, 95 },
            { 0, 0, 0, 0, 0, 0, 0 }
        }
    };

    void launch_weather(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL* Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool done = false;

        int win_w = 900;
        int win_h = 580;
        if (win_w > screen_width - 40) win_w = screen_width - 40;
        if (win_h > screen_height - 80) win_h = screen_height - 80;

        // Load persisted preferences from BlankReg
        int active_city = blankReg_get_int("weather.city_idx", 0);
        if (active_city < 0 || active_city >= 6) active_city = 0;
        bool use_celsius = (blankReg_get_int("weather.celsius", 0) == 1);

        bool prev_left = false;
        const char* day_labels[7] = { "Today", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

        while (!done) {
            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);

            // Read Real Hardware RTC Clock
            RTC_Time rt;
            get_rtc_time(&rt);
            uint8_t rtc_h = rt.hour, rtc_m = rt.minute, rtc_s = rt.second;

            // Keyboard input
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) {
                    done = true;
                    continue;
                } else if (Key.UnicodeChar >= '1' && Key.UnicodeChar <= '6') {
                    active_city = Key.UnicodeChar - '1';
                    blankReg_set_int("weather.city_idx", active_city);
                    blankReg_set_string("weather.city_name", cities[active_city].name);
                    play_system_sound("click");
                } else if (Key.UnicodeChar == 'c' || Key.UnicodeChar == 'C' ||
                           Key.UnicodeChar == 'f' || Key.UnicodeChar == 'F') {
                    use_celsius = !use_celsius;
                    blankReg_set_int("weather.celsius", use_celsius ? 1 : 0);
                    play_system_sound("click");
                }
            }

            // Mouse input
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
                        if (cursor_x >= screen_width) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y >= screen_height) cursor_y = screen_height - 1;
                    }
                    blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                    if (State.LeftButton && !prev_left) {
                        mouse_click = true;
                    }
                    prev_left = State.LeftButton;
                }
            }

            if (mouse_click) {
                if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                    done = true;
                }

                // Check Left Sidebar City Cards (6 cities)
                for (int i = 0; i < 6; i++) {
                    int cy = win_y + 58 + i * 74;
                    if (cursor_x >= win_x + 12 && cursor_x <= win_x + 212 &&
                        cursor_y >= cy && cursor_y <= cy + 66) {
                        active_city = i;
                        blankReg_set_int("weather.city_idx", active_city);
                        blankReg_set_string("weather.city_name", cities[active_city].name);
                        play_system_sound("click");
                    }
                }

                // Check °F / °C Toggle Pill
                int unit_x = win_x + 12;
                int unit_y = win_y + win_h - 46;
                if (cursor_x >= unit_x && cursor_x <= unit_x + 200 &&
                    cursor_y >= unit_y && cursor_y <= unit_y + 34) {
                    use_celsius = !use_celsius;
                    blankReg_set_int("weather.celsius", use_celsius ? 1 : 0);
                    play_system_sound("click");
                }
            }

            const CityWeather* cw = &cities[active_city];

            // Compute city local hour from real hardware RTC clock
            int local_hour = ((int)rtc_h + cw->tz_offset + 24) % 24;
            bool is_daytime = (local_hour >= 6 && local_hour < 19);

            // Diurnal temperature modulation based on real local hour
            int hour_delta_f = (w_sin1000((local_hour - 9) * 15) * 5) / 1000;
            int live_temp_f = cw->base_temp_f + hour_delta_f;

            // 1. Desktop background & menubar
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();

            // 2. Weather Window Backdrop with Dynamic Sky Gradient
            dui_shadow(win_x, win_y, win_w, win_h, 18, 22, 0x000000, 140);
            uint32_t sky_top = is_daytime ? cw->day_top : cw->night_top;
            uint32_t sky_bot = is_daytime ? cw->day_bot : cw->night_bot;
            dui_gradient_rounded(win_x, win_y, win_w, win_h, 18, sky_top, sky_bot);
            dui_rect_rounded_outline(win_x, win_y, win_w, win_h, 18, 0x64748B, 1);

            // 3. Left Frosted Sidebar (City Switcher + Unit Toggle)
            int side_w = 224;
            dui_rect_rounded(win_x + 2, win_y + 2, side_w, win_h - 4, 16, 0x090D16, 175);
            dui_line(win_x + side_w, win_y + 2, win_x + side_w, win_y + win_h - 2, 0x334155, 1);

            // Traffic lights
            dui_circle(win_x + 18, win_y + 20, 6, 0xFF5F56, 255);
            dui_circle(win_x + 38, win_y + 20, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 58, win_y + 20, 6, 0x27C93F, 255);
            dui_text(win_x + 78, win_y + 15, "Atmospheric", 0x94A3B8, 1);

            for (int i = 0; i < 6; i++) {
                int cy = win_y + 58 + i * 74;
                bool sel = (i == active_city);
                dui_rect_rounded(win_x + 12, cy, 200, 64, 12, sel ? 0x2563EB : 0x1E293B, sel ? 220 : 150);
                if (sel) {
                    dui_rect_rounded_outline(win_x + 12, cy, 200, 64, 12, 0x93C5FD, 1);
                }
                dui_text(win_x + 24, cy + 10, cities[i].name, 0xFFFFFF, 2);
                dui_text(win_x + 24, cy + 40, cities[i].condition, sel ? 0xE0F2FE : 0x94A3B8, 1);

                int c_hr = ((int)rtc_h + cities[i].tz_offset + 24) % 24;
                int c_tf = cities[i].base_temp_f + (w_sin1000((c_hr - 9) * 15) * 5) / 1000;
                char t_str[16];
                format_temp(c_tf, use_celsius, t_str);
                int tw = dui_text_width(t_str, 2);
                dui_text(win_x + 200 - tw, cy + 16, t_str, 0xFFFFFF, 2);
            }

            // Unit Toggle Button (°F / °C persisted in BlankReg)
            int unit_x = win_x + 12;
            int unit_y = win_y + win_h - 46;
            dui_rect_rounded(unit_x, unit_y, 200, 34, 10, 0x1E293B, 220);
            dui_rect_rounded_outline(unit_x, unit_y, 200, 34, 10, 0x38BDF8, 1);
            dui_text(unit_x + 22, unit_y + 12,
                     use_celsius ? "Unit: Celsius (C)  [Switch]" : "Unit: Fahrenheit (F) [Switch]",
                     0xF8FAFC, 1);

            // 4. Main Content Area
            int main_x = win_x + side_w + 20;
            int main_w = win_w - side_w - 40;

            // Hero Header: City Name, Live Temperature, Condition, Local RTC Time
            dui_text(main_x, win_y + 22, cw->name, 0xFFFFFF, 3);
            dui_text(main_x, win_y + 54, cw->region, 0xCBD5E1, 1);

            char rtc_str[48] = "Local RTC: ";
            char nb[12];
            if (local_hour < 10) w_strcat(rtc_str, "0", 48);
            w_int_to_str(local_hour, nb);
            w_strcat(rtc_str, nb, 48);
            w_strcat(rtc_str, ":", 48);
            if (rtc_m < 10) w_strcat(rtc_str, "0", 48);
            w_int_to_str(rtc_m, nb);
            w_strcat(rtc_str, nb, 48);
            w_strcat(rtc_str, is_daytime ? " (Daylight)" : " (Night Sky)", 48);
            dui_text(main_x, win_y + 74, rtc_str, 0x7DD3FC, 1);

            // Large Hero Temperature & Sun/Cloud Graphic on Right of Hero
            char hero_temp[16];
            format_temp(live_temp_f, use_celsius, hero_temp);
            int htw = dui_text_width(hero_temp, 4);
            dui_text(main_x + main_w - htw - 90, win_y + 24, hero_temp, 0xFFFFFF, 4);

            char hilo_str[48] = "";
            char h_s[16], l_s[16];
            format_temp(cw->high_f, use_celsius, h_s);
            format_temp(cw->low_f, use_celsius, l_s);
            w_strcat(hilo_str, cw->condition, 48);
            w_strcat(hilo_str, "   H:", 48);
            w_strcat(hilo_str, h_s, 48);
            w_strcat(hilo_str, "  L:", 48);
            w_strcat(hilo_str, l_s, 48);
            int hlw = dui_text_width(hilo_str, 1);
            dui_text(main_x + main_w - hlw - 90, win_y + 68, hilo_str, 0xE2E8F0, 1);

            if (is_daytime) {
                draw_sun_icon(main_x + main_w - 40, win_y + 50, 16, 10, 0xFBBF24);
            } else {
                dui_circle(main_x + main_w - 40, win_y + 50, 16, 0xFEF08A, 230);
                dui_circle(main_x + main_w - 32, win_y + 44, 13, sky_top, 255);
            }

            // 5. 24-Hour Atmospheric Forecast Curve Card
            int hr_y = win_y + 102;
            int hr_h = 132;
            dui_rect_rounded(main_x, hr_y, main_w, hr_h, 14, 0x0F172A, 155);
            dui_rect_rounded_outline(main_x, hr_y, main_w, hr_h, 14, 0x475569, 1);
            dui_text(main_x + 16, hr_y + 12, "24-HOUR ATMOSPHERIC TELEMETRY CURVE (REAL RTC SYNCHRONIZED)", 0x93C5FD, 1);

            int num_pts = 8;
            int step_x = (main_w - 60) / (num_pts - 1);
            int prev_px = 0, prev_py = 0;

            for (int i = 0; i < num_pts; i++) {
                int h_off = i * 3;
                int hr_val = (local_hour + h_off) % 24;
                int tf = cw->base_temp_f + (w_sin1000((hr_val - 9) * 15) * 6) / 1000;
                int px = main_x + 30 + i * step_x;
                int norm = (tf - (cw->base_temp_f - 8)) * 3;
                if (norm < 0) norm = 0;
                if (norm > 44) norm = 44;
                int py = hr_y + 88 - norm;

                if (i > 0) {
                    dui_line(prev_px, prev_py, px, py, 0x38BDF8, 2);
                }
                dui_circle(px, py, 4, (i == 0) ? 0xFBBF24 : 0xFFFFFF, 255);

                char t_lbl[16];
                format_temp(tf, use_celsius, t_lbl);
                draw_text_centered(px, py - 18, t_lbl, 0xF8FAFC, 1);

                char h_lbl[12];
                if (i == 0) {
                    w_strcpy(h_lbl, "Now", 12);
                } else {
                    w_int_to_str(hr_val, h_lbl);
                    w_strcat(h_lbl, ":00", 12);
                }
                draw_text_centered(px, hr_y + 108, h_lbl, 0xCBD5E1, 1);

                prev_px = px;
                prev_py = py;
            }

            // 6. Bottom Split: Left = 7-Day Forecast Rows, Right = Telemetry Cards (UV, Wind Compass, Humidity/Pressure)
            int bot_y = hr_y + hr_h + 14;
            int bot_h = win_y + win_h - bot_y - 16;
            int fc_w = (main_w * 52) / 100;

            // Left Card: 7-Day Forecast
            dui_rect_rounded(main_x, bot_y, fc_w, bot_h, 14, 0x0F172A, 155);
            dui_rect_rounded_outline(main_x, bot_y, fc_w, bot_h, 14, 0x475569, 1);
            dui_text(main_x + 16, bot_y + 12, "7-DAY SYNOPTIC FORECAST", 0x93C5FD, 1);

            int row_h = (bot_h - 38) / 7;
            for (int d = 0; d < 7; d++) {
                int ry = bot_y + 34 + d * row_h;
                dui_text(main_x + 16, ry + 6, day_labels[d], 0xF8FAFC, 1);

                int icx = main_x + 90;
                int icy = ry + 10;
                if (cw->daily_icon[d] == 0) draw_small_sun(icx, icy);
                else if (cw->daily_icon[d] == 1) draw_small_cloud(icx, icy);
                else draw_small_rain(icx, icy);

                char lo_s[16], hi_s[16];
                format_temp(cw->daily_lo[d], use_celsius, lo_s);
                format_temp(cw->daily_hi[d], use_celsius, hi_s);
                dui_text(main_x + 122, ry + 6, lo_s, 0x94A3B8, 1);

                // Temperature range bar
                int bar_x = main_x + 166;
                int bar_max_w = fc_w - 224;
                dui_rect_rounded(bar_x, ry + 8, bar_max_w, 6, 3, 0x1E293B, 220);
                int seg_x = bar_x + ((d * 7) % 18);
                int seg_w = bar_max_w - 22;
                if (seg_w < 20) seg_w = 20;
                dui_rect_rounded(seg_x, ry + 8, seg_w, 6, 3, 0xF59E0B, 255);

                dui_text(main_x + fc_w - 48, ry + 6, hi_s, 0xFFFFFF, 1);
            }

            // Right Telemetry Grid (UV Index Gauge, Wind Vector Compass, Humidity & Barometric Pressure)
            int tel_x = main_x + fc_w + 14;
            int tel_w = main_w - fc_w - 14;
            int card_h_top = (bot_h - 12) / 2;

            // Top-Left Telemetry: UV Index Gauge
            int uv_w = (tel_w - 12) / 2;
            dui_rect_rounded(tel_x, bot_y, uv_w, card_h_top, 14, 0x0F172A, 155);
            dui_rect_rounded_outline(tel_x, bot_y, uv_w, card_h_top, 14, 0x475569, 1);
            dui_text(tel_x + 14, bot_y + 12, "UV INDEX", 0x93C5FD, 1);

            char uv_num[8];
            w_int_to_str(cw->uv_index, uv_num);
            dui_text(tel_x + 14, bot_y + 34, uv_num, 0xFFFFFF, 3);
            const char* uv_desc = (cw->uv_index >= 8) ? "Very High" : ((cw->uv_index >= 5) ? "Moderate" : "Low");
            dui_text(tel_x + 14, bot_y + 72, uv_desc, 0xFBBF24, 1);

            // UV progress bar
            int uvb_w = uv_w - 28;
            dui_rect_rounded(tel_x + 14, bot_y + card_h_top - 24, uvb_w, 8, 4, 0x1E293B, 255);
            int uv_fill = (cw->uv_index * uvb_w) / 11;
            if (uv_fill > uvb_w) uv_fill = uvb_w;
            dui_rect_rounded(tel_x + 14, bot_y + card_h_top - 24, uv_fill, 8, 4, 0xF97316, 255);

            // Top-Right Telemetry: Wind Compass & Vector Card
            int wind_x = tel_x + uv_w + 12;
            dui_rect_rounded(wind_x, bot_y, uv_w, card_h_top, 14, 0x0F172A, 155);
            dui_rect_rounded_outline(wind_x, bot_y, uv_w, card_h_top, 14, 0x475569, 1);
            dui_text(wind_x + 14, bot_y + 12, "WIND VECTOR", 0x93C5FD, 1);

            char wspd[24] = "";
            w_int_to_str(cw->wind_mph, nb);
            w_strcat(wspd, nb, 24);
            w_strcat(wspd, " mph", 24);
            dui_text(wind_x + 14, bot_y + 36, wspd, 0xFFFFFF, 2);
            dui_text(wind_x + 14, bot_y + 64, cw->wind_dir, 0x38BDF8, 1);

            // Draw Compass Dial
            int comp_cx = wind_x + uv_w - 38;
            int comp_cy = bot_y + card_h_top / 2 + 8;
            int comp_r = 24;
            dui_circle(comp_cx, comp_cy, comp_r, 0x1E293B, 220);
            int wx1 = comp_cx + (w_sin1000(cw->wind_deg) * (comp_r - 4)) / 1000;
            int wy1 = comp_cy - (w_cos1000(cw->wind_deg) * (comp_r - 4)) / 1000;
            dui_line(comp_cx, comp_cy, wx1, wy1, 0x34D399, 2);
            dui_circle(comp_cx, comp_cy, 3, 0xFFFFFF, 255);

            // Bottom Telemetry Card: Humidity & Barometric Pressure
            int hp_y = bot_y + card_h_top + 12;
            int hp_h = bot_h - card_h_top - 12;
            dui_rect_rounded(tel_x, hp_y, tel_w, hp_h, 14, 0x0F172A, 155);
            dui_rect_rounded_outline(tel_x, hp_y, tel_w, hp_h, 14, 0x475569, 1);
            dui_text(tel_x + 16, hp_y + 12, "HUMIDITY & BAROMETRIC PRESSURE", 0x93C5FD, 1);

            char hum_s[32] = "Humidity: ";
            w_int_to_str(cw->humidity, nb);
            w_strcat(hum_s, nb, 32);
            w_strcat(hum_s, "%", 32);
            dui_text(tel_x + 16, hp_y + 40, hum_s, 0xFFFFFF, 2);

            char pres_s[32] = "Pressure: ";
            w_int_to_str(cw->pressure_hpa, nb);
            w_strcat(pres_s, nb, 32);
            w_strcat(pres_s, " hPa", 32);
            dui_text(tel_x + 16, hp_y + 72, pres_s, 0x34D399, 2);

            dui_text(tel_x + 16, hp_y + 104, "Persisted via BlankReg NVRAM Store", 0x94A3B8, 1);

            // 7. Cursor & Flip
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }
    }
}
