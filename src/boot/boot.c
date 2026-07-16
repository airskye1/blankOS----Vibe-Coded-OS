#include <efi.h>
#include <efilib.h>

extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    // Reset the console to prevent weird VirtualBox artifacts
    SystemTable->ConOut->Reset(SystemTable->ConOut, FALSE);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"  Welcome to BlankOS v1.1.2 Bootloader  \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] UEFI Firmware Detected.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Initializing BDRM Graphics Compositor...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Handing off control to monolithic kernel...\r\n\r\n");
    
    // Pass the UEFI SystemTable so the kernel can print to the screen!
    kernel_main(SystemTable);
    
    // Fallback halt
    while(1) { __asm__ volatile("hlt"); }
    
    return EFI_SUCCESS;
}
