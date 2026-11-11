#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern "C" {

extern volatile uint32_t* framebuffer;
extern int screen_width;
extern int screen_height;
extern int pixels_per_scanline;

extern void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha);
extern void draw_rect_filled(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
extern void draw_rect_rounded(int x, int y, int w, int h, int r, uint32_t color, uint8_t alpha);
extern void draw_circle_filled(int cx, int cy, int r, uint32_t color);
extern void draw_frosted_glass_rounded(int x, int y, int w, int h, int r, uint32_t tint_color, uint8_t tint_alpha);

extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
extern int dui_text_width(const char* text, int scale);
extern int dui_text_height(int scale);
extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);

extern void dui_icon_folder(int x, int y, int size);
extern void dui_icon_gear(int x, int y, int size);
extern void dui_icon_globe(int x, int y, int size);
extern void dui_icon_store(int x, int y, int size);
extern void dui_icon_terminal(int x, int y, int size);
extern void dui_icon_trash(int x, int y, int size);
extern void dui_icon_calculator(int x, int y, int size);
extern void dui_icon_weather(int x, int y, int size);
extern void dui_icon_updater(int x, int y, int size);
extern bool g_is_live_cd;

extern uint64_t memory_get_total_mb(void);
extern uint64_t memory_get_used_mb(void);
extern int pci_get_device_count(void);
extern const char* net_get_adapter_name(void);
extern const char* net_get_ipv4_str(void);
extern const char* net_get_mac_str(void);
extern int battery_get_percentage(void);

static bool reduce_motion_enabled = false;
int last_mouse_x = 0;
int last_mouse_y = 0;
static float dock_icon_scales[10] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

bool g_show_desktop_widgets = true;
bool g_show_dynamic_island = true;
bool apple_menu_open = false;
bool island_expanded = false;

static void ui_u64_to_str(uint64_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[24];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// Basic 8x8 bitmap font (subset for GUI text)
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

void blankUI_draw_char(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    for (int row = 0; row < 8; row++) {
        uint8_t byte = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            if (byte & (0x80 >> col)) {
                put_pixel_alpha(x + col, y + row, color, 255);
            }
        }
    }
}

void blankUI_draw_text(int x, int y, char* text) {
    if (!text) return;
    int cur_x = x;
    while (*text) {
        blankUI_draw_char(cur_x, y, *text, 0x000000);
        cur_x += 8;
        text++;
    }
}

void blankUI_draw_text_color(int x, int y, char* text, uint32_t color) {
    dui_text(x, y, text, color, 1);
}

void init_blankUI_toolkit(void) {
    reduce_motion_enabled = false;
}

void blankUI_set_reduce_motion(bool enabled) {
    reduce_motion_enabled = enabled;
}

void blankUI_animate_fade_in(void* component) { (void)component; }
void blankUI_animate_slide_up(void* component) { (void)component; }

void blankUI_draw_button(int x, int y, int width, int height, char* text) {
    draw_rect_rounded(x, y, width, height, 16, 0x18171c, 255);
    draw_rect_rounded(x, y, width, 1, 16, 0xFFFFFF, 40);

    int text_w = dui_text_width(text, 1);
    int text_h = dui_text_height(1);
    int text_x = x + (width - text_w) / 2;
    int text_y = y + (height - text_h) / 2;
    blankUI_draw_text_color(text_x, text_y, text, 0xFFFFFF);
}

extern bool wifi_connected;
extern char connected_ssid[32];
extern int wifi_signal_strength;
extern bool bluetooth_connected;
extern char paired_device_name[32];
extern bool bluetooth_enabled;

struct RTC_Time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
};
extern void get_rtc_time(RTC_Time *time);

bool wifi_menu_open = false;
bool bt_menu_open = false;
bool battery_menu_open = false;
bool rclick_menu_open = false;
int rclick_menu_x = 0;
int rclick_menu_y = 0;

static void format_time_str(char* buf) {
    RTC_Time t;
    get_rtc_time(&t);
    int h = t.hour;
    int m = t.minute;
    buf[0] = '0' + (h / 10);
    buf[1] = '0' + (h % 10);
    buf[2] = ':';
    buf[3] = '0' + (m / 10);
    buf[4] = '0' + (m % 10);
    buf[5] = '\0';
}

static void format_full_clock(char* time_buf, char* date_buf) {
    RTC_Time t;
    get_rtc_time(&t);
    time_buf[0] = '0' + (t.hour / 10);
    time_buf[1] = '0' + (t.hour % 10);
    time_buf[2] = ':';
    time_buf[3] = '0' + (t.minute / 10);
    time_buf[4] = '0' + (t.minute % 10);
    time_buf[5] = ':';
    time_buf[6] = '0' + (t.second / 10);
    time_buf[7] = '0' + (t.second % 10);
    time_buf[8] = '\0';

    uint32_t y = t.year ? t.year : 2026;
    date_buf[0] = '0' + ((y / 1000) % 10);
    date_buf[1] = '0' + ((y / 100) % 10);
    date_buf[2] = '0' + ((y / 10) % 10);
    date_buf[3] = '0' + (y % 10);
    date_buf[4] = '-';
    date_buf[5] = '0' + (t.month / 10);
    date_buf[6] = '0' + (t.month % 10);
    date_buf[7] = '-';
    date_buf[8] = '0' + (t.day / 10);
    date_buf[9] = '0' + (t.day % 10);
    date_buf[10] = '\0';
}

// Desktop Glass Widgets (macOS Sonoma/Sequoia Widget Layer with Hardware Telemetry Twist)
void blankUI_draw_desktop_widgets() {
    if (!g_show_desktop_widgets) return;

    int wx = screen_width - 268;
    int wy = 44;

    // Widget 1: Live Hardware RTC Clock & Date Glass Card
    dui_shadow(wx, wy, 248, 92, 16, 8, 0x000000, 60);
    dui_rect_rounded(wx, wy, 248, 92, 16, 0x1E1E2E, 200);
    dui_rect_rounded_outline(wx, wy, 248, 92, 16, 0x585B70, 1);

    char t_str[12], d_str[16];
    format_full_clock(t_str, d_str);
    dui_text(wx + 18, wy + 14, t_str, 0xFFFFFF, 2);
    dui_text(wx + 18, wy + 48, d_str, 0x89B4FA, 1);
    dui_text(wx + 18, wy + 68, g_is_live_cd ? "MODE: LIVE ISO9660" : "MODE: DISK FAT32", g_is_live_cd ? 0xFAB387 : 0xA6E3A1, 1);

    // Widget 2: Bare-Metal Hardware Telemetry Card
    int wy2 = wy + 106;
    dui_shadow(wx, wy2, 248, 124, 16, 8, 0x000000, 60);
    dui_rect_rounded(wx, wy2, 248, 124, 16, 0x1E1E2E, 200);
    dui_rect_rounded_outline(wx, wy2, 248, 124, 16, 0x585B70, 1);

    dui_text(wx + 16, wy2 + 12, "SYSTEM TELEMETRY", 0xCBA6F7, 1);

    char ram_line[48];
    int p = 0;
    const char* r1 = "RAM: ";
    while (*r1) ram_line[p++] = *r1++;
    char umb[12], tmb[12];
    ui_u64_to_str(memory_get_used_mb(), umb);
    ui_u64_to_str(memory_get_total_mb(), tmb);
    int k = 0; while (umb[k]) ram_line[p++] = umb[k++];
    ram_line[p++] = '/';
    k = 0; while (tmb[k]) ram_line[p++] = tmb[k++];
    const char* r2 = " MB";
    while (*r2) ram_line[p++] = *r2++;
    ram_line[p] = '\0';
    dui_text(wx + 16, wy2 + 36, ram_line, 0xFFFFFF, 1);

    char pci_line[48];
    p = 0;
    const char* p1 = "PCIe Devices: ";
    while (*p1) pci_line[p++] = *p1++;
    char pcnt[12];
    ui_u64_to_str((uint64_t)pci_get_device_count(), pcnt);
    k = 0; while (pcnt[k]) pci_line[p++] = pcnt[k++];
    const char* p2 = " Bound";
    while (*p2) pci_line[p++] = *p2++;
    pci_line[p] = '\0';
    dui_text(wx + 16, wy2 + 58, pci_line, 0xA6E3A1, 1);

    char net_line[48];
    p = 0;
    const char* n1 = "IP: ";
    while (*n1) net_line[p++] = *n1++;
    const char* ip = net_get_ipv4_str();
    while (*ip) net_line[p++] = *ip++;
    net_line[p] = '\0';
    dui_text(wx + 16, wy2 + 80, net_line, 0x89DCEB, 1);
    dui_text(wx + 16, wy2 + 100, "F1:Set F2:Term F6:Files F7:Mon", 0xA6ADC8, 1);
}

void blankUI_draw_menubar() {
    // Draw Desktop Widgets underneath menubar dropdowns so they appear on the desktop!
    blankUI_draw_desktop_widgets();

    // Top bar, frosted glass look
    draw_frosted_glass_rounded(0, 0, screen_width, 26, 0, 0xFFFFFF, 210);
    draw_rect_filled(0, 26, screen_width, 1, 0x000000, 25);

    // Left side items (macOS style menu items)
    dui_rect_rounded(8, 3, 86, 20, 6, apple_menu_open ? 0x007AFF : 0x1E1E2E, apple_menu_open ? 255 : 25);
    blankUI_draw_text_color(14, 6, (char*)"* BlankOS", apple_menu_open ? 0xFFFFFF : 0x000000);
    blankUI_draw_text_color(106, 6, (char*)"Files", 0x1C1C1E);
    blankUI_draw_text_color(156, 6, (char*)"Monitor", 0x1C1C1E);
    blankUI_draw_text_color(224, 6, (char*)"Theme", 0x1C1C1E);
    blankUI_draw_text_color(278, 6, (char*)"Terminal", 0x1C1C1E);

    // Center Twist: BlankIsland Dynamic Capsule!
    if (g_show_dynamic_island) {
        int isl_w = 240;
        int isl_x = (screen_width - isl_w) / 2;
        dui_rect_rounded(isl_x, 2, isl_w, 22, 11, 0x11111B, 245);
        dui_circle(isl_x + 14, 13, 4, 0x34C759, 255); // Live pulse LED

        char isl_txt[48];
        int p = 0;
        const char* s1 = "RAM ";
        while (*s1) isl_txt[p++] = *s1++;
        char umb[12], pcnt[12];
        ui_u64_to_str(memory_get_used_mb(), umb);
        ui_u64_to_str((uint64_t)pci_get_device_count(), pcnt);
        int k = 0; while (umb[k]) isl_txt[p++] = umb[k++];
        const char* s2 = "M | PCI ";
        while (*s2) isl_txt[p++] = *s2++;
        k = 0; while (pcnt[k]) isl_txt[p++] = pcnt[k++];
        const char* s3 = " | Hub";
        while (*s3) isl_txt[p++] = *s3++;
        isl_txt[p] = '\0';

        blankUI_draw_text_color(isl_x + 26, 6, isl_txt, 0xCDD6F4);

        if (island_expanded) {
            int ex_w = 340;
            int ex_h = 128;
            int ex_x = (screen_width - ex_w) / 2;
            int ex_y = 28;
            dui_shadow(ex_x, ex_y, ex_w, ex_h, 16, 8, 0x000000, 80);
            dui_rect_rounded(ex_x, ex_y, ex_w, ex_h, 16, 0x11111B, 245);
            dui_rect_rounded_outline(ex_x, ex_y, ex_w, ex_h, 16, 0x007AFF, 1);

            blankUI_draw_text_color(ex_x + 16, ex_y + 12, (char*)"BlankIsland Quick Command Center", 0x89B4FA);
            blankUI_draw_text_color(ex_x + 16, ex_y + 34, (char*)net_get_adapter_name(), 0xA6E3A1);

            // 4 Quick Pill Buttons inside expanded Dynamic Island
            dui_rect_rounded(ex_x + 16, ex_y + 60, 148, 26, 8, 0x007AFF, 255);
            blankUI_draw_text_color(ex_x + 30, ex_y + 66, (char*)"Files & BlankPad", 0xFFFFFF);

            dui_rect_rounded(ex_x + 176, ex_y + 60, 148, 26, 8, 0x34C759, 255);
            blankUI_draw_text_color(ex_x + 190, ex_y + 66, (char*)"Activity Monitor", 0xFFFFFF);

            dui_rect_rounded(ex_x + 16, ex_y + 92, 148, 26, 8, 0x5856D6, 255);
            blankUI_draw_text_color(ex_x + 32, ex_y + 98, (char*)"Registry Studio", 0xFFFFFF);

            dui_rect_rounded(ex_x + 176, ex_y + 92, 148, 26, 8, 0xFF9500, 255);
            blankUI_draw_text_color(ex_x + 198, ex_y + 98, (char*)"Cycle Theme", 0xFFFFFF);
        }
    }

    // Right side items - Clock
    char time_str[8];
    format_time_str(time_str);
    blankUI_draw_text_color(screen_width - 68, 6, time_str, 0x000000);

    // Battery widget
    int bx = screen_width - 110;
    draw_rect_rounded(bx, 8, 20, 10, 2, 0x555555, 255);
    draw_rect_filled(bx + 20, 11, 2, 4, 0x555555, 255);
    draw_rect_filled(bx + 2, 10, 16, 6, 0x34C759, 255);

    // Wi-Fi / Ethernet widget
    int wx = screen_width - 150;
    for (int b = 0; b < 4; b++) {
        uint32_t bar_color = 0xCCCCCC;
        if (wifi_connected && wifi_signal_strength > b) bar_color = 0x007AFF;
        draw_rect_filled(wx + b * 5, 18 - b * 3, 3, b * 3 + 2, bar_color, 255);
    }

    // Bluetooth / USB widget
    int btx = screen_width - 180;
    uint32_t bt_color = bluetooth_connected ? 0x007AFF : 0x8E8E93;
    dui_line(btx + 4, 6, btx + 4, 18, bt_color, 1);
    dui_line(btx + 4, 6, btx + 8, 9, bt_color, 1);
    dui_line(btx + 8, 9, btx + 4, 12, bt_color, 1);
    dui_line(btx + 4, 12, btx + 8, 15, bt_color, 1);
    dui_line(btx + 8, 15, btx + 4, 18, bt_color, 1);

    // Apple / BlankOS System Menu Dropdown
    if (apple_menu_open) {
        int mx = 8, my = 28, mw = 220, mh = 216;
        dui_shadow(mx, my, mw, mh, 12, 6, 0x000000, 50);
        draw_rect_rounded(mx, my, mw, mh, 12, 0xFFFFFF, 245);
        dui_rect_rounded_outline(mx, my, mw, mh, 12, 0xD1D1D6, 1);

        blankUI_draw_text_color(mx + 14, my + 10, (char*)"About BlankBook Pro...", 0x111111);
        blankUI_draw_text_color(mx + 14, my + 36, (char*)"System Settings...", 0x111111);
        blankUI_draw_text_color(mx + 14, my + 62, (char*)"App Store...", 0x111111);
        draw_rect_filled(mx + 12, my + 84, mw - 24, 1, 0x000000, 25);
        blankUI_draw_text_color(mx + 14, my + 92, (char*)"Activity Monitor...", 0x007AFF);
        blankUI_draw_text_color(mx + 14, my + 118, (char*)"Registry Studio...", 0x5856D6);
        blankUI_draw_text_color(mx + 14, my + 144, (char*)"Files & BlankPad...", 0x111111);
        draw_rect_filled(mx + 12, my + 166, mw - 24, 1, 0x000000, 25);
        blankUI_draw_text_color(mx + 14, my + 172, (char*)"Restart System", 0xFF9500);
        blankUI_draw_text_color(mx + 14, my + 194, (char*)"Shut Down System", 0xFF3B30);
    }

    // Network Dropdown
    if (wifi_menu_open) {
        int mx = screen_width - 250;
        dui_shadow(mx, 28, 240, 170, 12, 6, 0x000000, 45);
        draw_rect_rounded(mx, 28, 240, 170, 12, 0xFFFFFF, 245);
        blankUI_draw_text_color(mx + 14, 38, (char*)"Network & PCIe Link", 0x111111);
        draw_rect_filled(mx + 14, 56, 212, 1, 0x000000, 20);
        blankUI_draw_text_color(mx + 14, 64, (char*)net_get_adapter_name(), 0x007AFF);
        blankUI_draw_text_color(mx + 14, 84, (char*)net_get_mac_str(), 0x666666);
        blankUI_draw_text_color(mx + 14, 112, (char*)"1. Intel e1000 GbE Link", 0x333333);
        blankUI_draw_text_color(mx + 14, 134, (char*)"2. BlankOS_5G_Mesh", 0x333333);
        blankUI_draw_text_color(mx + 14, 156, (char*)net_get_ipv4_str(), 0x248A3D);
    }

    if (bt_menu_open) {
        int mx = screen_width - 240;
        dui_shadow(mx, 28, 230, 140, 12, 6, 0x000000, 45);
        draw_rect_rounded(mx, 28, 230, 140, 12, 0xFFFFFF, 245);
        blankUI_draw_text_color(mx + 16, 40, (char*)"USB HID & Bluetooth", 0x333333);
        draw_rect_filled(mx + 16, 56, 198, 1, 0x000000, 20);
        blankUI_draw_text_color(mx + 16, 68, paired_device_name, 0x007AFF);
        blankUI_draw_text_color(mx + 16, 94, (char*)"1. AirPods Pro (USB-C)", 0x333333);
        blankUI_draw_text_color(mx + 16, 114, (char*)"2. Magic Trackpad 3", 0x333333);
    }

    if (battery_menu_open) {
        int mx = screen_width - 190;
        dui_shadow(mx, 28, 180, 96, 12, 6, 0x000000, 45);
        draw_rect_rounded(mx, 28, 180, 96, 12, 0xFFFFFF, 245);
        blankUI_draw_text_color(mx + 14, 40, (char*)"Power: AC / High Perf", 0x248A3D);
        blankUI_draw_text_color(mx + 14, 62, (char*)"ACPI / UEFI Runtime", 0x333333);
        blankUI_draw_text_color(mx + 14, 84, (char*)"Watchdog: Disabled", 0x8E8E93);
    }

    // Desktop Right-Click Context Menu
    if (rclick_menu_open) {
        int mx = rclick_menu_x;
        int my = rclick_menu_y;
        int mw = 210, mh = 186;
        if (mx + mw > screen_width) mx = screen_width - mw - 8;
        if (my + mh > screen_height) my = screen_height - mh - 8;

        dui_shadow(mx, my, mw, mh, 10, 6, 0x000000, 50);
        draw_rect_rounded(mx, my, mw, mh, 10, 0xFFFFFF, 245);
        dui_rect_rounded_outline(mx, my, mw, mh, 10, 0xD1D1D6, 1);

        blankUI_draw_text_color(mx + 14, my + 10, (char*)"Files & BlankPad", 0x111111);
        blankUI_draw_text_color(mx + 14, my + 36, (char*)"Activity Monitor", 0x007AFF);
        blankUI_draw_text_color(mx + 14, my + 62, (char*)"Terminal (blanksh)", 0x111111);
        blankUI_draw_text_color(mx + 14, my + 88, (char*)"Cycle Wallpaper Theme", 0x5856D6);
        blankUI_draw_text_color(mx + 14, my + 114, (char*)"Toggle Glass Widgets", 0x248A3D);
        blankUI_draw_text_color(mx + 14, my + 140, (char*)"System Settings...", 0x333333);
        blankUI_draw_text_color(mx + 14, my + 164, (char*)"Trigger Kernel Panic", 0xFF3B30);
    }
}

void blankUI_draw_dock() {
    int icon_size = 48;
    int spacing = 10;
    int num_icons = 10;
    // 0: Files/BlankPad, 1: Monitor, 2: Settings, 3: Browser, 4: Store, 5: Terminal, 6: Calc, 7: Weather, 8: Update/Install, 9: RegEdit
    int dock_content_w = num_icons * icon_size + (num_icons - 1) * spacing + 36;
    int dock_w = dock_content_w;
    int dock_h = 66;
    int dock_x = (screen_width - dock_w) / 2;
    int dock_y = screen_height - dock_h - 8;

    dui_shadow(dock_x, dock_y, dock_w, dock_h, 20, 8, 0x000000, 65);
    draw_frosted_glass_rounded(dock_x, dock_y, dock_w, dock_h, 20, 0xFFFFFF, 190);
    draw_rect_rounded(dock_x, dock_y, dock_w, dock_h, 20, 0xFFFFFF, 95);

    int start_x = dock_x + 16;
    int start_y = dock_y + 8;

    for (int i = 0; i < num_icons; i++) {
        int base_x = start_x + (icon_size + spacing) * i;
        if (i == 9) base_x += 6;

        float target = 1.0f;
        if (last_mouse_x >= base_x && last_mouse_x <= base_x + icon_size &&
            last_mouse_y >= start_y && last_mouse_y <= start_y + icon_size) {
            target = 1.25f;
        }
        dock_icon_scales[i] += (target - dock_icon_scales[i]) * 0.25f;

        int s = (int)(icon_size * dock_icon_scales[i]);
        int cx = base_x - (s - icon_size) / 2;
        int cy = start_y - (s - icon_size);

        if (i == 0) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x007AFF, 255);
            dui_icon_folder(cx, cy, s);
            blankUI_draw_text_color(base_x + 6, start_y + icon_size + 1, (char*)"Files", 0x222222);
        } else if (i == 1) {
            // Activity Monitor icon (dark telemetry pulse tile)
            draw_rect_rounded(cx, cy, s, s, 12, 0x11111B, 255);
            dui_line(cx + 6, cy + s/2, cx + s/3, cy + s/2, 0x34C759, 2);
            dui_line(cx + s/3, cy + s/2, cx + s/2, cy + s/4, 0x34C759, 2);
            dui_line(cx + s/2, cy + s/4, cx + s*2/3, cy + s*3/4, 0x34C759, 2);
            dui_line(cx + s*2/3, cy + s*3/4, cx + s - 6, cy + s/2, 0x34C759, 2);
            blankUI_draw_text_color(base_x - 2, start_y + icon_size + 1, (char*)"Monitor", 0x222222);
        } else if (i == 2) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x8E8E93, 255);
            dui_icon_gear(cx, cy, s);
            blankUI_draw_text_color(base_x - 6, start_y + icon_size + 1, (char*)"Settings", 0x222222);
        } else if (i == 3) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x5AC8FA, 255);
            dui_icon_globe(cx, cy, s);
            blankUI_draw_text_color(base_x - 4, start_y + icon_size + 1, (char*)"Browser", 0x222222);
        } else if (i == 4) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x0A84FF, 255);
            dui_icon_store(cx, cy, s);
            blankUI_draw_text_color(base_x + 6, start_y + icon_size + 1, (char*)"Store", 0x222222);
        } else if (i == 5) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x1C1C1E, 255);
            dui_icon_terminal(cx, cy, s);
            blankUI_draw_text_color(base_x + 6, start_y + icon_size + 1, (char*)"Term", 0x222222);
        } else if (i == 6) {
            draw_rect_rounded(cx, cy, s, s, 12, 0xFF9500, 255);
            dui_icon_calculator(cx, cy, s);
            blankUI_draw_text_color(base_x + 8, start_y + icon_size + 1, (char*)"Calc", 0x222222);
        } else if (i == 7) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x30B0C7, 255);
            dui_icon_weather(cx, cy, s);
            blankUI_draw_text_color(base_x - 4, start_y + icon_size + 1, (char*)"Weather", 0x222222);
        } else if (i == 8) {
            if (g_is_live_cd) {
                draw_rect_rounded(cx, cy, s, s, 12, 0xFF9500, 255);
                dui_icon_updater(cx, cy, s);
                blankUI_draw_text_color(base_x - 2, start_y + icon_size + 1, (char*)"Install", 0x222222);
            } else {
                draw_rect_rounded(cx, cy, s, s, 12, 0x34C759, 255);
                dui_icon_updater(cx, cy, s);
                blankUI_draw_text_color(base_x, start_y + icon_size + 1, (char*)"Verify", 0x222222);
            }
        } else if (i == 9) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x5856D6, 255);
            dui_icon_trash(cx, cy, s);
            blankUI_draw_text_color(base_x + 2, start_y + icon_size + 1, (char*)"RegEdit", 0x222222);
        }

        if (i <= 5) {
            draw_circle_filled(base_x + icon_size / 2, dock_y + dock_h - 4, 2, 0x11111B);
        }
    }

    draw_rect_filled(start_x + (icon_size + spacing) * 9 - 3, start_y + 6, 1, icon_size - 12, 0x000000, 35);
}

static int current_cursor_type = 0;

static const uint8_t cursor_arrow[19][12] = {
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
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t cursor_hand[19][12] = {
    {0,0,0,1,1,0,0,0,0,0,0,0},
    {0,0,1,2,2,1,0,0,0,0,0,0},
    {0,0,1,2,2,1,0,0,0,0,0,0},
    {0,0,1,2,2,1,0,0,0,0,0,0},
    {0,0,1,2,2,1,1,1,0,0,0,0},
    {0,0,1,2,2,2,2,2,1,1,0,0},
    {0,1,1,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,2,2,2,2,1},
    {0,1,2,2,2,2,2,2,2,2,2,1},
    {0,0,1,2,2,2,2,2,2,2,1,0},
    {0,0,0,1,2,2,2,2,2,1,0,0},
    {0,0,0,0,1,2,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,1,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
};

static const uint8_t cursor_ibeam[19][12] = {
    {1,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,0}
};

static const uint8_t cursor_move[19][12] = {
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,1,2,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {1,1,0,0,1,2,1,0,0,1,1,0},
    {2,2,1,1,1,2,1,1,1,2,2,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {0,1,1,1,1,2,1,1,1,1,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,1,2,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,0,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
};

void blankUI_set_cursor_type(int type) {
    current_cursor_type = type;
}

void blankUI_draw_cursor(int x, int y) {
    last_mouse_x = x;
    last_mouse_y = y;
    const uint8_t (*bitmap)[12] = cursor_arrow;

    if (current_cursor_type == 1) bitmap = cursor_hand;
    else if (current_cursor_type == 2) bitmap = cursor_ibeam;
    else if (current_cursor_type == 3 || current_cursor_type == 4 || current_cursor_type == 5) bitmap = cursor_move;

    for (int row = 0; row < 19; row++) {
        for (int col = 0; col < 12; col++) {
            uint8_t pixel = bitmap[row][col];
            if (pixel == 1) {
                put_pixel_alpha(x + col, y + row, 0x000000, 255);
            } else if (pixel == 2) {
                put_pixel_alpha(x + col, y + row, 0xFFFFFF, 255);
            }
        }
    }
}

int blankUI_hit_test_dock(int cursor_x, int cursor_y) {
    int icon_size = 48;
    int spacing = 10;
    int num_icons = 10;
    int dock_content_w = num_icons * icon_size + (num_icons - 1) * spacing + 36;
    int dock_w = dock_content_w;
    int dock_h = 66;
    int dock_x = (screen_width - dock_w) / 2;
    int dock_y = screen_height - dock_h - 8;

    if (cursor_x >= dock_x && cursor_x <= dock_x + dock_w &&
        cursor_y >= dock_y && cursor_y <= dock_y + dock_h) {

        int start_x = dock_x + 16;
        int start_y = dock_y + 8;

        for (int i = 0; i < num_icons; i++) {
            int icon_x = start_x + (icon_size + spacing) * i;
            if (i == 9) icon_x += 6;

            if (cursor_x >= icon_x && cursor_x <= icon_x + icon_size &&
                cursor_y >= start_y && cursor_y <= start_y + icon_size) {
                return i;
            }
        }
    }
    return -1;
}

int active_win_x = -1;
int active_win_y = -1;
bool is_dragging = false;
int drag_offset_x = 0;
int drag_offset_y = 0;
int last_width = 0;
int last_height = 0;

void blankUI_get_window_pos(int* x, int* y, int width, int height) {
    if (width != last_width || height != last_height) {
        active_win_x = (screen_width - width) / 2;
        active_win_y = (screen_height - height) / 2;
        last_width = width;
        last_height = height;
        is_dragging = false;
    }
    *x = active_win_x;
    *y = active_win_y;
}

void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height) {
    int wx = 0, wy = 0;
    blankUI_get_window_pos(&wx, &wy, width, height);

    bool hover_title = (mouse_x >= wx && mouse_x <= wx + width &&
                        mouse_y >= wy && mouse_y <= wy + 32);

    bool hover_close = (mouse_x >= wx + 10 && mouse_x <= wx + 70 &&
                        mouse_y >= wy + 10 && mouse_y <= wy + 26);

    if (hover_close) {
        blankUI_set_cursor_type(1);
    } else if (hover_title && !is_dragging) {
        blankUI_set_cursor_type(3);
    } else if (is_dragging) {
        blankUI_set_cursor_type(3);
    } else {
        blankUI_set_cursor_type(0);
    }

    if (!mouse_pressed) {
        is_dragging = false;
        return;
    }

    if (mouse_pressed && !is_dragging) {
        if (hover_title && !hover_close) {
            is_dragging = true;
            drag_offset_x = mouse_x - wx;
            drag_offset_y = mouse_y - wy;
        }
    }

    if (is_dragging) {
        active_win_x = mouse_x - drag_offset_x;
        active_win_y = mouse_y - drag_offset_y;

        if (active_win_y < 26) active_win_y = 26;
        if (active_win_x < -width + 100) active_win_x = -width + 100;
        if (active_win_x > screen_width - 100) active_win_x = screen_width - 100;
    }
}

int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height) {
    int x, y;
    blankUI_get_window_pos(&x, &y, width, height);
    int cx = x + 16;
    int cy = y + 16;
    if ((cursor_x - cx) * (cursor_x - cx) + (cursor_y - cy) * (cursor_y - cy) <= 100) {
        return 1;
    }
    return 0;
}

void blankUI_draw_window_controls(int window_x, int window_y) {
    draw_circle_filled(window_x + 16, window_y + 16, 6, 0xFF5F56);
    draw_circle_filled(window_x + 36, window_y + 16, 6, 0xFFBD2E);
    draw_circle_filled(window_x + 56, window_y + 16, 6, 0x27C93F);
}

void blankUI_draw_window(int width, int height, char* title) {
    int x, y;
    blankUI_get_window_pos(&x, &y, width, height);

    draw_rect_rounded(x + 10, y + 10, width, height, 12, 0x000000, 45);
    draw_rect_rounded(x, y, width, height, 12, 0xFFFFFF, 242);
    draw_rect_rounded(x, y, width, height, 12, 0xFFFFFF, 100);
    draw_rect_filled(x, y + 32, width, 1, 0x000000, 20);

    blankUI_draw_window_controls(x, y);

    int title_w = dui_text_width(title, 1);
    int title_x = x + (width - title_w) / 2;
    blankUI_draw_text_color(title_x, y + 10, title, 0x2C2C2E);
}

void blankUI_draw_modal(int width, int height, char* title, char* content) {
    draw_rect_filled(0, 0, screen_width, screen_height, 0x000000, 80);
    blankUI_draw_window(width, height, title);

    int x = (screen_width - width) / 2;
    int y = (screen_height - height) / 2;

    blankUI_draw_text_color(x + 30, y + 60, content, 0x000000);
    blankUI_draw_button(x + width - 120, y + height - 50, 100, 32, (char*)"OK");
}

void blankUI_draw_topbar(char* app_title) {
    (void)app_title;
    blankUI_draw_menubar();
}

void blankUI_draw_search_bar(int x, int y, int width) {
    draw_rect_rounded(x, y, width, 28, 14, 0xFFFFFF, 180);
    draw_rect_rounded(x, y, width, 28, 14, 0x000000, 40);
    blankUI_draw_text_color(x + 10, y + 8, (char*)"Search...", 0x666666);
}

void blankUI_draw_dropdown(int x, int y, char** items, int item_count) {
    (void)x; (void)y; (void)items; (void)item_count;
}
void blankUI_draw_slider(int x, int y, int width, float value, char* label) {
    (void)x; (void)y; (void)width; (void)value; (void)label;
}
void blankUI_draw_toggle_switch(int x, int y, bool is_on, char* label) {
    (void)x; (void)y; (void)is_on; (void)label;
}

void blankUI_draw_progress_bar(int x, int y, int width, float percentage) {
    if (percentage < 0.0f) percentage = 0.0f;
    if (percentage > 1.0f) percentage = 1.0f;
    draw_rect_rounded(x, y, width, 8, 4, 0xDDDDDD, 255);
    int fill_w = (int)(width * percentage);
    if (fill_w > 0) {
        draw_rect_rounded(x, y, fill_w, 8, 4, 0x007AFF, 255);
    }
}

void blankUI_draw_toast(char* title, char* message) {
    if (!title && !message) return;
    int toast_w = 360;
    int toast_h = 66;
    int toast_x = (screen_width - toast_w) / 2;
    int toast_y = 36;

    dui_shadow(toast_x, toast_y, toast_w, toast_h, 14, 8, 0x000000, 70);
    draw_frosted_glass_rounded(toast_x, toast_y, toast_w, toast_h, 14, 0x1E1E2E, 235);
    draw_rect_rounded(toast_x, toast_y, toast_w, toast_h, 14, 0xFFFFFF, 35);

    if (title) {
        blankUI_draw_text_color(toast_x + 18, toast_y + 14, title, 0xFFFFFF);
    }
    if (message) {
        blankUI_draw_text_color(toast_x + 18, toast_y + 36, message, 0xA6ADC8);
    }
}

} // extern "C"
