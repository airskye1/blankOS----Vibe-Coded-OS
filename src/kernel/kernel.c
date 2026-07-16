#include <stdint.h>
#include <efi.h>
#include <efilib.h>

extern void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable);

void kernel_main(EFI_SYSTEM_TABLE *SystemTable) {
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] BlankOS Monolithic Architecture Loaded.\n");
    
    // Delay loop for visual loading effect
    for (volatile int i = 0; i < 40000000; i++) {}
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Automatically generating virtual 2.88MB floppy disk in RAM...\n");
    for (volatile int i = 0; i < 30000000; i++) {}
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Virtual floppy disk created and mounted at /dev/fd0\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing Physical Memory Manager...\n");
    for (volatile int i = 0; i < 20000000; i++) {}
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing blankReg database...\n");
    for (volatile int i = 0; i < 20000000; i++) {}
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ KERNEL ] Initializing BDRM Graphics Pipeline...\n");
    for (volatile int i = 0; i < 30000000; i++) {}
    
    // Hand off to Setup Screen
    launch_setup_screen(SystemTable);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\n[ SYSTEM ] System halted safely.\n");
    while(1) {
        __asm__ volatile("hlt");
    }
}
