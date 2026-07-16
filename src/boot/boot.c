#include <efi.h>
#include <efilib.h>

extern void kernel_entry(EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    
    // Reset the console to prevent weird VirtualBox artifacts
    SystemTable->ConOut->Reset(SystemTable->ConOut, FALSE);
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"  Welcome to BlankOS v1.2.0 Bootloader  \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n\r\n");
    
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] UEFI Firmware Detected.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Initializing BDRM Graphics Compositor...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Handing off control to pure Assembly Kernel...\r\n\r\n");
    
    // Pass the UEFI SystemTable to the Assembly CPU wrapper!
    kernel_entry(SystemTable);
    
    // Fallback halt
    while(1) { __asm__ volatile("hlt"); }
    
    return EFI_SUCCESS;
}
