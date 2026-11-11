#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/blankDUI.h"
#include "../ui/window_manager.h"
#include "../ui/theme.h"
#include "input.h"
#include "audio.h"
#include "net.h"

typedef struct {
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
} FramebufferInfo;

extern "C" {
    EFI_SYSTEM_TABLE *gSystemTable = NULL;
    EFI_SYSTEM_TABLE *global_ST = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g_gop = NULL;

    extern int g_wallpaper_theme;
    extern bool g_is_live_cd;
    extern bool g_show_desktop_widgets;
    extern bool g_show_dynamic_island;

    extern void blankUI_draw_desktop_widgets(const BlankInputState* in);
    extern void blankUI_draw_shell(BlankInputState* in);
    extern void process_notifications(void);

    // Kernel & Hardware subsystems
    extern void memory_init_efi(EFI_SYSTEM_TABLE *SystemTable);
    extern void pci_enumerate(EFI_SYSTEM_TABLE *SystemTable);
    extern void bdrm_init(void);
    extern void blankReg_init(void);
    extern bool blankReg_load(EFI_SYSTEM_TABLE *st);
    extern int blankReg_get_int(const char* key, int default_val);
    extern void blankReg_set_int(const char* key, int value);
    extern void vfs_init(EFI_SYSTEM_TABLE *st);
    extern void sync_ntp_time(EFI_SYSTEM_TABLE *SystemTable);
    extern void os_send_notification(const char* title, const char* message);

    // Pre-desktop stages
    extern void launch_loading_screen(EFI_SYSTEM_TABLE *SystemTable);
    extern bool is_os_installed(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_login_screen(EFI_SYSTEM_TABLE *SystemTable);

    // Desktop App registrations
    extern void blankpad_register(void);
    extern void browser_register(void);
    extern void sysinfo_register(void);
    extern void store_register(void);
    extern void terminal_register(void);
    extern void task_manager_register(void);
    extern void calculator_register(void);
    extern void weather_register(void);
    extern void blankreg_edit_register(void);
    extern void updater_register(void);

    static char boot_log[4096];
    static int boot_log_pos = 0;

    static void klog(const char* msg) {
        while (msg && *msg && boot_log_pos < 4090) {
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

        uint32_t* new_fb = (uint32_t*)(uintptr_t)g_gop->Mode->FrameBufferBase;
        uint32_t nw = g_gop->Mode->Info->HorizontalResolution;
        uint32_t nh = g_gop->Mode->Info->VerticalResolution;
        uint32_t nppsl = g_gop->Mode->Info->PixelsPerScanLine;

        init_compositor(new_fb, nw, nh, nppsl);
        dui_on_resolution_change();
        input_refresh_bounds((int)nw, (int)nh);
        play_system_sound((char*)"notify");
        return true;
    }
}

extern "C" void kernel_main(EFI_SYSTEM_TABLE *SystemTable, FramebufferInfo *fb_info) {
    gSystemTable = SystemTable;
    global_ST = SystemTable;

    klog("[ KERNEL ] BlankOS Horizon Kernel v2.0 Booting...");

    // 1. Physical Memory Allocator
    memory_init_efi(SystemTable);
    klog("[ MEMORY ] Physical Memory Map & Free-List Heap Initialized.");

    // 2. GOP Compositor & Horizon Spatial Glass Graphics Engine
    if (fb_info && fb_info->framebuffer) {
        g_gop = fb_info->gop;
        init_compositor(fb_info->framebuffer, fb_info->width, fb_info->height, fb_info->pixels_per_scanline);
        dui_init(SystemTable);
        klog("[ GPU    ] Compositor & Horizon Spatial Glass Engine Ready.");
    }

    // 3. Unified Input (USB Absolute Tablet + Relative Mouse + Keyboard)
    input_init(SystemTable);
    klog("[ INPUT  ] Unified HID Pointer & Keyboard Subsystem Active.");

    // 4. Hardware Discovery & Non-Blocking Chime Audio
    pci_enumerate(SystemTable);
    init_audio_driver(SystemTable);
    klog("[ PCI/AU ] PCIe Bus Enumerated; Non-Blocking Audio Active.");

    // 5. Real UEFI SNP Ethernet + ARP + DHCP + DNS + TCP + HTTP/1.1 Stack
    net_init(SystemTable);
    klog("[ NET    ] UEFI SNP Network Stack & DHCP Initialized.");

    // 6. Security, Registry & VFS
    bdrm_init();
    blankReg_init();
    if (blankReg_load(SystemTable)) {
        g_wallpaper_theme = blankReg_get_int("UI/WallpaperTheme", 0);
        g_show_desktop_widgets = (blankReg_get_int("UI/DesktopWidgets", 1) != 0);
        g_show_dynamic_island = (blankReg_get_int("UI/DynamicIsland", 1) != 0);
        dui_set_wallpaper_id(g_wallpaper_theme);
        klog("[ REG    ] Loaded Configuration from REGISTRY.INI.");
    } else {
        klog("[ REG    ] Using Default Horizon Registry Profile.");
    }

    vfs_init(SystemTable);
    klog("[ VFS    ] FAT32 & RAM Virtual File System Mounted.");

    sync_ntp_time(SystemTable);
    klog("[ TIME   ] Hardware RTC Synchronized.");

    if (SystemTable && SystemTable->ConOut) {
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    }

    // 7. Boot Splash -> Installer (Live CD) or Lock Screen (Installed)
    launch_loading_screen(SystemTable);

    if (is_os_installed(SystemTable)) {
        g_is_live_cd = false;
        klog("[ BOOT   ] Installed FAT32 System Detected. Opening Lock Screen.");
        launch_login_screen(SystemTable);
    } else {
        g_is_live_cd = true;
        klog("[ BOOT   ] Live Media Detected. Opening Setup Assistant.");
        launch_setup_screen(SystemTable);
    }

    // 8. Initialize Multi-Window Manager & Register All 10 Horizon Applications
    wm_init(SystemTable);
    blankpad_register();
    browser_register();
    sysinfo_register();
    store_register();
    terminal_register();
    task_manager_register();
    calculator_register();
    weather_register();
    blankreg_edit_register();
    updater_register();

    // Play non-blocking startup chime
    play_system_sound((char*)"startup");

    // Quick smooth fade-in
    for (int alpha = 240; alpha >= 0; alpha -= 24) {
        audio_tick();
        net_tick();
        dui_draw_wallpaper(dui_get_wallpaper_id());
        dui_rect(0, 0, (int)screen_width, (int)screen_height, 0x050810, (uint8_t)alpha);
        swap_buffers();
        if (SystemTable && SystemTable->BootServices) SystemTable->BootServices->Stall(12000);
    }

    os_send_notification("BlankOS Horizon", "Press Ctrl+K for Quick Search or click Browser to surf HTTP.");

    // 9. Non-Blocking Multi-Window Horizon Compositor Loop
    BlankInputState in;
    while (1) {
        // Non-blocking hardware polls
        input_poll(&in);
        net_tick();
        audio_tick();

        // Global Function Keys (F1..F8 shortcuts)
        if (in.has_key) {
            if (in.scan_code == BLANK_SCAN_F1) { wm_open_app(APP_SETTINGS); input_consume_key(&in); }
            else if (in.scan_code == BLANK_SCAN_F2) { wm_open_app(APP_TERMINAL); input_consume_key(&in); }
            else if (in.scan_code == BLANK_SCAN_F3) { wm_open_app(APP_CALCULATOR); input_consume_key(&in); }
            else if (in.scan_code == BLANK_SCAN_F4) { wm_open_app(APP_WEATHER); input_consume_key(&in); }
            else if (in.scan_code == BLANK_SCAN_F5) { wm_open_app(APP_BROWSER); input_consume_key(&in); }
        }

        // Layer 0: Cached Aurora Spatial Wallpaper
        dui_reset_clip();
        dui_draw_wallpaper(dui_get_wallpaper_id());

        // Layer 1: Desktop Telemetry Widget
        BlankInputState desk_in = in;
        if (wm_get_focused_window() != NULL) {
            desk_in.left_pressed = false;
        }
        blankUI_draw_desktop_widgets(&desk_in);

        // Layer 2: Multi-Window Manager (Z-ordered apps)
        wm_tick_and_render(&in);

        // Layer 3: Top Bar, Pulse Capsule, Dock, Spotlight, Control Center, Downloads
        blankUI_draw_shell(&in);

        // Layer 4: Toast Notifications
        process_notifications();

        // Layer 5: Hardware Cursor
        blankUI_draw_cursor(in.mx, in.my, 0);

        swap_buffers();
        if (SystemTable && SystemTable->BootServices) {
            SystemTable->BootServices->Stall(10000); // ~10ms pacing
        }
    }
}
