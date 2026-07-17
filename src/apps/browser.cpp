#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_gradient_v(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    extern void blankUI_set_cursor_type(int type);
    
    extern int screen_width;
    extern int screen_height;
    
    // Tab structure
    typedef struct {
        char url[128];
        char title[32];
        int page_type; // 0=home, 1=google, 2=github, 3=apple
        char search_query[64];
        bool is_active;
    } BrowserTab;

    static BrowserTab tabs[4];
    static int tab_count = 1;
    static int active_tab_idx = 0;
    
    // HTML link hit-testing box
    typedef struct {
        int x, y, w, h;
        char target_url[128];
    } LinkHitBox;
    
    static LinkHitBox links[10];
    static int link_count = 0;
    
    static void add_link(int x, int y, int w, int h, const char* target) {
        if (link_count >= 10) return;
        links[link_count].x = x;
        links[link_count].y = y;
        links[link_count].w = w;
        links[link_count].h = h;
        int j = 0;
        while (target[j] && j < 127) {
            links[link_count].target_url[j] = target[j];
            j++;
        }
        links[link_count].target_url[j] = '\0';
        link_count++;
    }

    static void str_copy(char* dst, const char* src, int max_len) {
        int i = 0;
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }

    static bool str_equals(const char* a, const char* b) {
        int i = 0;
        while (a[i] && b[i]) {
            if (a[i] != b[i]) return false;
            i++;
        }
        return a[i] == b[i];
    }
    
    static void navigate_to(int tab_idx, const char* url) {
        str_copy(tabs[tab_idx].url, url, 128);
        
        // Match mock domains
        if (str_equals(url, (char*)"google.com") || str_equals(url, "www.google.com") || str_equals(url, "http://google.com")) {
            tabs[tab_idx].page_type = 1;
            str_copy(tabs[tab_idx].title, "Google", 32);
        } else if (str_equals(url, "github.com") || str_equals(url, "www.github.com") || str_equals(url, "http://github.com")) {
            tabs[tab_idx].page_type = 2;
            str_copy(tabs[tab_idx].title, "GitHub", 32);
        } else if (str_equals(url, "apple.com") || str_equals(url, "www.apple.com") || str_equals(url, "http://apple.com")) {
            tabs[tab_idx].page_type = 3;
            str_copy(tabs[tab_idx].title, "Apple", 32);
        } else {
            tabs[tab_idx].page_type = 0;
            str_copy(tabs[tab_idx].title, "New Tab", 32);
        }
    }

    void launch_browser(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool done = false;
        
        int win_w = 900;
        int win_h = 600;
        
        // Initialize default tab
        if (tab_count == 1 && tabs[0].url[0] == '\0') {
            str_copy(tabs[0].url, "blank://home", 128);
            str_copy(tabs[0].title, "New Tab", 32);
            tabs[0].page_type = 0;
            tabs[0].search_query[0] = '\0';
        }
        
        int active_input = 0; // 0 = none, 1 = URL bar, 2 = Google Search input
        
        while(!done) {
            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);
            
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) { done = true; } // ESC
                else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                    if (active_input == 1) {
                        int len = 0;
                        while (tabs[active_tab_idx].url[len]) len++;
                        if (len < 127) {
                            tabs[active_tab_idx].url[len] = (char)Key.UnicodeChar;
                            tabs[active_tab_idx].url[len+1] = '\0';
                        }
                    } else if (active_input == 2) {
                        int len = 0;
                        while (tabs[active_tab_idx].search_query[len]) len++;
                        if (len < 63) {
                            tabs[active_tab_idx].search_query[len] = (char)Key.UnicodeChar;
                            tabs[active_tab_idx].search_query[len+1] = '\0';
                        }
                    }
                } else if (Key.UnicodeChar == 0x08) { // Backspace
                    if (active_input == 1) {
                        int len = 0;
                        while (tabs[active_tab_idx].url[len]) len++;
                        if (len > 0) tabs[active_tab_idx].url[len - 1] = '\0';
                    } else if (active_input == 2) {
                        int len = 0;
                        while (tabs[active_tab_idx].search_query[len]) len++;
                        if (len > 0) tabs[active_tab_idx].search_query[len - 1] = '\0';
                    }
                } else if (Key.UnicodeChar == 0x0D) { // Enter
                    if (active_input == 1) {
                        navigate_to(active_tab_idx, tabs[active_tab_idx].url);
                        active_input = 0;
                    }
                }
            }
            
            bool mouse_click = false;
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx || dy) {
                        cursor_x += dx; cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                    }
                    
                    // Drag tracking
                    blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                    
                    if (State.LeftButton) {
                        mouse_click = true;
                    }
                }
            }
            
            // Check click hit tests
            if (mouse_click) {
                // Window Close
                if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                    done = true;
                }
                
                // Tab Selection clicks
                for (int i = 0; i < tab_count; i++) {
                    int tx = win_x + 80 + i * 140;
                    if (cursor_x >= tx && cursor_x <= tx + 120 && cursor_y >= win_y + 8 && cursor_y <= win_y + 32) {
                        active_tab_idx = i;
                        active_input = 0;
                    }
                }
                
                // Plus button (New Tab)
                int px = win_x + 80 + tab_count * 140;
                if (cursor_x >= px && cursor_x <= px + 24 && cursor_y >= win_y + 12 && cursor_y <= win_y + 32) {
                    if (tab_count < 4) {
                        str_copy(tabs[tab_count].url, (char*)"blank://home", 128);
                        str_copy(tabs[tab_count].title, "New Tab", 32);
                        tabs[tab_count].page_type = 0;
                        tabs[tab_count].search_query[0] = '\0';
                        active_tab_idx = tab_count;
                        tab_count++;
                        active_input = 0;
                    }
                }
                
                // URL input bar click
                if (cursor_x >= win_x + 90 && cursor_x <= win_x + win_w - 90 &&
                    cursor_y >= win_y + 50 && cursor_y <= win_y + 80) {
                    active_input = 1;
                }
                
                // HTML Hyperlinks clicks
                for (int i = 0; i < link_count; i++) {
                    if (cursor_x >= links[i].x && cursor_x <= links[i].x + links[i].w &&
                        cursor_y >= links[i].y && cursor_y <= links[i].y + links[i].h) {
                        navigate_to(active_tab_idx, links[i].target_url);
                        active_input = 0;
                    }
                }
                
                // Google Input field click
                if (tabs[active_tab_idx].page_type == 1) { // Google
                    int gx = win_x + win_w/2 - 200;
                    int gy = win_y + 240;
                    if (cursor_x >= gx && cursor_x <= gx + 400 && cursor_y >= gy && cursor_y <= gy + 40) {
                        active_input = 2;
                    }
                }
            }
            
            // Check link hover cursors
            bool link_hovered = false;
            for (int i = 0; i < link_count; i++) {
                if (cursor_x >= links[i].x && cursor_x <= links[i].x + links[i].w &&
                    cursor_y >= links[i].y && cursor_y <= links[i].y + links[i].h) {
                    link_hovered = true;
                }
            }
            if (link_hovered) {
                blankUI_set_cursor_type(1); // Pointer Hand
            }
            
            // Clear links list for redraw rebuild
            link_count = 0;
            
            // Draw Workspace base
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            
            // Window Base & Shadow
            dui_shadow(win_x, win_y, win_w, win_h, 12, 12, 0x000000, 60);
            dui_rect_rounded(win_x, win_y, win_w, win_h, 12, 0xFFFFFF, 255);
            
            // Window Header (Tab Bar base)
            dui_rect_rounded(win_x, win_y, win_w, 40, 12, 0xE5E5EA, 255);
            dui_rect(win_x, win_y + 20, win_w, 20, 0xE5E5EA, 255); // square bottom
            
            // Traffic lights
            dui_circle(win_x + 16, win_y + 16, 6, 0xFF5F56, 255);
            dui_circle(win_x + 36, win_y + 16, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 56, win_y + 16, 6, 0x27C93F, 255);
            
            // Render Tabs
            for (int i = 0; i < tab_count; i++) {
                int tx = win_x + 80 + i * 140;
                uint32_t tab_color = (i == active_tab_idx) ? 0xFFFFFF : 0xD1D1D6;
                dui_rect_rounded(tx, win_y + 8, 130, 32, 8, tab_color, 255);
                dui_rect(tx, win_y + 24, 130, 16, tab_color, 255); // square base
                dui_text(tx + 12, win_y + 16, tabs[i].title, 0x333333, 1);
            }
            
            // Plus button
            int px = win_x + 80 + tab_count * 140;
            dui_text(px + 6, win_y + 16, (char*)"+", 0x666666, 1);
            
            // Toolbar Base
            dui_rect(win_x, win_y + 40, win_w, 50, 0xFFFFFF, 255);
            dui_rect(win_x, win_y + 89, win_w, 1, 0xDDDDDD, 255);
            
            // Back/Forward buttons (drawn)
            dui_triangle(win_x + 25, win_y + 65, win_x + 35, win_y + 55, win_x + 35, win_y + 75, 0x999999, 255);
            dui_triangle(win_x + 65, win_y + 65, win_x + 55, win_y + 55, win_x + 55, win_y + 75, 0x999999, 255);
            
            // URL Bar
            uint32_t url_bar_color = (active_input == 1) ? 0xE5E5EA : 0xF2F2F7;
            dui_rect_rounded(win_x + 90, win_y + 50, win_w - 180, 30, 6, url_bar_color, 255);
            dui_text(win_x + 100, win_y + 58, tabs[active_tab_idx].url, 0x333333, 1);
            
            // Content Area Layout
            int content_y = win_y + 90;
            int content_h = win_h - 90;
            
            if (tabs[active_tab_idx].page_type == 0) {
                // Home start page (Safari style)
                dui_text(win_x + win_w/2 - 120, content_y + 60, "Safari Start Page", 0x111827, 2);
                
                // Show Quick links (Clickable!)
                dui_text(win_x + 80, content_y + 140, "Favorites", 0x8E8E93, 1);
                
                // Draw favorite grids
                int grid_x = win_x + 80;
                int grid_y = content_y + 170;
                
                // Google Favorite Link
                dui_rect_rounded(grid_x, grid_y, 70, 70, 12, 0xF2F2F7, 255);
                dui_text(grid_x + 22, grid_y + 25, "G", 0x4285F4, 2);
                dui_text(grid_x + 12, grid_y + 80, "Google", 0x333333, 1);
                add_link(grid_x, grid_y, 70, 90, "google.com");
                
                // GitHub Favorite Link
                dui_rect_rounded(grid_x + 110, grid_y, 70, 70, 12, 0x18171C, 255);
                dui_text(grid_x + 132, grid_y + 25, "H", 0xFFFFFF, 2);
                dui_text(grid_x + 122, grid_y + 80, "GitHub", 0x333333, 1);
                add_link(grid_x + 110, grid_y, 70, 90, "github.com");
                
                // Apple Favorite Link
                dui_rect_rounded(grid_x + 220, grid_y, 70, 70, 12, 0xF2F2F7, 255);
                dui_text(grid_x + 242, grid_y + 25, "A", 0x000000, 2);
                dui_text(grid_x + 232, grid_y + 80, "Apple", 0x333333, 1);
                add_link(grid_x + 220, grid_y, 70, 90, "apple.com");
                
            } else if (tabs[active_tab_idx].page_type == 1) {
                // Google Page Mock
                dui_text(win_x + win_w/2 - 50, content_y + 60, "Google", 0x4285F4, 3);
                
                // Search Input Field
                int gx = win_x + win_w/2 - 200;
                int gy = content_y + 120;
                uint32_t g_color = (active_input == 2) ? 0xFFFFFF : 0xF2F2F7;
                dui_rect_rounded(gx, gy, 400, 40, 20, g_color, 255);
                dui_rect_rounded_outline(gx, gy, 400, 40, 20, 0xCCCCCC, 1);
                dui_text(gx + 20, gy + 12, tabs[active_tab_idx].search_query, 0x333333, 1);
                
                // Search button
                dui_rect_rounded(win_x + win_w/2 - 60, gy + 60, 120, 36, 6, 0xE5E5EA, 255);
                dui_text(win_x + win_w/2 - 44, gy + 72, "Google Search", 0x333333, 1);
                
                // Draw mock search result if search text present
                int query_len = 0;
                while (tabs[active_tab_idx].search_query[query_len]) query_len++;
                if (query_len > 0) {
                    dui_text(win_x + 80, gy + 120, "Search results for:", 0x8E8E93, 1);
                    dui_text(win_x + 230, gy + 120, tabs[active_tab_idx].search_query, 0x007AFF, 1);
                    
                    dui_text(win_x + 80, gy + 150, "1. BlankOS Monolithic operating system repository on GitHub", 0x007AFF, 1);
                    add_link(win_x + 80, gy + 150, 400, 20, "github.com");
                    
                    dui_text(win_x + 80, gy + 174, "2. Apple Safari and macOS official store design", 0x007AFF, 1);
                    add_link(win_x + 80, gy + 174, 400, 20, "apple.com");
                }
                
            } else if (tabs[active_tab_idx].page_type == 2) {
                // GitHub Mock
                dui_rect(win_x + 40, content_y + 30, win_w - 80, 40, 0xF6F8FA, 255);
                dui_text(win_x + 60, content_y + 40, "GitHub - airskye1 / blankOS----Vibe-Coded-OS", 0x24292F, 2);
                
                dui_text(win_x + 60, content_y + 90, "About the project:", 0x8E8E93, 1);
                dui_text(win_x + 60, content_y + 114, "BlankOS is a highly customized monolithic UEFI Operating System designed with", 0x24292F, 1);
                dui_text(win_x + 60, content_y + 134, "buttery-smooth macOS inspired transitions, custom vector drawing compositor,", 0x24292F, 1);
                dui_text(win_x + 60, content_y + 154, "hardware exception panics, and an advanced dynamic ELF loader platform.", 0x24292F, 1);
                
                // Code commits list
                dui_text(win_x + 60, content_y + 200, "Latest Commits:", 0x24292F, 2);
                dui_text(win_x + 60, content_y + 230, "* Overhaul: Added multi-state cursor system, real IDT and BSOD catcher", 0x34C759, 1);
                dui_text(win_x + 60, content_y + 250, "* Feature: Integrated shareware Doom 3D game port natively", 0x333333, 1);
                dui_text(win_x + 60, content_y + 270, "* Graphics: V-Sync locks and screen tearing composition fix", 0x333333, 1);
                
                // Clickable back to home
                dui_text(win_x + 60, content_y + 320, "<< Back to Start Page", 0x007AFF, 1);
                add_link(win_x + 60, content_y + 320, 200, 20, "blank://home");
                
            } else if (tabs[active_tab_idx].page_type == 3) {
                // Apple page Mock
                dui_rect(win_x + 40, content_y + 20, win_w - 80, 80, 0x1A1A1A, 255);
                dui_text(win_x + win_w/2 - 60, content_y + 45, "Apple Store", 0xFFFFFF, 3);
                
                dui_text(win_x + 60, content_y + 130, "macOS Sequoia - Elevate your productivity.", 0x111827, 2);
                dui_text(win_x + 60, content_y + 160, "Featuring hardware accelerated graphics pipeline and dynamic TrueType rendering.", 0x48484A, 1);
                
                // Apple Card Grid
                int ax = win_x + 60;
                int ay = content_y + 200;
                dui_rect_rounded(ax, ay, 200, 150, 16, 0xF5F5F7, 255);
                dui_text(ax + 20, ay + 20, "MacBook Pro", 0x111827, 2);
                dui_text(ax + 20, ay + 50, "Mind-blowing M3 Max.", 0x8E8E93, 1);
                
                dui_rect_rounded(ax + 220, ay, 200, 150, 16, 0x000000, 255); // Dark card
                dui_text(ax + 240, ay + 20, "Apple Vision Pro", 0xFFFFFF, 2);
                dui_text(ax + 240, ay + 50, "Welcome to spatial computing.", 0x8E8E93, 1);
                
                dui_text(win_x + 60, content_y + 380, "<< Return to Safari Start Page", 0x007AFF, 1);
                add_link(win_x + 60, content_y + 380, 260, 20, "blank://home");
            }
            
            // Draw desktop cursor & swap buffers
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            
            SystemTable->BootServices->Stall(16000); // 60 FPS
        }
    }
}
