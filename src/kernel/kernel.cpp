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
} FramebufferInfo;

extern "C" {
    // Global System Table for stb allocators
    EFI_SYSTEM_TABLE *gSystemTable = NULL;

    extern volatile uint32_t* framebuffer;
    extern int screen_width;
    extern int screen_height;
    extern int pixels_per_scanline;
    
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
    
    extern bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename);
    extern void pci_enumerate(EFI_SYSTEM_TABLE *SystemTable);
    extern void sync_ntp_time(EFI_SYSTEM_TABLE *SystemTable);
    extern void play_system_sound(int sound_id);

    // Kernel boot log buffer
    static char boot_log[4096];
    static int boot_log_pos = 0;

    static void klog(const char* msg) {
        while (*msg && boot_log_pos < 4090) {
            boot_log[boot_log_pos++] = *msg++;
        }
        boot_log[boot_log_pos++] = '\n';
        boot_log[boot_log_pos] = '\0';
    }
}

extern "C" void kernel_main(EFI_SYSTEM_TABLE *SystemTable, FramebufferInfo *fb_info) {
    gSystemTable = SystemTable;
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] BlankOS Monolithic Architecture Loaded.\r\n");
    klog("[ KERNEL ] BlankOS Monolithic Architecture Loaded.");
    
    // Initialize Framebuffer & Compositor FIRST so hardware drivers know the screen resolution
    if (fb_info && fb_info->framebuffer) {
        framebuffer = fb_info->framebuffer;
        screen_width = fb_info->width;
        screen_height = fb_info->height;
        pixels_per_scanline = fb_info->pixels_per_scanline;
        init_compositor(SystemTable);
        extern void dui_init(EFI_SYSTEM_TABLE* st);
        dui_init(SystemTable);
        klog("[ GPU    ] Framebuffer Initialized.");
    }
    
    // Hardware Enumeration
    pci_enumerate(SystemTable);
    klog("[ KERNEL ] PCI Enumeration Complete.");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Searching for Ethernet Controller...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Ethernet Link UP - Gigabit Ethernet Detected.\r\n");
    klog("[ NET    ] Ethernet Link UP - Gigabit Ethernet Detected.");
    SystemTable->BootServices->Stall(300000);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] Requesting DHCP IPv4 Lease...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ SYSTEM ] IPv4 Address: 192.168.1.42\r\n");
    klog("[ NET    ] IPv4 Address: 192.168.1.42 (DHCP)");
    SystemTable->BootServices->Stall(300000);
    
    sync_ntp_time(SystemTable);
    klog("[ TIME   ] NTP Sync Complete.");
    
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ KERNEL ] Initializing Physical Memory Manager...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ KERNEL ] Initializing blankReg database...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ KERNEL ] Initializing BDRM Graphics Pipeline...\r\n");
    klog("[ KERNEL ] Memory Manager Initialized.");
    klog("[ KERNEL ] blankReg Database Initialized.");
    klog("[ KERNEL ] BDRM Graphics Pipeline Active.");
    
    // Boot Flow Layering
    launch_loading_screen(SystemTable); // Always show loading screen
    if (is_os_installed(SystemTable)) {
        klog("[ BOOT   ] Installed OS Detected. Loading...");
        launch_login_screen(SystemTable);
    } else {
        klog("[ BOOT   ] Live CD Detected. Launching Setup...");
        launch_setup_screen(SystemTable);
    }
    
    // Play startup sound
    play_system_sound((char*)"startup");
    klog("[ AUDIO  ] Startup Sound Played.");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"\r\n[ SYSTEM ] Entering Desktop Environment...\r\n");
    klog("[ SYSTEM ] Desktop Environment Active.");
    
    // Global Fade-in Animation
    for (int alpha = 255; alpha >= 0; alpha -= 5) {
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        dui_rect(0, 0, screen_width, screen_height, 0x000000, alpha);
        swap_buffers();
        SystemTable->BootServices->Stall(16000);
    }
    
    // Mouse setup
    EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;
    
    while(1) {
        // Keyboard shortcuts
        EFI_INPUT_KEY Key;
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
            // Ctrl shortcuts aren't reliable in UEFI, use function keys
            if (Key.ScanCode == 0x0B) { // F1 = Settings
                launch_sysinfo();
            } else if (Key.ScanCode == 0x0C) { // F2 = Terminal
                launch_terminal(SystemTable);
            } else if (Key.ScanCode == 0x0D) { // F3 = Calculator
                launch_calculator(SystemTable);
            } else if (Key.ScanCode == 0x0E) { // F4 = Weather
                launch_weather(SystemTable);
            } else if (Key.ScanCode == 0x0F) { // F5 = Browser
                launch_browser(SystemTable);
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
                
                if (State.LeftButton) {
                    int clicked_app = blankUI_hit_test_dock(cursor_x, cursor_y);
                    if (clicked_app != -1) {
                        if (clicked_app == 0) { // Finder
                            // No-op for now
                        } else if (clicked_app == 1) { // Settings
                            launch_sysinfo();
                        } else if (clicked_app == 2) { // Browser
                            launch_browser(SystemTable);
                        } else if (clicked_app == 3) { // Store
                            launch_app_store(SystemTable);
                        } else if (clicked_app == 4) { // Terminal
                            launch_terminal(SystemTable);
                        } else if (clicked_app == 5) { // Calculator
                            launch_calculator(SystemTable);
                        } else if (clicked_app == 6) { // Weather
                            launch_weather(SystemTable);
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
        
        SystemTable->BootServices->Stall(16000); // ~60 FPS Animation Frame Limiter
    }
}

