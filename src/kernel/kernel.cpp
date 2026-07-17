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
    extern void launch_blankbrowser(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_settings(EFI_SYSTEM_TABLE *SystemTable);
}

extern "C" void kernel_main(EFI_SYSTEM_TABLE *SystemTable, FramebufferInfo *fb_info) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] BlankOS Monolithic Architecture Loaded.\r\n");
    
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
    
    // Hand off to Setup Screen (OOBE)
    launch_setup_screen(SystemTable);
    
    // Launch C++ apps
    launch_updater(SystemTable);
    
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
                            launch_settings(SystemTable);
                        } else if (clicked_app == 2) { // Browser (index 2)
                            launch_blankbrowser(SystemTable);
                        } else if (clicked_app == 3) { // App Store (index 3)
                            launch_app_store(SystemTable);
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
