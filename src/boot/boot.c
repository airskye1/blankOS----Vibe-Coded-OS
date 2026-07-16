#include <efi.h>
#include <efilib.h>

/*
 * kernel_main is defined in kernel.cpp with extern "C", so the C linker
 * finds it by its unmangled name.
 */
extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

/*
 * Declare the relocation function from libgnuefi.
 * On x86_64, gnu-efi's _relocate is compiled with the System V ABI.
 */
extern EFI_STATUS __attribute__((sysv_abi)) _relocate(long ldbase, void *dyn);

/* Linker symbols defined by the elf_x86_64_efi.lds linker script */
extern char _ImageBase[];
extern void *_DYNAMIC;

/*
 * Direct entry point bypasses gnu-efi's crt0 to avoid any ABI mismatch
 * during bootloader start. The UEFI firmware ALWAYS calls the entry point
 * using the Microsoft x64 ABI, so we decorate it with ms_abi.
 */
EFI_STATUS __attribute__((ms_abi)) _start(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    /* Apply dynamic relocations first so global variables/tables work */
    _relocate((long)_ImageBase, _DYNAMIC);

    /* Immediate sign-of-life — if this prints, the ABI is correct */
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"BlankOS UEFI Direct Boot Loader v1.2.5\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"  Welcome to BlankOS v1.2.5 Bootloader  \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] UEFI Firmware Detected.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Initializing BDRM Graphics Compositor...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Handing off to C++ kernel...\r\n\r\n");

    /* Call kernel_main. GCC handles ABI translation from ms_abi to sysv_abi automatically. */
    kernel_main(SystemTable);

    /* Fallback halt — should never reach here */
    while(1) { __asm__ volatile("hlt"); }
    return EFI_SUCCESS;
}
