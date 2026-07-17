#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include "os_api.h"

// Minimal ELF64 Definitions
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    
    typedef void (*AppMainFunc)(OS_API* api);

    bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename) {
        OS_API api;
        api.swap_buffers = swap_buffers;
        api.draw_macos_wallpaper = draw_macos_wallpaper;
        api.blankUI_draw_menubar = blankUI_draw_menubar;
        api.blankUI_draw_dock = blankUI_draw_dock;
        api.blankUI_draw_cursor = blankUI_draw_cursor;
        api.blankUI_draw_window = blankUI_draw_window;
        api.blankUI_draw_text_color = blankUI_draw_text_color;
        api.blankUI_draw_button = blankUI_draw_button;
        api.blankUI_hit_test_window_close = blankUI_hit_test_window_close;
        api.SystemTable = SystemTable;
        
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        EFI_STATUS Status = SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
            
        if (EFI_ERROR(Status) || numHandles == 0) return false;
        
        void* fileBuffer = NULL;
        UINTN fileSize = 0;
        
        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            EFI_FILE_HANDLE file = NULL;
            Status = root->Open(root, &file, filename, EFI_FILE_MODE_READ, 0);
            
            if (!EFI_ERROR(Status) && file != NULL) {
                fileSize = 4 * 1024 * 1024; // 4MB Max
                SystemTable->BootServices->AllocatePool(EfiLoaderData, fileSize, &fileBuffer);
                Status = file->Read(file, &fileSize, fileBuffer);
                file->Close(file);
                root->Close(root);
                if (!EFI_ERROR(Status) && fileSize > 0) break;
                SystemTable->BootServices->FreePool(fileBuffer);
                fileBuffer = NULL;
            } else {
                root->Close(root);
            }
        }
        
        SystemTable->BootServices->FreePool(handleBuffer);
        
        if (!fileBuffer) {
            return false; // App not found
        }
        
        Elf64_Ehdr* header = (Elf64_Ehdr*)fileBuffer;
        if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' || 
            header->e_ident[2] != 'L' || header->e_ident[3] != 'F') {
            SystemTable->BootServices->FreePool(fileBuffer);
            return false; // Not a valid ELF
        }
        
        // Very basic ELF execution jump
        // In a true paging OS, we'd map PT_LOAD segments here.
        // For our PIC flat ELFs, we compute offset and jump.
        AppMainFunc app_entry = (AppMainFunc)((uint8_t*)fileBuffer + header->e_entry);
        
        // System Call! Give control to the user app
        app_entry(&api);
        
        // App returned! Free memory and return to OS desktop
        SystemTable->BootServices->FreePool(fileBuffer);
        return true;
    }
}
