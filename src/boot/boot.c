#include <efi.h>
#include <efilib.h>

/*
 * kernel_main is defined in kernel.cpp with extern "C", so the C linker
 * finds it by its unmangled name. Both boot.c and kernel.cpp are compiled
 * with GCC's default System V ABI, so the compiler automatically puts
 * SystemTable in the correct register (RDI). No assembly shim needed.
 */
extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable);

/*
 * EFIAPI on efi_main ensures it matches whatever ABI crt0 uses:
 *   - Modern gnu-efi (HAVE_USE_MS_ABI): EFIAPI = __attribute__((ms_abi)) → args in RCX/RDX
 *   - Old gnu-efi: EFIAPI = nothing → args in RDI/RSI (System V default)
 * Either way, the compiler handles it. No manual register shuffling.
 *
 * We do NOT call InitializeLib() because it is declared without EFIAPI in
 * the gnu-efi headers. If the library was compiled with -mabi=ms but our
 * code uses System V as default, the call would use the wrong registers
 * and crash before printing a single character.
 */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    /* Immediate sign-of-life — if this prints, the ABI is correct */
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"BlankOS UEFI Alive!\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"  Welcome to BlankOS v1.2.4 Bootloader  \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"========================================\r\n\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] UEFI Firmware Detected.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Initializing BDRM Graphics Compositor...\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OK ] Handing off to C++ kernel...\r\n\r\n");

    /* Direct C-linkage call. Compiler handles the ABI automatically. */
    kernel_main(SystemTable);

    /* Fallback halt — should never reach here */
    while(1) { __asm__ volatile("hlt"); }
    return EFI_SUCCESS;
}
