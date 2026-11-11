#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/theme.h"
#include "../ui/blankDUI.h"
#include "../ui/window_manager.h"
#include "../kernel/input.h"
#include "../kernel/net.h"
#include "../kernel/audio.h"

static int wx_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void wx_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void wx_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int dl = wx_len(dst);
    int i = 0;
    while (src[i] && (dl + i) < max_len - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = '\0';
}

static char wx_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static bool wx_icontains(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && wx_lower(hay[i + j]) == wx_lower(needle[j])) j++;
        if (!needle[j]) return true;
    }
    return false;
}

static void wx_sanitize_ascii(const char* src, char* dst, int max_len) {
    if (!dst || max_len <= 0) return;
    if (!src) { dst[0] = '\0'; return; }
    int w = 0;
    for (int i = 0; src[i] && w < max_len - 1; i++) {
        uint8_t c = (uint8_t)src[i];
        if (c == '\r' || c == '\n') continue;
        // Convert UTF-8 degree sign (0xC2 0xB0) to clean space/deg
        if (c == 0xC2 && (uint8_t)src[i + 1] == 0xB0) {
            i++;
            continue;
        }
        if (c >= 32 && c <= 126) {
            // Collapse repeated spaces left by stripped UTF-8 weather icons
            if (c == ' ' && w > 0 && dst[w - 1] == ' ') continue;
            dst[w++] = (char)c;
        }
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}

static char g_wx_city_input[48] = "";
static bool g_wx_input_focused = false;
static int  g_wx_http_summary_id = -1;
static int  g_wx_http_detail_id = -1;
static bool g_wx_auto_started = false;
static bool g_wx_has_data = false;
static bool g_wx_loading = false;
static char g_wx_error[128] = "";

static char g_wx_summary_line[128] = "";
static char g_wx_location[64] = "";
static char g_wx_condition[48] = "";
static char g_wx_temp[24] = "";
static char g_wx_feels[24] = "";
static char g_wx_humidity[24] = "";
static char g_wx_wind[32] = "";
static char g_wx_pressure[24] = "";

static void wx_build_url(const char* city, const char* query, char* out, int max_len) {
    wx_copy(out, "http://wttr.in/", max_len);
    if (city && city[0]) {
        int pos = wx_len(out);
        for (int i = 0; city[i] && pos < max_len - 20; i++) {
            char c = city[i];
            if (c == ' ') out[pos++] = '+';
            else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ',') {
                out[pos++] = c;
            }
        }
        out[pos] = '\0';
    }
    wx_cat(out, query, max_len);
}

static bool wx_start_fetch(void) {
    NetStatus ns;
    net_get_status(&ns);
    if (!ns.nic_present || !ns.link_up || !ns.dhcp_bound) {
        g_wx_loading = false;
        wx_copy(g_wx_error, "Offline - Connect Ethernet/DHCP to fetch live weather", sizeof(g_wx_error));
        return false;
    }

    if (g_wx_http_summary_id >= 0) { net_http_close(g_wx_http_summary_id); g_wx_http_summary_id = -1; }
    if (g_wx_http_detail_id >= 0)  { net_http_close(g_wx_http_detail_id);  g_wx_http_detail_id = -1; }

    char url[160];
    if (g_wx_city_input[0]) {
        wx_build_url(g_wx_city_input, "?format=3", url, sizeof(url));
    } else {
        wx_copy(url, "http://wttr.in/?format=3", sizeof(url));
    }

    int id = net_http_start(url, NULL);
    if (id < 0) {
        g_wx_loading = false;
        wx_copy(g_wx_error, "Unable to open HTTP request to wttr.in", sizeof(g_wx_error));
        return false;
    }
    g_wx_http_summary_id = id;
    g_wx_loading = true;
    g_wx_error[0] = '\0';
    return true;
}

static void wx_start_detail_fetch(void) {
    char url[160];
    wx_build_url(g_wx_city_input, "?format=%C|%t|%f|%h|%w|%P", url, sizeof(url));
    int id = net_http_start(url, NULL);
    if (id >= 0) g_wx_http_detail_id = id;
}

static void wx_parse_summary(const char* raw) {
    wx_sanitize_ascii(raw, g_wx_summary_line, sizeof(g_wx_summary_line));
    // Extract location before ':' and temp/summary after ':'
    int colon = -1;
    for (int i = 0; g_wx_summary_line[i]; i++) {
        if (g_wx_summary_line[i] == ':') { colon = i; break; }
    }
    if (colon > 0) {
        int l = colon < (int)sizeof(g_wx_location) - 1 ? colon : (int)sizeof(g_wx_location) - 1;
        for (int i = 0; i < l; i++) g_wx_location[i] = g_wx_summary_line[i];
        g_wx_location[l] = '\0';
        const char* after = g_wx_summary_line + colon + 1;
        while (*after == ' ') after++;
        if (*after && !g_wx_temp[0]) wx_copy(g_wx_temp, after, sizeof(g_wx_temp));
    } else {
        wx_copy(g_wx_location, g_wx_city_input[0] ? g_wx_city_input : "Local Station", sizeof(g_wx_location));
    }
}

static void wx_parse_detail(const char* raw) {
    char clean[160];
    wx_sanitize_ascii(raw, clean, sizeof(clean));
    char* parts[6] = { g_wx_condition, g_wx_temp, g_wx_feels, g_wx_humidity, g_wx_wind, g_wx_pressure };
    int   caps[6]  = { 48, 24, 24, 24, 32, 24 };
    int field = 0, w = 0;
    for (int i = 0; clean[i] && field < 6; i++) {
        if (clean[i] == '|') {
            parts[field][w] = '\0';
            field++;
            w = 0;
        } else if (w < caps[field] - 1) {
            parts[field][w++] = clean[i];
        }
    }
    if (field < 6) parts[field][w] = '\0';
}

static void wx_draw_emblem(int cx, int cy, const char* cond) {
    if (wx_icontains(cond, "rain") || wx_icontains(cond, "drizzle") || wx_icontains(cond, "shower")) {
        dui_circle(cx - 10, cy - 2, 15, THEME_TEXT_SECONDARY, 220);
        dui_circle(cx + 10, cy, 13, THEME_TEXT_SECONDARY, 220);
        dui_rect_rounded(cx - 22, cy - 2, 46, 15, 8, THEME_TEXT_SECONDARY, 225);
        dui_line(cx - 10, cy + 17, cx - 14, cy + 27, THEME_ACCENT_CYAN, 235);
        dui_line(cx,      cy + 17, cx - 4,  cy + 27, THEME_ACCENT_CYAN, 235);
        dui_line(cx + 10, cy + 17, cx + 6,  cy + 27, THEME_ACCENT_CYAN, 235);
    } else if (wx_icontains(cond, "cloud") || wx_icontains(cond, "overcast") || wx_icontains(cond, "mist") || wx_icontains(cond, "fog")) {
        dui_circle(cx + 12, cy - 10, 11, THEME_ACCENT_AMBER, 225);
        dui_circle_outline(cx + 12, cy - 10, 12, THEME_BORDER_SPECULAR, 150);
        dui_circle(cx - 10, cy + 2, 15, THEME_TEXT_PRIMARY, 225);
        dui_circle(cx + 10, cy + 4, 13, THEME_TEXT_PRIMARY, 225);
        dui_rect_rounded(cx - 22, cy + 2, 46, 15, 8, THEME_TEXT_PRIMARY, 230);
    } else {
        // Clear / Sun (crisp 1px orbital ring, no blurry low-alpha halo)
        dui_circle(cx, cy, 16, THEME_ACCENT_AMBER, 245);
        dui_circle_outline(cx, cy, 21, THEME_ACCENT_AMBER, 175);
        dui_circle_outline(cx, cy, 22, THEME_BORDER_SPECULAR, 95);
    }
}

static bool wx_ieq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (wx_lower(a[i]) != wx_lower(b[i])) return false;
        i++;
    }
    return a[i] == b[i];
}

static bool wx_pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void weather_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    NetStatus ns;
    net_get_status(&ns);
    bool online = (ns.nic_present && ns.link_up && ns.dhcp_bound);

    // Auto-fetch once on first open if online
    if (!g_wx_auto_started) {
        g_wx_auto_started = true;
        if (online) wx_start_fetch();
    }

    // Poll non-blocking HTTP summary request
    if (g_wx_http_summary_id >= 0) {
        const HttpTransfer* tr = net_http_get(g_wx_http_summary_id);
        if (!tr) {
            g_wx_http_summary_id = -1;
            g_wx_loading = false;
        } else if (tr->state == HTTP_DONE) {
            g_wx_temp[0] = '\0';
            g_wx_condition[0] = '\0';
            g_wx_feels[0] = '\0';
            g_wx_humidity[0] = '\0';
            g_wx_wind[0] = '\0';
            g_wx_pressure[0] = '\0';
            if (tr->body_buf && tr->bytes_received > 0) {
                wx_parse_summary(tr->body_buf);
                g_wx_has_data = true;
                g_wx_error[0] = '\0';
                wx_start_detail_fetch();
            } else {
                wx_copy(g_wx_error, "Empty response from wttr.in", sizeof(g_wx_error));
            }
            net_http_close(g_wx_http_summary_id);
            g_wx_http_summary_id = -1;
            g_wx_loading = false;
        } else if (tr->state == HTTP_ERROR) {
            wx_copy(g_wx_error, tr->error_msg[0] ? tr->error_msg : "HTTP error reaching wttr.in", sizeof(g_wx_error));
            net_http_close(g_wx_http_summary_id);
            g_wx_http_summary_id = -1;
            g_wx_loading = false;
        }
    }

    // Poll optional detail request
    if (g_wx_http_detail_id >= 0) {
        const HttpTransfer* tr = net_http_get(g_wx_http_detail_id);
        if (!tr) {
            g_wx_http_detail_id = -1;
        } else if (tr->state == HTTP_DONE) {
            if (tr->body_buf && tr->bytes_received > 0) wx_parse_detail(tr->body_buf);
            net_http_close(g_wx_http_detail_id);
            g_wx_http_detail_id = -1;
        } else if (tr->state == HTTP_ERROR) {
            net_http_close(g_wx_http_detail_id);
            g_wx_http_detail_id = -1;
        }
    }

    bool click_avail = (in && in->left_pressed);
    bool pointer_down = (in && in->left_down);
    int mx = in ? in->mx : -1;
    int my = in ? in->my : -1;

    // Keyboard input for city box
    if (in && in->has_key && g_wx_input_focused) {
        int clen = wx_len(g_wx_city_input);
        if (in->key_char == '\n') {
            bool ok = wx_start_fetch();
            play_system_sound(ok ? (char*)"tool" : (char*)"error");
        } else if (in->key_char == '\b') {
            if (clen > 0) g_wx_city_input[clen - 1] = '\0';
        } else if (in->scan_code == BLANK_SCAN_ESC) {
            g_wx_input_focused = false;
            play_system_sound((char*)"close");
        } else if (in->key_char >= 32 && in->key_char <= 126 && clen < 44) {
            g_wx_city_input[clen] = in->key_char;
            g_wx_city_input[clen + 1] = '\0';
        }
    }

    dui_rect(cx, cy, cw, ch, THEME_BG_DEEP, 200);

    int pad = 14;
    int inner_w = cw - pad * 2;

    // Top City Input + Action Bar
    int box_w = inner_w - 210;
    if (box_w < 120) box_w = 120;
    dui_rect_rounded(cx + pad, cy + 12, box_w, 32, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 240);
    dui_rect_rounded_outline(cx + pad, cy + 12, box_w, 32, THEME_RADIUS_MD,
                             g_wx_input_focused ? THEME_BORDER_FOCUS : THEME_BORDER_SUBTLE, 210);
    dui_text_ellipsis(cx + pad + 12, cy + 20, box_w - 24,
                      g_wx_city_input[0] ? g_wx_city_input : "City (blank = Auto IP via wttr.in)",
                      g_wx_city_input[0] ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);

    if (click_avail && wx_pt_in(mx, my, cx + pad, cy + 12, box_w, 32)) {
        click_avail = false;
        g_wx_input_focused = true;
        play_system_sound((char*)"tool");
    }

    if (dui_button_tactile(cx + pad + box_w + 8, cy + 12, 94, 32, g_wx_loading ? "Syncing..." : "Fetch Now",
                           THEME_ACCENT_TEAL, true, mx, my, pointer_down, click_avail)) {
        click_avail = false;
        bool ok = wx_start_fetch();
        play_system_sound(ok ? (char*)"tool" : (char*)"error");
    }

    if (dui_button_tactile(cx + pad + box_w + 108, cy + 12, 98, 32, "Renew DHCP",
                           THEME_SURFACE_ELEVATED, false, mx, my, pointer_down, click_avail)) {
        click_avail = false;
        if (net_dhcp_renew()) {
            wx_start_fetch();
            play_system_sound((char*)"success");
        } else {
            wx_copy(g_wx_error, "Offline - Connect Ethernet/DHCP to fetch live weather", sizeof(g_wx_error));
            play_system_sound((char*)"error");
        }
    }

    // Quick City Presets
    const char* presets[4] = { "Auto", "London", "Tokyo", "New York" };
    int py = cy + 52;
    for (int p = 0; p < 4; p++) {
        int px = cx + pad + p * 92;
        bool active_preset = (p == 0 && !g_wx_city_input[0]) || (p > 0 && wx_ieq(g_wx_city_input, presets[p]));
        if (dui_button_tactile(px, py, 84, 26, presets[p],
                               active_preset ? THEME_ACCENT_TEAL : THEME_SURFACE_CARD,
                               active_preset, mx, my, pointer_down, click_avail)) {
            click_avail = false;
            if (p == 0) g_wx_city_input[0] = '\0';
            else wx_copy(g_wx_city_input, presets[p], sizeof(g_wx_city_input));
            bool ok = wx_start_fetch();
            play_system_sound(ok ? (char*)"tab" : (char*)"error");
        }
    }

    // Main Weather Bento Hero Card
    int hero_y = cy + 88;
    int hero_h = 166;
    dui_bento_card(cx + pad, hero_y, inner_w, hero_h, THEME_RADIUS_CARD, THEME_SURFACE_CARD, 232, g_wx_has_data);
    dui_dot_grid_overlay(cx + pad + 14, hero_y + 12, inner_w - 28, hero_h - 24, 16, THEME_TEXT_PRIMARY, 12);

    if (!online) {
        dui_status_pill(cx + pad + inner_w - 118, hero_y + 14, 102, 22, "Offline", THEME_ACCENT_AMBER, THEME_SURFACE_ELEVATED, false);
        dui_text(cx + pad + 22, hero_y + 28, "Offline - Connect Ethernet/DHCP to fetch live weather", THEME_ACCENT_AMBER, 1);
        dui_text_ellipsis(cx + pad + 22, hero_y + 58, inner_w - 44,
                          ns.nic_present ? "NIC detected, waiting for active link / DHCP IPv4 lease." : "No UEFI SNP network adapter detected.",
                          THEME_TEXT_SECONDARY, 1);
        dui_text_ellipsis(cx + pad + 22, hero_y + 88, inner_w - 44,
                          "Click 'Renew DHCP' then 'Fetch Now' to query http://wttr.in/?format=3", THEME_TEXT_MUTED, 1);
    } else if (g_wx_loading && !g_wx_has_data) {
        dui_status_pill(cx + pad + inner_w - 118, hero_y + 14, 102, 22, "Syncing", THEME_ACCENT_CYAN, THEME_SURFACE_ELEVATED, true);
        dui_text_ellipsis(cx + pad + 22, hero_y + 44, inner_w - 150, "Connecting to wttr.in ...", THEME_ACCENT_TEAL, 2);
        dui_text_ellipsis(cx + pad + 22, hero_y + 84, inner_w - 44, "Resolving DNS and streaming live atmospheric telemetry.", THEME_TEXT_SECONDARY, 1);
    } else if (g_wx_error[0] && !g_wx_has_data) {
        dui_status_pill(cx + pad + inner_w - 118, hero_y + 14, 102, 22, "Error", THEME_ACCENT_ROSE, THEME_SURFACE_ELEVATED, true);
        dui_text_ellipsis(cx + pad + 22, hero_y + 38, inner_w - 150, "Live Weather Request Failed", THEME_ACCENT_ROSE, 2);
        dui_text_ellipsis(cx + pad + 22, hero_y + 78, inner_w - 44, g_wx_error, THEME_TEXT_SECONDARY, 1);
    } else if (g_wx_has_data) {
        dui_status_pill(cx + pad + inner_w - 118, hero_y + 14, 102, 22, "wttr.in Live", THEME_ACCENT_GREEN, THEME_SURFACE_ELEVATED, true);
        dui_bento_card(cx + pad + 18, hero_y + 32, 76, 76, THEME_RADIUS_LG, THEME_SURFACE_ELEVATED, 215, false);
        wx_draw_emblem(cx + pad + 56, hero_y + 70, g_wx_condition[0] ? g_wx_condition : g_wx_summary_line);

        dui_text_ellipsis(cx + pad + 112, hero_y + 20, inner_w - 240,
                          g_wx_location[0] ? g_wx_location : "Live Station", THEME_TEXT_PRIMARY, 2);
        dui_text_ellipsis(cx + pad + 112, hero_y + 54, inner_w - 134,
                          g_wx_temp[0] ? g_wx_temp : g_wx_summary_line, THEME_ACCENT_TEAL, 3);
        dui_text_ellipsis(cx + pad + 112, hero_y + 102, inner_w - 134,
                          g_wx_condition[0] ? g_wx_condition : g_wx_summary_line, THEME_TEXT_SECONDARY, 1);
        dui_text_ellipsis(cx + pad + 112, hero_y + 128, inner_w - 134,
                          g_wx_summary_line, THEME_TEXT_MUTED, 1);
    } else {
        dui_status_pill(cx + pad + inner_w - 118, hero_y + 14, 102, 22, "Ready", THEME_ACCENT_TEAL, THEME_SURFACE_ELEVATED, false);
        dui_text(cx + pad + 22, hero_y + 46, "Atmosphere Ready", THEME_TEXT_PRIMARY, 2);
        dui_text(cx + pad + 22, hero_y + 84, "Click 'Fetch Now' to retrieve live weather from wttr.in.", THEME_TEXT_SECONDARY, 1);
    }

    // Bottom 4 Live Metric Bento Cards (only real parsed values, "--" when absent)
    int met_y = hero_y + hero_h + 12;
    int met_h = (cy + ch - pad) - met_y;
    if (met_h > 106) met_h = 106;
    int card_w = (inner_w - 30) / 4;

    const char* labels[4] = { "Feels Like", "Humidity", "Wind", "Pressure" };
    const uint32_t dot_cols[4] = { THEME_ACCENT_AMBER, THEME_ACCENT_CYAN, THEME_ACCENT_TEAL, THEME_ACCENT_IRIS };
    const char* vals[4] = {
        g_wx_feels[0] ? g_wx_feels : "--",
        g_wx_humidity[0] ? g_wx_humidity : "--",
        g_wx_wind[0] ? g_wx_wind : "--",
        g_wx_pressure[0] ? g_wx_pressure : "--"
    };

    for (int i = 0; i < 4; i++) {
        int mx0 = cx + pad + i * (card_w + 10);
        bool has_val = (g_wx_has_data && vals[i][0] != '-');
        dui_bento_card(mx0, met_y, card_w, met_h, THEME_RADIUS_LG, THEME_SURFACE_BASE, 225, has_val);
        dui_circle(mx0 + 15, met_y + 20, 3, has_val ? dot_cols[i] : THEME_TEXT_MUTED, 235);
        dui_text(mx0 + 24, met_y + 13, labels[i], THEME_TEXT_MUTED, 1);
        dui_text_ellipsis(mx0 + 14, met_y + 44, card_w - 28, vals[i],
                          has_val ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);
    }
}

extern "C" {

void weather_register(void) {
    wm_register_app(APP_WEATHER, "Atmosphere", 620, 440, THEME_ACCENT_TEAL, weather_render, NULL);
}

void launch_weather(EFI_SYSTEM_TABLE* st) {
    (void)st;
    wm_open_app(APP_WEATHER);
}

} // extern "C"
