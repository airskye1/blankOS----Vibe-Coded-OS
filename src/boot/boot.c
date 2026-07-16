#include <efi.h>
#include <efilib.h>

/*
 * kernel_main is defined in kernel.cpp with extern "C", so the C linker
 * finds it by its unmangled name. Both boot.c and kernel.cpp are compiled
 * with GCC's default System V ABI, so the compiler automatically puts
 * SystemTable in the correct register (RDI).
 */
extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

/*
 * We explicitly declare efi_main with __attribute__((sysv_abi)) because
 * the precompiled gnu-efi crt0-efi-x86_64.o on ubuntu-latest expects
 * efi_main to be a System V ABI function.
 *
 * However, we still compile with -DGNU_EFI_USE_MS_ABI in the Makefile so that
 * all internal UEFI function pointers (like OutputString) are compiled as
 * Microsoft x64 ms_abi calls, matching the UEFI firmware requirements.
 */
EFI_STATUS __attribute__((sysv_abi)) efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    /* Immediate sign-of-life — if this prints, the ABI is correct */
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"BlankOS UEFI Hybrid Boot Loader v1.2.6\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"  Welcome to BlankOS v1.2.6 Bootloader  \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] UEFI Firmware Detected.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Initializing BDRM Graphics Compositor...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Handing off to C++ kernel...\r\n\r\n");

    /* Call kernel_main. Both are System V ABI, so it is a direct call. */
    kernel_main(SystemTable);

    /* Fallback halt — should never reach here */
    while(1) { __asm__ volatile("hlt"); }
    return EFI_SUCCESS;
}
