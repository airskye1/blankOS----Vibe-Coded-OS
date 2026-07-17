#include <stdbool.h>
#include <stdint.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_gradient_rounded(int x, int y, int w, int h, int radius, uint32_t top_color, uint32_t bottom_color);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern int screen_width;
    extern int screen_height;

    void launch_sysinfo(void) {
        // We need EFI for input, but launch_sysinfo has no SystemTable param.
        // Use the global one.
        extern EFI_SYSTEM_TABLE *gSystemTable;
        EFI_SYSTEM_TABLE *SystemTable = gSystemTable;
        if (!SystemTable) return;

        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        int active_tab = 0;
        bool done = false;

        int win_w = 700;
        int win_h = 460;
        int win_x = (screen_width - win_w) / 2;
        int win_y = (screen_height - win_h) / 2;
        int sidebar_w = 160;

        while (!done) {
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) { done = true; } // ESC
            }

            bool redraw = false;
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
                        redraw = true;
                    }
                    if (State.LeftButton) {
                        // Sidebar clicks
                        int sx = win_x; int sy = win_y + 40;
                        if (cursor_x >= sx && cursor_x <= sx + sidebar_w) {
                            if (cursor_y >= sy && cursor_y < sy + 36) active_tab = 0;
                            else if (cursor_y >= sy + 36 && cursor_y < sy + 72) active_tab = 1;
                            else if (cursor_y >= sy + 72 && cursor_y < sy + 108) active_tab = 2;
                        }
                        // Close button
                        int cx = win_x + 16, cy = win_y + 16;
                        if ((cursor_x - cx)*(cursor_x - cx) + (cursor_y - cy)*(cursor_y - cy) <= 64) done = true;
                        redraw = true;
                    }
                }
            }

            // Always redraw for responsiveness
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();

            // Window shadow
            dui_shadow(win_x, win_y, win_w, win_h, 12, 8, 0x000000, 60);
            // Window body
            dui_rect_rounded(win_x, win_y, win_w, win_h, 12, 0xF5F5F7, 245);
            // Title bar
            dui_rect(win_x, win_y, win_w, 36, 0xE8E8ED, 255);
            // Traffic lights
            dui_circle(win_x + 16, win_y + 18, 6, 0xFF5F56, 255);
            dui_circle(win_x + 36, win_y + 18, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 56, win_y + 18, 6, 0x27C93F, 255);
            dui_text(win_x + win_w / 2 - dui_text_width("System Settings", 1) / 2, win_y + 12, "System Settings", 0x333333, 1);

            // Sidebar
            dui_rect(win_x, win_y + 36, sidebar_w, win_h - 36, 0xE8E8ED, 240);

            const char* tabs[] = {"Displays", "GPU & Rendering", "About BlankOS"};
            for (int i = 0; i < 3; i++) {
                int ty = win_y + 44 + i * 36;
                if (i == active_tab) {
                    dui_rect_rounded(win_x + 4, ty, sidebar_w - 8, 32, 6, 0x007AFF, 255);
                    dui_text(win_x + 12, ty + 10, tabs[i], 0xFFFFFF, 1);
                } else {
                    dui_text(win_x + 12, ty + 10, tabs[i], 0x333333, 1);
                }
            }

            // Content area
            int cx = win_x + sidebar_w + 20;
            int cy = win_y + 56;

            if (active_tab == 0) {
                dui_text(cx, cy, "Display Resolution", 0x000000, 2);
                dui_text(cx, cy + 30, "Select a resolution. Requires reboot.", 0x888888, 1);
                dui_rect_rounded(cx, cy + 50, 130, 36, 8, 0x007AFF, 255);
                dui_text(cx + 12, cy + 62, "1920 x 1080", 0xFFFFFF, 1);
                dui_rect_rounded(cx + 140, cy + 50, 130, 36, 8, 0x3A3A3C, 255);
                dui_text(cx + 152, cy + 62, "1024 x 768", 0xFFFFFF, 1);
                dui_rect_rounded(cx + 280, cy + 50, 130, 36, 8, 0x3A3A3C, 255);
                dui_text(cx + 296, cy + 62, "800 x 600", 0xFFFFFF, 1);

                dui_text(cx, cy + 120, "Refresh Rate", 0x000000, 2);
                dui_rect_rounded(cx, cy + 150, 100, 36, 8, 0x007AFF, 255);
                dui_text(cx + 24, cy + 162, "60 Hz", 0xFFFFFF, 1);
                dui_rect_rounded(cx + 110, cy + 150, 100, 36, 8, 0x3A3A3C, 255);
                dui_text(cx + 130, cy + 162, "144 Hz", 0xFFFFFF, 1);

            } else if (active_tab == 1) {
                dui_text(cx, cy, "GPU Hardware Acceleration", 0x000000, 2);
                dui_text(cx, cy + 30, "Driver: BDRM (BlankOS Direct Rendering Manager)", 0x888888, 1);
                dui_text(cx, cy + 50, "Status: Hardware Blitting Active", 0x008800, 1);
                dui_text(cx, cy + 80, "Double Buffering: Enabled", 0x333333, 1);
                dui_text(cx, cy + 100, "Compositor: blankDUI v1.0", 0x333333, 1);
                dui_text(cx, cy + 120, "Framebuffer Format: BGRA 32-bit", 0x333333, 1);

                dui_rect_rounded(cx, cy + 160, 220, 36, 8, 0x3A3A3C, 255);
                dui_text(cx + 12, cy + 172, "Toggle V-Sync", 0xFFFFFF, 1);
                dui_rect_rounded(cx + 230, cy + 160, 220, 36, 8, 0x3A3A3C, 255);
                dui_text(cx + 242, cy + 172, "Toggle Anti-Aliasing", 0xFFFFFF, 1);

            } else {
                dui_text(cx, cy, "BlankOS", 0x000000, 2);
                dui_text(cx, cy + 30, "Vibe Coded Edition v1.2.9", 0x888888, 1);
                dui_text(cx, cy + 60, "Kernel: Custom Monolithic UEFI", 0x333333, 1);
                dui_text(cx, cy + 80, "Architecture: x86_64 Bare Metal", 0x333333, 1);
                dui_text(cx, cy + 100, "Graphics: BDRM + blankDUI", 0x333333, 1);
                dui_text(cx, cy + 120, "UI Framework: blankUI", 0x333333, 1);
                dui_text(cx, cy + 140, "Audio: PIT PC Speaker Driver", 0x333333, 1);
                dui_text(cx, cy + 170, "Built with love. No stdlib. No libc.", 0x007AFF, 1);
            }

            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
        }
    }
}

