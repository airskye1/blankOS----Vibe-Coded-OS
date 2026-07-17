#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"

static bool str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return (*a == *b);
}

static void str_copy(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

extern "C" int main(OS_API* api) {
    int win_w = 840;
    int win_h = 600;
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

    // Browser navigation
    char url_input[128] = "blank://home";
    int url_len = 12;
    bool url_active = false;
    
    char active_page[128] = "blank://home";
    
    // Page load animation variables
    bool loading = false;
    int loading_timer = 0;

    while (!done) {
        mouse_click = false;
        if (mouse) {
            EFI_SIMPLE_POINTER_STATE mstate;
            if (mouse->GetState(mouse, &mstate) == EFI_SUCCESS) {
                int dx = mstate.RelativeMovementX / 1000;
                int dy = mstate.RelativeMovementY / 1000;
                mouse_x += dx;
                mouse_y += dy;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_x > 1023) mouse_x = 1023;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_y > 767) mouse_y = 767;
                if (mstate.LeftButton) {
                    mouse_click = true;
                }
            }
        }

        EFI_INPUT_KEY key;
        if (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) { // Escape
                done = true;
            }
            
            if (url_active) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) { // Backspace
                    if (url_len > 0) {
                        url_len--;
                        url_input[url_len] = '\0';
                    }
                } else if (key.UnicodeChar == 13 || key.UnicodeChar == 10) { // Enter
                    if (url_len > 0) {
                        url_active = false;
                        loading = true;
                        loading_timer = 0;
                        api->play_sound("click");
                    }
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126 && url_len < 127) {
                    url_input[url_len++] = (char)key.UnicodeChar;
                    url_input[url_len] = '\0';
                }
            }
        }

        if (loading) {
            loading_timer++;
            if (loading_timer > 10) {
                loading = false;
                str_copy(active_page, url_input, 128);
                api->play_sound("startup");
            }
        }

        // Draw system UI
        api->draw_wallpaper();
        api->draw_menubar();
        api->draw_dock();

        // Draw Browser window
        api->draw_window(win_x, win_y, win_w, win_h, "BlankBrowser");

        // Close window hit test
        if (mouse_click && mouse_x >= win_x + 10 && mouse_x <= win_x + 22 && mouse_y >= win_y + 10 && mouse_y <= win_y + 22) {
            done = true;
        }

        // Toolbar background
        int tb_y = win_y + 33;
        api->draw_rect(win_x + 1, tb_y, win_w - 2, 45, 0xF5F5F7, 255);
        api->draw_line(win_x + 1, tb_y + 45, win_x + win_w - 2, tb_y + 45, 0xD2D2D7, 1);

        // Back / Forward Mock Buttons
        api->draw_rect_rounded(win_x + 12, tb_y + 8, 30, 28, 6, 0xE8E8ED, 255);
        api->draw_text(win_x + 22, tb_y + 16, "<", 0x8E8E93, 1);

        api->draw_rect_rounded(win_x + 50, tb_y + 8, 30, 28, 6, 0xE8E8ED, 255);
        api->draw_text(win_x + 60, tb_y + 16, ">", 0x8E8E93, 1);

        // URL Input Bar
        int url_x = win_x + 95;
        int url_w = win_w - 210;
        api->draw_rect_rounded(url_x, tb_y + 8, url_w, 28, 6, url_active ? 0xFFFFFF : 0xE3E3E8, 255);
        if (url_active) {
            api->draw_rect_outline(url_x, tb_y + 8, url_w, 28, 0x007AFF, 1);
        }
        api->draw_text(url_x + 10, tb_y + 16, url_input, 0x1C1C1E, 1);

        if (mouse_click) {
            url_active = (mouse_x >= url_x && mouse_x <= url_x + url_w && mouse_y >= tb_y + 8 && mouse_y <= tb_y + 36);
        }

        // Refresh button
        int ref_x = win_x + win_w - 100;
        if (api->draw_button(ref_x, tb_y + 8, 85, 28, "Refresh", mouse_x, mouse_y, mouse_click)) {
            loading = true;
            loading_timer = 0;
            api->play_sound("click");
        }

        // Web Content Area
        int web_y = tb_y + 46;
        int web_h = win_h - 46 - 34;
        api->draw_rect(win_x + 1, web_y, win_w - 2, web_h, 0xFFFFFF, 255);

        if (loading) {
            // Spinning/Loading Indicator
            api->draw_text(win_x + (win_w - 18 * 8)/2, web_y + 120, "Resolving DNS Host...", 0x8E8E93, 1);
            api->draw_rect_rounded(win_x + (win_w - 200)/2, web_y + 150, 200, 10, 5, 0xE5E5EA, 255);
            api->draw_rect_rounded(win_x + (win_w - 200)/2, web_y + 150, loading_timer * 20, 10, 5, 0x007AFF, 255);
        }
        else {
            // Render pages
            if (str_eq(active_page, "blank://home")) {
                api->draw_text(win_x + 50, web_y + 40, "BlankOS Browser Homepage", 0x1C1C1E, 2);
                api->draw_text(win_x + 50, web_y + 75, "Welcome to the custom freestanding browser! Quick navigation links:", 0x8E8E93, 1);

                // Quick links
                int link_x = win_x + 50;
                int link_y = web_y + 120;
                
                // Google Tile
                api->draw_rect_rounded(link_x, link_y, 160, 100, 10, 0xF2F2F7, 255);
                api->draw_text(link_x + 20, link_y + 35, "Google Search", 0x007AFF, 1);
                if (mouse_click && mouse_x >= link_x && mouse_x <= link_x + 160 && mouse_y >= link_y && mouse_y <= link_y + 100) {
                    str_copy(url_input, "google.com", 128);
                    url_len = 10;
                    loading = true;
                    loading_timer = 0;
                    api->play_sound("click");
                }

                // airskye Blog Tile
                api->draw_rect_rounded(link_x + 190, link_y, 160, 100, 10, 0xF2F2F7, 255);
                api->draw_text(link_x + 210, link_y + 35, "airskye's Blog", 0xFF9500, 1);
                if (mouse_click && mouse_x >= link_x + 190 && mouse_x <= link_x + 350 && mouse_y >= link_y && mouse_y <= link_y + 100) {
                    str_copy(url_input, "blog.airskye.com", 128);
                    url_len = 16;
                    loading = true;
                    loading_timer = 0;
                    api->play_sound("click");
                }

                // GitHub Tile
                api->draw_rect_rounded(link_x + 380, link_y, 160, 100, 10, 0xF2F2F7, 255);
                api->draw_text(link_x + 410, link_y + 35, "GitHub Repo", 0x1C1C1E, 1);
                if (mouse_click && mouse_x >= link_x + 380 && mouse_x <= link_x + 540 && mouse_y >= link_y && mouse_y <= link_y + 100) {
                    str_copy(url_input, "github.com", 128);
                    url_len = 10;
                    loading = true;
                    loading_timer = 0;
                    api->play_sound("click");
                }
            } 
            else if (str_eq(active_page, "google.com") || str_eq(active_page, "www.google.com")) {
                api->draw_text(win_x + (win_w - 6 * 16)/2, web_y + 50, "Google", 0x1C1C1E, 2);
                
                // Mock search box
                int box_x = win_x + (win_w - 400)/2;
                api->draw_rect_rounded(box_x, web_y + 100, 400, 36, 18, 0xF2F2F7, 255);
                api->draw_text(box_x + 20, web_y + 112, "Query: Vibe-coded UEFI OS", 0x8E8E93, 1);

                // Mock search results
                api->draw_text(box_x - 50, web_y + 170, "Results for 'Vibe-coded UEFI OS':", 0x1C1C1E, 1);

                api->draw_text(box_x - 50, web_y + 200, "1. github.com/airskye1/blankOS", 0x007AFF, 1);
                api->draw_text(box_x - 50, web_y + 220, "   A fully custom UEFI kernel written in modern freestanding C++.", 0x333333, 1);

                api->draw_text(box_x - 50, web_y + 255, "2. blog.airskye.com/blankos-raycaster", 0x007AFF, 1);
                api->draw_text(box_x - 50, web_y + 275, "   Inside the design of blankDUI vector rendering primitive engine.", 0x333333, 1);
            }
            else if (str_eq(active_page, "blog.airskye.com")) {
                api->draw_text(win_x + 50, web_y + 40, "airskye's Developer Blog", 0xFF9500, 2);
                api->draw_text(win_x + 50, web_y + 70, "Vibe-coding an entire OS: The Journey to 1.3", 0x333333, 1);

                api->draw_rect_rounded(win_x + 50, web_y + 110, win_w - 100, 200, 8, 0xF2F2F7, 255);
                
                api->draw_text(win_x + 70, web_y + 130, "July 17, 2026", 0x8E8E93, 1);
                api->draw_text(win_x + 70, web_y + 160, "Today we optimized our EFI HTTP protocol drivers to allow native", 0x333333, 1);
                api->draw_text(win_x + 70, web_y + 185, "dynamic ELF loadings straight from the GitHub app store catalog.", 0x333333, 1);
                api->draw_text(win_x + 70, web_y + 210, "The compositor now syncs with monitor vertical refresh locking", 0x333333, 1);
                api->draw_text(win_x + 70, web_y + 235, "to completely eliminate layout screen tearing.", 0x333333, 1);
                api->draw_text(win_x + 70, web_y + 260, "More updates are coming soon! Keep checking.", 0x333333, 1);
            }
            else {
                // 404 Page
                api->draw_text(win_x + 50, web_y + 50, "404 Not Found", 0xFF3B30, 2);
                api->draw_text(win_x + 50, web_y + 90, "The requested website cannot be resolved or is offline.", 0x333333, 1);
                api->draw_text(win_x + 50, web_y + 120, "Check your Wi-Fi settings or type a different address.", 0x8E8E93, 1);
            }
        }

        // Draw cursor
        api->draw_cursor(mouse_x, mouse_y);
        api->swap_buffers();
    }
    return 0;
}
