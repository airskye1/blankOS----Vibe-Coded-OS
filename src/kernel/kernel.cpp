#include <stdint.h>
#include <efi.h>
#include <efilib.h>

extern void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable);

extern "C" void kernel_main(EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] BlankOS Monolithic Architecture Loaded.\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Automatically generating virtual 2.88MB floppy disk in RAM...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Virtual floppy disk created and mounted at /dev/fd0\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing Physical Memory Manager...\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing blankReg database...\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing BDRM Graphics Pipeline...\r\n");
    
    // Hand off to Setup Screen
    launch_setup_screen(SystemTable);
    
    // Launch the requested C++ apps directly in the boot stream!
    extern void launch_app_store(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_blankbrowser(EFI_SYSTEM_TABLE *SystemTable);
    
    launch_app_store(SystemTable);
    launch_blankbrowser(SystemTable);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n[ SYSTEM ] System halted safely.\r\n");
    while(1) {
        __asm__ volatile("hlt");
    }
}
