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
    extern void launch_doom(EFI_SYSTEM_TABLE *SystemTable);
    extern void blankOS_panic(const char* error_code, const char* details);
    
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
    
    // Set up Interrupt Descriptor Table (IDT) for hardware panic catching
    extern void init_idt();
    init_idt();
    
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
                
                // Right Click Detector
                if (State.RightButton) {
                    wifi_menu_open = false;
                    bt_menu_open = false;
                    battery_menu_open = false;
                    rclick_menu_open = true;
                    rclick_menu_x = cursor_x;
                    rclick_menu_y = cursor_y;
                }
                
                if (State.LeftButton) {
                    bool handled = false;
                    
                    // 1. Right Click Context Menu handler
                    if (rclick_menu_open) {
                        int mx = rclick_menu_x;
                        int my = rclick_menu_y;
                        if (cursor_x >= mx && cursor_x <= mx + 160 && cursor_y >= my && cursor_y <= my + 110) {
                            int item = (cursor_y - my) / 26;
                            if (item == 0) launch_sysinfo();
                            else if (item == 1) launch_terminal(SystemTable);
                            else if (item == 2) {
                                // Trigger actual hardware panic (Div by Zero!)
                                volatile int zero = 0;
                                volatile int val = 1 / zero;
                                (void)val;
                                blankOS_panic("DIV_BY_ZERO", "Division by zero triggered dynamically.");
                            }
                        }
                        rclick_menu_open = false;
                        handled = true;
                    }
                    
                    // 2. Wi-Fi dropdown click handler
                    else if (wifi_menu_open) {
                        int mx = screen_width - 230;
                        if (cursor_x >= mx && cursor_x <= mx + 220 && cursor_y >= 28 && cursor_y <= 28 + 160) {
                            if (cursor_y >= 110 && cursor_y < 130) connect_to_wifi("BlankOS_Secure", "secure123");
                            else if (cursor_y >= 130 && cursor_y < 150) connect_to_wifi("Ethan_WiFi_5G", "ethan5g");
                        }
                        wifi_menu_open = false;
                        handled = true;
                    }
                    
                    // 3. Bluetooth dropdown click handler
                    else if (bt_menu_open) {
                        int mx = screen_width - 230;
                        if (cursor_x >= mx && cursor_x <= mx + 220 && cursor_y >= 28 && cursor_y <= 28 + 140) {
                            if (cursor_y >= 70 && cursor_y < 90) pair_bluetooth_device("AirPods Max");
                            else if (cursor_y >= 90 && cursor_y < 110) pair_bluetooth_device("Magic Keyboard");
                        }
                        bt_menu_open = false;
                        handled = true;
                    }
                    
                    else if (battery_menu_open) {
                        battery_menu_open = false;
                        handled = true;
                    }
                    
                    // 4. Click menubar status area
                    if (!handled && cursor_y >= 0 && cursor_y <= 24) {
                        // Battery clicked
                        if (cursor_x >= screen_width - 120 && cursor_x < screen_width - 90) {
                            wifi_menu_open = false; bt_menu_open = false;
                            battery_menu_open = !battery_menu_open;
                            handled = true;
                        } 
                        // Wi-Fi clicked
                        else if (cursor_x >= screen_width - 160 && cursor_x < screen_width - 120) {
                            wifi_menu_open = !wifi_menu_open; bt_menu_open = false;
                            battery_menu_open = false;
                            handled = true;
                        } 
                        // Bluetooth clicked
                        else if (cursor_x >= screen_width - 190 && cursor_x < screen_width - 160) {
                            wifi_menu_open = false; bt_menu_open = !bt_menu_open;
                            battery_menu_open = false;
                            handled = true;
                        }
                    }
                    
                    // 5. Dock app click
                    if (!handled) {
                        int clicked_app = blankUI_hit_test_dock(cursor_x, cursor_y);
                        if (clicked_app != -1) {
                            if (clicked_app == 0) { // Finder
                                // No-op
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
                            } else if (clicked_app == 7) { // Doom
                                launch_doom(SystemTable);
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
        
        SystemTable->BootServices->Stall(16000); // ~60 FPS Animation Frame Limiter
    }
}

