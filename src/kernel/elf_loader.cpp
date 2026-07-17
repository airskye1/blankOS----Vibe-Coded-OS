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
    extern void dui_draw_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_set_cursor_type(int type);
    extern void blankUI_draw_cursor(int x, int y);
    
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern int dui_text_height(int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    
    extern void play_system_sound(char* sound_name);
    extern void blankOS_panic(const char* error_code, const char* details);
    
    struct RTC_Time {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint32_t year;
    };
    extern void get_rtc_time(RTC_Time *time);
    
    typedef void (*AppMainFunc)(OS_API* api);

    bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename) {
        OS_API api;
        api.swap_buffers = swap_buffers;
        api.draw_wallpaper = dui_draw_wallpaper;
        api.draw_menubar = blankUI_draw_menubar;
        api.draw_dock = blankUI_draw_dock;
        
        api.set_cursor_type = [](SDK_CursorType type) {
            blankUI_set_cursor_type((int)type);
        };
        api.draw_cursor = blankUI_draw_cursor;
        
        api.draw_rect = dui_rect;
        api.draw_rect_rounded = dui_rect_rounded;
        api.draw_rect_outline = dui_rect_outline;
        api.draw_circle = dui_circle;
        api.draw_line = dui_line;
        api.draw_text = dui_text;
        api.get_text_width = dui_text_width;
        api.get_text_height = dui_text_height;
        
        api.draw_window = [](int x, int y, int w, int h, const char* title) {
            dui_shadow(x, y, w, h, 12, 12, 0x000000, 60);
            dui_rect_rounded(x, y, w, h, 12, 0xFFFFFF, 255);
            dui_rect_rounded(x, y, w, 32, 12, 0xE5E5EA, 255);
            dui_rect(x, y + 20, w, 12, 0xE5E5EA, 255);
            dui_circle(x + 16, y + 16, 6, 0xFF5F56, 255);
            dui_circle(x + 36, y + 16, 6, 0xFFBD2E, 255);
            dui_circle(x + 56, y + 16, 6, 0x27C93F, 255);
            dui_text(x + 80, y + 12, title, 0x333333, 1);
        };
        
        api.draw_button = [](int x, int y, int w, int h, const char* text, int mouse_x, int mouse_y, bool mouse_click) -> bool {
            bool hover = (mouse_x >= x && mouse_x <= x + w && mouse_y >= y && mouse_y <= y + h);
            uint32_t color = hover ? 0x0056B3 : 0x007AFF;
            dui_rect_rounded(x, y, w, h, 8, color, 255);
            int tw = dui_text_width(text, 1);
            dui_text(x + (w - tw)/2, y + (h - 8)/2, text, 0xFFFFFF, 1);
            return hover && mouse_click;
        };
        
        api.open_file = [](const char* path, void** buffer, uint64_t* size) -> bool {
            return false;
        };
        
        api.write_file = [](const char* path, void* buffer, uint64_t size) -> bool {
            return false;
        };
        
        api.play_sound = [](const char* sound_name) {
            play_system_sound((char*)sound_name);
        };
        
        api.get_time = [](int* hour, int* minute, int* second) {
            RTC_Time t;
            get_rtc_time(&t);
            if (hour) *hour = t.hour;
            if (minute) *minute = t.minute;
            if (second) *second = t.second;
        };
        
        api.panic = [](const char* error_code, const char* details) {
            blankOS_panic(error_code, details);
        };
        
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
        
        AppMainFunc app_entry = (AppMainFunc)((uint8_t*)fileBuffer + header->e_entry);
        
        app_entry(&api);
        
        SystemTable->BootServices->FreePool(fileBuffer);
        return true;
    }
}
