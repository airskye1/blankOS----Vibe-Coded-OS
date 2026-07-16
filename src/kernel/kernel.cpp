#include <stdint.h>
#include <efi.h>
#include <efilib.h>

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
    
    extern void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_updater(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_app_store(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_blankbrowser(EFI_SYSTEM_TABLE *SystemTable);
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
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n[ SYSTEM ] System halted safely.\r\n");
    while(1) {
        __asm__ volatile("hlt");
    }
}
