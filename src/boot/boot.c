#include <stdint.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>

/*
 * Framebuffer info struct passed from bootloader to kernel_main.
 */
typedef struct {
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
} FramebufferInfo;

/*
 * kernel_main is defined in kernel.cpp with extern "C", so the C linker
 * finds it by its unmangled name. Both boot.c and kernel.cpp are compiled
 * with GCC's default System V ABI, so the compiler automatically puts
 * SystemTable in the correct register (RDI) and FramebufferInfo in (RSI).
 */
extern void kernel_main(EFI_SYSTEM_TABLE *SystemTable, FramebufferInfo *fb_info);

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
    FramebufferInfo fb_info = {0};
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    /* CRITICAL: Disable UEFI firmware watchdog timer so the VM never resets after 5 minutes */
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    /* Immediate sign-of-life */
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"BlankOS UEFI Hybrid Boot Loader v1.3.0\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"--------------------------------------\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"  Welcome to BlankOS v1.3.0 Bootloader  \r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"========================================\r\n\r\n");

    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ OK ] UEFI Firmware Detected & Watchdog Disabled.\r\n");
    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ OK ] Locating Graphics Output Protocol (GOP)...\r\n");

    /* Retrieve the Graphics Output Protocol (GOP) handle */
    EFI_STATUS status = SystemTable->BootServices->LocateProtocol(&gopGuid, NULL, (VOID**)&gop);
    if (status == EFI_SUCCESS && gop != NULL) {
        UINT32 best_mode = gop->Mode->Mode;
        BOOLEAN mode_found = FALSE;
        
        // 1. Search for 1024x768 to match the standard UI layouts
        for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
            UINTN sizeOfInfo = 0;
            if (gop->QueryMode(gop, i, &sizeOfInfo, &info) == EFI_SUCCESS && info != NULL) {
                if (info->HorizontalResolution == 1024 && info->VerticalResolution == 768) {
                    best_mode = i;
                    mode_found = TRUE;
                    SystemTable->BootServices->FreePool(info);
                    break;
                }
                SystemTable->BootServices->FreePool(info);
            }
        }
        
        // 2. Fallback to 1280x800 or 1280x720
        if (!mode_found) {
            for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
                UINTN sizeOfInfo = 0;
                if (gop->QueryMode(gop, i, &sizeOfInfo, &info) == EFI_SUCCESS && info != NULL) {
                    if ((info->HorizontalResolution == 1280 && info->VerticalResolution == 800) ||
                        (info->HorizontalResolution == 1280 && info->VerticalResolution == 720)) {
                        best_mode = i;
                        mode_found = TRUE;
                        SystemTable->BootServices->FreePool(info);
                        break;
                    }
                    SystemTable->BootServices->FreePool(info);
                }
            }
        }

        // 3. Fallback to highest available
        if (!mode_found) {
            UINT32 max_res = 0;
            for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
                UINTN sizeOfInfo = 0;
                if (gop->QueryMode(gop, i, &sizeOfInfo, &info) == EFI_SUCCESS && info != NULL) {
                    UINT32 res = info->HorizontalResolution * info->VerticalResolution;
                    if (res > max_res) {
                        max_res = res;
                        best_mode = i;
                    }
                    SystemTable->BootServices->FreePool(info);
                }
            }
        }
        
        gop->SetMode(gop, best_mode);
        
        fb_info.framebuffer = (uint32_t*)gop->Mode->FrameBufferBase;
        fb_info.width = gop->Mode->Info->HorizontalResolution;
        fb_info.height = gop->Mode->Info->VerticalResolution;
        fb_info.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
        fb_info.gop = gop;
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ OK ] Graphics Mode Initialized successfully.\r\n");
    } else {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ WARN ] GOP not found! Defaulting to Text Mode.\r\n");
    }

    SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ OK ] Handing off to C++ kernel...\r\n\r\n");

    /* Call kernel_main with the SystemTable and our GOP Framebuffer details */
    kernel_main(SystemTable, &fb_info);

    /* Fallback halt - should never reach here */
    while(1) { __asm__ volatile("hlt"); }
    return EFI_SUCCESS;
}
