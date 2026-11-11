#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

// Framebuffer Info struct matching boot.c
typedef struct {
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
} FramebufferInfo;

extern "C" {
    EFI_SYSTEM_TABLE *gSystemTable = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop = NULL;

    extern volatile uint32_t* framebuffer;
    extern uint32_t* backbuffer;
    extern int screen_width;
    extern int screen_height;
    extern int pixels_per_scanline;
    extern int last_mouse_x;
    extern int last_mouse_y;
    extern bool g_is_live_cd;
    extern int g_wallpaper_theme;
    extern bool g_show_desktop_widgets;
    extern bool g_show_dynamic_island;
    extern bool apple_menu_open;
    extern bool island_expanded;

    extern void init_compositor(EFI_SYSTEM_TABLE *SystemTable);
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern int blankUI_hit_test_dock(int cursor_x, int cursor_y);

    extern void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_updater(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_app_store(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_loading_screen(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_login_screen(EFI_SYSTEM_TABLE *SystemTable);
    extern bool is_os_installed(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_sysinfo(void);
    extern void launch_calculator(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_weather(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_browser(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_terminal(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_blankpad(void);
    extern void launch_task_manager(void);
    extern void launch_blankreg_edit(void);
    extern void blankOS_panic(const char* error_code, const char* details);

    extern void memory_init_efi(EFI_SYSTEM_TABLE *SystemTable);
    extern void blankReg_init(void);
    extern bool blankReg_load(EFI_SYSTEM_TABLE *st);
    extern int blankReg_get_int(const char* key, int default_val);
    extern void net_init_hardware(EFI_SYSTEM_TABLE *SystemTable);
    extern const char* net_get_adapter_name(void);
    extern const char* net_get_mac_str(void);
    extern void bdrm_init(void);
    extern void vfs_init(EFI_SYSTEM_TABLE *st);
    extern void os_restart(void);
    extern void os_shutdown(void);

    extern bool wifi_menu_open;
    extern bool bt_menu_open;
    extern bool battery_menu_open;
    extern bool rclick_menu_open;
    extern int rclick_menu_x;
    extern int rclick_menu_y;

    extern bool connect_to_wifi(const char* ssid, const char* password);
    extern bool pair_bluetooth_device(const char* name);

    extern bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename);
    extern void pci_enumerate(EFI_SYSTEM_TABLE *SystemTable);
    extern void sync_ntp_time(EFI_SYSTEM_TABLE *SystemTable);
    extern void play_system_sound(char* sound_name);

    static char boot_log[4096];
    static int boot_log_pos = 0;

    static void klog(const char* msg) {
        while (*msg && boot_log_pos < 4090) {
            boot_log[boot_log_pos++] = *msg++;
        }
        boot_log[boot_log_pos++] = '\n';
        boot_log[boot_log_pos] = '\0';
    }

    const char* kernel_get_boot_log(void) {
        return boot_log;
    }

    bool set_screen_resolution(uint32_t target_w, uint32_t target_h) {
        if (!g_gop || !gSystemTable) return false;

        UINT32 target_mode = 0xFFFFFFFF;
        for (UINT32 i = 0; i < g_gop->Mode->MaxMode; i++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
            UINTN sizeOfInfo = 0;
            EFI_STATUS st = g_gop->QueryMode(g_gop, i, &sizeOfInfo, &info);
            if (st == EFI_SUCCESS && info != NULL) {
                if (info->HorizontalResolution == target_w && info->VerticalResolution == target_h) {
                    target_mode = i;
                    gSystemTable->BootServices->FreePool(info);
                    break;
                }
                gSystemTable->BootServices->FreePool(info);
            }
        }

        if (target_mode == 0xFFFFFFFF) return false;

        EFI_STATUS status = g_gop->SetMode(g_gop, target_mode);
        if (EFI_ERROR(status)) return false;

        framebuffer = (volatile uint32_t*)(uintptr_t)g_gop->Mode->FrameBufferBase;
        screen_width = g_gop->Mode->Info->HorizontalResolution;
        screen_height = g_gop->Mode->Info->VerticalResolution;
        pixels_per_scanline = g_gop->Mode->Info->PixelsPerScanLine;

        uint32_t* new_backbuffer = NULL;
        status = gSystemTable->BootServices->AllocatePool(EfiLoaderData, screen_width * screen_height * 4, (void**)&new_backbuffer);
        if (!EFI_ERROR(status) && new_backbuffer != NULL) {
            backbuffer = new_backbuffer;
        }

        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        swap_buffers();
        play_system_sound((char*)"notification");
        return true;
    }
}

extern "C" {
    extern EFI_SYSTEM_TABLE* global_ST;
}

extern "C" void kernel_main(EFI_SYSTEM_TABLE *SystemTable, FramebufferInfo *fb_info) {
    gSystemTable = SystemTable;
    global_ST = SystemTable;

    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ KERNEL ] BlankOS Sequoia Monolithic Kernel Loaded.\r\n");
    klog("[ KERNEL ] BlankOS Sequoia Monolithic Kernel Loaded.");

    // Initialize Real Physical Memory Manager from UEFI Memory Map
    memory_init_efi(SystemTable);
    klog("[ MEMORY ] Physical Memory Manager & Free-List Heap Active.");

    // Initialize Framebuffer & Compositor FIRST so hardware drivers know the screen resolution
    if (fb_info && fb_info->framebuffer) {
        framebuffer = fb_info->framebuffer;
        screen_width = fb_info->width;
        screen_height = fb_info->height;
        pixels_per_scanline = fb_info->pixels_per_scanline;
        g_gop = fb_info->gop;
        init_compositor(SystemTable);
        extern void dui_init(EFI_SYSTEM_TABLE* st);
        dui_init(SystemTable);
        klog("[ GPU    ] Framebuffer & BlankDUI Vector Engine Initialized.");
    }

    // Hardware Enumeration (PCIe, CPUID, SVGA II, HDA)
    pci_enumerate(SystemTable);
    klog("[ PCIe   ] Hardware Bus Enumeration Complete.");

    // Network & USB/Bluetooth Hardware Probe
    net_init_hardware(SystemTable);
    klog("[ NET    ] PCIe Network & UEFI SNP Probe Complete.");

    // Initialize BDRM & System Registry + VFS
    bdrm_init();
    blankReg_init();
    if (blankReg_load(SystemTable)) {
        g_wallpaper_theme = blankReg_get_int("UI/WallpaperTheme", 0);
        g_show_dynamic_island = (blankReg_get_int("UI/DynamicIsland", 1) != 0);
        g_show_desktop_widgets = (blankReg_get_int("UI/DesktopWidgets", 1) != 0);
        klog("[ REG    ] Loaded persisted configuration from EFI\\BOOT\\REGISTRY.INI.");
    } else {
        klog("[ REG    ] Initialized default blankReg database.");
    }

    vfs_init(SystemTable);
    klog("[ VFS    ] FAT32 & RAM Virtual File System Mounted.");

    sync_ntp_time(SystemTable);
    klog("[ TIME   ] Hardware RTC & Time Subsystem Synchronized.");

    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);

    // Boot Flow Layering
    launch_loading_screen(SystemTable);
    if (is_os_installed(SystemTable)) {
        klog("[ BOOT   ] Installed FAT32 OS Volume Detected. Launching Lock Screen...");
        launch_login_screen(SystemTable);
    } else {
        klog("[ BOOT   ] Live ISO9660 Volume Detected. Launching Setup...");
        launch_setup_screen(SystemTable);
    }

    // Play warm polyphonic startup chord
    play_system_sound((char*)"startup");
    klog("[ AUDIO  ] Polyphonic Startup Chord Played.");

    // Global Fade-in Animation
    for (int alpha = 255; alpha >= 0; alpha -= 15) {
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        dui_rect(0, 0, screen_width, screen_height, 0x000000, alpha);
        swap_buffers();
        SystemTable->BootServices->Stall(12000);
    }

    // Mouse setup
    EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;

    while (1) {
        // Function Key Shortcuts (F1..F8)
        EFI_INPUT_KEY Key;
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
            if (Key.ScanCode == 0x0B) { // F1 = System Settings
                launch_sysinfo();
            } else if (Key.ScanCode == 0x0C) { // F2 = Terminal
                launch_terminal(SystemTable);
            } else if (Key.ScanCode == 0x0D) { // F3 = Calculator
                launch_calculator(SystemTable);
            } else if (Key.ScanCode == 0x0E) { // F4 = Weather
                launch_weather(SystemTable);
            } else if (Key.ScanCode == 0x0F) { // F5 = Browser
                launch_browser(SystemTable);
            } else if (Key.ScanCode == 0x10) { // F6 = Files & BlankPad
                launch_blankpad();
            } else if (Key.ScanCode == 0x11) { // F7 = Activity Monitor
                launch_task_manager();
            } else if (Key.ScanCode == 0x12) { // F8 = Registry Studio
                launch_blankreg_edit();
            }
        }

        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;

                if (dx != 0 || dy != 0) {
                    cursor_x += dx;
                    cursor_y += dy;
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                }

                last_mouse_x = cursor_x;
                last_mouse_y = cursor_y;

                // Right Click Detector
                if (State.RightButton) {
                    wifi_menu_open = false;
                    bt_menu_open = false;
                    battery_menu_open = false;
                    apple_menu_open = false;
                    island_expanded = false;
                    rclick_menu_open = true;
                    rclick_menu_x = cursor_x;
                    rclick_menu_y = cursor_y;
                }

                if (State.LeftButton) {
                    bool handled = false;

                    // 1. Apple / BlankOS System Menu Dropdown Handler
                    if (apple_menu_open) {
                        int mx = 8, my = 28, mw = 220, mh = 216;
                        if (cursor_x >= mx && cursor_x <= mx + mw && cursor_y >= my && cursor_y <= my + mh) {
                            int rel_y = cursor_y - my;
                            apple_menu_open = false;
                            play_system_sound((char*)"click");
                            if (rel_y < 30) launch_sysinfo();
                            else if (rel_y < 56) launch_sysinfo();
                            else if (rel_y < 84) launch_app_store(SystemTable);
                            else if (rel_y < 112) launch_task_manager();
                            else if (rel_y < 138) launch_blankreg_edit();
                            else if (rel_y < 166) launch_blankpad();
                            else if (rel_y < 190) os_restart();
                            else os_shutdown();
                        } else {
                            apple_menu_open = false;
                        }
                        handled = true;
                    }

                    // 2. Expanded Dynamic Island Capsule Handler
                    else if (island_expanded) {
                        int ex_w = 340, ex_h = 128;
                        int ex_x = (screen_width - ex_w) / 2;
                        int ex_y = 28;
                        if (cursor_x >= ex_x && cursor_x <= ex_x + ex_w && cursor_y >= ex_y && cursor_y <= ex_y + ex_h) {
                            island_expanded = false;
                            play_system_sound((char*)"click");
                            if (cursor_y >= ex_y + 60 && cursor_y <= ex_y + 86) {
                                if (cursor_x < ex_x + ex_w / 2) launch_blankpad();
                                else launch_task_manager();
                            } else if (cursor_y >= ex_y + 92 && cursor_y <= ex_y + 118) {
                                if (cursor_x < ex_x + ex_w / 2) launch_blankreg_edit();
                                else g_wallpaper_theme = (g_wallpaper_theme + 1) % 4;
                            }
                        } else {
                            island_expanded = false;
                        }
                        handled = true;
                    }

                    // 3. Right-Click Context Menu Handler
                    else if (rclick_menu_open) {
                        int mx = rclick_menu_x;
                        int my = rclick_menu_y;
                        int mw = 210, mh = 186;
                        if (mx + mw > screen_width) mx = screen_width - mw - 8;
                        if (my + mh > screen_height) my = screen_height - mh - 8;

                        if (cursor_x >= mx && cursor_x <= mx + mw && cursor_y >= my && cursor_y <= my + mh) {
                            int item = (cursor_y - my) / 26;
                            rclick_menu_open = false;
                            play_system_sound((char*)"click");
                            if (item == 0) launch_blankpad();
                            else if (item == 1) launch_task_manager();
                            else if (item == 2) launch_terminal(SystemTable);
                            else if (item == 3) g_wallpaper_theme = (g_wallpaper_theme + 1) % 4;
                            else if (item == 4) g_show_desktop_widgets = !g_show_desktop_widgets;
                            else if (item == 5) launch_sysinfo();
                            else if (item >= 6) {
                                blankOS_panic("USER_PANIC_TEST", "Kernel panic diagnostic triggered from Desktop menu.");
                            }
                        } else {
                            rclick_menu_open = false;
                        }
                        handled = true;
                    }

                    // 4. Wi-Fi / Network dropdown click handler
                    else if (wifi_menu_open) {
                        int mx = screen_width - 250;
                        if (cursor_x >= mx && cursor_x <= mx + 240 && cursor_y >= 28 && cursor_y <= 198) {
                            if (cursor_y >= 108 && cursor_y < 130) connect_to_wifi("Intel e1000 GbE Link", "");
                            else if (cursor_y >= 130 && cursor_y < 154) connect_to_wifi("BlankOS_5G_Mesh", "mesh1234");
                        }
                        wifi_menu_open = false;
                        handled = true;
                    }

                    // 5. Bluetooth dropdown click handler
                    else if (bt_menu_open) {
                        int mx = screen_width - 240;
                        if (cursor_x >= mx && cursor_x <= mx + 230 && cursor_y >= 28 && cursor_y <= 168) {
                            if (cursor_y >= 90 && cursor_y < 112) pair_bluetooth_device("AirPods Pro (USB-C)");
                            else if (cursor_y >= 112 && cursor_y < 136) pair_bluetooth_device("Magic Trackpad 3");
                        }
                        bt_menu_open = false;
                        handled = true;
                    }

                    else if (battery_menu_open) {
                        battery_menu_open = false;
                        handled = true;
                    }

                    // 6. Click Top Menubar (y: 0..26)
                    if (!handled && cursor_y >= 0 && cursor_y <= 26) {
                        int isl_w = 240;
                        int isl_x = (screen_width - isl_w) / 2;

                        if (cursor_x >= 8 && cursor_x <= 96) {
                            apple_menu_open = !apple_menu_open;
                            wifi_menu_open = false; bt_menu_open = false; battery_menu_open = false; island_expanded = false;
                            play_system_sound((char*)"click");
                            handled = true;
                        } else if (cursor_x >= 100 && cursor_x < 150) {
                            play_system_sound((char*)"click");
                            launch_blankpad();
                            handled = true;
                        } else if (cursor_x >= 150 && cursor_x < 218) {
                            play_system_sound((char*)"click");
                            launch_task_manager();
                            handled = true;
                        } else if (cursor_x >= 218 && cursor_x < 272) {
                            play_system_sound((char*)"click");
                            g_wallpaper_theme = (g_wallpaper_theme + 1) % 4;
                            handled = true;
                        } else if (cursor_x >= 272 && cursor_x < 345) {
                            play_system_sound((char*)"click");
                            launch_terminal(SystemTable);
                            handled = true;
                        } else if (g_show_dynamic_island && cursor_x >= isl_x && cursor_x <= isl_x + isl_w) {
                            island_expanded = !island_expanded;
                            apple_menu_open = false; wifi_menu_open = false; bt_menu_open = false; battery_menu_open = false;
                            play_system_sound((char*)"click");
                            handled = true;
                        } else if (cursor_x >= screen_width - 120 && cursor_x < screen_width - 85) {
                            wifi_menu_open = false; bt_menu_open = false; apple_menu_open = false;
                            battery_menu_open = !battery_menu_open;
                            handled = true;
                        } else if (cursor_x >= screen_width - 160 && cursor_x < screen_width - 120) {
                            wifi_menu_open = !wifi_menu_open; bt_menu_open = false; battery_menu_open = false; apple_menu_open = false;
                            handled = true;
                        } else if (cursor_x >= screen_width - 195 && cursor_x < screen_width - 160) {
                            wifi_menu_open = false; bt_menu_open = !bt_menu_open; battery_menu_open = false; apple_menu_open = false;
                            handled = true;
                        }
                    }

                    // 7. Dock App Click (10 icons: 0..9)
                    if (!handled) {
                        int clicked_app = blankUI_hit_test_dock(cursor_x, cursor_y);
                        if (clicked_app != -1) {
                            play_system_sound((char*)"click");
                            if (clicked_app == 0) {
                                launch_blankpad();
                            } else if (clicked_app == 1) {
                                launch_task_manager();
                            } else if (clicked_app == 2) {
                                launch_sysinfo();
                            } else if (clicked_app == 3) {
                                launch_browser(SystemTable);
                            } else if (clicked_app == 4) {
                                launch_app_store(SystemTable);
                            } else if (clicked_app == 5) {
                                launch_terminal(SystemTable);
                            } else if (clicked_app == 6) {
                                launch_calculator(SystemTable);
                            } else if (clicked_app == 7) {
                                launch_weather(SystemTable);
                            } else if (clicked_app == 8) {
                                if (g_is_live_cd) {
                                    launch_setup_screen(SystemTable);
                                } else {
                                    launch_updater(SystemTable);
                                }
                            } else if (clicked_app == 9) {
                                launch_blankreg_edit();
                            }
                        }
                    }
                }
            }
        }

        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();

        SystemTable->BootServices->Stall(16000);
    }
}
