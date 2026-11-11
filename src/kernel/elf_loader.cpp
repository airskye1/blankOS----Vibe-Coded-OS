#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include "os_api.h"
#include "bloe_loader.h"

// Minimal ELF64 Definitions (freestanding, no <elf.h> required)
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr  d_ptr;
    } d_un;
} Elf64_Dyn;

#define ELF_PT_LOAD        1
#define ELF_PT_DYNAMIC     2
#define ELF_SHT_SYMTAB     2
#define ELF_SHT_DYNSYM     11
#define ELF_DT_NULL        0
#define ELF_DT_RELA        7
#define ELF_DT_RELASZ      8
#define ELF_DT_RELAENT     9
#define ELF_R_X86_64_64    1
#define ELF_R_X86_64_REL   8
#define ELF_EM_X86_64      62
#define ELF_MAX_FILE_BYTES (8ULL * 1024ULL * 1024ULL)
#define ELF_MAX_IMG_BYTES  (16ULL * 1024ULL * 1024ULL)

static EFI_SYSTEM_TABLE* g_elf_api_st = NULL;
static EFI_GUID g_elf_file_info_guid = EFI_FILE_INFO_ID;

static void elf_zero(void* dst, uint64_t len) {
    uint8_t* p = (uint8_t*)dst;
    for (uint64_t i = 0; i < len; i++) p[i] = 0;
}

static void elf_copy(void* dst, const void* src, uint64_t len) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
}

static bool elf_streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void elf_ascii_to_wide_norm(const char* src, CHAR16* dst, int max_chars) {
    if (!dst || max_chars <= 0) return;
    int i = 0, j = 0;
    while (src && (src[i] == '/' || src[i] == '\\')) i++;
    while (src && src[i] && j < max_chars - 1) {
        char c = src[i++];
        if (c == '/') c = '\\';
        dst[j++] = (CHAR16)(uint8_t)c;
    }
    dst[j] = 0;
}

static bool elf_api_open_file(const char* path, void** buffer, uint64_t* size) {
    if (!g_elf_api_st || !g_elf_api_st->BootServices || !path || !buffer || !size) return false;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(g_elf_api_st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles)) || !handles) {
        return false;
    }

    CHAR16 wpath[192];
    elf_ascii_to_wide_norm(path, wpath, 192);
    if (!wpath[0]) {
        g_elf_api_st->BootServices->FreePool(handles);
        return false;
    }

    bool ok = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(g_elf_api_st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE file = NULL;
        if (!EFI_ERROR(root->Open(root, &file, wpath, EFI_FILE_MODE_READ, 0)) && file) {
            uint8_t info_buf[384];
            UINTN info_sz = sizeof(info_buf);
            UINT64 fsz = 0;
            if (!EFI_ERROR(file->GetInfo(file, &g_elf_file_info_guid, &info_sz, info_buf))) {
                fsz = ((EFI_FILE_INFO*)info_buf)->FileSize;
            }
            if (fsz == 0 || fsz > ELF_MAX_FILE_BYTES) fsz = ELF_MAX_FILE_BYTES;
            void* pool = NULL;
            if (!EFI_ERROR(g_elf_api_st->BootServices->AllocatePool(EfiLoaderData, (UINTN)(fsz + 1), &pool)) && pool) {
                UINTN rsz = (UINTN)fsz;
                if (!EFI_ERROR(file->Read(file, &rsz, pool)) && rsz > 0) {
                    ((uint8_t*)pool)[rsz] = 0;
                    *buffer = pool;
                    *size = (uint64_t)rsz;
                    ok = true;
                } else {
                    g_elf_api_st->BootServices->FreePool(pool);
                }
            }
            file->Close(file);
        }
        root->Close(root);
        if (ok) break;
    }

    g_elf_api_st->BootServices->FreePool(handles);
    return ok;
}

static bool elf_api_write_file(const char* path, void* buffer, uint64_t size) {
    if (!g_elf_api_st || !g_elf_api_st->BootServices || !path || (!buffer && size > 0)) return false;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(g_elf_api_st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles)) || !handles) {
        return false;
    }

    CHAR16 wpath[192];
    elf_ascii_to_wide_norm(path, wpath, 192);
    if (!wpath[0]) {
        g_elf_api_st->BootServices->FreePool(handles);
        return false;
    }

    bool wrote = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(g_elf_api_st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE file = NULL;
        if (!EFI_ERROR(root->Open(root, &file, wpath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)) && file) {
            UINTN wsz = (UINTN)size;
            if (wsz == 0 || (!EFI_ERROR(file->Write(file, &wsz, buffer)) && wsz == (UINTN)size)) {
                file->Flush(file);
                wrote = true;
            }
            file->Close(file);
        }
        root->Close(root);
        if (wrote) break;
    }

    g_elf_api_st->BootServices->FreePool(handles);
    return wrote;
}

static uint64_t elf_find_symbol_vaddr(const uint8_t* file_data, uint64_t file_size, const Elf64_Ehdr* hdr, const char* sym_name) {
    if (!hdr->e_shoff || hdr->e_shentsize < sizeof(Elf64_Shdr) || hdr->e_shnum == 0) return UINT64_MAX;
    uint64_t sh_end = hdr->e_shoff + (uint64_t)hdr->e_shnum * (uint64_t)hdr->e_shentsize;
    if (sh_end > file_size) return UINT64_MAX;

    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)(file_data + hdr->e_shoff + (uint64_t)i * hdr->e_shentsize);
        if (sh->sh_type != ELF_SHT_SYMTAB && sh->sh_type != ELF_SHT_DYNSYM) continue;
        if (sh->sh_link >= hdr->e_shnum) continue;
        const Elf64_Shdr* str_sh = (const Elf64_Shdr*)(file_data + hdr->e_shoff + (uint64_t)sh->sh_link * hdr->e_shentsize);
        if (sh->sh_offset + sh->sh_size > file_size) continue;
        if (str_sh->sh_offset + str_sh->sh_size > file_size) continue;

        uint64_t entsz = sh->sh_entsize ? sh->sh_entsize : sizeof(Elf64_Sym);
        if (entsz < sizeof(Elf64_Sym)) continue;
        uint64_t nsyms = sh->sh_size / entsz;
        const char* strtab = (const char*)(file_data + str_sh->sh_offset);

        for (uint64_t s = 0; s < nsyms; s++) {
            const Elf64_Sym* sym = (const Elf64_Sym*)(file_data + sh->sh_offset + s * entsz);
            if (sym->st_shndx == 0 || sym->st_name >= str_sh->sh_size) continue;
            const char* name = strtab + sym->st_name;
            uint64_t max_rem = str_sh->sh_size - sym->st_name;
            bool null_term = false;
            for (uint64_t k = 0; k < max_rem; k++) {
                if (name[k] == '\0') { null_term = true; break; }
            }
            if (null_term && elf_streq(name, sym_name)) {
                return sym->st_value;
            }
        }
    }
    return UINT64_MAX;
}

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper(int theme_id);
    extern int dui_get_wallpaper_id(void);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_set_cursor_type(int type);
    extern void blankUI_draw_cursor(int x, int y, int type);

    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern int dui_text_height(int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int elevation);
    extern void dui_glass_card(int x, int y, int w, int h, int radius, uint32_t tint, uint8_t alpha, bool focused);

    extern void play_system_sound(const char* sound_name);
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

    void elf_init_os_api(EFI_SYSTEM_TABLE* SystemTable, OS_API* api) {
        if (!api) return;
        g_elf_api_st = SystemTable;
        elf_zero(api, sizeof(OS_API));

        api->swap_buffers = swap_buffers;
        api->draw_wallpaper = []() { dui_draw_wallpaper(dui_get_wallpaper_id()); };
        api->draw_menubar = blankUI_draw_menubar;
        api->draw_dock = blankUI_draw_dock;

        api->set_cursor_type = [](SDK_CursorType type) {
            blankUI_set_cursor_type((int)type);
        };
        api->draw_cursor = [](int x, int y) {
            blankUI_draw_cursor(x, y, 0);
        };

        api->draw_rect = dui_rect;
        api->draw_rect_rounded = dui_rect_rounded;
        api->draw_rect_outline = dui_rect_outline;
        api->draw_circle = dui_circle;
        api->draw_line = [](int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
            (void)thickness;
            dui_line(x0, y0, x1, y1, color, 255);
        };
        api->draw_text = dui_text;
        api->get_text_width = dui_text_width;
        api->get_text_height = dui_text_height;

        api->draw_window = [](int x, int y, int w, int h, const char* title) {
            dui_shadow(x, y, w, h, 18, 10);
            dui_glass_card(x, y, w, h, 18, 0x0E1422, 242, true);
            dui_rect_rounded(x + 1, y + 1, w - 2, 34, 16, 0x161E31, 245);
            dui_rect(x + 1, y + 22, w - 2, 13, 0x161E31, 245);
            dui_line(x + 8, y + 35, x + w - 8, y + 35, 0x2A3652, 180);
            dui_circle(x + 18, y + 18, 5, 0xFB7185, 255);
            dui_circle(x + 36, y + 18, 5, 0xFBBF24, 255);
            dui_circle(x + 54, y + 18, 5, 0x34D399, 255);
            dui_text(x + 76, y + 10, title, 0xF8FAFC, 1);
        };

        api->draw_button = [](int x, int y, int w, int h, const char* text, int mouse_x, int mouse_y, bool mouse_click) -> bool {
            bool hover = (mouse_x >= x && mouse_x <= x + w && mouse_y >= y && mouse_y <= y + h);
            uint32_t bg = hover ? 0x22D3EE : 0x1E293B;
            uint32_t fg = hover ? 0x07101C : 0xF1F5F9;
            dui_rect_rounded(x, y, w, h, 10, bg, 245);
            dui_rect_outline(x, y, w, h, hover ? 0x67E8F9 : 0x334155, 1);
            int tw = dui_text_width(text, 1);
            int th = dui_text_height(1);
            dui_text(x + (w - tw) / 2, y + (h - th) / 2, text, fg, 1);
            return hover && mouse_click;
        };

        api->open_file = elf_api_open_file;
        api->write_file = elf_api_write_file;

        api->play_sound = [](const char* sound_name) {
            play_system_sound((char*)sound_name);
        };

        api->get_time = [](int* hour, int* minute, int* second) {
            RTC_Time t;
            get_rtc_time(&t);
            if (hour) *hour = t.hour;
            if (minute) *minute = t.minute;
            if (second) *second = t.second;
        };

        api->panic = [](const char* error_code, const char* details) {
            blankOS_panic(error_code, details);
        };

        api->SystemTable = SystemTable;
    }

    bool elf_run_from_memory(EFI_SYSTEM_TABLE* SystemTable, const uint8_t* file_data, uint64_t file_size) {
        if (!SystemTable || !SystemTable->BootServices || !file_data || file_size < sizeof(Elf64_Ehdr)) {
            return false;
        }

        const Elf64_Ehdr* hdr = (const Elf64_Ehdr*)file_data;
        if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
            hdr->e_ident[2] != 'L'  || hdr->e_ident[3] != 'F') {
            return false;
        }
        if (hdr->e_ident[4] != 2 || hdr->e_ident[5] != 1 || hdr->e_machine != ELF_EM_X86_64) {
            return false;
        }
        if (hdr->e_phoff == 0 || hdr->e_phnum == 0 || hdr->e_phentsize < sizeof(Elf64_Phdr)) {
            return false;
        }
        if (hdr->e_phoff + (uint64_t)hdr->e_phnum * (uint64_t)hdr->e_phentsize > file_size) {
            return false;
        }

        uint64_t min_vaddr = UINT64_MAX;
        uint64_t max_vaddr = 0;
        const Elf64_Phdr* dyn_phdr = NULL;

        for (uint16_t i = 0; i < hdr->e_phnum; i++) {
            const Elf64_Phdr* ph = (const Elf64_Phdr*)(file_data + hdr->e_phoff + (uint64_t)i * hdr->e_phentsize);
            if (ph->p_type == ELF_PT_DYNAMIC) {
                dyn_phdr = ph;
            } else if (ph->p_type == ELF_PT_LOAD && ph->p_memsz > 0) {
                if (ph->p_filesz > ph->p_memsz) return false;
                if (ph->p_offset + ph->p_filesz > file_size) return false;
                if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
                if (ph->p_vaddr + ph->p_memsz > max_vaddr) max_vaddr = ph->p_vaddr + ph->p_memsz;
            }
        }

        if (min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) return false;
        uint64_t img_size = max_vaddr - min_vaddr;
        if (img_size > ELF_MAX_IMG_BYTES) return false;

        void* image_base = NULL;
        if (EFI_ERROR(SystemTable->BootServices->AllocatePool(EfiLoaderCode, (UINTN)img_size, &image_base)) || !image_base) {
            return false;
        }
        elf_zero(image_base, img_size);

        for (uint16_t i = 0; i < hdr->e_phnum; i++) {
            const Elf64_Phdr* ph = (const Elf64_Phdr*)(file_data + hdr->e_phoff + (uint64_t)i * hdr->e_phentsize);
            if (ph->p_type == ELF_PT_LOAD && ph->p_filesz > 0) {
                elf_copy((uint8_t*)image_base + (ph->p_vaddr - min_vaddr), file_data + ph->p_offset, ph->p_filesz);
            }
        }

        // Apply dynamic R_X86_64_RELATIVE / R_X86_64_64 relocations for PIE/shared apps
        if (dyn_phdr && dyn_phdr->p_vaddr >= min_vaddr && dyn_phdr->p_vaddr + dyn_phdr->p_memsz <= max_vaddr) {
            const Elf64_Dyn* dyn = (const Elf64_Dyn*)((uint8_t*)image_base + (dyn_phdr->p_vaddr - min_vaddr));
            uint64_t max_dyn = dyn_phdr->p_memsz / sizeof(Elf64_Dyn);
            uint64_t rela_vaddr = 0;
            uint64_t rela_sz = 0;
            uint64_t rela_ent = sizeof(Elf64_Rela);
            bool has_rela = false;

            for (uint64_t i = 0; i < max_dyn && dyn[i].d_tag != ELF_DT_NULL; i++) {
                if (dyn[i].d_tag == ELF_DT_RELA) { rela_vaddr = dyn[i].d_un.d_ptr; has_rela = true; }
                else if (dyn[i].d_tag == ELF_DT_RELASZ) rela_sz = dyn[i].d_un.d_val;
                else if (dyn[i].d_tag == ELF_DT_RELAENT && dyn[i].d_un.d_val >= sizeof(Elf64_Rela)) rela_ent = dyn[i].d_un.d_val;
            }

            if (has_rela && rela_sz > 0 && rela_vaddr >= min_vaddr && (rela_vaddr - min_vaddr) + rela_sz <= img_size) {
                const uint8_t* rptr = (const uint8_t*)image_base + (rela_vaddr - min_vaddr);
                uint64_t nrel = rela_sz / rela_ent;
                uint64_t base_delta = (uint64_t)(uintptr_t)image_base - min_vaddr;
                for (uint64_t r = 0; r < nrel; r++) {
                    const Elf64_Rela* rela = (const Elf64_Rela*)(rptr + r * rela_ent);
                    uint32_t rtype = (uint32_t)(rela->r_info & 0xFFFFFFFFULL);
                    if (rtype == ELF_R_X86_64_REL || rtype == ELF_R_X86_64_64) {
                        if (rela->r_offset >= min_vaddr && (rela->r_offset - min_vaddr) + 8 <= img_size) {
                            uint64_t* target = (uint64_t*)((uint8_t*)image_base + (rela->r_offset - min_vaddr));
                            if (rtype == ELF_R_X86_64_REL) {
                                *target = (uint64_t)((int64_t)base_delta + rela->r_addend);
                            }
                        }
                    }
                }
            }
        }

        // Prefer named entry symbol ("main" / "bloe_main" / "_start") so helper functions linked first aren't called by mistake
        uint64_t entry_vaddr = elf_find_symbol_vaddr(file_data, file_size, hdr, "main");
        if (entry_vaddr == UINT64_MAX) entry_vaddr = elf_find_symbol_vaddr(file_data, file_size, hdr, "bloe_main");
        if (entry_vaddr == UINT64_MAX) entry_vaddr = elf_find_symbol_vaddr(file_data, file_size, hdr, "_start");
        if (entry_vaddr == UINT64_MAX) entry_vaddr = hdr->e_entry;

        if (entry_vaddr < min_vaddr || (entry_vaddr - min_vaddr) >= img_size) {
            SystemTable->BootServices->FreePool(image_base);
            return false;
        }

        OS_API api;
        elf_init_os_api(SystemTable, &api);

        AppMainFunc app_entry = (AppMainFunc)((uint8_t*)image_base + (entry_vaddr - min_vaddr));
        app_entry(&api);

        SystemTable->BootServices->FreePool(image_base);
        return true;
    }

    bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename) {
        if (!SystemTable || !SystemTable->BootServices || !filename) return false;

        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        EFI_STATUS Status = SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
        if (EFI_ERROR(Status) || numHandles == 0 || !handleBuffer) return false;

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
                uint8_t info_buf[384];
                UINTN info_sz = sizeof(info_buf);
                UINT64 exact_sz = ELF_MAX_FILE_BYTES;
                if (!EFI_ERROR(file->GetInfo(file, &g_elf_file_info_guid, &info_sz, info_buf))) {
                    UINT64 fsz = ((EFI_FILE_INFO*)info_buf)->FileSize;
                    if (fsz > 0 && fsz <= ELF_MAX_FILE_BYTES) exact_sz = fsz;
                }
                fileSize = (UINTN)exact_sz;
                if (!EFI_ERROR(SystemTable->BootServices->AllocatePool(EfiLoaderData, fileSize, &fileBuffer)) && fileBuffer) {
                    Status = file->Read(file, &fileSize, fileBuffer);
                    file->Close(file);
                    root->Close(root);
                    if (!EFI_ERROR(Status) && fileSize > 0) break;
                    SystemTable->BootServices->FreePool(fileBuffer);
                    fileBuffer = NULL;
                } else {
                    file->Close(file);
                    root->Close(root);
                }
            } else {
                root->Close(root);
            }
        }

        SystemTable->BootServices->FreePool(handleBuffer);
        if (!fileBuffer) return false;

        bool ok = elf_run_from_memory(SystemTable, (const uint8_t*)fileBuffer, (uint64_t)fileSize);
        SystemTable->BootServices->FreePool(fileBuffer);
        return ok;
    }
}
