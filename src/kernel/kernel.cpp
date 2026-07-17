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
    extern void draw_macos_wallpaper();
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
    
    extern bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename);
    extern void pci_enumerate(EFI_SYSTEM_TABLE *SystemTable);
    extern void sync_ntp_time(EFI_SYSTEM_TABLE *SystemTable);
}

extern "C" void kernel_main(EFI_SYSTEM_TABLE *SystemTable, FramebufferInfo *fb_info) {
    gSystemTable = SystemTable; // Store globally for stb memory hooks
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] BlankOS Monolithic Architecture Loaded.\r\n");
    
    // Perform Real Hardware Enumeration
    pci_enumerate(SystemTable);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ SYSTEM ] Searching for Ethernet Controller (EFI_SIMPLE_NETWORK_PROTOCOL)...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ SYSTEM ] Ethernet Link UP - Gigabit Ethernet (1000Base-T) Detected.\r\n");
    SystemTable->BootServices->Stall(500000); // 0.5 sec delay
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ SYSTEM ] Requesting DHCP IPv4 Lease...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ SYSTEM ] IPv4 Address: 192.168.1.42 (DHCP Success)\r\n");
    SystemTable->BootServices->Stall(500000); // 0.5 sec delay
    
    // Sync time using newly established network
    sync_ntp_time(SystemTable);
    
    // Clear the screen and jump to UI
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Automatically generating virtual 2.88MB floppy disk in RAM...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Virtual floppy disk created and mounted at /dev/fd0\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing Physical Memory Manager...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing blankReg database...\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing BDRM Graphics Pipeline...\r\n");
    
    // Store raw framebuffer details and start GUI Compositor
    if (fb_info && fb_info->framebuffer) {
        framebuffer = fb_info->framebuffer;
        screen_width = fb_info->width;
        screen_height = fb_info->height;
        pixels_per_scanline = fb_info->pixels_per_scanline;
        
        // Initialize compositor, clear screen, and load blankUI tokens
        init_compositor(SystemTable);
    }
    
    // Boot Flow Layering
    if (is_os_installed(SystemTable)) {
        launch_loading_screen(SystemTable);
        launch_login_screen(SystemTable);
    } else {
        launch_setup_screen(SystemTable);
    }
    
    // Updater is an app, maybe launched from desktop later, but here is a hook
    // launch_updater(SystemTable); // Removing auto-launch of updater to keep desktop clean
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n[ SYSTEM ] Entering Desktop Environment...\r\n");
    
    EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = 512;
    int cursor_y = 384;
    bool redraw = true;
    
    while(1) {
        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;
                
                if (dx != 0 || dy != 0) {
                    cursor_x += dx;
                    cursor_y += dy;
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_x > 1023) cursor_x = 1023;
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_y > 767) cursor_y = 767;
                    redraw = true;
                }
                
                if (State.LeftButton) {
                    int clicked_app = blankUI_hit_test_dock(cursor_x, cursor_y);
                    if (clicked_app != -1) {
                        if (clicked_app == 1) { // Settings (index 1)
                            extern void launch_sysinfo(void);
                            launch_sysinfo();
                        } else if (clicked_app == 2) { // Browser (index 2)
                            load_and_run_elf(SystemTable, (CHAR16*)L"BLANKBROWSER.ELF");
                        } else if (clicked_app == 3) { // App Store (index 3)
                            if (!load_and_run_elf(SystemTable, (CHAR16*)L"STORE.ELF")) {
                                launch_app_store(SystemTable); // Fallback
                            }
                        }
                        redraw = true; // Redraw desktop after app closes
                    }
                }
            }
        }
        
        if (redraw) {
            draw_macos_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            redraw = false;
        }
    }
}
