#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
    extern int screen_width;
    extern int screen_height;

    extern void swap_buffers(void);
    extern void play_system_sound(char* sound_name);

    extern void dui_draw_wallpaper(void);
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);

    extern void blankUI_draw_menubar(void);
    extern void blankUI_draw_dock(void);
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_draw_topbar(char* app_title);
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    extern void blankUI_set_cursor_type(int type);
}

#define VFS_MAX_FILES 32
#define VFS_NAME_MAX  64
#define VFS_DATA_MAX  4096

typedef struct {
    char name[VFS_NAME_MAX];
    char content[VFS_DATA_MAX];
    int size;
    bool on_disk;
    bool is_binary;
    bool used;
} VfsEntry;

static VfsEntry g_vfs_files[VFS_MAX_FILES];
static int g_vfs_count = 0;
static bool g_vfs_initialized = false;
static bool g_fat32_writable = false;
static int g_fat32_synced_count = 0;

static int bp_str_len(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void bp_str_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void bp_str_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int dlen = bp_str_len(dst);
    int i = 0;
    while (src[i] && (dlen + i) < max_len - 1) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static char bp_to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static bool bp_str_eq_nocase(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        char ca = bp_to_upper(a[i]);
        char cb = bp_to_upper(b[i]);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static bool bp_str_ends_with_nocase(const char* str, const char* suffix) {
    int slen = bp_str_len(str);
    int xlen = bp_str_len(suffix);
    if (xlen > slen) return false;
    return bp_str_eq_nocase(str + (slen - xlen), suffix);
}

static void bp_int_to_str(int val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    bool neg = false;
    if (val < 0) {
        neg = true;
        val = -val;
    }
    char tmp[24];
    int pos = 0;
    while (val > 0 && pos < 20) {
        tmp[pos++] = (char)('0' + (val % 10));
        val /= 10;
    }
    if (neg && pos < 22) tmp[pos++] = '-';
    int j = 0;
    while (pos > 0 && j < max_len - 1) {
        out[j++] = tmp[--pos];
    }
    out[j] = '\0';
}

static void bp_ascii_to_wide(const char* src, CHAR16* dst, int max_chars) {
    if (!dst || max_chars <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max_chars - 1) {
            char c = src[i];
            if (c == '/') c = '\\';
            dst[i] = (CHAR16)(uint8_t)c;
            i++;
        }
    }
    dst[i] = 0;
}

static void bp_wide_to_ascii(const CHAR16* src, char* dst, int max_chars) {
    if (!dst || max_chars <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max_chars - 1) {
            CHAR16 wc = src[i];
            dst[i] = (wc >= 32 && wc <= 126) ? (char)wc : '?';
            i++;
        }
    }
    dst[i] = '\0';
}

static int bp_find_file_index(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < g_vfs_count; i++) {
        if (g_vfs_files[i].used && bp_str_eq_nocase(g_vfs_files[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int bp_classify_file(const char* name, bool on_disk) {
    // 1 = Documents, 2 = System Config, 3 = Disk Volume
    if (bp_str_ends_with_nocase(name, ".ini") ||
        bp_str_ends_with_nocase(name, ".cfg") ||
        bp_str_ends_with_nocase(name, ".json") ||
        bp_str_ends_with_nocase(name, ".reg") ||
        bp_str_ends_with_nocase(name, ".efi") ||
        bp_str_ends_with_nocase(name, ".sh")) {
        return 2;
    }
    if (bp_str_ends_with_nocase(name, ".md") ||
        bp_str_ends_with_nocase(name, ".txt") ||
        bp_str_ends_with_nocase(name, ".c") ||
        bp_str_ends_with_nocase(name, ".cpp") ||
        bp_str_ends_with_nocase(name, ".h")) {
        return 1;
    }
    return on_disk ? 3 : 1;
}

static uint32_t bp_file_badge_color(const char* name) {
    if (bp_str_ends_with_nocase(name, ".c") || bp_str_ends_with_nocase(name, ".cpp") || bp_str_ends_with_nocase(name, ".h")) {
        return 0x0A84FF; // Code Blue
    }
    if (bp_str_ends_with_nocase(name, ".ini") || bp_str_ends_with_nocase(name, ".json") || bp_str_ends_with_nocase(name, ".reg")) {
        return 0xFF9F0A; // Config Amber
    }
    if (bp_str_ends_with_nocase(name, ".md")) {
        return 0xBF5AF2; // Markdown Purple
    }
    if (bp_str_ends_with_nocase(name, ".efi") || bp_str_ends_with_nocase(name, ".elf")) {
        return 0x30D158; // Binary Green
    }
    return 0x5E5CE6; // Text Indigo
}

static const char* bp_file_badge_label(const char* name) {
    if (bp_str_ends_with_nocase(name, ".c")) return "C";
    if (bp_str_ends_with_nocase(name, ".cpp")) return "C++";
    if (bp_str_ends_with_nocase(name, ".md")) return "MD";
    if (bp_str_ends_with_nocase(name, ".ini")) return "INI";
    if (bp_str_ends_with_nocase(name, ".json")) return "JSON";
    if (bp_str_ends_with_nocase(name, ".efi")) return "EFI";
    if (bp_str_ends_with_nocase(name, ".elf")) return "ELF";
    if (bp_str_ends_with_nocase(name, ".ttf")) return "TTF";
    if (bp_str_ends_with_nocase(name, ".sh")) return "SH";
    return "TXT";
}

static bool bp_fat32_write_raw(EFI_SYSTEM_TABLE *st, const char* name, const char* content, int len) {
    if (!st || !st->BootServices || !name || !content) return false;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles);
    if (EFI_ERROR(status) || numHandles == 0 || !handles) return false;

    CHAR16 wideName[VFS_NAME_MAX];
    bp_ascii_to_wide(name, wideName, VFS_NAME_MAX);

    bool wrote = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        // First delete existing file if present so truncated content doesn't leave stale tail bytes
        EFI_FILE_HANDLE oldFile = NULL;
        if (!EFI_ERROR(root->Open(root, &oldFile, wideName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && oldFile) {
            oldFile->Delete(oldFile);
        }

        EFI_FILE_HANDLE file = NULL;
        status = root->Open(root, &file, wideName,
                            EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (!EFI_ERROR(status) && file) {
            UINTN writeSize = (UINTN)len;
            EFI_STATUS wst = file->Write(file, &writeSize, (void*)content);
            file->Flush(file);
            file->Close(file);
            if (!EFI_ERROR(wst)) {
                wrote = true;
                g_fat32_writable = true;
            }
        }
        root->Close(root);
        if (wrote) break;
    }

    st->BootServices->FreePool(handles);
    return wrote;
}

static bool bp_fat32_delete_raw(EFI_SYSTEM_TABLE *st, const char* name) {
    if (!st || !st->BootServices || !name) return false;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles);
    if (EFI_ERROR(status) || numHandles == 0 || !handles) return false;

    CHAR16 wideName[VFS_NAME_MAX];
    bp_ascii_to_wide(name, wideName, VFS_NAME_MAX);

    bool deleted = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE file = NULL;
        status = root->Open(root, &file, wideName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (!EFI_ERROR(status) && file) {
            if (!EFI_ERROR(file->Delete(file))) {
                deleted = true;
            }
        }
        root->Close(root);
    }

    st->BootServices->FreePool(handles);
    return deleted;
}

static bool bp_fat32_read_raw(EFI_SYSTEM_TABLE *st, const char* name, char* out_buf, int max_len, int* out_real_size, bool* out_is_binary) {
    if (!st || !st->BootServices || !name || !out_buf || max_len <= 1) return false;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles);
    if (EFI_ERROR(status) || numHandles == 0 || !handles) return false;

    CHAR16 wideName[VFS_NAME_MAX];
    bp_ascii_to_wide(name, wideName, VFS_NAME_MAX);

    bool ok = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE file = NULL;
        status = root->Open(root, &file, wideName, EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status) && file) {
            uint8_t raw[VFS_DATA_MAX];
            UINTN readBytes = (UINTN)(max_len - 1);
            if (readBytes > VFS_DATA_MAX - 1) readBytes = VFS_DATA_MAX - 1;
            EFI_STATUS rst = file->Read(file, &readBytes, raw);
            if (!EFI_ERROR(rst)) {
                bool is_bin = false;
                for (UINTN b = 0; b < readBytes; b++) {
                    uint8_t ch = raw[b];
                    if (ch == 0 || (ch < 9) || (ch > 13 && ch < 32)) {
                        is_bin = true;
                        break;
                    }
                }
                if (out_is_binary) *out_is_binary = is_bin;
                if (!is_bin) {
                    int w = 0;
                    for (UINTN b = 0; b < readBytes && w < max_len - 1; b++) {
                        if (raw[b] == '\r') continue;
                        out_buf[w++] = (char)raw[b];
                    }
                    out_buf[w] = '\0';
                    if (out_real_size && *out_real_size <= 0) *out_real_size = (int)readBytes;
                } else {
                    // Format real binary header inspection preview from actual disk bytes
                    const char* hex = "0123456789ABCDEF";
                    out_buf[0] = '\0';
                    bp_str_cat(out_buf, "# Binary File on UEFI FAT32 Partition: ", max_len);
                    bp_str_cat(out_buf, name, max_len);
                    bp_str_cat(out_buf, "\n# Real Disk Header Hex Dump (First 128 Bytes):\n\n", max_len);
                    UINTN dumpLen = readBytes < 128 ? readBytes : 128;
                    for (UINTN row = 0; row < dumpLen; row += 16) {
                        char line[96];
                        int lp = 0;
                        line[lp++] = hex[(row >> 4) & 0xF];
                        line[lp++] = hex[row & 0xF];
                        line[lp++] = ':';
                        line[lp++] = ' ';
                        for (UINTN col = 0; col < 16; col++) {
                            if (row + col < dumpLen) {
                                uint8_t v = raw[row + col];
                                line[lp++] = hex[(v >> 4) & 0xF];
                                line[lp++] = hex[v & 0xF];
                                line[lp++] = ' ';
                            } else {
                                line[lp++] = ' ';
                                line[lp++] = ' ';
                                line[lp++] = ' ';
                            }
                        }
                        line[lp++] = ' ';
                        line[lp++] = '|';
                        for (UINTN col = 0; col < 16 && (row + col) < dumpLen; col++) {
                            uint8_t v = raw[row + col];
                            line[lp++] = (v >= 32 && v <= 126) ? (char)v : '.';
                        }
                        line[lp++] = '|';
                        line[lp++] = '\n';
                        line[lp] = '\0';
                        bp_str_cat(out_buf, line, max_len);
                    }
                }
                ok = true;
            }
            file->Close(file);
        }
        root->Close(root);
        if (ok) break;
    }

    st->BootServices->FreePool(handles);
    return ok;
}

static int bp_upsert_memory_entry(const char* name, const char* content, int explicit_size, bool on_disk, bool is_binary) {
    if (!name || !name[0]) return -1;
    int idx = bp_find_file_index(name);
    if (idx < 0) {
        if (g_vfs_count >= VFS_MAX_FILES) return -1;
        idx = g_vfs_count++;
    }
    g_vfs_files[idx].used = true;
    bp_str_copy(g_vfs_files[idx].name, name, VFS_NAME_MAX);
    bp_str_copy(g_vfs_files[idx].content, content ? content : "", VFS_DATA_MAX);
    g_vfs_files[idx].size = (explicit_size > 0) ? explicit_size : bp_str_len(g_vfs_files[idx].content);
    g_vfs_files[idx].on_disk = on_disk;
    g_vfs_files[idx].is_binary = is_binary;
    return idx;
}

static void bp_scan_directory_handle(EFI_SYSTEM_TABLE *st, EFI_FILE_HANDLE dir, const char* prefix) {
    if (!dir) return;
    dir->SetPosition(dir, 0);

    uint8_t infoBuf[512];
    int safety = 0;
    while (safety++ < 64) {
        UINTN bufSize = sizeof(infoBuf);
        EFI_STATUS status = dir->Read(dir, &bufSize, infoBuf);
        if (EFI_ERROR(status) || bufSize == 0) break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO*)infoBuf;
        char entryName[48];
        bp_wide_to_ascii(info->FileName, entryName, sizeof(entryName));
        if (entryName[0] == '\0' || entryName[0] == '.') continue;

        if (info->Attribute & EFI_FILE_DIRECTORY) continue;

        char fullPath[VFS_NAME_MAX];
        fullPath[0] = '\0';
        if (prefix && prefix[0]) {
            bp_str_copy(fullPath, prefix, VFS_NAME_MAX);
            bp_str_cat(fullPath, "\\", VFS_NAME_MAX);
        }
        bp_str_cat(fullPath, entryName, VFS_NAME_MAX);

        char contentBuf[VFS_DATA_MAX];
        int diskSize = (int)info->FileSize;
        bool isBin = false;
        if (bp_fat32_read_raw(st, fullPath, contentBuf, VFS_DATA_MAX, &diskSize, &isBin)) {
            bp_upsert_memory_entry(fullPath, contentBuf, diskSize, true, isBin);
            g_fat32_synced_count++;
        }
    }
}

static void bp_sync_from_fat32(EFI_SYSTEM_TABLE *st) {
    if (!st || !st->BootServices) return;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles);
    if (EFI_ERROR(status) || numHandles == 0 || !handles) return;

    g_fat32_synced_count = 0;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        // 1. Scan root directory entries
        bp_scan_directory_handle(st, root, "");

        // 2. Scan EFI\BOOT directory
        EFI_FILE_HANDLE bootDir = NULL;
        if (!EFI_ERROR(root->Open(root, &bootDir, (CHAR16*)L"EFI\\BOOT", EFI_FILE_MODE_READ, 0)) && bootDir) {
            bp_scan_directory_handle(st, bootDir, "EFI\\BOOT");
            bootDir->Close(bootDir);
        }

        // 3. Scan assets directory
        EFI_FILE_HANDLE assetsDir = NULL;
        if (!EFI_ERROR(root->Open(root, &assetsDir, (CHAR16*)L"assets", EFI_FILE_MODE_READ, 0)) && assetsDir) {
            bp_scan_directory_handle(st, assetsDir, "assets");
            assetsDir->Close(assetsDir);
        }

        root->Close(root);
    }

    st->BootServices->FreePool(handles);

    // Also explicitly sync known disk files if present on any FAT32 volume
    const char* probe_files[7] = {
        "README.TXT", "NOTES.TXT", "CONFIG.INI", "BOOTLOG.TXT", "HELLO.SH", "REGISTRY.INI", "version.json"
    };
    for (int p = 0; p < 7; p++) {
        char probeBuf[VFS_DATA_MAX];
        int probeSz = 0;
        bool probeBin = false;
        if (bp_fat32_read_raw(st, probe_files[p], probeBuf, VFS_DATA_MAX, &probeSz, &probeBin)) {
            bp_upsert_memory_entry(probe_files[p], probeBuf, probeSz, true, probeBin);
        }
    }
}

extern "C" {

void vfs_init(EFI_SYSTEM_TABLE *st) {
    EFI_SYSTEM_TABLE *activeSt = st ? st : gSystemTable;
    if (!g_vfs_initialized) {
        g_vfs_count = 0;
        for (int i = 0; i < VFS_MAX_FILES; i++) {
            g_vfs_files[i].used = false;
        }
        g_vfs_initialized = true;

        const char* readme_default =
            "# BlankOS Sequoia Twist - Files & BlankPad Studio\n"
            "Welcome to the unified macOS Finder + BlankPad Code Studio.\n\n"
            "## Keyboard Shortcuts\n"
            "- Arrow Keys : Move editor cursor (Up / Down / Left / Right)\n"
            "- Enter / BS : Insert newline / delete character\n"
            "- Ctrl + S   : Flush active buffer to UEFI FAT32 Volume\n"
            "- ESC        : Close window and return to desktop\n\n"
            "## Storage Architecture\n"
            "- Real-time sync with EFI_SIMPLE_FILE_SYSTEM_PROTOCOL\n"
            "- Direct FAT32 root & EFI\\BOOT directory enumeration\n";

        const char* notes_default =
            "BlankOS Engineering Notes & Todo:\n"
            "1. [DONE] TrueType font rendering via stb_truetype\n"
            "2. [DONE] UEFI GOP hardware-accelerated double buffer\n"
            "3. [DONE] Real FAT32 file read/write/delete persistence\n"
            "4. [TODO] Test custom registry keys in blankReg Studio\n";

        const char* sysini_default =
            "[Boot]\n"
            "kernel=EFI\\BOOT\\BOOTX64.EFI\n"
            "resolution=auto\n"
            "audio=hda\n"
            "theme=glass_twist\n"
            "vfs_mount=fat32_rw\n";

        const char* helloc_default =
            "// Sample Freestanding BlankOS Application\n"
            "#include <stdint.h>\n\n"
            "extern \"C\" void dui_text(int x, int y, const char* s, uint32_t c, int sc);\n\n"
            "void blankos_app_entry(void) {\n"
            "    dui_text(40, 40, \"Hello from BlankPad Studio!\", 0x0A84FF, 1);\n"
            "}\n";

        const char* credits_default =
            "BlankOS Operating System Credits\n"
            "================================\n"
            "- Kernel Core     : UEFI x86_64 Long Mode Executive\n"
            "- Compositor      : blankDUI + Glassmorphic Sequoia UI\n"
            "- Storage Stack   : UEFI FAT32 + In-Memory VFS Layer\n"
            "- Audio Subsystem : Intel HDA + PC Speaker Synth\n";

        bp_upsert_memory_entry("README.md", readme_default, 0, false, false);
        bp_upsert_memory_entry("NOTES.txt", notes_default, 0, false, false);
        bp_upsert_memory_entry("SYSTEM.ini", sysini_default, 0, false, false);
        bp_upsert_memory_entry("HELLO.c", helloc_default, 0, false, false);
        bp_upsert_memory_entry("CREDITS.txt", credits_default, 0, false, false);
    }

    if (activeSt) {
        // Scan real files from the UEFI FAT32 partition first
        bp_sync_from_fat32(activeSt);

        // Ensure default text files are persisted to writable FAT32 volume if not yet on disk
        for (int i = 0; i < g_vfs_count; i++) {
            if (g_vfs_files[i].used && !g_vfs_files[i].on_disk && !g_vfs_files[i].is_binary) {
                if (bp_fat32_write_raw(activeSt, g_vfs_files[i].name, g_vfs_files[i].content, g_vfs_files[i].size)) {
                    g_vfs_files[i].on_disk = true;
                }
            }
        }
    }
}

int vfs_get_file_count(void) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    return g_vfs_count;
}

const char* vfs_get_file_name(int idx) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    if (idx < 0 || idx >= g_vfs_count || !g_vfs_files[idx].used) return "";
    return g_vfs_files[idx].name;
}

int vfs_get_file_size(int idx) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    if (idx < 0 || idx >= g_vfs_count || !g_vfs_files[idx].used) return 0;
    return g_vfs_files[idx].size;
}

const char* vfs_get_file_content(int idx) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    if (idx < 0 || idx >= g_vfs_count || !g_vfs_files[idx].used) return "";
    return g_vfs_files[idx].content;
}

bool vfs_read_file(const char* name, char* out_buf, int max_len) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    if (!name || !out_buf || max_len <= 0) return false;

    int idx = bp_find_file_index(name);
    if (idx >= 0) {
        // Refresh from real FAT32 disk if backed by disk and not binary
        if (g_vfs_files[idx].on_disk && !g_vfs_files[idx].is_binary && gSystemTable) {
            int realSz = g_vfs_files[idx].size;
            bool isBin = false;
            if (bp_fat32_read_raw(gSystemTable, g_vfs_files[idx].name, g_vfs_files[idx].content, VFS_DATA_MAX, &realSz, &isBin)) {
                g_vfs_files[idx].size = bp_str_len(g_vfs_files[idx].content);
            }
        }
        bp_str_copy(out_buf, g_vfs_files[idx].content, max_len);
        return true;
    }

    // Try reading directly from UEFI FAT32 partition if not yet cached
    if (gSystemTable) {
        int diskSz = 0;
        bool isBin = false;
        char tempBuf[VFS_DATA_MAX];
        if (bp_fat32_read_raw(gSystemTable, name, tempBuf, VFS_DATA_MAX, &diskSz, &isBin)) {
            bp_upsert_memory_entry(name, tempBuf, diskSz, true, isBin);
            bp_str_copy(out_buf, tempBuf, max_len);
            return true;
        }
    }
    return false;
}

bool vfs_write_file(const char* name, const char* content) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    if (!name || !name[0] || !content) return false;

    int len = bp_str_len(content);
    bool disk_ok = false;
    if (gSystemTable) {
        disk_ok = bp_fat32_write_raw(gSystemTable, name, content, len);
    }
    int idx = bp_upsert_memory_entry(name, content, len, disk_ok, false);
    return (idx >= 0);
}

bool vfs_delete_file(const char* name) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    if (!name || !name[0]) return false;

    if (gSystemTable) {
        bp_fat32_delete_raw(gSystemTable, name);
    }

    int idx = bp_find_file_index(name);
    if (idx < 0) return false;

    for (int i = idx; i < g_vfs_count - 1; i++) {
        g_vfs_files[i] = g_vfs_files[i + 1];
    }
    if (g_vfs_count > 0) {
        g_vfs_files[g_vfs_count - 1].used = false;
        g_vfs_count--;
    }
    return true;
}

} // extern "C"

static void bp_compute_line_col(const char* buf, int cursor, int* out_line, int* out_col) {
    int line = 0;
    int col = 0;
    for (int i = 0; i < cursor && buf[i]; i++) {
        if (buf[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    *out_line = line;
    *out_col = col;
}

static int bp_offset_for_line_col(const char* buf, int target_line, int target_col) {
    int line = 0;
    int i = 0;
    while (buf[i] && line < target_line) {
        if (buf[i] == '\n') line++;
        i++;
    }
    int col = 0;
    while (buf[i] && buf[i] != '\n' && col < target_col) {
        i++;
        col++;
    }
    return i;
}

extern "C" void launch_blankpad(void) {
    EFI_SYSTEM_TABLE *SystemTable = gSystemTable;
    blankUI_draw_topbar((char*)"Files & BlankPad Studio");
    vfs_init(SystemTable);

    if (!SystemTable) return;

    EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;

    const int win_w = 780;
    const int win_h = 500;

    int view_mode = 0;       // 0 = Split View, 1 = Finder Grid, 2 = Editor Mode
    int category_filter = 0; // 0 = All Files, 1 = Documents, 2 = System Config, 3 = Disk Volume (FAT32)
    int selected_file = 0;
    int cursor_pos = 0;
    int scroll_line = 0;
    bool dirty = false;
    bool editing_filename = false;
    char filename_edit_buf[VFS_NAME_MAX];
    char status_pill[64];
    bp_str_copy(status_pill, "Saved to FAT32 & VFS", sizeof(status_pill));

    if (g_vfs_count > 0) {
        bp_str_copy(filename_edit_buf, g_vfs_files[0].name, VFS_NAME_MAX);
        cursor_pos = bp_str_len(g_vfs_files[0].content);
    } else {
        bp_str_copy(filename_edit_buf, "UNTITLED.txt", VFS_NAME_MAX);
    }

    int new_file_seq = 1;
    bool prev_left = false;
    uint32_t blink_tick = 0;
    bool done = false;

    while (!done) {
        blink_tick++;
        int win_x = 0, win_y = 0;
        blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);

        // Build filtered file index mapping
        int filtered_indices[VFS_MAX_FILES];
        int filtered_count = 0;
        for (int i = 0; i < g_vfs_count; i++) {
            if (!g_vfs_files[i].used) continue;
            bool match = false;
            if (category_filter == 0) match = true;
            else if (category_filter == 1 && bp_classify_file(g_vfs_files[i].name, g_vfs_files[i].on_disk) == 1) match = true;
            else if (category_filter == 2 && bp_classify_file(g_vfs_files[i].name, g_vfs_files[i].on_disk) == 2) match = true;
            else if (category_filter == 3 && g_vfs_files[i].on_disk) match = true;
            if (match) {
                filtered_indices[filtered_count++] = i;
            }
        }

        if (selected_file >= g_vfs_count) selected_file = g_vfs_count - 1;
        if (selected_file < 0 && g_vfs_count > 0) selected_file = 0;

        // Keyboard input
        EFI_INPUT_KEY Key;
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
            if (Key.ScanCode == 0x17) {
                done = true;
            } else if (Key.UnicodeChar == 0x13 || Key.ScanCode == 0x0C) {
                // Ctrl+S (0x13) or F2 (0x0C): Save to FAT32 & VFS
                if (selected_file >= 0 && selected_file < g_vfs_count) {
                    if (editing_filename && filename_edit_buf[0]) {
                        bp_str_copy(g_vfs_files[selected_file].name, filename_edit_buf, VFS_NAME_MAX);
                        editing_filename = false;
                    }
                    bool disk_saved = vfs_write_file(g_vfs_files[selected_file].name, g_vfs_files[selected_file].content);
                    dirty = false;
                    bp_str_copy(status_pill, disk_saved ? "Saved to FAT32 & VFS" : "Saved in VFS Memory", sizeof(status_pill));
                    play_system_sound((char*)"notification");
                }
            } else if (editing_filename) {
                int flen = bp_str_len(filename_edit_buf);
                if (Key.UnicodeChar == 0x0D) {
                    if (filename_edit_buf[0] && selected_file >= 0) {
                        bp_str_copy(g_vfs_files[selected_file].name, filename_edit_buf, VFS_NAME_MAX);
                    }
                    editing_filename = false;
                } else if (Key.UnicodeChar == 0x08) {
                    if (flen > 0) filename_edit_buf[flen - 1] = '\0';
                } else if (Key.UnicodeChar >= 33 && Key.UnicodeChar <= 126 && flen < VFS_NAME_MAX - 1) {
                    filename_edit_buf[flen] = (char)Key.UnicodeChar;
                    filename_edit_buf[flen + 1] = '\0';
                }
            } else if (selected_file >= 0 && selected_file < g_vfs_count && view_mode != 1 && !g_vfs_files[selected_file].is_binary) {
                char* buf = g_vfs_files[selected_file].content;
                int len = bp_str_len(buf);
                if (cursor_pos > len) cursor_pos = len;
                if (cursor_pos < 0) cursor_pos = 0;

                if (Key.ScanCode == 0x01) { // Up arrow
                    int cur_line = 0, cur_col = 0;
                    bp_compute_line_col(buf, cursor_pos, &cur_line, &cur_col);
                    if (cur_line > 0) {
                        cursor_pos = bp_offset_for_line_col(buf, cur_line - 1, cur_col);
                    } else {
                        cursor_pos = 0;
                    }
                } else if (Key.ScanCode == 0x02) { // Down arrow
                    int cur_line = 0, cur_col = 0;
                    bp_compute_line_col(buf, cursor_pos, &cur_line, &cur_col);
                    cursor_pos = bp_offset_for_line_col(buf, cur_line + 1, cur_col);
                } else if (Key.ScanCode == 0x03) { // Right arrow
                    if (cursor_pos < len) cursor_pos++;
                } else if (Key.ScanCode == 0x04) { // Left arrow
                    if (cursor_pos > 0) cursor_pos--;
                } else if (Key.UnicodeChar == 0x08) { // Backspace
                    if (cursor_pos > 0 && len > 0) {
                        for (int i = cursor_pos - 1; i < len; i++) {
                            buf[i] = buf[i + 1];
                        }
                        cursor_pos--;
                        g_vfs_files[selected_file].size = bp_str_len(buf);
                        dirty = true;
                        bp_str_copy(status_pill, "Modified (Unsaved)", sizeof(status_pill));
                    }
                } else if (Key.UnicodeChar == 0x0D) { // Enter -> newline
                    if (len < VFS_DATA_MAX - 2) {
                        for (int i = len + 1; i > cursor_pos; i--) {
                            buf[i] = buf[i - 1];
                        }
                        buf[cursor_pos] = '\n';
                        cursor_pos++;
                        g_vfs_files[selected_file].size = bp_str_len(buf);
                        dirty = true;
                        bp_str_copy(status_pill, "Modified (Unsaved)", sizeof(status_pill));
                    }
                } else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                    if (len < VFS_DATA_MAX - 2) {
                        for (int i = len + 1; i > cursor_pos; i--) {
                            buf[i] = buf[i - 1];
                        }
                        buf[cursor_pos] = (char)Key.UnicodeChar;
                        cursor_pos++;
                        g_vfs_files[selected_file].size = bp_str_len(buf);
                        dirty = true;
                        bp_str_copy(status_pill, "Modified (Unsaved)", sizeof(status_pill));
                    }
                }
            }
        }

        // Mouse input
        bool mouse_clicked = false;
        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;
                cursor_x += dx;
                cursor_y += dy;
                if (cursor_x < 0) cursor_x = 0;
                if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                if (cursor_y < 0) cursor_y = 0;
                if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;

                blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                if (State.LeftButton && !prev_left) {
                    mouse_clicked = true;
                }
                prev_left = State.LeftButton;
            }
        }

        int toolbar_y = win_y + 36;
        int content_y = win_y + 76;
        int content_h = win_h - 76;
        int sidebar_w = 190;
        int main_x = win_x + sidebar_w;
        int main_w = win_w - sidebar_w;

        if (mouse_clicked) {
            if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                play_system_sound((char*)"click");
                done = true;
            }

            // Top Toolbar Mode Switch buttons:
            // [ Split View ] (x: main_x + 10 .. +88)
            // [ Finder Grid ] (x: main_x + 92 .. +174)
            // [ Editor Mode ] (x: main_x + 178 .. +260)
            if (cursor_y >= toolbar_y + 6 && cursor_y <= toolbar_y + 32) {
                if (cursor_x >= main_x + 10 && cursor_x <= main_x + 88) {
                    view_mode = 0;
                    play_system_sound((char*)"click");
                } else if (cursor_x >= main_x + 92 && cursor_x <= main_x + 174) {
                    view_mode = 1;
                    play_system_sound((char*)"click");
                } else if (cursor_x >= main_x + 178 && cursor_x <= main_x + 260) {
                    view_mode = 2;
                    play_system_sound((char*)"click");
                }
                // Action buttons:
                // [ + New File ] (main_x + 270 .. +346)
                // [ Save to Disk ] (main_x + 350 .. +436)
                // [ Duplicate ] (main_x + 440 .. +512)
                // [ Delete ] (main_x + 516 .. +576)
                else if (cursor_x >= main_x + 270 && cursor_x <= main_x + 346) {
                    char newName[32];
                    bp_str_copy(newName, "NOTE_", sizeof(newName));
                    char numBuf[8];
                    bp_int_to_str(new_file_seq++, numBuf, sizeof(numBuf));
                    bp_str_cat(newName, numBuf, sizeof(newName));
                    bp_str_cat(newName, ".txt", sizeof(newName));

                    const char* initBody = "# New BlankPad Document\nType here and press Save to Disk (or Ctrl+S).\n";
                    vfs_write_file(newName, initBody);
                    int nidx = bp_find_file_index(newName);
                    if (nidx >= 0) {
                        selected_file = nidx;
                        bp_str_copy(filename_edit_buf, g_vfs_files[nidx].name, VFS_NAME_MAX);
                        cursor_pos = bp_str_len(g_vfs_files[nidx].content);
                        scroll_line = 0;
                        dirty = false;
                    }
                    bp_str_copy(status_pill, "Created on FAT32", sizeof(status_pill));
                    play_system_sound((char*)"click");
                } else if (cursor_x >= main_x + 350 && cursor_x <= main_x + 436) {
                    if (selected_file >= 0 && selected_file < g_vfs_count) {
                        if (editing_filename && filename_edit_buf[0]) {
                            bp_str_copy(g_vfs_files[selected_file].name, filename_edit_buf, VFS_NAME_MAX);
                            editing_filename = false;
                        }
                        bool disk_saved = vfs_write_file(g_vfs_files[selected_file].name, g_vfs_files[selected_file].content);
                        dirty = false;
                        bp_str_copy(status_pill, disk_saved ? "Saved to FAT32 & VFS" : "Saved in VFS Memory", sizeof(status_pill));
                        play_system_sound((char*)"notification");
                    }
                } else if (cursor_x >= main_x + 440 && cursor_x <= main_x + 512) {
                    if (selected_file >= 0 && selected_file < g_vfs_count) {
                        char dupName[VFS_NAME_MAX];
                        bp_str_copy(dupName, "COPY_", VFS_NAME_MAX);
                        bp_str_cat(dupName, g_vfs_files[selected_file].name, VFS_NAME_MAX);
                        vfs_write_file(dupName, g_vfs_files[selected_file].content);
                        int didx = bp_find_file_index(dupName);
                        if (didx >= 0) {
                            selected_file = didx;
                            bp_str_copy(filename_edit_buf, g_vfs_files[didx].name, VFS_NAME_MAX);
                            cursor_pos = bp_str_len(g_vfs_files[didx].content);
                        }
                        bp_str_copy(status_pill, "Duplicated to FAT32", sizeof(status_pill));
                        play_system_sound((char*)"click");
                    }
                } else if (cursor_x >= main_x + 516 && cursor_x <= main_x + 576) {
                    if (selected_file >= 0 && selected_file < g_vfs_count && g_vfs_count > 1) {
                        char delTarget[VFS_NAME_MAX];
                        bp_str_copy(delTarget, g_vfs_files[selected_file].name, VFS_NAME_MAX);
                        vfs_delete_file(delTarget);
                        if (selected_file >= g_vfs_count) selected_file = g_vfs_count - 1;
                        if (selected_file >= 0) {
                            bp_str_copy(filename_edit_buf, g_vfs_files[selected_file].name, VFS_NAME_MAX);
                            cursor_pos = bp_str_len(g_vfs_files[selected_file].content);
                        }
                        dirty = false;
                        bp_str_copy(status_pill, "Deleted from FAT32", sizeof(status_pill));
                        play_system_sound((char*)"click");
                    }
                }
            }

            // Left Sidebar clicks (Favorites + VFS file list + Rescan button)
            if (cursor_x >= win_x + 8 && cursor_x <= win_x + sidebar_w - 8) {
                // Favorites: 4 items starting at content_y + 24
                for (int f = 0; f < 4; f++) {
                    int fy = content_y + 22 + f * 24;
                    if (cursor_y >= fy && cursor_y <= fy + 22) {
                        category_filter = f;
                        play_system_sound((char*)"click");
                    }
                }
                // File list in sidebar starting at content_y + 144
                int list_start_y = content_y + 144;
                int max_sidebar_files = 10;
                for (int i = 0; i < filtered_count && i < max_sidebar_files; i++) {
                    int ry = list_start_y + i * 24;
                    if (cursor_y >= ry && cursor_y <= ry + 22) {
                        int real_idx = filtered_indices[i];
                        selected_file = real_idx;
                        // Read fresh bytes from disk if it's a real disk text file
                        char refreshBuf[VFS_DATA_MAX];
                        vfs_read_file(g_vfs_files[real_idx].name, refreshBuf, VFS_DATA_MAX);
                        bp_str_copy(filename_edit_buf, g_vfs_files[real_idx].name, VFS_NAME_MAX);
                        cursor_pos = bp_str_len(g_vfs_files[real_idx].content);
                        scroll_line = 0;
                        editing_filename = false;
                        play_system_sound((char*)"click");
                    }
                }
                // Rescan FAT32 button at bottom of sidebar
                int rescan_y = win_y + win_h - 34;
                if (cursor_y >= rescan_y && cursor_y <= rescan_y + 24) {
                    bp_sync_from_fat32(SystemTable);
                    bp_str_copy(status_pill, "Synced FAT32 Volume", sizeof(status_pill));
                    play_system_sound((char*)"notification");
                }
            }

            // Main Content Area clicks
            if (cursor_x >= main_x && cursor_x <= win_x + win_w && cursor_y >= content_y && cursor_y <= win_y + win_h) {
                if (view_mode == 1) {
                    // Finder Grid Mode: 3 columns of cards
                    int cols = 3;
                    int card_w = 176;
                    int card_h = 92;
                    int gap_x = 14;
                    int gap_y = 14;
                    for (int i = 0; i < filtered_count && i < 12; i++) {
                        int r = i / cols;
                        int c = i % cols;
                        int cx = main_x + 18 + c * (card_w + gap_x);
                        int cy = content_y + 42 + r * (card_h + gap_y);
                        if (cursor_x >= cx && cursor_x <= cx + card_w && cursor_y >= cy && cursor_y <= cy + card_h) {
                            int real_idx = filtered_indices[i];
                            if (selected_file == real_idx) {
                                // Second click opens file in Editor!
                                view_mode = 0;
                            } else {
                                selected_file = real_idx;
                                char refreshBuf[VFS_DATA_MAX];
                                vfs_read_file(g_vfs_files[real_idx].name, refreshBuf, VFS_DATA_MAX);
                                bp_str_copy(filename_edit_buf, g_vfs_files[real_idx].name, VFS_NAME_MAX);
                                cursor_pos = bp_str_len(g_vfs_files[real_idx].content);
                                scroll_line = 0;
                            }
                            play_system_sound((char*)"click");
                        }
                    }
                    // "Edit Selected File" button in Finder Grid header
                    if (cursor_x >= main_x + main_w - 130 && cursor_x <= main_x + main_w - 16 &&
                        cursor_y >= content_y + 8 && cursor_y <= content_y + 32) {
                        view_mode = 0;
                        play_system_sound((char*)"click");
                    }
                } else {
                    // Split View (0) or Editor Mode (2)
                    int editor_x = (view_mode == 0) ? (main_x + 165) : main_x;
                    if (view_mode == 0 && cursor_x < editor_x) {
                        // Clicked in Split View middle Finder column
                        for (int i = 0; i < filtered_count && i < 9; i++) {
                            int card_y = content_y + 34 + i * 42;
                            if (cursor_y >= card_y && cursor_y <= card_y + 38) {
                                int real_idx = filtered_indices[i];
                                selected_file = real_idx;
                                char refreshBuf[VFS_DATA_MAX];
                                vfs_read_file(g_vfs_files[real_idx].name, refreshBuf, VFS_DATA_MAX);
                                bp_str_copy(filename_edit_buf, g_vfs_files[real_idx].name, VFS_NAME_MAX);
                                cursor_pos = bp_str_len(g_vfs_files[real_idx].content);
                                scroll_line = 0;
                                editing_filename = false;
                                play_system_sound((char*)"click");
                            }
                        }
                    } else {
                        // Clicked inside Editor pane
                        if (cursor_y >= content_y + 6 && cursor_y <= content_y + 30 &&
                            cursor_x >= editor_x + 12 && cursor_x <= editor_x + 220) {
                            editing_filename = true;
                            if (selected_file >= 0) {
                                bp_str_copy(filename_edit_buf, g_vfs_files[selected_file].name, VFS_NAME_MAX);
                            }
                        } else {
                            editing_filename = false;
                            if (selected_file >= 0 && selected_file < g_vfs_count) {
                                int text_top = content_y + 40;
                                int text_left = editor_x + 44;
                                int clicked_line = scroll_line + (cursor_y - text_top) / 18;
                                if (clicked_line < 0) clicked_line = 0;
                                int clicked_col = (cursor_x - text_left) / 7;
                                if (clicked_col < 0) clicked_col = 0;
                                cursor_pos = bp_offset_for_line_col(g_vfs_files[selected_file].content, clicked_line, clicked_col);
                            }
                        }
                    }
                }
            }
        }

        // Keep active cursor line in visible scroll window
        if (selected_file >= 0 && selected_file < g_vfs_count) {
            int cur_line = 0, cur_col = 0;
            bp_compute_line_col(g_vfs_files[selected_file].content, cursor_pos, &cur_line, &cur_col);
            int visible_lines = 19;
            if (cur_line < scroll_line) scroll_line = cur_line;
            if (cur_line >= scroll_line + visible_lines) scroll_line = cur_line - visible_lines + 1;
        }

        // ======================== RENDER FRAME ========================
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();

        blankUI_draw_window(win_w, win_h, (char*)"Files & BlankPad Studio");

        // Top Toolbar background (macOS Sequoia frosted header bar)
        dui_rect(win_x + 1, toolbar_y, win_w - 2, 38, 0xEBEBF0, 255);
        dui_line(win_x, toolbar_y + 38, win_x + win_w, toolbar_y + 38, 0xD1D1D6, 1);

        // Status Pill on left side of toolbar above sidebar
        uint32_t pill_bg = dirty ? 0xFFF4E5 : 0xE3F9E5;
        uint32_t pill_fg = dirty ? 0xB25000 : 0x1B7F2A;
        dui_rect_rounded(win_x + 10, toolbar_y + 7, sidebar_w - 20, 24, 12, pill_bg, 255);
        dui_circle(win_x + 22, toolbar_y + 19, 4, pill_fg, 255);
        dui_text(win_x + 32, toolbar_y + 13, status_pill, pill_fg, 1);

        // Mode Switch Segmented Control
        const char* mode_labels[3] = { "Split View", "Finder Grid", "Editor Mode" };
        int mode_xs[3] = { main_x + 10, main_x + 92, main_x + 178 };
        int mode_ws[3] = { 78, 82, 82 };
        for (int m = 0; m < 3; m++) {
            bool active = (view_mode == m);
            dui_rect_rounded(mode_xs[m], toolbar_y + 6, mode_ws[m], 26, 6, active ? 0x0A84FF : 0xD8D8DF, 255);
            dui_text(mode_xs[m] + 8, toolbar_y + 13, mode_labels[m], active ? 0xFFFFFF : 0x2C2C2E, 1);
        }

        // Action Buttons: [ + New File ] [ Save to Disk ] [ Duplicate ] [ Delete ]
        dui_rect_rounded(main_x + 270, toolbar_y + 6, 76, 26, 6, 0xFFFFFF, 255);
        dui_rect_rounded_outline(main_x + 270, toolbar_y + 6, 76, 26, 6, 0xC7C7CC, 1);
        dui_text(main_x + 278, toolbar_y + 13, "+ New File", 0x1C1C1E, 1);

        dui_rect_rounded(main_x + 350, toolbar_y + 6, 86, 26, 6, 0x30D158, 255);
        dui_text(main_x + 358, toolbar_y + 13, "Save to Disk", 0xFFFFFF, 1);

        dui_rect_rounded(main_x + 440, toolbar_y + 6, 72, 26, 6, 0xFFFFFF, 255);
        dui_rect_rounded_outline(main_x + 440, toolbar_y + 6, 72, 26, 6, 0xC7C7CC, 1);
        dui_text(main_x + 449, toolbar_y + 13, "Duplicate", 0x1C1C1E, 1);

        dui_rect_rounded(main_x + 516, toolbar_y + 6, 60, 26, 6, 0xFF3B30, 255);
        dui_text(main_x + 528, toolbar_y + 13, "Delete", 0xFFFFFF, 1);

        // Left Sidebar (width 190px)
        dui_rect(win_x + 1, content_y, sidebar_w - 1, content_h - 1, 0xF2F2F7, 245);
        dui_line(win_x + sidebar_w, content_y, win_x + sidebar_w, win_y + win_h, 0xD1D1D6, 1);

        dui_text(win_x + 14, content_y + 6, "FAVORITES", 0x8E8E93, 1);
        const char* fav_names[4] = {
            "All Files",
            "Documents",
            "System Config",
            "Disk Volume (FAT32)"
        };
        uint32_t fav_dots[4] = { 0x0A84FF, 0x5E5CE6, 0xFF9F0A, 0x30D158 };
        for (int f = 0; f < 4; f++) {
            int fy = content_y + 22 + f * 24;
            if (category_filter == f) {
                dui_rect_rounded(win_x + 8, fy, sidebar_w - 16, 22, 6, 0x0A84FF, 255);
            }
            dui_circle(win_x + 18, fy + 11, 4, (category_filter == f) ? 0xFFFFFF : fav_dots[f], 255);
            dui_text(win_x + 28, fy + 5, fav_names[f], (category_filter == f) ? 0xFFFFFF : 0x1C1C1E, 1);
        }

        // VFS / FAT32 File List in Sidebar
        dui_line(win_x + 12, content_y + 122, win_x + sidebar_w - 12, content_y + 122, 0xD1D1D6, 1);
        dui_text(win_x + 14, content_y + 127, "WORKSPACE & FAT32", 0x8E8E93, 1);

        int list_start_y = content_y + 144;
        for (int i = 0; i < filtered_count && i < 10; i++) {
            int real_idx = filtered_indices[i];
            int ry = list_start_y + i * 24;
            bool is_sel = (real_idx == selected_file);
            if (is_sel) {
                dui_rect_rounded(win_x + 8, ry, sidebar_w - 16, 22, 5, 0xD1E4FF, 255);
            }

            uint32_t bcol = bp_file_badge_color(g_vfs_files[real_idx].name);
            dui_rect_rounded(win_x + 12, ry + 3, 24, 15, 3, bcol, 255);
            dui_text(win_x + 14, ry + 5, bp_file_badge_label(g_vfs_files[real_idx].name), 0xFFFFFF, 1);

            // Truncate display name to fit sidebar
            char dispName[16];
            bp_str_copy(dispName, g_vfs_files[real_idx].name, 14);
            dui_text(win_x + 40, ry + 5, dispName, is_sel ? 0x0040DD : 0x1C1C1E, 1);

            char szStr[12];
            bp_int_to_str(g_vfs_files[real_idx].size, szStr, sizeof(szStr));
            bp_str_cat(szStr, "B", sizeof(szStr));
            int sw = dui_text_width(szStr, 1);
            dui_text(win_x + sidebar_w - 12 - sw, ry + 5, szStr, 0x8E8E93, 1);
        }

        // Rescan FAT32 Partition button at bottom of sidebar
        int rescan_y = win_y + win_h - 34;
        dui_rect_rounded(win_x + 12, rescan_y, sidebar_w - 24, 24, 6, 0xE5E5EA, 255);
        dui_text(win_x + 28, rescan_y + 6, "Rescan FAT32 Disk", 0x0A84FF, 1);

        // Main Content Area
        if (view_mode == 1) {
            // ==================== FINDER GRID MODE ====================
            dui_rect(main_x, content_y, main_w - 1, content_h - 1, 0xFFFFFF, 255);
            dui_text(main_x + 18, content_y + 14, "FAT32 & VFS Volume Browser (Click to select, click again to edit)", 0x3A3A3C, 1);

            dui_rect_rounded(main_x + main_w - 130, content_y + 8, 114, 24, 6, 0x0A84FF, 255);
            dui_text(main_x + main_w - 118, content_y + 14, "Edit File ->", 0xFFFFFF, 1);

            int cols = 3;
            int card_w = 176;
            int card_h = 92;
            int gap_x = 14;
            int gap_y = 14;
            for (int i = 0; i < filtered_count && i < 12; i++) {
                int real_idx = filtered_indices[i];
                int r = i / cols;
                int c = i % cols;
                int cx = main_x + 18 + c * (card_w + gap_x);
                int cy = content_y + 42 + r * (card_h + gap_y);
                bool is_sel = (real_idx == selected_file);

                dui_rect_rounded(cx, cy, card_w, card_h, 10, is_sel ? 0xE8F2FF : 0xF7F7FA, 255);
                dui_rect_rounded_outline(cx, cy, card_w, card_h, 10, is_sel ? 0x0A84FF : 0xE5E5EA, is_sel ? 2 : 1);

                // Document icon graphic
                uint32_t badge_col = bp_file_badge_color(g_vfs_files[real_idx].name);
                dui_rect_rounded(cx + 12, cy + 12, 34, 42, 5, badge_col, 255);
                dui_text(cx + 16, cy + 26, bp_file_badge_label(g_vfs_files[real_idx].name), 0xFFFFFF, 1);

                char cardTitle[18];
                bp_str_copy(cardTitle, g_vfs_files[real_idx].name, 17);
                dui_text(cx + 54, cy + 14, cardTitle, 0x1C1C1E, 1);

                char metaLine[32];
                bp_int_to_str(g_vfs_files[real_idx].size, metaLine, sizeof(metaLine));
                bp_str_cat(metaLine, " bytes", sizeof(metaLine));
                dui_text(cx + 54, cy + 32, metaLine, 0x636366, 1);

                const char* storageTag = g_vfs_files[real_idx].on_disk ? "FAT32 Disk" : "VFS RAM";
                dui_rect_rounded(cx + 54, cy + 50, 74, 16, 4, g_vfs_files[real_idx].on_disk ? 0xD4F7DC : 0xE5E5EA, 255);
                dui_text(cx + 60, cy + 52, storageTag, g_vfs_files[real_idx].on_disk ? 0x157328 : 0x636366, 1);

                // Preview line of content
                char prevLine[24];
                int p = 0;
                const char* rawc = g_vfs_files[real_idx].content;
                while (rawc[p] && rawc[p] != '\n' && p < 22) {
                    prevLine[p] = rawc[p];
                    p++;
                }
                prevLine[p] = '\0';
                dui_text(cx + 12, cy + 72, prevLine, 0x8E8E93, 1);
            }
        } else {
            // ==================== SPLIT VIEW / EDITOR MODE ====================
            int editor_x = main_x;
            int editor_w = main_w;

            if (view_mode == 0) {
                // Middle Finder Column in Split View (width 165px)
                int mid_w = 165;
                dui_rect(main_x, content_y, mid_w, content_h - 1, 0xFAFAFC, 255);
                dui_line(main_x + mid_w, content_y, main_x + mid_w, win_y + win_h, 0xD1D1D6, 1);
                dui_text(main_x + 10, content_y + 10, "FILES (", 0x636366, 1);
                char cntStr[16];
                bp_int_to_str(filtered_count, cntStr, sizeof(cntStr));
                bp_str_cat(cntStr, ")", sizeof(cntStr));
                dui_text(main_x + 56, content_y + 10, cntStr, 0x636366, 1);

                for (int i = 0; i < filtered_count && i < 9; i++) {
                    int real_idx = filtered_indices[i];
                    int cy = content_y + 34 + i * 42;
                    bool is_sel = (real_idx == selected_file);
                    dui_rect_rounded(main_x + 6, cy, mid_w - 12, 38, 6, is_sel ? 0x0A84FF : 0xFFFFFF, 255);
                    if (!is_sel) {
                        dui_rect_rounded_outline(main_x + 6, cy, mid_w - 12, 38, 6, 0xE5E5EA, 1);
                    }
                    char fTitle[18];
                    bp_str_copy(fTitle, g_vfs_files[real_idx].name, 17);
                    dui_text(main_x + 12, cy + 6, fTitle, is_sel ? 0xFFFFFF : 0x1C1C1E, 1);

                    char infoLine[28];
                    bp_int_to_str(g_vfs_files[real_idx].size, infoLine, sizeof(infoLine));
                    bp_str_cat(infoLine, " B  ", sizeof(infoLine));
                    bp_str_cat(infoLine, g_vfs_files[real_idx].on_disk ? "[FAT32]" : "[VFS]", sizeof(infoLine));
                    dui_text(main_x + 12, cy + 21, infoLine, is_sel ? 0xD1E4FF : 0x8E8E93, 1);
                }

                editor_x = main_x + mid_w;
                editor_w = main_w - mid_w;
            }

            // Sleek Dark Studio Editor Surface
            dui_rect(editor_x, content_y, editor_w - 1, content_h - 1, 0x1E1E24, 255);

            // Editor Sub-header bar (filename + cursor position + encoding)
            dui_rect(editor_x, content_y, editor_w - 1, 32, 0x282830, 255);
            dui_line(editor_x, content_y + 32, editor_x + editor_w, content_y + 32, 0x3A3A44, 1);

            if (selected_file >= 0 && selected_file < g_vfs_count) {
                if (editing_filename) {
                    dui_rect_rounded(editor_x + 10, content_y + 5, 200, 22, 4, 0x141418, 255);
                    dui_rect_rounded_outline(editor_x + 10, content_y + 5, 200, 22, 4, 0x0A84FF, 1);
                    dui_text(editor_x + 16, content_y + 9, filename_edit_buf, 0xFFFFFF, 1);
                } else {
                    dui_text(editor_x + 12, content_y + 9, g_vfs_files[selected_file].name, 0xFFFFFF, 1);
                    int nw = dui_text_width(g_vfs_files[selected_file].name, 1);
                    dui_text(editor_x + 20 + nw, content_y + 9, "(click name to rename)", 0x6E6E77, 1);
                }

                // Line / Col indicator on right of editor header
                int cur_line = 0, cur_col = 0;
                bp_compute_line_col(g_vfs_files[selected_file].content, cursor_pos, &cur_line, &cur_col);
                char posInfo[48];
                bp_str_copy(posInfo, "Ln ", sizeof(posInfo));
                char tmpNum[12];
                bp_int_to_str(cur_line + 1, tmpNum, sizeof(tmpNum));
                bp_str_cat(posInfo, tmpNum, sizeof(posInfo));
                bp_str_cat(posInfo, ", Col ", sizeof(posInfo));
                bp_int_to_str(cur_col + 1, tmpNum, sizeof(tmpNum));
                bp_str_cat(posInfo, tmpNum, sizeof(posInfo));
                bp_str_cat(posInfo, " | UTF-8", sizeof(posInfo));
                int pw = dui_text_width(posInfo, 1);
                dui_text(editor_x + editor_w - pw - 14, content_y + 9, posInfo, 0x9898A6, 1);

                // Gutter background for line numbers
                int gutter_w = 36;
                int body_y = content_y + 33;
                int body_h = content_h - 58;
                dui_rect(editor_x, body_y, gutter_w, body_h, 0x18181D, 255);
                dui_line(editor_x + gutter_w, body_y, editor_x + gutter_w, body_y + body_h, 0x2E2E38, 1);

                // Render lines of text with line numbers, active line highlight, and blinking caret
                const char* text = g_vfs_files[selected_file].content;
                int idx = 0;
                int line_no = 0;
                int max_vis_lines = 19;
                int max_chars_per_line = (editor_w - gutter_w - 20) / 7;
                if (max_chars_per_line > 96) max_chars_per_line = 96;

                while (line_no < scroll_line + max_vis_lines) {
                    char lineBuf[104];
                    int c = 0;
                    while (text[idx] && text[idx] != '\n') {
                        if (c < max_chars_per_line) {
                            lineBuf[c++] = text[idx];
                        }
                        idx++;
                    }
                    lineBuf[c] = '\0';

                    if (line_no >= scroll_line) {
                        int vis_row = line_no - scroll_line;
                        int ry = body_y + 6 + vis_row * 18;

                        if (line_no == cur_line) {
                            dui_rect(editor_x + gutter_w + 1, ry - 2, editor_w - gutter_w - 2, 18, 0x2C2C38, 255);
                        }

                        char lnStr[12];
                        bp_int_to_str(line_no + 1, lnStr, sizeof(lnStr));
                        dui_text(editor_x + 8, ry, lnStr, (line_no == cur_line) ? 0x0A84FF : 0x63636E, 1);

                        // Simple syntax tint for comments/headers
                        uint32_t line_color = 0xE5E5EA;
                        if (lineBuf[0] == '#' || (lineBuf[0] == '/' && lineBuf[1] == '/')) {
                            line_color = 0x30D158;
                        } else if (lineBuf[0] == '[') {
                            line_color = 0xFF9F0A;
                        }
                        dui_text(editor_x + gutter_w + 8, ry, lineBuf, line_color, 1);

                        // Blinking caret on active line
                        if (line_no == cur_line && !editing_filename && ((blink_tick / 15) % 2 == 0)) {
                            char prefixSub[104];
                            int pc = cur_col < max_chars_per_line ? cur_col : max_chars_per_line;
                            for (int k = 0; k < pc; k++) prefixSub[k] = lineBuf[k];
                            prefixSub[pc] = '\0';
                            int caret_dx = dui_text_width(prefixSub, 1);
                            int caret_x = editor_x + gutter_w + 8 + caret_dx;
                            dui_rect(caret_x, ry - 1, 2, 15, 0x0A84FF, 255);
                        }
                    }

                    if (text[idx] == '\n') {
                        idx++;
                        line_no++;
                    } else {
                        break;
                    }
                }

                // Bottom Status Bar inside Editor
                int foot_y = win_y + win_h - 24;
                dui_rect(editor_x, foot_y, editor_w - 1, 23, 0x141418, 255);
                char footText[96];
                bp_str_copy(footText, g_vfs_files[selected_file].on_disk ? "FAT32 Hardware Volume Synced | " : "VFS Volume | ", sizeof(footText));
                char szNum[16];
                bp_int_to_str(g_vfs_files[selected_file].size, szNum, sizeof(szNum));
                bp_str_cat(footText, szNum, sizeof(footText));
                bp_str_cat(footText, " bytes | Ctrl+S to Save", sizeof(footText));
                dui_text(editor_x + 12, foot_y + 5, footText, 0x8E8E93, 1);
            }
        }

        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();
        SystemTable->BootServices->Stall(16000);
    }
}

extern "C" void launch_files_app(EFI_SYSTEM_TABLE *SystemTable) {
    if (SystemTable) {
        gSystemTable = SystemTable;
    }
    vfs_init(gSystemTable);
    launch_blankpad();
}
