#include "blankDUI.h"
#include "window_manager.h"
#include "theme.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"
#include "../kernel/net.h"
#include <efi.h>
#include <stdint.h>
#include <stdbool.h>

extern "C" {

// External OS subsystem hooks
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} RTC_Time;

extern void get_rtc_time(RTC_Time* out_time);
extern void os_restart(void);
extern void os_shutdown(void);
extern void os_sleep(void);
extern bool set_screen_resolution(uint32_t target_w, uint32_t target_h);
extern void blankReg_set_int(const char* key, int value);
extern bool blankReg_save(EFI_SYSTEM_TABLE* st);
extern void os_send_notification(const char* title, const char* message);
extern void launch_login_screen(EFI_SYSTEM_TABLE* SystemTable);
extern int vfs_get_file_count(void);
extern const char* vfs_get_file_name(int idx);
extern uint64_t memory_get_used_mb(void);
extern uint64_t memory_get_total_mb(void);
extern EFI_SYSTEM_TABLE* gSystemTable;

// Desktop Shell State Globals
bool g_is_live_cd = false;
extern int g_wallpaper_theme;
bool g_show_desktop_widgets = true;
bool g_show_dynamic_island = true;
bool apple_menu_open = false;
bool island_expanded = false;
bool wifi_menu_open = false;
bool bt_menu_open = false;
bool battery_menu_open = false;
bool rclick_menu_open = false;
int rclick_menu_x = 0;
int rclick_menu_y = 0;

static bool g_brand_menu_open = false;
static bool g_control_center_open = false;
static bool g_downloads_open = false;
static bool g_spotlight_open = false;

static char g_spotlight_query[64] = {0};
static int g_spotlight_len = 0;

// Smooth animation state (0.0 .. 1.0)
static float g_anim_cc = 0.0f;
static float g_anim_spot = 0.0f;
static float g_anim_pulse = 0.0f;
static bool g_topbar_collapsed = false;
static float g_anim_topbar = 1.0f;
static float g_dock_scale[APP_COUNT] = {0};

// 8x8 fallback bitmap font (exported for blankDUI.cpp)
extern const uint8_t font8x8_basic[95][8];
const uint8_t font8x8_basic[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' (32)
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // '!' (33)
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // '"' (34)
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#' (35)
    {0x18,0x7C,0xC2,0x7C,0x06,0xFC,0x18,0x00}, // '$' (36)
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%' (37)
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&' (38)
    {0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, // '\'' (39)
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '(' (40)
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')' (41)
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*' (42)
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+' (43)
    {0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00}, // ',' (44)
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-' (45)
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.' (46)
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/' (47)
    {0x3E,0x62,0x6A,0x72,0x7A,0x62,0x3E,0x00}, // '0' (48)
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1' (49)
    {0x3E,0x62,0x04,0x18,0x20,0x40,0x7E,0x00}, // '2' (50)
    {0x3E,0x62,0x02,0x1C,0x02,0x62,0x3E,0x00}, // '3' (51)
    {0x08,0x18,0x28,0x48,0x7E,0x08,0x08,0x00}, // '4' (52)
    {0x7E,0x40,0x7C,0x02,0x02,0x42,0x3E,0x00}, // '5' (53)
    {0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00}, // '6' (54)
    {0x7E,0x02,0x04,0x08,0x10,0x20,0x20,0x00}, // '7' (55)
    {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00}, // '8' (56)
    {0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00}, // '9' (57)
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':' (58)
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';' (59)
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<' (60)
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // '=' (61)
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // '>' (62)
    {0x3E,0x62,0x02,0x0C,0x18,0x00,0x18,0x00}, // '?' (63)
    {0x3E,0x42,0x9A,0xAA,0xAA,0x9E,0x02,0x3C}, // '@' (64)
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // 'A' (65)
    {0xFC,0x66,0x66,0xFC,0x66,0x66,0xFC,0x00}, // 'B' (66)
    {0x3C,0x62,0x40,0x40,0x40,0x62,0x3C,0x00}, // 'C' (67)
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 'D' (68)
    {0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00}, // 'E' (69)
    {0x7E,0x40,0x40,0x78,0x40,0x40,0x40,0x00}, // 'F' (70)
    {0x3C,0x62,0x40,0x4E,0x46,0x62,0x3C,0x00}, // 'G' (71)
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 'H' (72)
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 'I' (73)
    {0x1E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 'J' (74)
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 'K' (75)
    {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00}, // 'L' (76)
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // 'M' (77)
    {0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00}, // 'N' (78)
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'O' (79)
    {0xFC,0x66,0x66,0xFC,0x40,0x40,0x40,0x00}, // 'P' (80)
    {0x3C,0x66,0x66,0x66,0x6E,0x7C,0x0E,0x01}, // 'Q' (81)
    {0xFC,0x66,0x66,0xFC,0x70,0x68,0x66,0x00}, // 'R' (82)
    {0x3C,0x62,0x30,0x18,0x0C,0x46,0x3C,0x00}, // 'S' (83)
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 'T' (84)
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'U' (85)
    {0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x00}, // 'V' (86)
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 'W' (87)
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 'X' (88)
    {0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x00}, // 'Y' (89)
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 'Z' (90)
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // '[' (91)
    {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00}, // '\\' (92)
    {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, // ']' (93)
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // '^' (94)
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, // '_' (95)
    {0x18,0x18,0x08,0x00,0x00,0x00,0x00,0x00}, // '`' (96)
    {0x00,0x00,0x3C,0x02,0x3E,0x46,0x3E,0x00}, // 'a' (97)
    {0x40,0x40,0x7C,0x46,0x46,0x46,0x7C,0x00}, // 'b' (98)
    {0x00,0x00,0x3E,0x40,0x40,0x42,0x3C,0x00}, // 'c' (99)
    {0x02,0x02,0x3E,0x46,0x46,0x46,0x3E,0x00}, // 'd' (100)
    {0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00}, // 'e' (101)
    {0x1C,0x22,0x20,0x78,0x20,0x20,0x20,0x00}, // 'f' (102)
    {0x00,0x00,0x3E,0x46,0x46,0x3E,0x02,0x3C}, // 'g' (103)
    {0x40,0x40,0x7C,0x46,0x46,0x46,0x46,0x00}, // 'h' (104)
    {0x18,0x00,0x38,0x18,0x18,0x18,0x7E,0x00}, // 'i' (105)
    {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x4C,0x38}, // 'j' (106)
    {0x40,0x40,0x46,0x4C,0x78,0x4C,0x46,0x00}, // 'k' (107)
    {0x38,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 'l' (108)
    {0x00,0x00,0x6C,0x92,0x92,0x92,0x92,0x00}, // 'm' (109)
    {0x00,0x00,0x7C,0x46,0x46,0x46,0x46,0x00}, // 'n' (110)
    {0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00}, // 'o' (111)
    {0x00,0x00,0x7C,0x46,0x46,0x7C,0x40,0x40}, // 'p' (112)
    {0x00,0x00,0x3E,0x46,0x46,0x3E,0x02,0x02}, // 'q' (113)
    {0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00}, // 'r' (114)
    {0x00,0x00,0x3E,0x40,0x3C,0x02,0x7C,0x00}, // 's' (115)
    {0x20,0x20,0x70,0x20,0x20,0x22,0x1C,0x00}, // 't' (116)
    {0x00,0x00,0x46,0x46,0x46,0x46,0x3A,0x00}, // 'u' (117)
    {0x00,0x00,0x46,0x46,0x46,0x2C,0x18,0x00}, // 'v' (118)
    {0x00,0x00,0x81,0x81,0x99,0x99,0x66,0x00}, // 'w' (119)
    {0x00,0x00,0x46,0x28,0x10,0x28,0x46,0x00}, // 'x' (120)
    {0x00,0x00,0x46,0x46,0x46,0x3E,0x02,0x3C}, // 'y' (121)
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 'z' (122)
    {0x0C,0x18,0x18,0x30,0x18,0x18,0x0C,0x00}, // '{' (123)
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|' (124)
    {0x30,0x18,0x18,0x0C,0x18,0x18,0x30,0x00}, // '}' (125)
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  // '~' (126)
};

// Sharp 14x18 arrow cursor with alpha border + inner fill (no transparency punch-through issues)
static const uint8_t g_arrow_bitmap[18][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,0},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0}
};

typedef struct {
    AppId id;
    const char* name;
    const char* short_name;
    const char* keywords;
} DockAppItem;

static const DockAppItem g_dock_items[APP_COUNT] = {
    { APP_FILES,      "Files & Notes",    "Files",    "files finder notes blankpad editor" },
    { APP_BROWSER,    "Horizon Web",      "Browser",  "browser web internet http download surf" },
    { APP_TERMINAL,   "Terminal",         "Term",     "terminal shell bash ping dhcp" },
    { APP_STORE,      "Software Hub",     "Store",    "store apps install pkg repo" },
    { APP_SETTINGS,   "System Settings",  "Settings", "settings display audio wallpaper network" },
    { APP_MONITOR,    "System Pulse",     "Monitor",  "monitor cpu memory task pci activity" },
    { APP_CALCULATOR, "Calculator",       "Calc",     "calc math calculator" },
    { APP_WEATHER,    "Atmosphere",       "Weather",  "weather climate forecast" },
    { APP_REGEDIT,    "Registry Studio",  "Registry", "regedit registry ini config" },
    { APP_UPDATER,    "System Updater",   "Updates",  "updater verify integrity os update" }
};

static bool pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static bool str_icontains(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j]) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static void fmt_two_digits(int val, char* out) {
    out[0] = (char)('0' + ((val / 10) % 10));
    out[1] = (char)('0' + (val % 10));
    out[2] = 0;
}

void blankUI_draw_cursor(int x, int y, int type) {
    (void)type;
    // Soft shadow under cursor
    for (int r = 0; r < 18; r++) {
        for (int c = 0; c < 12; c++) {
            if (g_arrow_bitmap[r][c] != 0) {
                put_pixel_alpha(x + c + 2, y + r + 2, 0x000000, 90);
            }
        }
    }
    // Crisp dark rim + bright cyan-white core
    for (int r = 0; r < 18; r++) {
        for (int c = 0; c < 12; c++) {
            uint8_t px = g_arrow_bitmap[r][c];
            if (px == 1) put_pixel(x + c, y + r, 0x070B14);
            else if (px == 2) put_pixel(x + c, y + r, 0xF8FAFC);
        }
    }
}

void blankUI_draw_toast(const char* title, const char* message) {
    os_send_notification(title, message);
}

void blankUI_draw_desktop_widgets(const BlankInputState* in) {
    if (!g_show_desktop_widgets) return;
    int sw = (int)screen_width;
    int sh = (int)screen_height;

    // Lira-style Desktop Bento Telemetry Card (generous 24px corners + architectural dot lattice)
    int wx = 26;
    int wy = THEME_TOPBAR_H + 34;
    int ww = 286;
    int wh = 152;
    if (sw < 800 || sh < 560) return;

    dui_bento_card(wx, wy, ww, wh, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 198, false);
    dui_dot_grid_overlay(wx + 12, wy + 12, ww - 24, 58, 14, THEME_TEXT_PRIMARY, 14);

    RTC_Time now;
    get_rtc_time(&now);

    char time_buf[16];
    fmt_two_digits(now.hour, time_buf);
    time_buf[2] = ':';
    fmt_two_digits(now.minute, time_buf + 3);
    time_buf[5] = 0;

    NetStatus ns;
    net_get_status(&ns);

    dui_text(wx + 20, wy + 18, time_buf, THEME_TEXT_PRIMARY, 3);
    dui_status_pill(wx + ww - 94, wy + 20, 76, 22,
                    ns.dhcp_bound ? "Online" : "Local",
                    ns.dhcp_bound ? THEME_ACCENT_GREEN : THEME_ACCENT_CYAN,
                    THEME_SURFACE_CARD, ns.dhcp_bound);

    // Subtle hairline separator
    dui_rect(wx + 20, wy + 66, ww - 40, 1, THEME_BORDER_SUBTLE, 140);

    char sub[48];
    int used_mb = (int)memory_get_used_mb();
    int tot_mb = (int)memory_get_total_mb();
    if (tot_mb <= 0) tot_mb = 512;

    int pos = 0;
    const char* pfx = "Memory ";
    while (*pfx) sub[pos++] = *pfx++;
    int tmp = used_mb;
    char digits[12]; int dc = 0;
    if (tmp == 0) digits[dc++] = '0';
    else while (tmp > 0 && dc < 10) { digits[dc++] = (char)('0' + (tmp % 10)); tmp /= 10; }
    for (int k = dc - 1; k >= 0; k--) sub[pos++] = digits[k];
    const char* mid = " MB  |  ";
    while (*mid) sub[pos++] = *mid++;
    const char* ip = ns.dhcp_bound ? ns.ip_str : (ns.nic_present ? "Link Up" : "Offline");
    while (*ip && pos < 45) sub[pos++] = *ip++;
    sub[pos] = 0;

    dui_text(wx + 20, wy + 78, sub, THEME_TEXT_SECONDARY, 1);

    // Tactile bevel pill buttons
    if (in) {
        if (dui_button(wx + 20, wy + 106, 114, 32, "Open Browser", THEME_ACCENT_CYAN, true, in->mx, in->my, in->left_pressed)) {
            wm_open_app(APP_BROWSER);
        }
        if (dui_button(wx + 144, wy + 106, 122, 32, "Search Ctrl+K", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
            g_spotlight_open = true;
            g_spotlight_len = 0;
            g_spotlight_query[0] = 0;
            play_system_sound((char*)"open");
        }
    }
}

static void close_all_popovers(void) {
    g_brand_menu_open = false;
    g_control_center_open = false;
    g_downloads_open = false;
    g_spotlight_open = false;
    rclick_menu_open = false;
    island_expanded = false;
}

void blankUI_draw_shell(BlankInputState* in) {
    if (!in) return;
    dui_set_pointer_down(in->left_down);
    int sw = (int)screen_width;
    int sh = (int)screen_height;

    // Keyboard global shortcuts: Ctrl+K opens Spotlight, Esc closes active overlay
    if (in->has_key) {
        if (in->ctrl_held && in->key_char == 'k') {
            bool was = g_spotlight_open;
            close_all_popovers();
            g_spotlight_open = !was;
            g_spotlight_len = 0;
            g_spotlight_query[0] = 0;
            play_system_sound((char*)(g_spotlight_open ? "open" : "close"));
            input_consume_key(in);
        } else if (in->scan_code == BLANK_SCAN_ESC) {
            if (g_brand_menu_open || g_control_center_open || g_downloads_open || g_spotlight_open || rclick_menu_open || island_expanded) {
                close_all_popovers();
                play_system_sound((char*)"close");
                input_consume_key(in);
            }
        }
    }

    // ------------------------------------------------------------------------
    // 1. Lira-Inspired Centered Collapsible Floating Pill Topbar
    // ------------------------------------------------------------------------
    NetStatus ns;
    net_get_status(&ns);
    int dl_count = net_download_count();
    int active_dl = 0;
    for (int i = 0; i < dl_count; i++) {
        const DownloadEntry* de = net_get_download(i);
        if (de && de->state != HTTP_DONE && de->state != HTTP_ERROR && de->state != HTTP_IDLE) active_dl++;
    }

    RTC_Time now;
    get_rtc_time(&now);
    char clk[16];
    fmt_two_digits(now.hour, clk);
    clk[2] = ':';
    fmt_two_digits(now.minute, clk + 3);
    clk[5] = ':';
    fmt_two_digits(now.second, clk + 6);
    clk[8] = 0;

    int eq_bands[4] = {0, 0, 0, 0};
    audio_get_eq_bands(eq_bands, 4);
    bool eq_live = audio_is_playing();

    g_anim_topbar = horizon_spring_step(g_anim_topbar, g_topbar_collapsed ? 0.0f : 1.0f, 0.34f);
    float tb_ease = horizon_ease_lira(g_anim_topbar);

    int expanded_w = 780;
    if (expanded_w > sw - 28) expanded_w = sw - 28;
    int collapsed_w = 244;
    int cap_w = collapsed_w + (int)((float)(expanded_w - collapsed_w) * tb_ease);
    int bar_h = 40;
    int bar_y = 10;
    int bar_x = (sw - cap_w) / 2;
    int left_x = bar_x + 6; // origin anchor for brand popover

    bool shell_active = g_brand_menu_open || g_control_center_open || g_downloads_open || g_spotlight_open || active_dl > 0;
    dui_shadow(bar_x, bar_y, cap_w, bar_h, THEME_RADIUS_PILL, 7);
    dui_glass_card(bar_x, bar_y, cap_w, bar_h, THEME_RADIUS_PILL, THEME_SURFACE_BASE, 232, shell_active);

    // Brand Orb Pill Trigger (always visible)
    BlankWindow* focused = wm_get_focused_window();
    int brand_w = g_topbar_collapsed ? 118 : 168;
    bool hov_brand = pt_in(in->mx, in->my, left_x, bar_y + 4, brand_w, 32);
    if (hov_brand || g_brand_menu_open) {
        dui_rect_rounded(left_x, bar_y + 4, brand_w, 32, 16, THEME_SURFACE_HOVER, 215);
    }
    dui_circle(left_x + 16, bar_y + 20, 8, THEME_ACCENT_CYAN, 255);
    dui_circle(left_x + 16, bar_y + 20, 4, THEME_BG_VOID, 255);
    dui_text_ellipsis(left_x + 30, bar_y + 12, brand_w - 36,
                      focused ? focused->title : (g_is_live_cd ? "Horizon Live" : "Horizon"),
                      (hov_brand || g_brand_menu_open) ? THEME_ACCENT_CYAN : THEME_TEXT_PRIMARY, 1);

    if (in->left_pressed && hov_brand) {
        bool next = !g_brand_menu_open;
        close_all_popovers();
        g_brand_menu_open = next;
        play_system_sound((char*)(next ? "open" : "close"));
        input_consume_mouse(in);
    }

    // Crisp vertical separator
    int sep_x = left_x + brand_w + 6;
    dui_rect(sep_x, bar_y + 10, 1, 20, THEME_BORDER_SUBTLE, 180);

    // Collapse / Expand Circular Pill Toggle (far right of floating bar)
    int tog_x = bar_x + cap_w - 34;
    int tog_y = bar_y + 6;
    bool hov_tog = pt_in(in->mx, in->my, tog_x, tog_y, 28, 28);
    int tog_dy = (hov_tog && in->left_down) ? 1 : 0;
    dui_rect_rounded(tog_x, tog_y + tog_dy, 28, 28, 14, hov_tog ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 225);
    dui_rect_rounded_outline(tog_x, tog_y + tog_dy, 28, 28, 14, hov_tog ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE, 180);
    // Draw clean '-' or '+' glyph
    dui_rect(tog_x + 9, tog_y + tog_dy + 13, 10, 2, THEME_TEXT_PRIMARY, 240);
    if (g_topbar_collapsed) {
        dui_rect(tog_x + 13, tog_y + tog_dy + 9, 2, 10, THEME_ACCENT_CYAN, 240);
    }
    if (in->left_pressed && hov_tog) {
        g_topbar_collapsed = !g_topbar_collapsed;
        close_all_popovers();
        play_system_sound((char*)(g_topbar_collapsed ? "close" : "open"));
        input_consume_mouse(in);
    }

    if (tb_ease > 0.55f) {
        // Expanded navigation pills
        int cur_x = sep_x + 10;

        // Clock & Link Status Chip
        int clk_w = 104;
        dui_circle(cur_x + 8, bar_y + 20, 4, (active_dl > 0) ? THEME_ACCENT_AMBER : (ns.dhcp_bound ? THEME_ACCENT_GREEN : THEME_ACCENT_CYAN), 255);
        dui_text(cur_x + 18, bar_y + 12, clk, THEME_TEXT_PRIMARY, 1);
        cur_x += clk_w;

        // Vertical hairline before utility pills
        dui_rect(cur_x, bar_y + 10, 1, 20, THEME_BORDER_SUBTLE, 170);
        cur_x += 10;

        // Search pill
        if (dui_icon_pill(cur_x, bar_y + 6, 78, 28, "Find", 0, g_spotlight_open, in->mx, in->my, in->left_pressed)) {
            bool next = !g_spotlight_open;
            close_all_popovers();
            g_spotlight_open = next;
            g_spotlight_len = 0; g_spotlight_query[0] = 0;
            play_system_sound((char*)(next ? "open" : "close"));
            input_consume_mouse(in);
        }
        cur_x += 84;

        // Downloads / Files pill
        if (dui_icon_pill(cur_x, bar_y + 6, 98, 28, (active_dl > 0) ? "Syncing" : "Downloads", 8, g_downloads_open, in->mx, in->my, in->left_pressed)) {
            bool next = !g_downloads_open;
            close_all_popovers();
            g_downloads_open = next;
            play_system_sound((char*)(next ? "open" : "close"));
            input_consume_mouse(in);
        }
        cur_x += 104;

        // Live Lira Sound EQ Pill (click toggles mute cleanly)
        int eq_x = cur_x;
        int eq_w = 56;
        bool hov_eq = pt_in(in->mx, in->my, eq_x, bar_y + 6, eq_w, 28);
        int eq_dy = (hov_eq && in->left_down) ? 1 : 0;
        dui_rect_rounded(eq_x, bar_y + 6 + eq_dy, eq_w, 28, 14,
                         is_audio_muted() ? THEME_SURFACE_INPUT : (hov_eq ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD), 220);
        dui_rect_rounded_outline(eq_x, bar_y + 6 + eq_dy, eq_w, 28, 14,
                                 eq_live ? THEME_ACCENT_CYAN : (hov_eq ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE), 185);
        dui_draw_sys_icon(eq_x + 8, bar_y + 13 + eq_dy, 14, 2, is_audio_muted() ? THEME_ACCENT_ROSE : THEME_TEXT_PRIMARY);
        dui_eq_bars(eq_x + 25, bar_y + 11 + eq_dy, 24, 18, eq_bands, 4, THEME_ACCENT_CYAN, eq_live);
        if (in->left_pressed && hov_eq) {
            toggle_audio_mute();
            input_consume_mouse(in);
        }
        cur_x += eq_w + 6;

        // Control Center trigger pill (fits cleanly up to tog_x - 8)
        int cc_w = (tog_x - 8) - cur_x;
        if (cc_w > 78) cc_w = 104;
        if (cc_w >= 64) {
            if (dui_icon_pill(cur_x, bar_y + 6, cc_w, 28, "Controls", 4, g_control_center_open, in->mx, in->my, in->left_pressed)) {
                bool next = !g_control_center_open;
                close_all_popovers();
                g_control_center_open = next;
                play_system_sound((char*)(next ? "open" : "close"));
                input_consume_mouse(in);
            }
        }
    } else {
        // Collapsed compact view: time + live EQ bars right inside compact pill
        dui_text(sep_x + 10, bar_y + 12, clk, THEME_TEXT_PRIMARY, 1);
        dui_eq_bars(tog_x - 28, bar_y + 11, 20, 18, eq_bands, 4, THEME_ACCENT_CYAN, eq_live);
    }

    // ------------------------------------------------------------------------
    // 2. Floating Lira Capsule Dock (Bottom)
    // ------------------------------------------------------------------------
    int dock_count = APP_COUNT;
    int slot_w = 52;
    int dock_pad_x = 16;
    int dock_w = dock_count * slot_w + dock_pad_x * 2;
    int dock_h = 60;
    int dock_x = (sw - dock_w) / 2;
    int dock_y = sh - dock_h - 10;

    dui_shadow(dock_x, dock_y, dock_w, dock_h, dock_h / 2, 8);
    dui_glass_card(dock_x, dock_y, dock_w, dock_h, dock_h / 2, THEME_SURFACE_BASE, 230, false);

    int hovered_dock = -1;
    for (int i = 0; i < dock_count; i++) {
        int ix = dock_x + dock_pad_x + i * slot_w + 6;
        int iy = dock_y + 9;
        bool hov = pt_in(in->mx, in->my, ix - 4, iy - 10, slot_w - 2, dock_h + 8);
        if (hov) hovered_dock = i;

        g_dock_scale[i] = horizon_spring_step(g_dock_scale[i], hov ? 1.0f : 0.0f, 0.34f);
        int lift = (int)(g_dock_scale[i] * 7.0f);
        int isize = 38 + (int)(g_dock_scale[i] * 5.0f);
        int draw_ix = ix - (isize - 38) / 2;
        int draw_iy = iy - lift;

        dui_draw_squircle_icon(draw_ix, draw_iy, isize, (int)g_dock_items[i].id, hov);

        // Minimalist active dot indicator beneath open/focused app
        if (wm_is_app_open(g_dock_items[i].id)) {
            BlankWindow* w = wm_get_window(g_dock_items[i].id);
            bool is_top = (w && w == wm_get_focused_window());
            dui_circle(ix + 19, dock_y + dock_h - 6, is_top ? 3 : 2,
                       is_top ? THEME_ACCENT_CYAN : THEME_TEXT_SECONDARY, 245);
        }

        if (hov && in->left_pressed) {
            BlankWindow* w = wm_get_window(g_dock_items[i].id);
            if (w && w->open && w == wm_get_focused_window() && !w->minimized) {
                wm_minimize_window(w);
            } else {
                wm_open_app(g_dock_items[i].id);
            }
            input_consume_mouse(in);
        }
    }

    // Pill tooltip above hovered dock item
    if (hovered_dock >= 0) {
        const char* tip = g_dock_items[hovered_dock].name;
        int tw = dui_text_width(tip, 1) + 22;
        int tx = dock_x + dock_pad_x + hovered_dock * slot_w + 26 - tw / 2;
        int ty = dock_y - 34;
        dui_shadow(tx, ty, tw, 26, 13, 5);
        dui_glass_card(tx, ty, tw, 26, 13, THEME_SURFACE_ELEVATED, 244, true);
        dui_text(tx + 11, ty + 6, tip, THEME_TEXT_PRIMARY, 1);
    }

    // ------------------------------------------------------------------------
    // 3. Origin-Aware Bento Popovers & Overlays
    // ------------------------------------------------------------------------

    // A) Brand Menu Dropdown (anchored cleanly under topbar capsule left)
    if (g_brand_menu_open) {
        int mx0 = bar_x;
        int my0 = bar_y + bar_h + 10;
        int mw = 236;
        int mh = 242;
        dui_shadow(mx0, my0, mw, mh, THEME_RADIUS_XL, 9);
        dui_bento_card(mx0, my0, mw, mh, THEME_RADIUS_XL, THEME_SURFACE_CARD, 246, true);

        const char* labels[6] = {
            "About BlankOS Horizon",
            "System Settings",
            "Quick Search (Ctrl+K)",
            "Lock Session",
            "Restart System",
            "Shut Down"
        };
        for (int i = 0; i < 6; i++) {
            int ry = my0 + 12 + i * 36;
            bool hov = pt_in(in->mx, in->my, mx0 + 10, ry, mw - 20, 30);
            if (hov) dui_rect_rounded(mx0 + 10, ry, mw - 20, 30, 10, (i >= 4) ? THEME_ACCENT_ROSE : THEME_SURFACE_HOVER, 190);
            dui_text(mx0 + 20, ry + 7, labels[i], (i == 5) ? THEME_ACCENT_ROSE : THEME_TEXT_PRIMARY, 1);
            if (hov && in->left_pressed) {
                g_brand_menu_open = false;
                input_consume_mouse(in);
                if (i == 0) wm_open_app(APP_MONITOR);
                else if (i == 1) wm_open_app(APP_SETTINGS);
                else if (i == 2) { g_spotlight_open = true; play_system_sound((char*)"open"); }
                else if (i == 3 && gSystemTable) launch_login_screen(gSystemTable);
                else if (i == 4) os_restart();
                else if (i == 5) os_shutdown();
            }
        }
    }

    // B) Bento Control Center Popover
    g_anim_cc = horizon_spring_step(g_anim_cc, g_control_center_open ? 1.0f : 0.0f, 0.34f);
    if (g_control_center_open || g_anim_cc > 0.05f) {
        int cw = 336;
        int ch = 382;
        int cx0 = bar_x + cap_w - cw;
        if (cx0 + cw > sw - 14) cx0 = sw - cw - 14;
        if (cx0 < 14) cx0 = 14;
        int cy0 = bar_y + bar_h + 10 - horizon_enter_offset_y(g_anim_cc, 12);

        dui_shadow(cx0, cy0, cw, ch, THEME_RADIUS_CARD, 10);
        dui_bento_card(cx0, cy0, cw, ch, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 246, true);

        dui_text(cx0 + 20, cy0 + 18, "Control Center", THEME_TEXT_PRIMARY, 2);
        dui_status_pill(cx0 + cw - 92, cy0 + 18, 74, 22,
                        ns.dhcp_bound ? "Online" : "Local",
                        ns.dhcp_bound ? THEME_ACCENT_GREEN : THEME_ACCENT_CYAN,
                        THEME_SURFACE_CARD, ns.dhcp_bound);

        // Network Bento Tile
        dui_bento_card(cx0 + 14, cy0 + 52, cw - 28, 60, THEME_RADIUS_LG, THEME_SURFACE_CARD, 220, false);
        dui_draw_sys_icon(cx0 + 26, cy0 + 72, 20, 1, ns.link_up ? THEME_ACCENT_GREEN : THEME_TEXT_MUTED);
        dui_text_ellipsis(cx0 + 56, cy0 + 62, 162, ns.nic_present ? ns.adapter_name : "No NIC", THEME_TEXT_PRIMARY, 1);
        dui_text(cx0 + 56, cy0 + 82, ns.dhcp_bound ? ns.ip_str : (ns.nic_present ? "No DHCP" : "Offline"), THEME_TEXT_SECONDARY, 1);
        if (dui_button(cx0 + cw - 94, cy0 + 68, 68, 28, "Renew", THEME_ACCENT_CYAN, false, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            net_dhcp_renew();
            play_system_sound((char*)"tool");
            input_consume_mouse(in);
        }

        // Audio + Live EQ Bento Tile
        dui_bento_card(cx0 + 14, cy0 + 122, cw - 28, 70, THEME_RADIUS_LG, THEME_SURFACE_CARD, 220, eq_live);
        dui_text(cx0 + 26, cy0 + 132, is_audio_muted() ? "Audio Output (Muted)" : "Audio Output", THEME_TEXT_PRIMARY, 1);
        dui_eq_bars(cx0 + cw - 118, cy0 + 131, 24, 18, eq_bands, 4, THEME_ACCENT_CYAN, eq_live);
        if (dui_button(cx0 + cw - 86, cy0 + 128, 60, 24, is_audio_muted() ? "Unmute" : "Mute", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            toggle_audio_mute();
            input_consume_mouse(in);
        }
        int sl_x = cx0 + 26, sl_y = cy0 + 166, sl_w = cw - 52;
        dui_rect_rounded(sl_x, sl_y, sl_w, 8, 4, THEME_SURFACE_INPUT, 240);
        int fill_w = (sl_w * get_master_volume()) / 100;
        dui_rect_rounded(sl_x, sl_y, fill_w, 8, 4, THEME_ACCENT_CYAN, 255);
        dui_circle(sl_x + fill_w, sl_y + 4, 7, 0xF8FAFC, 255);
        if (g_control_center_open && in->left_down && pt_in(in->mx, in->my, sl_x - 6, sl_y - 8, sl_w + 12, 24)) {
            int nv = ((in->mx - sl_x) * 100) / sl_w;
            set_master_volume(nv);
        }

        // Wallpaper Swatches (5 Horizon themes)
        dui_text(cx0 + 20, cy0 + 204, "Canvas Theme", THEME_TEXT_SECONDARY, 1);
        const uint32_t theme_swatches[5] = { 0x1E3A8A, 0x065F46, 0x3730A3, 0x7F1D1D, 0x0F766E };
        for (int t = 0; t < 5; t++) {
            int sx = cx0 + 18 + t * 62;
            int sy = cy0 + 226;
            bool act = (dui_get_wallpaper_id() == t);
            dui_rect_rounded(sx, sy, 52, 32, 10, theme_swatches[t], 255);
            dui_rect_rounded_outline(sx, sy, 52, 32, 10, act ? THEME_ACCENT_CYAN : THEME_BORDER_SUBTLE, act ? 245 : 150);
            if (g_control_center_open && in->left_pressed && pt_in(in->mx, in->my, sx, sy, 52, 32)) {
                dui_set_wallpaper_id(t);
                g_wallpaper_theme = t;
                blankReg_set_int("UI/WallpaperTheme", t);
                if (gSystemTable) blankReg_save(gSystemTable);
                play_system_sound((char*)"tool");
                input_consume_mouse(in);
            }
        }

        // Display Mode + Power Tactile Pills
        dui_text(cx0 + 20, cy0 + 270, "Display Resolution", THEME_TEXT_SECONDARY, 1);
        if (dui_button(cx0 + 18, cy0 + 290, 144, 30, "1024 x 768", (screen_width == 1024) ? THEME_ACCENT_CYAN : THEME_SURFACE_ELEVATED, screen_width == 1024, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            set_screen_resolution(1024, 768);
            play_system_sound((char*)"tool");
            input_consume_mouse(in);
        }
        if (dui_button(cx0 + 174, cy0 + 290, 144, 30, "1280 x 800", (screen_width == 1280) ? THEME_ACCENT_CYAN : THEME_SURFACE_ELEVATED, screen_width == 1280, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            set_screen_resolution(1280, 800);
            play_system_sound((char*)"tool");
            input_consume_mouse(in);
        }

        if (dui_button(cx0 + 18, cy0 + 334, 94, 32, "Lock", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            g_control_center_open = false;
            play_system_sound((char*)"lock");
            if (gSystemTable) launch_login_screen(gSystemTable);
            input_consume_mouse(in);
        }
        if (dui_button(cx0 + 120, cy0 + 334, 94, 32, "Restart", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            os_restart();
        }
        if (dui_button(cx0 + 222, cy0 + 334, 96, 32, "Power Off", THEME_ACCENT_ROSE, true, in->mx, in->my, in->left_pressed && g_control_center_open)) {
            os_shutdown();
        }
    }

    // C) Downloads Popover
    if (g_downloads_open) {
        int dw = 340;
        int dh = 288;
        int dx0 = bar_x + cap_w - dw - 24;
        if (dx0 + dw > sw - 14) dx0 = sw - dw - 14;
        if (dx0 < 14) dx0 = 14;
        int dy0 = bar_y + bar_h + 10;

        dui_shadow(dx0, dy0, dw, dh, THEME_RADIUS_CARD, 9);
        dui_bento_card(dx0, dy0, dw, dh, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 246, true);
        dui_text(dx0 + 18, dy0 + 16, "Downloads (\\DOWNLOADS\\)", THEME_TEXT_PRIMARY, 1);

        if (dui_button(dx0 + dw - 78, dy0 + 12, 62, 24, "Clear", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
            net_clear_finished_downloads();
            play_system_sound((char*)"tool");
            input_consume_mouse(in);
        }

        if (dl_count == 0) {
            dui_text(dx0 + 20, dy0 + 102, "No downloaded files yet.", THEME_TEXT_SECONDARY, 1);
            if (dui_button(dx0 + 20, dy0 + 134, 168, 32, "Open Browser", THEME_ACCENT_CYAN, true, in->mx, in->my, in->left_pressed)) {
                g_downloads_open = false;
                wm_open_app(APP_BROWSER);
                input_consume_mouse(in);
            }
        } else {
            int max_show = (dl_count > 4) ? 4 : dl_count;
            for (int i = 0; i < max_show; i++) {
                const DownloadEntry* de = net_get_download(i);
                if (!de) continue;
                int ry = dy0 + 52 + i * 54;
                bool hov = pt_in(in->mx, in->my, dx0 + 12, ry, dw - 24, 46);
                dui_bento_card(dx0 + 12, ry, dw - 24, 46, 12, hov ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 225, hov);
                dui_text_ellipsis(dx0 + 22, ry + 8, dw - 100, de->filename, THEME_TEXT_PRIMARY, 1);
                dui_text_ellipsis(dx0 + 22, ry + 26, dw - 100, de->status_text, THEME_TEXT_SECONDARY, 1);
                if (hov && in->left_pressed && de->state == HTTP_DONE) {
                    g_downloads_open = false;
                    wm_request_open_file(de->disk_path);
                    input_consume_mouse(in);
                }
            }
        }
    }

    // D) Spotlight Command Palette Overlay (24px Bento Modal)
    g_anim_spot = horizon_spring_step(g_anim_spot, g_spotlight_open ? 1.0f : 0.0f, 0.34f);
    if (g_spotlight_open) {
        dui_rect(0, 0, sw, sh, 0x04070E, 140);

        int spw = 516;
        if (spw > sw - 40) spw = sw - 40;
        int sph = 342;
        int spx = (sw - spw) / 2;
        int spy = (sh - sph) / 2 - 40 - horizon_enter_offset_y(g_anim_spot, 10);

        dui_shadow(spx, spy, spw, sph, THEME_RADIUS_CARD, 12);
        dui_bento_card(spx, spy, spw, sph, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 248, true);

        // Tactile Search Input Pill
        dui_rect_rounded(spx + 18, spy + 18, spw - 36, 46, THEME_RADIUS_LG, THEME_SURFACE_INPUT, 245);
        dui_rect_rounded_outline(spx + 18, spy + 18, spw - 36, 46, THEME_RADIUS_LG, THEME_BORDER_FOCUS, 200);
        dui_draw_sys_icon(spx + 32, spy + 32, 18, 0, THEME_ACCENT_CYAN);

        if (g_spotlight_len > 0) {
            dui_text(spx + 58, spy + 32, g_spotlight_query, THEME_TEXT_PRIMARY, 1);
        } else {
            dui_text(spx + 58, spy + 32, "Type to launch apps or open URL (http://...)", THEME_TEXT_MUTED, 1);
        }

        // Capture keyboard typing cleanly inside Spotlight
        if (in->has_key) {
            if (in->key_char == '\b') {
                if (g_spotlight_len > 0) {
                    g_spotlight_query[--g_spotlight_len] = 0;
                }
            } else if (in->key_char == '\n') {
                if (str_icontains(g_spotlight_query, "http://") || str_icontains(g_spotlight_query, "blank://")) {
                    g_spotlight_open = false;
                    wm_request_open_url(g_spotlight_query);
                } else {
                    for (int i = 0; i < APP_COUNT; i++) {
                        if (str_icontains(g_dock_items[i].name, g_spotlight_query) ||
                            str_icontains(g_dock_items[i].keywords, g_spotlight_query)) {
                            g_spotlight_open = false;
                            wm_open_app(g_dock_items[i].id);
                            break;
                        }
                    }
                }
            } else if (in->key_char >= 32 && in->key_char < 127 && g_spotlight_len < 60) {
                g_spotlight_query[g_spotlight_len++] = in->key_char;
                g_spotlight_query[g_spotlight_len] = 0;
                play_system_sound((char*)"tool");
            }
            input_consume_key(in);
        }

        // Matching apps list
        int shown = 0;
        for (int i = 0; i < APP_COUNT && shown < 5; i++) {
            if (str_icontains(g_dock_items[i].name, g_spotlight_query) ||
                str_icontains(g_dock_items[i].keywords, g_spotlight_query)) {
                int ry = spy + 78 + shown * 48;
                bool hov = pt_in(in->mx, in->my, spx + 18, ry, spw - 36, 42);
                dui_rect_rounded(spx + 18, ry, spw - 36, 42, THEME_RADIUS_MD, hov ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 215);
                dui_draw_squircle_icon(spx + 26, ry + 7, 28, (int)g_dock_items[i].id, false);
                dui_text(spx + 64, ry + 13, g_dock_items[i].name, THEME_TEXT_PRIMARY, 1);
                if (hov && in->left_pressed) {
                    g_spotlight_open = false;
                    wm_open_app(g_dock_items[i].id);
                    input_consume_mouse(in);
                }
                shown++;
            }
        }

        // Click outside Spotlight dismisses it cleanly
        if (in->left_pressed && !pt_in(in->mx, in->my, spx, spy, spw, sph)) {
            g_spotlight_open = false;
            play_system_sound((char*)"close");
            input_consume_mouse(in);
        }
    }

    // E) Desktop Right-Click Context Menu
    if (in->right_pressed && !wm_get_focused_window()) {
        close_all_popovers();
        rclick_menu_open = true;
        rclick_menu_x = in->mx;
        rclick_menu_y = in->my;
        if (rclick_menu_x + 200 > sw - 12) rclick_menu_x = sw - 212;
        if (rclick_menu_y + 164 > sh - 80) rclick_menu_y = sh - 244;
        play_system_sound((char*)"open");
    }

    if (rclick_menu_open) {
        int mw = 200, mh = 156;
        dui_shadow(rclick_menu_x, rclick_menu_y, mw, mh, THEME_RADIUS_LG, 8);
        dui_bento_card(rclick_menu_x, rclick_menu_y, mw, mh, THEME_RADIUS_LG, THEME_SURFACE_CARD, 246, true);
        const char* items[4] = { "New Terminal", "Open Horizon Web", "Next Wallpaper", "Display Settings" };
        for (int i = 0; i < 4; i++) {
            int ry = rclick_menu_y + 10 + i * 34;
            bool hov = pt_in(in->mx, in->my, rclick_menu_x + 8, ry, mw - 16, 30);
            if (hov) dui_rect_rounded(rclick_menu_x + 8, ry, mw - 16, 30, 8, THEME_SURFACE_HOVER, 210);
            dui_text(rclick_menu_x + 16, ry + 7, items[i], THEME_TEXT_PRIMARY, 1);
            if (hov && in->left_pressed) {
                rclick_menu_open = false;
                input_consume_mouse(in);
                if (i == 0) wm_open_app(APP_TERMINAL);
                else if (i == 1) wm_open_app(APP_BROWSER);
                else if (i == 2) {
                    int nt = (dui_get_wallpaper_id() + 1) % 5;
                    dui_set_wallpaper_id(nt);
                    g_wallpaper_theme = nt;
                    play_system_sound((char*)"tool");
                } else if (i == 3) wm_open_app(APP_SETTINGS);
            }
        }
        if (in->left_pressed && !pt_in(in->mx, in->my, rclick_menu_x, rclick_menu_y, mw, mh)) {
            rclick_menu_open = false;
        }
    }
}

// Legacy compatibility stubs (in case external callers still reference them)
void blankUI_set_cursor_type(int type) { (void)type; }
void blankUI_draw_menubar(void) {}
void blankUI_draw_dock(void) {}
int blankUI_hit_test_dock(int cx, int cy) { (void)cx; (void)cy; return -1; }

} // extern "C"
