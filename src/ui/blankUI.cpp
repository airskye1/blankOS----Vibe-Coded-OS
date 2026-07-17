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

extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
extern int dui_text_width(const char* text, int scale);
extern int dui_text_height(int scale);
extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);

static bool reduce_motion_enabled = false;
static int last_mouse_x = 0;
static int last_mouse_y = 0;
static float dock_icon_scales[9] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

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
        blankUI_draw_char(cur_x, y, *text, 0x000000); // Default to black text for light theme
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

void blankUI_animate_fade_in(void* component) {}
void blankUI_animate_slide_up(void* component) {}

void blankUI_draw_button(int x, int y, int width, int height, char* text) {
    // Premium dark rounded button
    draw_rect_rounded(x, y, width, height, 16, 0x18171c, 255);
    draw_rect_rounded(x, y, width, 1, 16, 0xFFFFFF, 40); // Top highlight
    
    int text_w = dui_text_width(text, 1);
    int text_h = dui_text_height(1);
    int text_x = x + (width - text_w) / 2;
    int text_y = y + (height - text_h) / 2;
    blankUI_draw_text_color(text_x, text_y, text, 0xFFFFFF); // White text on dark button
}

extern (char*)(char*)(char*)"C" {
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
}

void blankUI_draw_menubar() {
    // Top bar, frosted glass look, light mode
    draw_frosted_glass_rounded(0, 0, screen_width, 24, 0, 0xFFFFFF, 180);
    draw_rect_filled(0, 24, screen_width, 1, 0x000000, 20); // subtle shadow line
    
    // Left side items
    blankUI_draw_text_color(16, 8, (char*)(char*)(char*)(char*)"*", 0x000000); // BlankOS logo icon
    blankUI_draw_text_color(40, 8, (char*)(char*)(char*)(char*)"BlankOS", 0x000000);
    blankUI_draw_text_color(120, 8, (char*)(char*)(char*)(char*)"File", 0x000000);
    blankUI_draw_text_color(170, 8, (char*)(char*)(char*)(char*)"Edit", 0x000000);
    blankUI_draw_text_color(220, 8, (char*)(char*)(char*)(char*)"View", 0x000000);
    blankUI_draw_text_color(270, 8, (char*)(char*)(char*)(char*)"Window", 0x000000);
    blankUI_draw_text_color(340, 8, (char*)(char*)(char*)(char*)"Help", 0x000000);
    
    // Right side items - Clock (Dynamic!)
    char time_str[8];
    format_time_str(time_str);
    blankUI_draw_text_color(screen_width - 70, 8, time_str, 0x000000);
    
    // Battery widget
    int bx = screen_width - 110;
    draw_rect_rounded(bx, 8, 20, 10, 2, 0x555555, 255); // Outline
    draw_rect_filled(bx + 20, 11, 2, 4, 0x555555, 255); // Notch
    draw_rect_filled(bx + 2, 10, 16, 6, 0x34C759, 255); // Green charge level (80%)
    
    // Wi-Fi widget
    int wx = screen_width - 150;
    // Draw 4 signal bars
    for (int b = 0; b < 4; b++) {
        uint32_t bar_color = 0xCCCCCC; // Inactive grey
        if (wifi_connected && wifi_signal_strength > b) bar_color = 0x007AFF; // Active blue
        draw_rect_filled(wx + b*5, 18 - b*3, 3, b*3 + 2, bar_color, 255);
    }
    
    // Bluetooth widget (Runic B shape)
    int btx = screen_width - 180;
    uint32_t bt_color = bluetooth_connected ? 0x007AFF : 0x8E8E93;
    // Draw simplified Bluetooth rune
    dui_line(btx + 4, 6, btx + 4, 18, bt_color, 1);
    dui_line(btx + 4, 6, btx + 8, 9, bt_color, 1);
    dui_line(btx + 8, 9, btx + 4, 12, bt_color, 1);
    dui_line(btx + 4, 12, btx + 8, 15, bt_color, 1);
    dui_line(btx + 8, 15, btx + 4, 18, bt_color, 1);

    // Render Dropdown Menus if open
    if (wifi_menu_open) {
        int mx = screen_width - 230;
        dui_shadow(mx, 28, 220, 160, 12, 6, 0x000000, 40);
        draw_rect_rounded(mx, 28, 220, 160, 12, 0xFFFFFF, 240);
        draw_rect_rounded(mx, 28, 220, 160, 12, 0xFFFFFF, 80);
        
        blankUI_draw_text_color(mx + 16, 40, (char*)(char*)(char*)(char*)"Wi-Fi Networks", 0x333333);
        draw_rect_filled(mx + 16, 56, 188, 1, 0x000000, 20); // Divider
        
        if (wifi_connected) {
            blankUI_draw_text_color(mx + 16, 68, (char*)(char*)(char*)(char*)"Connected to:", 0x8E8E93);
            blankUI_draw_text_color(mx + 16, 88, connected_ssid, 0x007AFF);
        } else {
            blankUI_draw_text_color(mx + 16, 78, (char*)(char*)(char*)(char*)"Select a network...", 0x8E8E93);
        }
        
        blankUI_draw_text_color(mx + 16, 114, (char*)(char*)(char*)(char*)"1. BlankOS_Secure", 0x333333);
        blankUI_draw_text_color(mx + 16, 134, (char*)(char*)(char*)(char*)"2. Ethan_WiFi_5G", 0x333333);
    }
    
    if (bt_menu_open) {
        int mx = screen_width - 230;
        dui_shadow(mx, 28, 220, 140, 12, 6, 0x000000, 40);
        draw_rect_rounded(mx, 28, 220, 140, 12, 0xFFFFFF, 240);
        draw_rect_rounded(mx, 28, 220, 140, 12, 0xFFFFFF, 80);
        
        blankUI_draw_text_color(mx + 16, 40, (char*)(char*)(char*)(char*)"Bluetooth Devices", 0x333333);
        draw_rect_filled(mx + 16, 56, 188, 1, 0x000000, 20);
        
        if (bluetooth_connected) {
            blankUI_draw_text_color(mx + 16, 68, (char*)(char*)(char*)(char*)"Paired device:", 0x8E8E93);
            blankUI_draw_text_color(mx + 16, 88, paired_device_name, 0x007AFF);
        } else {
            blankUI_draw_text_color(mx + 16, 78, (char*)(char*)(char*)(char*)"1. AirPods Max", 0x333333);
            blankUI_draw_text_color(mx + 16, 98, (char*)(char*)(char*)(char*)"2. Magic Keyboard", 0x333333);
        }
    }
    
    if (battery_menu_open) {
        int mx = screen_width - 170;
        dui_shadow(mx, 28, 160, 90, 12, 6, 0x000000, 40);
        draw_rect_rounded(mx, 28, 160, 90, 12, 0xFFFFFF, 240);
        draw_rect_rounded(mx, 28, 160, 90, 12, 0xFFFFFF, 80);
        
        blankUI_draw_text_color(mx + 16, 40, (char*)(char*)(char*)(char*)"Battery Level: 80%", 0x333333);
        blankUI_draw_text_color(mx + 16, 60, (char*)(char*)(char*)(char*)"Power Source: DC", 0x8E8E93);
    }
    
    // Context Menu Draw
    if (rclick_menu_open) {
        int mx = rclick_menu_x;
        int my = rclick_menu_y;
        dui_shadow(mx, my, 160, 110, 8, 6, 0x000000, 40);
        draw_rect_rounded(mx, my, 160, 110, 8, 0xFFFFFF, 240);
        draw_rect_rounded(mx, my, 160, 110, 8, 0xFFFFFF, 80);
        
        blankUI_draw_text_color(mx + 12, my + 10, (char*)(char*)(char*)(char*)"System Info", 0x333333);
        blankUI_draw_text_color(mx + 12, my + 36, (char*)(char*)(char*)(char*)"Terminal", 0x333333);
        blankUI_draw_text_color(mx + 12, my + 62, (char*)(char*)(char*)(char*)"Trigger Panic", 0xFF3B30); // Red panic trigger
        blankUI_draw_text_color(mx + 12, my + 88, (char*)(char*)(char*)(char*)"Cancel Menu", 0x8E8E93);
    }
}

void blankUI_draw_dock() {
    int icon_size = 48;
    int spacing = 10;
    int num_icons = 9; // Finder, Settings, Browser, Store, Terminal, Calculator, Weather, Doom, Trash
    int dock_content_w = num_icons * icon_size + (num_icons - 1) * spacing + 32;
    int dock_w = dock_content_w;
    int dock_h = 64;
    int dock_x = (screen_width - dock_w) / 2;
    int dock_y = screen_height - dock_h - 8;
    
    // Frosted glass dock
    draw_frosted_glass_rounded(dock_x, dock_y, dock_w, dock_h, 20, 0xFFFFFF, 160);
    draw_rect_rounded(dock_x, dock_y, dock_w, dock_h, 20, 0xFFFFFF, 80);
    
    int start_x = dock_x + 16;
    int start_y = dock_y + 4;
    
    for (int i = 0; i < 9; i++) {
        int base_x = start_x + (icon_size + spacing) * i;
        if (i == 8) base_x += 8; // Trash offset
        
        float target = 1.0f;
        if (last_mouse_x >= base_x && last_mouse_x <= base_x + icon_size &&
            last_mouse_y >= start_y && last_mouse_y <= start_y + icon_size) {
            target = 1.3f;
        }
        dock_icon_scales[i] += (target - dock_icon_scales[i]) * 0.2f;
        
        int s = (int)(icon_size * dock_icon_scales[i]);
        int cx = base_x - (s - icon_size) / 2;
        int cy = start_y - (s - icon_size); // Pop upwards
        
        if (i == 0) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x007AFF, 255);
            blankUI_draw_text_color(base_x + 4, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Finder", 0x333333);
        } else if (i == 1) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x8E8E93, 255);
            blankUI_draw_text_color(base_x - 8, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Settings", 0x333333);
        } else if (i == 2) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x5AC8FA, 255);
            blankUI_draw_text_color(base_x - 4, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Browser", 0x333333);
        } else if (i == 3) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x007AFF, 255);
            blankUI_draw_text_color(base_x + 8, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Store", 0x333333);
        } else if (i == 4) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x1C1C1E, 255);
            blankUI_draw_text_color(base_x - 8, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Terminal", 0x333333);
        } else if (i == 5) {
            draw_rect_rounded(cx, cy, s, s, 12, 0xFF9500, 255);
            blankUI_draw_text_color(base_x - 4, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Calc", 0x333333);
        } else if (i == 6) {
            draw_rect_rounded(cx, cy, s, s, 12, 0x30B0C7, 255);
            blankUI_draw_text_color(base_x - 4, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Weather", 0x333333);
        } else if (i == 7) {
            draw_rect_rounded(cx, cy, s, s, 12, 0xFF3B30, 255); // Red Doom app box
            blankUI_draw_text_color(base_x + 8, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Doom", 0x333333);
        } else if (i == 8) {
            draw_rect_rounded(cx, cy, s, s, 12, 0xE5E5EA, 255);
            blankUI_draw_text_color(base_x + 12, start_y + icon_size + 2, (char*)(char*)(char*)(char*)"Trash", 0x333333);
        }
    }
    
    // Divider moved for 9 icons
    draw_rect_filled(start_x + (icon_size+spacing)*8, start_y + 8, 1, icon_size - 16, 0x000000, 40);
}

// Multi-cursor state & bitmaps (0=transparent, 1=black, 2=white)
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
                put_pixel_alpha(x + col, y + row, 0x000000, 255); // Black border
            } else if (pixel == 2) {
                put_pixel_alpha(x + col, y + row, 0xFFFFFF, 255); // White fill
            }
        }
    }
}

int blankUI_hit_test_dock(int cursor_x, int cursor_y) {
    int icon_size = 48;
    int spacing = 10;
    int num_icons = 8;
    int dock_content_w = num_icons * icon_size + (num_icons - 1) * spacing + 32;
    int dock_w = dock_content_w;
    int dock_h = 64;
    int dock_x = (screen_width - dock_w) / 2;
    int dock_y = screen_height - dock_h - 8;
    
    if (cursor_x >= dock_x && cursor_x <= dock_x + dock_w &&
        cursor_y >= dock_y && cursor_y <= dock_y + dock_h) {
        
        int start_x = dock_x + 16;
        int start_y = dock_y + 4;
        
        for (int i = 0; i < num_icons; i++) {
            int icon_x = start_x + (icon_size + spacing) * i;
            if (i == 7) icon_x += 8; // Trash has extra offset after divider
            
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
        blankUI_set_cursor_type(1); // Hand
    } else if (hover_title && !is_dragging) {
        blankUI_set_cursor_type(3); // Move
    } else if (is_dragging) {
        blankUI_set_cursor_type(3); // Keep Move while dragging
    } else {
        blankUI_set_cursor_type(0); // Arrow
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
        
        if (active_win_y < 24) active_win_y = 24;
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
    draw_circle_filled(window_x + 16, window_y + 16, 6, 0xFF5F56); // Red
    draw_circle_filled(window_x + 36, window_y + 16, 6, 0xFFBD2E); // Yellow
    draw_circle_filled(window_x + 56, window_y + 16, 6, 0x27C93F); // Green
}

void blankUI_draw_window(int width, int height, char* title) {
    int x, y;
    blankUI_get_window_pos(&x, &y, width, height);

    // Soft drop shadow
    draw_rect_rounded(x + 10, y + 10, width, height, 12, 0x000000, 40);

    // Light mode frosted window
    draw_rect_rounded(x, y, width, height, 12, 0xFFFFFF, 240); 
    draw_rect_rounded(x, y, width, height, 12, 0xFFFFFF, 100); // Inner border highlight
    
    // Header divider
    draw_rect_filled(x, y + 32, width, 1, 0x000000, 20);

    blankUI_draw_window_controls(x, y);

    int title_w = dui_text_width(title, 1);
    int title_x = x + (width - title_w) / 2;
    blankUI_draw_text_color(title_x, y + 12, title, 0x333333);
}

void blankUI_draw_modal(int width, int height, char* title, char* content) {
    draw_rect_filled(0, 0, screen_width, screen_height, 0x000000, 80);
    blankUI_draw_window(width, height, title);
    
    int x = (screen_width - width) / 2;
    int y = (screen_height - height) / 2;
    
    blankUI_draw_text_color(x + 30, y + 60, content, 0x000000);
    blankUI_draw_button(x + width - 120, y + height - 50, 100, 32, (char*)(char*)(char*)(char*)"OK");
}

void blankUI_draw_topbar(char* app_title) {
    blankUI_draw_menubar();
}

void blankUI_draw_search_bar(int x, int y, int width) {
    draw_rect_rounded(x, y, width, 28, 14, 0xFFFFFF, 180);
    draw_rect_rounded(x, y, width, 28, 14, 0x000000, 40);
    blankUI_draw_text_color(x + 10, y + 10, (char*)(char*)(char*)(char*)"Search...", 0x666666);
}

void blankUI_draw_dropdown(int x, int y, char** items, int item_count) {}
void blankUI_draw_slider(int x, int y, int width, float value, char* label) {}
void blankUI_draw_toggle_switch(int x, int y, bool is_on, char* label) {}
void blankUI_draw_progress_bar(int x, int y, int width, float percentage) {
    draw_rect_rounded(x, y, width, 8, 4, 0xDDDDDD, 255);
    draw_rect_rounded(x, y, (int)(width * percentage), 8, 4, 0x007AFF, 255);
}
void blankUI_draw_tabs(int x, int y, char** tab_names, int tab_count, int active_index) {}

} // extern (char*)(char*)(char*)"C"
