#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"

// Simple helper to copy memory
static void app_memcpy(void* dest, const void* src, int n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}

// Simple integer to string helper
static void int_to_str(int val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char temp[12];
    int i = 0;
    while (val > 0) {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

extern "C" int main(OS_API* api) {
    int win_w = 760;
    int win_h = 520;
    int win_x = (1024 - win_w) / 2;
    int win_y = (768 - win_h) / 2;

    EFI_SIMPLE_POINTER_PROTOCOL* mouse = NULL;
    EFI_GUID mouse_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    api->SystemTable->BootServices->LocateProtocol(&mouse_guid, NULL, (void**)&mouse);
    if (mouse) mouse->Reset(mouse, TRUE);

    int mouse_x = 512;
    int mouse_y = 384;
    bool mouse_click = false;
    bool done = false;

    int active_tab = 0; // 0=Wi-Fi, 1=Bluetooth, 2=About
    bool wifi_enabled = true;
    bool bt_enabled = false;
    int connected_wifi_index = -1;
    bool pairing_device = false;
    int pairing_counter = 0;

    const char* wifi_networks[] = { "airskye_5G", "Starlink_Home", "Coffee_Shop_Free", "OSDev_TestNet" };
    const char* bt_devices[] = { "airskye's AirPods", "Magic Trackpad", "Logitech MX Master", "Sony WH-1000XM4" };
    bool bt_paired[] = { false, false, false, false };

    while (!done) {
        mouse_click = false;
        if (mouse) {
            EFI_SIMPLE_POINTER_STATE state;
            if (mouse->GetState(mouse, &state) == EFI_SUCCESS) {
                int dx = state.RelativeMovementX / 1000;
                int dy = state.RelativeMovementY / 1000;
                mouse_x += dx;
                mouse_y += dy;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_x > 1023) mouse_x = 1023;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_y > 767) mouse_y = 767;
                if (state.LeftButton) {
                    mouse_click = true;
                }
            }
        }

        EFI_INPUT_KEY key;
        if (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) { // Escape
                done = true;
            }
        }

        // Draw background
        api->draw_wallpaper();
        api->draw_menubar();
        api->draw_dock();

        // Draw window frame
        api->draw_window(win_x, win_y, win_w, win_h, "System Settings");

        // Close window hit test
        // Red close button is at (win_x + 16, win_y + 16, radius 6)
        if (mouse_click && mouse_x >= win_x + 10 && mouse_x <= win_x + 22 && mouse_y >= win_y + 10 && mouse_y <= win_y + 22) {
            done = true;
        }

        // Sidebar background
        api->draw_rect(win_x + 1, win_y + 33, 180, win_h - 34, 0xF2F2F7, 255);
        api->draw_line(win_x + 181, win_y + 33, win_x + 181, win_y + win_h - 1, 0xCCCCCC, 1);

        // Sidebar tabs
        for (int t = 0; t < 3; t++) {
            int tab_y = win_y + 60 + t * 45;
            bool is_active = (active_tab == t);
            if (is_active) {
                api->draw_rect_rounded(win_x + 8, tab_y, 164, 35, 6, 0x007AFF, 255);
            } else {
                // Hover effect
                if (mouse_x >= win_x + 8 && mouse_x <= win_x + 172 && mouse_y >= tab_y && mouse_y <= tab_y + 35) {
                    api->draw_rect_rounded(win_x + 8, tab_y, 164, 35, 6, 0xE5E5EA, 255);
                    if (mouse_click) {
                        active_tab = t;
                        api->play_sound("click");
                    }
                }
            }

            const char* tab_name = (t == 0) ? "Wi-Fi" : ((t == 1) ? "Bluetooth" : "About System");
            uint32_t text_color = is_active ? 0xFFFFFF : 0x333333;
            api->draw_text(win_x + 20, tab_y + 10, tab_name, text_color, 1);
        }

        // Content Area
        int content_x = win_x + 200;
        int content_y = win_y + 60;

        if (active_tab == 0) { // Wi-Fi settings
            api->draw_text(content_x, content_y, "Wi-Fi Networking", 0x1C1C1E, 2);
            api->draw_text(content_x, content_y + 35, "Connect to local wireless networks.", 0x8E8E93, 1);

            // Enable/disable toggle
            api->draw_text(content_x, content_y + 75, "Enable Wi-Fi", 0x333333, 1);
            const char* toggle_lbl = wifi_enabled ? "ON" : "OFF";
            if (api->draw_button(content_x + 440, content_y + 70, 80, 25, toggle_lbl, mouse_x, mouse_y, mouse_click)) {
                wifi_enabled = !wifi_enabled;
                if (!wifi_enabled) connected_wifi_index = -1;
                api->play_sound("click");
            }

            api->draw_line(content_x, content_y + 110, win_x + win_w - 20, content_y + 110, 0xE5E5EA, 1);

            if (wifi_enabled) {
                api->draw_text(content_x, content_y + 130, "Available Networks", 0x1C1C1E, 1);
                
                for (int i = 0; i < 4; i++) {
                    int item_y = content_y + 160 + i * 50;
                    api->draw_rect_rounded(content_x, item_y, 520, 40, 8, 0xF2F2F7, 255);
                    api->draw_text(content_x + 15, item_y + 12, wifi_networks[i], 0x333333, 1);

                    if (connected_wifi_index == i) {
                        api->draw_text(content_x + 300, item_y + 12, "Connected", 0x34C759, 1);
                        if (api->draw_button(content_x + 410, item_y + 8, 100, 24, "Disconnect", mouse_x, mouse_y, mouse_click)) {
                            connected_wifi_index = -1;
                            api->play_sound("click");
                        }
                    } else {
                        if (api->draw_button(content_x + 410, item_y + 8, 100, 24, "Connect", mouse_x, mouse_y, mouse_click)) {
                            connected_wifi_index = i;
                            api->play_sound("startup");
                        }
                    }
                }
            } else {
                api->draw_text(content_x, content_y + 160, "Wi-Fi is turned off.", 0x8E8E93, 1);
            }
        } 
        else if (active_tab == 1) { // Bluetooth settings
            api->draw_text(content_x, content_y, "Bluetooth", 0x1C1C1E, 2);
            api->draw_text(content_x, content_y + 35, "Pair wireless accessories and peripherals.", 0x8E8E93, 1);

            // Toggle
            api->draw_text(content_x, content_y + 75, "Enable Bluetooth", 0x333333, 1);
            const char* toggle_lbl = bt_enabled ? "ON" : "OFF";
            if (api->draw_button(content_x + 440, content_y + 70, 80, 25, toggle_lbl, mouse_x, mouse_y, mouse_click)) {
                bt_enabled = !bt_enabled;
                if (!bt_enabled) {
                    pairing_device = false;
                    for (int i=0; i<4; i++) bt_paired[i] = false;
                }
                api->play_sound("click");
            }

            api->draw_line(content_x, content_y + 110, win_x + win_w - 20, content_y + 110, 0xE5E5EA, 1);

            if (bt_enabled) {
                api->draw_text(content_x, content_y + 130, "My Devices", 0x1C1C1E, 1);

                if (pairing_device) {
                    pairing_counter++;
                    api->draw_text(content_x, content_y + 160, "Connecting to device...", 0x007AFF, 1);
                    if (pairing_counter > 15) {
                        pairing_device = false;
                        api->play_sound("startup");
                    }
                } else {
                    for (int i = 0; i < 4; i++) {
                        int item_y = content_y + 160 + i * 50;
                        api->draw_rect_rounded(content_x, item_y, 520, 40, 8, 0xF2F2F7, 255);
                        api->draw_text(content_x + 15, item_y + 12, bt_devices[i], 0x333333, 1);

                        if (bt_paired[i]) {
                            api->draw_text(content_x + 300, item_y + 12, "Connected", 0x007AFF, 1);
                            if (api->draw_button(content_x + 410, item_y + 8, 100, 24, "Unpair", mouse_x, mouse_y, mouse_click)) {
                                bt_paired[i] = false;
                                api->play_sound("click");
                            }
                        } else {
                            if (api->draw_button(content_x + 410, item_y + 8, 100, 24, "Pair", mouse_x, mouse_y, mouse_click)) {
                                bt_paired[i] = true;
                                pairing_device = true;
                                pairing_counter = 0;
                                api->play_sound("click");
                            }
                        }
                    }
                }
            } else {
                api->draw_text(content_x, content_y + 160, "Bluetooth is turned off.", 0x8E8E93, 1);
            }
        } 
        else if (active_tab == 2) { // About System
            api->draw_text(content_x, content_y, "About BlankOS", 0x1C1C1E, 2);
            api->draw_text(content_x, content_y + 35, "BlankOS Monolithic Architecture Platform", 0x8E8E93, 1);

            api->draw_rect_rounded(content_x, content_y + 75, 520, 280, 10, 0xF2F2F7, 255);

            api->draw_text(content_x + 20, content_y + 100, "System Specifications:", 0x1C1C1E, 1);
            
            api->draw_text(content_x + 20, content_y + 140, "OS Name:      BlankOS Vibe-Coded Platform", 0x333333, 1);
            api->draw_text(content_x + 20, content_y + 170, "OS Version:   1.3.1 (Monolithic UEFI Core)", 0x333333, 1);
            api->draw_text(content_x + 20, content_y + 200, "Arch:         x86_64 Long Mode", 0x333333, 1);
            api->draw_text(content_x + 20, content_y + 230, "Compositor:   V-Sync Compositor Engine", 0x333333, 1);
            api->draw_text(content_x + 20, content_y + 260, "Firmware:     Intel TianoCore UEFI Framework", 0x333333, 1);
            api->draw_text(content_x + 20, content_y + 290, "UI Engine:    blankUI & blankDUI Vector Renderer", 0x333333, 1);
            
            if (api->draw_button(content_x + 360, content_y + 95, 140, 30, "Diagnostics", mouse_x, mouse_y, mouse_click)) {
                api->panic("SYSTEM_DIAG", "User-initiated diagnostic check from Settings panel.");
            }
        }

        // Draw cursor
        api->draw_cursor(mouse_x, mouse_y);
        api->swap_buffers();
    }
    return 0;
}
