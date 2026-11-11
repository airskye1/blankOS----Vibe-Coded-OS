#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

static void u64_to_str(uint64_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[24];
    int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void read_cpuid_brand(char* out_brand) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
    if (eax >= 0x80000004) {
        uint32_t* p = (uint32_t*)out_brand;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            __asm__ volatile("cpuid" : "=a"(p[0]), "=b"(p[1]), "=c"(p[2]), "=d"(p[3]) : "a"(leaf));
            p += 4;
        }
        out_brand[48] = '\0';
        // Trim leading spaces
        int s = 0;
        while (out_brand[s] == ' ') s++;
        if (s > 0) {
            int i = 0;
            while (out_brand[s + i]) { out_brand[i] = out_brand[s + i]; i++; }
            out_brand[i] = '\0';
        }
    } else {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
        uint32_t* v = (uint32_t*)out_brand;
        v[0] = ebx; v[1] = edx; v[2] = ecx;
        out_brand[12] = '\0';
    }
}

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern int screen_width;
    extern int screen_height;
    extern int g_wallpaper_theme;
    extern bool g_show_desktop_widgets;
    extern bool g_show_dynamic_island;
    extern bool set_screen_resolution(uint32_t target_w, uint32_t target_h);
    extern void play_system_sound(char* sound_name);
    extern void set_master_volume(float volume);
    extern float get_master_volume(void);
    extern void toggle_audio_mute(void);
    extern bool is_audio_muted(void);

    extern uint64_t memory_get_total_mb(void);
    extern uint64_t memory_get_used_mb(void);
    extern int pci_get_device_count(void);
    extern uint32_t svga_get_vram_size(void);
    extern const char* net_get_adapter_name(void);
    extern const char* net_get_mac_str(void);
    extern const char* net_get_ipv4_str(void);
    extern void launch_task_manager(void);
    extern void launch_blankreg_edit(void);
    extern void blankReg_set_int(char* key, int val);
    extern bool blankReg_save(EFI_SYSTEM_TABLE *st);

    void launch_sysinfo(void) {
        extern EFI_SYSTEM_TABLE *gSystemTable;
        EFI_SYSTEM_TABLE *SystemTable = gSystemTable;
        if (!SystemTable) return;

        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        char cpu_brand[64];
        for (int i = 0; i < 64; i++) cpu_brand[i] = 0;
        read_cpuid_brand(cpu_brand);

        char fw_vendor[48];
        int fw_i = 0;
        if (SystemTable->FirmwareVendor) {
            while (SystemTable->FirmwareVendor[fw_i] && fw_i < 46) {
                fw_vendor[fw_i] = (char)(SystemTable->FirmwareVendor[fw_i] & 0x7F);
                fw_i++;
            }
        }
        fw_vendor[fw_i] = '\0';

        // Probe real BlockIO disks once on open
        uint64_t disk_sizes_mb[6];
        uint32_t disk_block_sizes[6];
        int disk_count = 0;
        EFI_GUID bioGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handles = NULL;
        if (!EFI_ERROR(SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &bioGuid, NULL, &numHandles, &handles))) {
            for (UINTN i = 0; i < numHandles && disk_count < 6; i++) {
                EFI_BLOCK_IO_PROTOCOL *bio = NULL;
                if (!EFI_ERROR(SystemTable->BootServices->HandleProtocol(handles[i], &bioGuid, (void**)&bio)) && bio && bio->Media) {
                    if (!bio->Media->LogicalPartition && bio->Media->LastBlock > 0) {
                        uint64_t bytes = (bio->Media->LastBlock + 1) * (uint64_t)bio->Media->BlockSize;
                        disk_sizes_mb[disk_count] = bytes / (1024 * 1024);
                        disk_block_sizes[disk_count] = bio->Media->BlockSize;
                        disk_count++;
                    }
                }
            }
            if (handles) SystemTable->BootServices->FreePool(handles);
        }

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        int active_tab = 0;
        bool done = false;

        int win_w = 740;
        int win_h = 490;
        int win_x = (screen_width - win_w) / 2;
        int win_y = (screen_height - win_h) / 2;
        int sidebar_w = 185;

        while (!done) {
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) { done = true; } // ESC
            }

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
                    if (State.LeftButton) {
                        // Close traffic light
                        int cx_btn = win_x + 18, cy_btn = win_y + 19;
                        if ((cursor_x - cx_btn)*(cursor_x - cx_btn) + (cursor_y - cy_btn)*(cursor_y - cy_btn) <= 64) {
                            play_system_sound((char*)"click");
                            done = true;
                        }

                        // Sidebar tab clicks (5 tabs)
                        int sx = win_x; int sy = win_y + 48;
                        if (cursor_x >= sx && cursor_x <= sx + sidebar_w) {
                            for (int t = 0; t < 5; t++) {
                                int ty = sy + t * 40;
                                if (cursor_y >= ty && cursor_y < ty + 36) {
                                    active_tab = t;
                                    play_system_sound((char*)"click");
                                    break;
                                }
                            }
                        }

                        int content_x = win_x + sidebar_w + 20;
                        int content_y = win_y + 52;

                        if (active_tab == 0) {
                            // Displays tab: 4 resolution presets
                            const uint32_t res_w[4] = {1024, 1280, 1440, 1920};
                            const uint32_t res_h[4] = {768,  720,  900,  1080};
                            for (int r = 0; r < 4; r++) {
                                int rx = content_x + (r % 2) * 230;
                                int ry = content_y + 56 + (r / 2) * 48;
                                if (cursor_x >= rx && cursor_x <= rx + 210 && cursor_y >= ry && cursor_y <= ry + 38) {
                                    if (set_screen_resolution(res_w[r], res_h[r])) {
                                        win_x = (screen_width - win_w) / 2;
                                        win_y = (screen_height - win_h) / 2;
                                        blankReg_set_int((char*)"Display/Width", (int)res_w[r]);
                                        blankReg_set_int((char*)"Display/Height", (int)res_h[r]);
                                        blankReg_save(SystemTable);
                                    }
                                }
                            }
                        } else if (active_tab == 1) {
                            // Wallpaper & Twist tab
                            for (int th = 0; th < 4; th++) {
                                int card_x = content_x + (th % 2) * 230;
                                int card_y = content_y + 48 + (th / 2) * 100;
                                if (cursor_x >= card_x && cursor_x <= card_x + 210 &&
                                    cursor_y >= card_y && cursor_y <= card_y + 84) {
                                    g_wallpaper_theme = th;
                                    blankReg_set_int((char*)"UI/WallpaperTheme", th);
                                    blankReg_save(SystemTable);
                                    play_system_sound((char*)"click");
                                    break;
                                }
                            }
                            // Toggle Dynamic Island
                            if (cursor_x >= content_x && cursor_x <= content_x + 220 &&
                                cursor_y >= content_y + 265 && cursor_y <= content_y + 305) {
                                g_show_dynamic_island = !g_show_dynamic_island;
                                blankReg_set_int((char*)"UI/DynamicIsland", g_show_dynamic_island ? 1 : 0);
                                blankReg_save(SystemTable);
                                play_system_sound((char*)"click");
                            }
                            // Toggle Desktop Glass Widgets
                            if (cursor_x >= content_x + 235 && cursor_x <= content_x + 465 &&
                                cursor_y >= content_y + 265 && cursor_y <= content_y + 305) {
                                g_show_desktop_widgets = !g_show_desktop_widgets;
                                blankReg_set_int((char*)"UI/DesktopWidgets", g_show_desktop_widgets ? 1 : 0);
                                blankReg_save(SystemTable);
                                play_system_sound((char*)"click");
                            }
                        } else if (active_tab == 2) {
                            // Audio tab
                            int vol_y = content_y + 50;
                            const float vols[4] = {0.25f, 0.50f, 0.75f, 1.00f};
                            for (int v = 0; v < 4; v++) {
                                int vx = content_x + v * 80;
                                if (cursor_x >= vx && cursor_x <= vx + 70 &&
                                    cursor_y >= vol_y && cursor_y <= vol_y + 34) {
                                    set_master_volume(vols[v]);
                                    blankReg_set_int((char*)"Audio/MasterVolume", (int)(vols[v] * 100));
                                    blankReg_save(SystemTable);
                                    play_system_sound((char*)"click");
                                }
                            }
                            int mute_x = content_x + 330;
                            if (cursor_x >= mute_x && cursor_x <= mute_x + 90 &&
                                cursor_y >= vol_y && cursor_y <= vol_y + 34) {
                                toggle_audio_mute();
                                play_system_sound((char*)"click");
                            }

                            int test_y = content_y + 140;
                            const char* s_names[5] = {"startup", "notification", "update", "ambient", "error"};
                            for (int s = 0; s < 5; s++) {
                                int sx_btn = content_x + (s % 3) * 150;
                                int sy_btn = test_y + (s / 3) * 50;
                                if (cursor_x >= sx_btn && cursor_x <= sx_btn + 140 &&
                                    cursor_y >= sy_btn && cursor_y <= sy_btn + 38) {
                                    play_system_sound((char*)s_names[s]);
                                }
                            }
                        } else if (active_tab == 4) {
                            // About tab quick launch buttons
                            if (cursor_x >= content_x && cursor_x <= content_x + 210 &&
                                cursor_y >= content_y + 340 && cursor_y <= content_y + 378) {
                                play_system_sound((char*)"click");
                                launch_task_manager();
                            }
                            if (cursor_x >= content_x + 225 && cursor_x <= content_x + 435 &&
                                cursor_y >= content_y + 340 && cursor_y <= content_y + 378) {
                                play_system_sound((char*)"click");
                                launch_blankreg_edit();
                            }
                        }
                    }
                }
            }

            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();

            dui_shadow(win_x, win_y, win_w, win_h, 14, 10, 0x000000, 70);
            dui_rect_rounded(win_x, win_y, win_w, win_h, 14, 0xF5F5F7, 245);
            dui_rect(win_x, win_y, win_w, 38, 0xE8E8ED, 255);
            dui_circle(win_x + 18, win_y + 19, 6, 0xFF5F56, 255);
            dui_circle(win_x + 38, win_y + 19, 6, 0xFFBD2E, 255);
            dui_circle(win_x + 58, win_y + 19, 6, 0x27C93F, 255);
            dui_text(win_x + win_w / 2 - dui_text_width("System Settings", 1) / 2, win_y + 14, "System Settings", 0x2C2C2E, 1);

            dui_rect(win_x, win_y + 38, sidebar_w, win_h - 38, 0xE8E8ED, 240);
            dui_line(win_x + sidebar_w, win_y + 38, win_x + sidebar_w, win_y + win_h, 0xD1D1D6, 1);

            const char* tabs[5] = {"Displays & GPU", "Wallpaper & Twist", "Audio & Sound", "Network & Disks", "About BlankBook"};
            for (int i = 0; i < 5; i++) {
                int ty = win_y + 48 + i * 40;
                if (i == active_tab) {
                    dui_rect_rounded(win_x + 8, ty, sidebar_w - 16, 34, 8, 0x007AFF, 255);
                    dui_text(win_x + 18, ty + 10, tabs[i], 0xFFFFFF, 1);
                } else {
                    dui_text(win_x + 18, ty + 10, tabs[i], 0x333333, 1);
                }
            }

            int cx = win_x + sidebar_w + 20;
            int cy = win_y + 52;

            if (active_tab == 0) {
                dui_text(cx, cy, "Display & BDRM Graphics", 0x111111, 2);
                dui_text(cx, cy + 30, "Switch UEFI GOP / SVGA II resolution dynamically:", 0x666666, 1);

                const uint32_t res_w[4] = {1024, 1280, 1440, 1920};
                const uint32_t res_h[4] = {768,  720,  900,  1080};
                const char* res_lbl[4]  = {"1024 x 768 (XGA)", "1280 x 720 (HD)", "1440 x 900 (WXGA+)", "1920 x 1080 (FHD)"};
                for (int r = 0; r < 4; r++) {
                    int rx = cx + (r % 2) * 230;
                    int ry = cy + 56 + (r / 2) * 48;
                    bool is_cur = ((uint32_t)screen_width == res_w[r] && (uint32_t)screen_height == res_h[r]);
                    dui_rect_rounded(rx, ry, 210, 38, 8, is_cur ? 0x007AFF : 0x2C2C2E, 255);
                    dui_text(rx + 18, ry + 12, res_lbl[r], 0xFFFFFF, 1);
                }

                dui_rect_rounded(cx, cy + 168, 490, 190, 10, 0xFFFFFF, 235);
                dui_rect_rounded_outline(cx, cy + 168, 490, 190, 10, 0xD1D1D6, 1);
                dui_text(cx + 16, cy + 184, "Active Framebuffer & GPU Telemetry:", 0x111111, 1);

                char fb_str[64];
                int p = 0;
                const char* f1 = "Current Mode: ";
                while (*f1) fb_str[p++] = *f1++;
                char wbuf[12], hbuf[12], vbuf[16];
                u64_to_str(screen_width, wbuf);
                u64_to_str(screen_height, hbuf);
                u64_to_str(svga_get_vram_size() / (1024 * 1024), vbuf);
                int k = 0; while (wbuf[k]) fb_str[p++] = wbuf[k++];
                fb_str[p++] = 'x';
                k = 0; while (hbuf[k]) fb_str[p++] = hbuf[k++];
                const char* f2 = " @ 32-bit BGRA  |  VRAM: ";
                while (*f2) fb_str[p++] = *f2++;
                k = 0; while (vbuf[k]) fb_str[p++] = vbuf[k++];
                fb_str[p++] = ' '; fb_str[p++] = 'M'; fb_str[p++] = 'B'; fb_str[p] = '\0';

                dui_text(cx + 16, cy + 214, fb_str, 0x007AFF, 1);
                dui_text(cx + 16, cy + 240, "Scanout Engine: Pitch-Aware 64-bit Double Buffer Blitter", 0x48484A, 1);
                dui_text(cx + 16, cy + 264, "Hardware Driver: VMware SVGA II FIFO / Bochs VBE / UEFI GOP", 0x48484A, 1);
                dui_text(cx + 16, cy + 288, "Vector Compositor: BlankDUI Anti-Aliased TrueType Engine", 0x48484A, 1);
                dui_text(cx + 16, cy + 312, "Registry Persistence: Saved automatically to REGISTRY.INI", 0x248A3D, 1);

            } else if (active_tab == 1) {
                dui_text(cx, cy, "Wallpaper & BlankOS Twist", 0x111111, 2);
                dui_text(cx, cy + 28, "Customize macOS Sequoia glass aesthetics & desktop widgets:", 0x666666, 1);

                const char* theme_titles[4] = {"Sequoia Indigo", "Solar Sunset", "Cyberpunk Neon", "Titanium Slate"};
                const char* theme_descs[4]  = {"Deep indigo & warm rose", "Plum & golden horizon", "Midnight & neon aqua", "Pro monochrome dark"};
                uint32_t theme_swatches[4]  = {0x5B21B6, 0xE65100, 0x00F5D4, 0x2C2C2E};

                for (int th = 0; th < 4; th++) {
                    int card_x = cx + (th % 2) * 235;
                    int card_y = cy + 48 + (th / 2) * 100;
                    bool active = (g_wallpaper_theme == th);

                    dui_shadow(card_x, card_y, 215, 84, 8, 4, 0x000000, 30);
                    dui_rect_rounded(card_x, card_y, 215, 84, 8, active ? 0xE8F0FE : 0xFFFFFF, 255);
                    dui_rect_rounded_outline(card_x, card_y, 215, 84, 8, active ? 0x007AFF : 0xD1D1D6, active ? 2 : 1);

                    dui_circle(card_x + 24, card_y + 28, 12, theme_swatches[th], 255);
                    dui_text(card_x + 46, card_y + 16, theme_titles[th], 0x111111, 1);
                    dui_text(card_x + 46, card_y + 36, theme_descs[th], 0x777777, 1);
                    if (active) dui_text(card_x + 46, card_y + 58, "ACTIVE THEME", 0x007AFF, 1);
                }

                // Twist Toggles
                dui_rect_rounded(cx, cy + 265, 220, 40, 10, g_show_dynamic_island ? 0x007AFF : 0x8E8E93, 255);
                dui_text(cx + 14, cy + 278, g_show_dynamic_island ? "BlankIsland Capsule: ON" : "BlankIsland Capsule: OFF", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 235, cy + 265, 230, 40, 10, g_show_desktop_widgets ? 0x34C759 : 0x8E8E93, 255);
                dui_text(cx + 249, cy + 278, g_show_desktop_widgets ? "Desktop Glass Widgets: ON" : "Desktop Glass Widgets: OFF", 0xFFFFFF, 1);

            } else if (active_tab == 2) {
                dui_text(cx, cy, "Audio & HDA Synthesizer", 0x111111, 2);
                dui_text(cx, cy + 28, "Master Output Gain & Polyphonic PCM Test:", 0x555555, 1);

                float cur_vol = get_master_volume();
                bool muted = is_audio_muted();

                dui_rect_rounded(cx, cy + 50, 70, 34, 6, (!muted && cur_vol <= 0.3f) ? 0x007AFF : 0x3A3A3C, 255);
                dui_text(cx + 20, cy + 60, "25%", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 80, cy + 50, 70, 34, 6, (!muted && cur_vol > 0.3f && cur_vol <= 0.6f) ? 0x007AFF : 0x3A3A3C, 255);
                dui_text(cx + 100, cy + 60, "50%", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 160, cy + 50, 70, 34, 6, (!muted && cur_vol > 0.6f && cur_vol <= 0.85f) ? 0x007AFF : 0x3A3A3C, 255);
                dui_text(cx + 180, cy + 60, "75%", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 240, cy + 50, 70, 34, 6, (!muted && cur_vol > 0.85f) ? 0x007AFF : 0x3A3A3C, 255);
                dui_text(cx + 254, cy + 60, "100%", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 330, cy + 50, 90, 34, 6, muted ? 0xFF3B30 : 0x8E8E93, 255);
                dui_text(cx + 348, cy + 60, muted ? "MUTED" : "MUTE", 0xFFFFFF, 1);

                dui_text(cx, cy + 110, "Trigger Live Harmonic Sound Profiles:", 0x111111, 2);

                dui_rect_rounded(cx, cy + 140, 140, 38, 8, 0x007AFF, 255);
                dui_text(cx + 14, cy + 152, "Startup Chime", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 150, cy + 140, 140, 38, 8, 0x007AFF, 255);
                dui_text(cx + 172, cy + 152, "Notification", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 300, cy + 140, 140, 38, 8, 0x34C759, 255);
                dui_text(cx + 314, cy + 152, "Update Fanfare", 0xFFFFFF, 1);

                dui_rect_rounded(cx, cy + 190, 140, 38, 8, 0x5856D6, 255);
                dui_text(cx + 18, cy + 202, "Ambient Synth", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 150, cy + 190, 140, 38, 8, 0xFF3B30, 255);
                dui_text(cx + 174, cy + 202, "Alert Tone", 0xFFFFFF, 1);

            } else if (active_tab == 3) {
                dui_text(cx, cy, "Network & Physical Storage", 0x111111, 2);

                // Network Card
                dui_rect_rounded(cx, cy + 36, 490, 118, 10, 0xFFFFFF, 240);
                dui_rect_rounded_outline(cx, cy + 36, 490, 118, 10, 0xD1D1D6, 1);
                dui_text(cx + 16, cy + 50, "Detected PCIe Network Controller:", 0x8E8E93, 1);
                dui_text(cx + 16, cy + 70, net_get_adapter_name(), 0x111111, 1);

                char net_line[96];
                int p = 0;
                const char* m1 = "Hardware MAC: ";
                while (*m1) net_line[p++] = *m1++;
                const char* mac = net_get_mac_str();
                while (*mac) net_line[p++] = *mac++;
                const char* m2 = "   |   IPv4: ";
                while (*m2) net_line[p++] = *m2++;
                const char* ip = net_get_ipv4_str();
                while (*ip) net_line[p++] = *ip++;
                net_line[p] = '\0';
                dui_text(cx + 16, cy + 96, net_line, 0x007AFF, 1);
                dui_text(cx + 16, cy + 120, "Link Status: Full-Duplex Active (UEFI SNP / PCIe MMIO)", 0x248A3D, 1);

                // BlockIO Disks Card
                dui_text(cx, cy + 172, "Attached UEFI BlockIO Storage Volumes:", 0x111111, 1);
                for (int d = 0; d < disk_count; d++) {
                    int dy = cy + 198 + d * 48;
                    dui_rect_rounded(cx, dy, 490, 42, 8, 0xFFFFFF, 235);
                    dui_rect_rounded_outline(cx, dy, 490, 42, 8, 0xD1D1D6, 1);

                    char dline[96];
                    int dp = 0;
                    const char* dl1 = "Physical Drive #";
                    while (*dl1) dline[dp++] = *dl1++;
                    dline[dp++] = '0' + d;
                    const char* dl2 = "  —  Capacity: ";
                    while (*dl2) dline[dp++] = *dl2++;
                    char mb_str[16], bs_str[12];
                    u64_to_str(disk_sizes_mb[d], mb_str);
                    u64_to_str(disk_block_sizes[d], bs_str);
                    int k = 0; while (mb_str[k]) dline[dp++] = mb_str[k++];
                    const char* dl3 = " MB  (Block Size: ";
                    while (*dl3) dline[dp++] = *dl3++;
                    k = 0; while (bs_str[k]) dline[dp++] = bs_str[k++];
                    const char* dl4 = " B)";
                    while (*dl4) dline[dp++] = *dl4++;
                    dline[dp] = '\0';
                    dui_text(cx + 16, dy + 14, dline, 0x2C2C2E, 1);
                }

            } else {
                // About BlankBook (Real CPUID, RAM, Firmware, PCI count)
                dui_text(cx, cy, "BlankBook Pro — BlankOS Sequoia", 0x111111, 2);
                dui_text(cx, cy + 28, "macOS Inspired Architecture with Bare-Metal UEFI Twist", 0x007AFF, 1);

                dui_rect_rounded(cx, cy + 56, 490, 266, 12, 0xFFFFFF, 240);
                dui_rect_rounded_outline(cx, cy + 56, 490, 266, 12, 0xD1D1D6, 1);

                dui_text(cx + 18, cy + 74, "Processor (CPUID):", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 74, cpu_brand, 0x111111, 1);

                char ram_str[64];
                int rp = 0;
                char tmb[16], umb[16];
                u64_to_str(memory_get_total_mb(), tmb);
                u64_to_str(memory_get_used_mb(), umb);
                int k = 0; while (tmb[k]) ram_str[rp++] = tmb[k++];
                const char* rl1 = " MB Physical RAM (";
                while (*rl1) ram_str[rp++] = *rl1++;
                k = 0; while (umb[k]) ram_str[rp++] = umb[k++];
                const char* rl2 = " MB Kernel Heap Used)";
                while (*rl2) ram_str[rp++] = *rl2++;
                ram_str[rp] = '\0';

                dui_text(cx + 18, cy + 106, "Memory (UEFI Map):", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 106, ram_str, 0x111111, 1);

                dui_text(cx + 18, cy + 138, "UEFI Firmware:", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 138, fw_vendor[0] ? fw_vendor : "UEFI x86_64 Compliant Firmware", 0x111111, 1);

                char pci_str[48];
                char pcibuf[12];
                u64_to_str((uint64_t)pci_get_device_count(), pcibuf);
                int pp = 0;
                k = 0; while (pcibuf[k]) pci_str[pp++] = pcibuf[k++];
                const char* pl1 = " Active PCI/PCIe Hardware Functions Bound";
                while (*pl1) pci_str[pp++] = *pl1++;
                pci_str[pp] = '\0';

                dui_text(cx + 18, cy + 170, "PCIe Bus Devices:", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 170, pci_str, 0x248A3D, 1);

                dui_text(cx + 18, cy + 202, "Network Adapter:", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 202, net_get_adapter_name(), 0x111111, 1);

                dui_text(cx + 18, cy + 234, "Graphics Engine:", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 234, "BlankDUI QuartzGlass + BDRM Direct Scanout", 0x111111, 1);

                dui_text(cx + 18, cy + 266, "Crypto & Storage:", 0x8E8E93, 1);
                dui_text(cx + 175, cy + 266, "FIPS SHA-256 + GPT/FAT32 VFS Persistence", 0x111111, 1);

                // Quick launch buttons
                dui_rect_rounded(cx, cy + 340, 210, 38, 10, 0x007AFF, 255);
                dui_text(cx + 22, cy + 352, "Open Activity Monitor", 0xFFFFFF, 1);

                dui_rect_rounded(cx + 225, cy + 340, 210, 38, 10, 0x1C1C1E, 255);
                dui_text(cx + 247, cy + 352, "Open Registry Studio", 0xFFFFFF, 1);
            }

            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }
    }
}
