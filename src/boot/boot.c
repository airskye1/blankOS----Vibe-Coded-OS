#include <efi.h>
#include <efilib.h>

extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    
    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    
    Print(L"========================================\n");
    Print(L"  Welcome to BlankOS v1.1.1 Bootloader  \n");
    Print(L"========================================\n\n");
    
    Print(L"[ OK ] UEFI Firmware Detected.\n");
    Print(L"[ OK ] Initializing BDRM Graphics Compositor...\n");
    Print(L"[ OK ] Handing off control to monolithic kernel...\n\n");
    
    // Pass the UEFI SystemTable so the kernel can print to the screen!
    kernel_main(SystemTable);
    
    // Fallback halt
    while(1) {}
    
    return EFI_SUCCESS;
}
