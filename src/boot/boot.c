#include <efi.h>
#include <efilib.h>

extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

/*
 * UEFI firmware invokes an x86_64 application using the Microsoft x64 ABI.
 * GNU-EFI's startup object forwards those registers directly to efi_main, so
 * EFIAPI is required here.  Without it, GCC expects SystemTable in RSI
 * instead of RDX and the first firmware call faults before anything prints.
 */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    
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
