#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/theme.h"
#include "../ui/blankDUI.h"
#include "../ui/window_manager.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
}

#define VFS_MAX_FILES 48
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

static bool bp_str_starts_with_nocase(const char* str, const char* prefix) {
    if (!str || !prefix) return false;
    int i = 0;
    while (prefix[i]) {
        if (!str[i]) return false;
        char ca = bp_to_upper(str[i]);
        char cb = bp_to_upper(prefix[i]);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
        i++;
    }
    return true;
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

static void bp_normalize_path(const char* raw, char* out, int max_len) {
    if (!raw || !out || max_len <= 0) return;
    int start = 0;
    while (raw[start] == '\\' || raw[start] == '/') start++;
    int j = 0;
    for (int i = start; raw[i] && j < max_len - 1; i++) {
        char c = raw[i];
        if (c == '/') c = '\\';
        out[j++] = c;
    }
    out[j] = '\0';
}

static int bp_find_file_index(const char* name) {
    if (!name) return -1;
    char norm[VFS_NAME_MAX];
    bp_normalize_path(name, norm, VFS_NAME_MAX);
    for (int i = 0; i < g_vfs_count; i++) {
        if (g_vfs_files[i].used &&
            (bp_str_eq_nocase(g_vfs_files[i].name, name) || bp_str_eq_nocase(g_vfs_files[i].name, norm))) {
            return i;
        }
    }
    return -1;
}

static int bp_classify_file(const char* name, bool on_disk) {
    // 1 = Docs, 2 = Config, 3 = Disk Volume / Downloads
    if (bp_str_starts_with_nocase(name, "DOWNLOADS\\")) return 3;
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
        bp_str_ends_with_nocase(name, ".html") ||
        bp_str_ends_with_nocase(name, ".htm") ||
        bp_str_ends_with_nocase(name, ".c") ||
        bp_str_ends_with_nocase(name, ".cpp") ||
        bp_str_ends_with_nocase(name, ".h")) {
        return 1;
    }
    return on_disk ? 3 : 1;
}

static uint32_t bp_file_badge_color(const char* name) {
    if (bp_str_ends_with_nocase(name, ".c") || bp_str_ends_with_nocase(name, ".cpp") || bp_str_ends_with_nocase(name, ".h")) {
        return THEME_ACCENT_CYAN;
    }
    if (bp_str_ends_with_nocase(name, ".ini") || bp_str_ends_with_nocase(name, ".json") || bp_str_ends_with_nocase(name, ".reg")) {
        return THEME_ACCENT_AMBER;
    }
    if (bp_str_ends_with_nocase(name, ".md") || bp_str_ends_with_nocase(name, ".html") || bp_str_ends_with_nocase(name, ".htm")) {
        return THEME_ACCENT_IRIS;
    }
    if (bp_str_ends_with_nocase(name, ".efi") || bp_str_ends_with_nocase(name, ".elf")) {
        return THEME_ACCENT_GREEN;
    }
    return THEME_ACCENT_TEAL;
}

static const char* bp_file_badge_label(const char* name) {
    if (bp_str_ends_with_nocase(name, ".c")) return "C";
    if (bp_str_ends_with_nocase(name, ".cpp")) return "C++";
    if (bp_str_ends_with_nocase(name, ".md")) return "MD";
    if (bp_str_ends_with_nocase(name, ".ini")) return "INI";
    if (bp_str_ends_with_nocase(name, ".json")) return "JSN";
    if (bp_str_ends_with_nocase(name, ".html") || bp_str_ends_with_nocase(name, ".htm")) return "HTM";
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

    char norm[VFS_NAME_MAX];
    bp_normalize_path(name, norm, VFS_NAME_MAX);
    CHAR16 wideName[VFS_NAME_MAX];
    bp_ascii_to_wide(norm, wideName, VFS_NAME_MAX);

    bool wrote = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        if (bp_str_starts_with_nocase(norm, "DOWNLOADS\\")) {
            EFI_FILE_HANDLE dlDir = NULL;
            if (!EFI_ERROR(root->Open(root, &dlDir, (CHAR16*)L"DOWNLOADS",
                                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                      EFI_FILE_DIRECTORY)) && dlDir) {
                dlDir->Close(dlDir);
            }
        }

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

    char norm[VFS_NAME_MAX];
    bp_normalize_path(name, norm, VFS_NAME_MAX);
    CHAR16 wideName[VFS_NAME_MAX];
    bp_ascii_to_wide(norm, wideName, VFS_NAME_MAX);

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

    char norm[VFS_NAME_MAX];
    bp_normalize_path(name, norm, VFS_NAME_MAX);
    CHAR16 wideName[VFS_NAME_MAX];
    bp_ascii_to_wide(norm, wideName, VFS_NAME_MAX);

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
                    const char* hex = "0123456789ABCDEF";
                    out_buf[0] = '\0';
                    bp_str_cat(out_buf, "# Binary File on UEFI FAT32: ", max_len);
                    bp_str_cat(out_buf, norm, max_len);
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
    char norm[VFS_NAME_MAX];
    bp_normalize_path(name, norm, VFS_NAME_MAX);
    int idx = bp_find_file_index(norm);
    if (idx < 0) {
        if (g_vfs_count >= VFS_MAX_FILES) return -1;
        idx = g_vfs_count++;
    }
    g_vfs_files[idx].used = true;
    bp_str_copy(g_vfs_files[idx].name, norm, VFS_NAME_MAX);
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

        bp_scan_directory_handle(st, root, "");

        EFI_FILE_HANDLE dlDir = NULL;
        if (!EFI_ERROR(root->Open(root, &dlDir, (CHAR16*)L"DOWNLOADS", EFI_FILE_MODE_READ, 0)) && dlDir) {
            bp_scan_directory_handle(st, dlDir, "DOWNLOADS");
            dlDir->Close(dlDir);
        }

        EFI_FILE_HANDLE bootDir = NULL;
        if (!EFI_ERROR(root->Open(root, &bootDir, (CHAR16*)L"EFI\\BOOT", EFI_FILE_MODE_READ, 0)) && bootDir) {
            bp_scan_directory_handle(st, bootDir, "EFI\\BOOT");
            bootDir->Close(bootDir);
        }

        EFI_FILE_HANDLE assetsDir = NULL;
        if (!EFI_ERROR(root->Open(root, &assetsDir, (CHAR16*)L"assets", EFI_FILE_MODE_READ, 0)) && assetsDir) {
            bp_scan_directory_handle(st, assetsDir, "assets");
            assetsDir->Close(assetsDir);
        }

        root->Close(root);
    }

    st->BootServices->FreePool(handles);

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
            "# BlankOS Horizon - Files & Notes\n"
            "Unified FAT32 explorer and non-blocking text/code studio.\n\n"
            "## Shortcuts\n"
            "- Arrow Keys  : Move cursor\n"
            "- Ctrl + S    : Save active file to FAT32 & VFS\n"
            "- Ctrl + N    : Create a new note\n"
            "- Mouse Wheel : Scroll editor or file sidebar\n\n"
            "## Storage\n"
            "- Syncs root, \\DOWNLOADS\\, \\EFI\\BOOT\\, and RAM VFS.\n";

        const char* notes_default =
            "BlankOS Horizon Notes:\n"
            "1. Non-blocking multi-window desktop compositor\n"
            "2. Real UEFI SNP Ethernet + HTTP/1.1 downloads\n"
            "3. Real FAT32 persistence via EFI_SIMPLE_FILE_SYSTEM_PROTOCOL\n";

        const char* sysini_default =
            "[Boot]\n"
            "kernel=EFI\\BOOT\\BOOTX64.EFI\n"
            "theme=horizon_glass\n"
            "vfs_mount=fat32_rw\n";

        const char* helloc_default =
            "// BlankOS Horizon Freestanding Sample\n"
            "#include <stdint.h>\n\n"
            "extern \"C\" void dui_text(int x, int y, const char* s, uint32_t c, int sc);\n\n"
            "void blankos_app_entry(void) {\n"
            "    dui_text(40, 40, \"Hello Horizon!\", 0x38BDF8, 1);\n"
            "}\n";

        bp_upsert_memory_entry("README.md", readme_default, 0, false, false);
        bp_upsert_memory_entry("NOTES.txt", notes_default, 0, false, false);
        bp_upsert_memory_entry("SYSTEM.ini", sysini_default, 0, false, false);
        bp_upsert_memory_entry("HELLO.c", helloc_default, 0, false, false);
    }

    if (activeSt) {
        bp_sync_from_fat32(activeSt);
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

// ============================================================================
// Horizon Spatial Glass Editor State & Helpers
// ============================================================================

static int  g_bp_selected = 0;
static int  g_bp_cat = 0;        // 0=All, 1=Docs, 2=Cfg, 3=Disk/DL
static bool g_bp_show_sidebar = true;
static int  g_bp_sidebar_scroll = 0;
static int  g_bp_cursor = 0;
static int  g_bp_scroll_line = 0;
static bool g_bp_dirty = false;
static bool g_bp_editing_name = false;
static char g_bp_name_buf[VFS_NAME_MAX] = {0};
static char g_bp_status[64] = "VFS Ready";
static int  g_bp_note_seq = 1;

static bool bp_pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void bp_compute_line_col(const char* buf, int cursor, int* out_line, int* out_col) {
    int line = 0, col = 0;
    for (int i = 0; i < cursor && buf[i]; i++) {
        if (buf[i] == '\n') { line++; col = 0; }
        else col++;
    }
    *out_line = line;
    *out_col = col;
}

static int bp_offset_for_line_col(const char* buf, int target_line, int target_col) {
    int line = 0, i = 0;
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

static int bp_count_lines(const char* buf) {
    int lines = 1;
    for (int i = 0; buf && buf[i]; i++) {
        if (buf[i] == '\n') lines++;
    }
    return lines;
}

static void bp_select_index(int idx) {
    if (idx < 0 || idx >= g_vfs_count || !g_vfs_files[idx].used) return;
    g_bp_selected = idx;
    g_bp_cursor = 0;
    g_bp_scroll_line = 0;
    g_bp_dirty = false;
    g_bp_editing_name = false;
    if (g_vfs_files[idx].on_disk && !g_vfs_files[idx].is_binary && gSystemTable) {
        int realSz = g_vfs_files[idx].size;
        bool isBin = false;
        if (bp_fat32_read_raw(gSystemTable, g_vfs_files[idx].name, g_vfs_files[idx].content, VFS_DATA_MAX, &realSz, &isBin)) {
            g_vfs_files[idx].size = bp_str_len(g_vfs_files[idx].content);
        }
    }
    bp_str_copy(g_bp_status, g_vfs_files[idx].is_binary ? "Binary Hex View" : (g_vfs_files[idx].on_disk ? "FAT32 Backed" : "RAM Buffer"), sizeof(g_bp_status));
}

static void bp_create_new_file(void) {
    char candidate[VFS_NAME_MAX];
    while (g_bp_note_seq < 999) {
        char num[12];
        bp_int_to_str(g_bp_note_seq++, num, sizeof(num));
        bp_str_copy(candidate, "NOTE_", sizeof(candidate));
        bp_str_cat(candidate, num, sizeof(candidate));
        bp_str_cat(candidate, ".txt", sizeof(candidate));
        if (bp_find_file_index(candidate) < 0) break;
    }
    if (vfs_write_file(candidate, "")) {
        int idx = bp_find_file_index(candidate);
        if (idx >= 0) {
            g_bp_cat = 0;
            bp_select_index(idx);
            bp_str_copy(g_bp_name_buf, candidate, sizeof(g_bp_name_buf));
            g_bp_editing_name = true;
            bp_str_copy(g_bp_status, "Created New File", sizeof(g_bp_status));
        }
    }
}

static void bp_save_active_file(void) {
    if (g_bp_selected < 0 || g_bp_selected >= g_vfs_count || !g_vfs_files[g_bp_selected].used) return;
    VfsEntry* f = &g_vfs_files[g_bp_selected];
    if (f->is_binary) {
        bp_str_copy(g_bp_status, "Read-Only Binary", sizeof(g_bp_status));
        return;
    }
    f->size = bp_str_len(f->content);
    bool ok = vfs_write_file(f->name, f->content);
    g_bp_dirty = false;
    if (ok) {
        play_system_sound((char*)"success");
        bp_str_copy(g_bp_status, f->on_disk ? "Saved to FAT32" : "Saved to RAM VFS", sizeof(g_bp_status));
    } else {
        play_system_sound((char*)"error");
        bp_str_copy(g_bp_status, "Save Failed", sizeof(g_bp_status));
    }
}

static void bp_open_pending_path(const char* raw_path) {
    if (!raw_path || !raw_path[0]) return;
    char norm[VFS_NAME_MAX];
    bp_normalize_path(raw_path, norm, VFS_NAME_MAX);
    if (gSystemTable) bp_sync_from_fat32(gSystemTable);

    int idx = bp_find_file_index(norm);
    if (idx < 0) {
        char tmp[VFS_DATA_MAX];
        if (vfs_read_file(norm, tmp, VFS_DATA_MAX)) {
            idx = bp_find_file_index(norm);
        }
    }
    if (idx >= 0) {
        g_bp_cat = 0;
        bp_select_index(idx);
        bp_str_copy(g_bp_status, "Opened File", sizeof(g_bp_status));
    } else {
        bp_str_copy(g_bp_status, "File Not Found", sizeof(g_bp_status));
    }
}

static void bp_commit_rename(void) {
    if (g_bp_selected < 0 || g_bp_selected >= g_vfs_count) {
        g_bp_editing_name = false;
        return;
    }
    char norm[VFS_NAME_MAX];
    bp_normalize_path(g_bp_name_buf, norm, VFS_NAME_MAX);
    if (norm[0] && !bp_str_eq_nocase(norm, g_vfs_files[g_bp_selected].name)) {
        char old_name[VFS_NAME_MAX];
        char old_content[VFS_DATA_MAX];
        bp_str_copy(old_name, g_vfs_files[g_bp_selected].name, VFS_NAME_MAX);
        bp_str_copy(old_content, g_vfs_files[g_bp_selected].content, VFS_DATA_MAX);
        vfs_delete_file(old_name);
        vfs_write_file(norm, old_content);
        int nidx = bp_find_file_index(norm);
        if (nidx >= 0) bp_select_index(nidx);
        bp_str_copy(g_bp_status, "Renamed File", sizeof(g_bp_status));
    }
    g_bp_editing_name = false;
}

static void blankpad_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    if (!g_vfs_initialized) vfs_init(gSystemTable);

    const char* pending = wm_consume_pending_file();
    if (pending && pending[0]) {
        bp_open_pending_path(pending);
    }

    if (g_bp_selected < 0 || g_bp_selected >= g_vfs_count || !g_vfs_files[g_bp_selected].used) {
        g_bp_selected = (g_vfs_count > 0) ? 0 : -1;
    }

    bool click_avail = (in && in->left_pressed);
    bool down = (in && in->left_down);
    int mx = in ? in->mx : -1;
    int my = in ? in->my : -1;

    // Subtle spatial glass backdrop
    dui_rect(cx, cy, cw, ch, THEME_BG_DEEP, 195);

    int sidebar_w = g_bp_show_sidebar ? 216 : 0;
    if (g_bp_show_sidebar && cw < 480) sidebar_w = 168;

    // ------------------------------------------------------------------------
    // Left Sidebar: Volumes & Files
    // ------------------------------------------------------------------------
    if (g_bp_show_sidebar) {
        int sx = cx + 8;
        int sy = cy + 8;
        int sw = sidebar_w - 8;
        int sh = ch - 16;

        dui_bento_card(sx, sy, sw, sh, THEME_RADIUS_LG, THEME_SURFACE_CARD, 222, false);
        dui_dot_grid_overlay(sx + 8, sy + 6, sw - 16, 28, 12, THEME_TEXT_MUTED, 14);
        dui_text(sx + 12, sy + 11, "Explorer", THEME_TEXT_PRIMARY, 1);

        if (dui_button_tactile(sx + sw - 96, sy + 7, 42, 24, "Sync", THEME_SURFACE_ELEVATED, false, mx, my, down, click_avail)) {
            click_avail = false;
            if (gSystemTable) bp_sync_from_fat32(gSystemTable);
            bp_str_copy(g_bp_status, "FAT32 Synced", sizeof(g_bp_status));
            play_system_sound((char*)"tool");
        }
        if (dui_button_tactile(sx + sw - 50, sy + 7, 42, 24, "+New", THEME_ACCENT_CYAN, true, mx, my, down, click_avail)) {
            click_avail = false;
            bp_create_new_file();
            play_system_sound((char*)"open");
        }

        // Category filter pills
        const char* cats[4] = { "All", "Docs", "Cfg", "Disk" };
        int pill_w = (sw - 22) / 4;
        for (int c = 0; c < 4; c++) {
            int px = sx + 8 + c * (pill_w + 2);
            int py = sy + 38;
            bool active = (g_bp_cat == c);
            if (dui_button_tactile(px, py, pill_w, 22, cats[c], active ? THEME_ACCENT_CYAN : THEME_SURFACE_ELEVATED, active, mx, my, down, click_avail)) {
                click_avail = false;
                g_bp_cat = c;
                g_bp_sidebar_scroll = 0;
                play_system_sound((char*)"tab");
            }
        }

        // Build filtered index list
        int vis_idx[VFS_MAX_FILES];
        int vis_cnt = 0;
        for (int i = 0; i < g_vfs_count; i++) {
            if (!g_vfs_files[i].used) continue;
            if (g_bp_cat != 0 && bp_classify_file(g_vfs_files[i].name, g_vfs_files[i].on_disk) != g_bp_cat) continue;
            vis_idx[vis_cnt++] = i;
        }

        int list_y = sy + 68;
        int list_h = sh - 76;
        int row_h = 34;
        int max_rows = list_h / row_h;
        if (max_rows < 1) max_rows = 1;

        if (in && in->scroll_y != 0 && bp_pt_in(mx, my, sx, list_y, sw, list_h)) {
            g_bp_sidebar_scroll -= in->scroll_y;
        }
        if (g_bp_sidebar_scroll > vis_cnt - max_rows) g_bp_sidebar_scroll = vis_cnt - max_rows;
        if (g_bp_sidebar_scroll < 0) g_bp_sidebar_scroll = 0;

        dui_push_clip(sx + 4, list_y, sw - 8, list_h);
        for (int r = 0; r < max_rows && (g_bp_sidebar_scroll + r) < vis_cnt; r++) {
            int idx = vis_idx[g_bp_sidebar_scroll + r];
            VfsEntry* e = &g_vfs_files[idx];
            int ry = list_y + r * row_h;
            bool sel = (idx == g_bp_selected);
            bool hov = bp_pt_in(mx, my, sx + 6, ry, sw - 12, row_h - 4);

            uint32_t row_col = sel ? THEME_SURFACE_ELEVATED : (hov ? THEME_SURFACE_HOVER : THEME_SURFACE_BASE);
            dui_rect_rounded(sx + 6, ry, sw - 12, row_h - 4, THEME_RADIUS_SM, row_col, sel ? 240 : (hov ? 210 : 120));
            if (sel || hov) {
                dui_rect_rounded_outline(sx + 6, ry, sw - 12, row_h - 4, THEME_RADIUS_SM,
                                         sel ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE, sel ? 205 : 150);
            }
            if (sel) {
                dui_rect_rounded(sx + 6, ry + 5, 3, row_h - 14, 2, THEME_ACCENT_CYAN, 255);
            }

            // Extension badge
            uint32_t bcol = bp_file_badge_color(e->name);
            dui_rect_rounded(sx + 14, ry + 6, 30, 18, 4, bcol, 215);
            dui_text(sx + 17, ry + 7, bp_file_badge_label(e->name), THEME_TEXT_ON_ACCENT, 1);

            dui_text_ellipsis(sx + 50, ry + 7, sw - 64, e->name, sel ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);

            if (hov && click_avail) {
                click_avail = false;
                bp_select_index(idx);
                play_system_sound((char*)"open");
            }
        }
        dui_pop_clip();
    }

    // ------------------------------------------------------------------------
    // Right Workspace: Toolbar + Code/Note Editor + Status Bar
    // ------------------------------------------------------------------------
    int ex = cx + sidebar_w + 8;
    int ey = cy + 8;
    int ew = cw - sidebar_w - 16;
    int eh = ch - 16;

    dui_bento_card(ex, ey, ew, eh, THEME_RADIUS_LG, THEME_SURFACE_BASE, 228, true);

    // Top toolbar
    int tb_h = 42;
    dui_rect(ex + 1, ey + tb_h, ew - 2, 1, THEME_BORDER_SUBTLE, 180);

    if (dui_button_tactile(ex + 8, ey + 9, 66, 24, g_bp_show_sidebar ? "Focus" : "Files", THEME_SURFACE_ELEVATED, false, mx, my, down, click_avail)) {
        click_avail = false;
        g_bp_show_sidebar = !g_bp_show_sidebar;
        play_system_sound((char*)"tab");
    }

    VfsEntry* active = (g_bp_selected >= 0 && g_bp_selected < g_vfs_count && g_vfs_files[g_bp_selected].used)
                       ? &g_vfs_files[g_bp_selected] : NULL;

    int name_x = ex + 82;
    int name_w = ew - 240;
    if (name_w < 80) name_w = 80;

    if (active) {
        if (g_bp_editing_name) {
            dui_rect_rounded(name_x, ey + 8, name_w, 26, THEME_RADIUS_SM, THEME_SURFACE_INPUT, 245);
            dui_rect_rounded_outline(name_x, ey + 8, name_w, 26, THEME_RADIUS_SM, THEME_BORDER_FOCUS, 220);
            dui_text_ellipsis(name_x + 8, ey + 13, name_w - 16, g_bp_name_buf, THEME_TEXT_PRIMARY, 1);
        } else {
            bool hov_name = bp_pt_in(mx, my, name_x, ey + 8, name_w, 26);
            if (hov_name) {
                dui_rect_rounded(name_x, ey + 8, name_w, 26, THEME_RADIUS_SM, THEME_SURFACE_HOVER, 140);
                dui_rect_rounded_outline(name_x, ey + 8, name_w, 26, THEME_RADIUS_SM, THEME_BORDER_SUBTLE, 160);
                if (click_avail) {
                    click_avail = false;
                    bp_str_copy(g_bp_name_buf, active->name, sizeof(g_bp_name_buf));
                    g_bp_editing_name = true;
                    play_system_sound((char*)"tool");
                }
            }
            dui_text_ellipsis(name_x + 6, ey + 13, name_w - 12, active->name, THEME_TEXT_PRIMARY, 1);
        }
    } else {
        dui_text(name_x + 6, ey + 13, "No File Selected", THEME_TEXT_MUTED, 1);
    }

    if (dui_button_tactile(ex + ew - 142, ey + 9, 64, 24, "Save", THEME_ACCENT_CYAN, true, mx, my, down, click_avail)) {
        click_avail = false;
        bp_save_active_file();
    }
    if (dui_button_tactile(ex + ew - 72, ey + 9, 64, 24, "Delete", THEME_ACCENT_ROSE, false, mx, my, down, click_avail)) {
        click_avail = false;
        if (active) {
            char del_name[VFS_NAME_MAX];
            bp_str_copy(del_name, active->name, VFS_NAME_MAX);
            vfs_delete_file(del_name);
            g_bp_dirty = false;
            g_bp_cursor = 0;
            g_bp_scroll_line = 0;
            if (g_bp_selected >= g_vfs_count) g_bp_selected = g_vfs_count - 1;
            if (g_bp_selected >= 0) bp_select_index(g_bp_selected);
            bp_str_copy(g_bp_status, "Deleted File", sizeof(g_bp_status));
            play_system_sound((char*)"close");
        }
    }

    // Editor content area
    int body_x = ex + 8;
    int body_y = ey + tb_h + 6;
    int body_w = ew - 16;
    int body_h = eh - tb_h - 42;
    int gutter_w = 40;
    int line_h = 20;
    int vis_lines = body_h / line_h;
    if (vis_lines < 1) vis_lines = 1;

    dui_rect_rounded(body_x, body_y, body_w, body_h, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 240);
    dui_rect(body_x + 1, body_y + 1, gutter_w, body_h - 2, THEME_BG_DEEP, 180);
    dui_rect(body_x + gutter_w, body_y + 4, 1, body_h - 8, THEME_BORDER_SUBTLE, 140);
    dui_rect_rounded_outline(body_x, body_y, body_w, body_h, THEME_RADIUS_MD, THEME_BORDER_SUBTLE, 175);

    if (active) {
        char* buf = active->content;
        int len = bp_str_len(buf);
        if (g_bp_cursor < 0) g_bp_cursor = 0;
        if (g_bp_cursor > len) g_bp_cursor = len;

        // Keyboard handling
        if (in && in->has_key) {
            if (g_bp_editing_name) {
                int nlen = bp_str_len(g_bp_name_buf);
                if (in->key_char == '\n') {
                    bp_commit_rename();
                } else if (in->scan_code == BLANK_SCAN_ESC) {
                    g_bp_editing_name = false;
                } else if (in->key_char == '\b') {
                    if (nlen > 0) g_bp_name_buf[nlen - 1] = '\0';
                } else if (in->key_char >= 32 && in->key_char <= 126 && nlen < VFS_NAME_MAX - 2) {
                    g_bp_name_buf[nlen] = in->key_char;
                    g_bp_name_buf[nlen + 1] = '\0';
                }
            } else if (in->ctrl_held && (in->key_char == 's' || in->key_char == 'S')) {
                bp_save_active_file();
            } else if (in->ctrl_held && (in->key_char == 'n' || in->key_char == 'N')) {
                bp_create_new_file();
            } else if (!active->is_binary) {
                int cur_line = 0, cur_col = 0;
                bp_compute_line_col(buf, g_bp_cursor, &cur_line, &cur_col);

                if (in->scan_code == BLANK_SCAN_LEFT) {
                    if (g_bp_cursor > 0) g_bp_cursor--;
                } else if (in->scan_code == BLANK_SCAN_RIGHT) {
                    if (g_bp_cursor < len) g_bp_cursor++;
                } else if (in->scan_code == BLANK_SCAN_UP) {
                    if (cur_line > 0) g_bp_cursor = bp_offset_for_line_col(buf, cur_line - 1, cur_col);
                } else if (in->scan_code == BLANK_SCAN_DOWN) {
                    int total_l = bp_count_lines(buf);
                    if (cur_line + 1 < total_l) g_bp_cursor = bp_offset_for_line_col(buf, cur_line + 1, cur_col);
                } else if (in->scan_code == BLANK_SCAN_HOME) {
                    g_bp_cursor = bp_offset_for_line_col(buf, cur_line, 0);
                } else if (in->scan_code == BLANK_SCAN_END) {
                    g_bp_cursor = bp_offset_for_line_col(buf, cur_line, 1024);
                } else if (in->scan_code == BLANK_SCAN_PGUP) {
                    int nl = cur_line - vis_lines;
                    if (nl < 0) nl = 0;
                    g_bp_cursor = bp_offset_for_line_col(buf, nl, cur_col);
                } else if (in->scan_code == BLANK_SCAN_PGDN) {
                    int total_l = bp_count_lines(buf);
                    int nl = cur_line + vis_lines;
                    if (nl >= total_l) nl = total_l - 1;
                    g_bp_cursor = bp_offset_for_line_col(buf, nl, cur_col);
                } else if (in->key_char == '\b') {
                    if (g_bp_cursor > 0) {
                        for (int i = g_bp_cursor - 1; i < len; i++) buf[i] = buf[i + 1];
                        g_bp_cursor--;
                        active->size = len - 1;
                        g_bp_dirty = true;
                    }
                } else if (in->scan_code == BLANK_SCAN_DELETE) {
                    if (g_bp_cursor < len) {
                        for (int i = g_bp_cursor; i < len; i++) buf[i] = buf[i + 1];
                        active->size = len - 1;
                        g_bp_dirty = true;
                    }
                } else if (in->key_char == '\t') {
                    if (len + 2 < VFS_DATA_MAX - 1) {
                        for (int i = len; i >= g_bp_cursor; i--) buf[i + 2] = buf[i];
                        buf[g_bp_cursor++] = ' ';
                        buf[g_bp_cursor++] = ' ';
                        active->size = len + 2;
                        g_bp_dirty = true;
                    }
                } else if (in->key_char == '\n' || (in->key_char >= 32 && in->key_char <= 126)) {
                    if (len + 1 < VFS_DATA_MAX - 1) {
                        for (int i = len; i >= g_bp_cursor; i--) buf[i + 1] = buf[i];
                        buf[g_bp_cursor++] = in->key_char;
                        active->size = len + 1;
                        g_bp_dirty = true;
                    }
                }

                // Auto-scroll to keep cursor in view
                bp_compute_line_col(buf, g_bp_cursor, &cur_line, &cur_col);
                if (cur_line < g_bp_scroll_line) g_bp_scroll_line = cur_line;
                if (cur_line >= g_bp_scroll_line + vis_lines) g_bp_scroll_line = cur_line - vis_lines + 1;
            }
        }

        // Mouse wheel scrolling & click-to-place cursor
        int total_lines = bp_count_lines(buf);
        if (in && in->scroll_y != 0 && bp_pt_in(mx, my, body_x, body_y, body_w, body_h)) {
            g_bp_scroll_line -= in->scroll_y * 2;
        }
        if (g_bp_scroll_line > total_lines - vis_lines) g_bp_scroll_line = total_lines - vis_lines;
        if (g_bp_scroll_line < 0) g_bp_scroll_line = 0;

        if (click_avail && bp_pt_in(mx, my, body_x + gutter_w, body_y, body_w - gutter_w, body_h)) {
            click_avail = false;
            g_bp_editing_name = false;
            int clicked_line = g_bp_scroll_line + (my - (body_y + 6)) / line_h;
            if (clicked_line < 0) clicked_line = 0;
            if (clicked_line >= total_lines) clicked_line = total_lines - 1;
            int line_start = bp_offset_for_line_col(buf, clicked_line, 0);
            int rel_x = mx - (body_x + gutter_w + 10);
            int best_col = 0;
            if (rel_x > 0) {
                char seg[128];
                int col = 0;
                while (buf[line_start + col] && buf[line_start + col] != '\n' && col < 126) {
                    seg[col] = buf[line_start + col];
                    seg[col + 1] = '\0';
                    if (dui_text_width(seg, 1) > rel_x) break;
                    col++;
                    best_col = col;
                }
            }
            g_bp_cursor = bp_offset_for_line_col(buf, clicked_line, best_col);
        }

        int cur_line = 0, cur_col = 0;
        bp_compute_line_col(buf, g_bp_cursor, &cur_line, &cur_col);

        // Render visible lines
        dui_push_clip(body_x + 2, body_y + 4, body_w - 4, body_h - 8);
        int pos = 0;
        int line_idx = 0;
        while (buf[pos] && line_idx < g_bp_scroll_line) {
            if (buf[pos] == '\n') line_idx++;
            pos++;
        }

        for (int row = 0; row < vis_lines; row++) {
            int ln = g_bp_scroll_line + row;
            if (ln >= total_lines) break;

            int ly = body_y + 6 + row * line_h;
            if (ln == cur_line && !active->is_binary) {
                dui_rect(body_x + gutter_w + 1, ly - 1, body_w - gutter_w - 3, line_h, THEME_SURFACE_ELEVATED, 120);
            }

            char lnum[8];
            bp_int_to_str(ln + 1, lnum, sizeof(lnum));
            dui_text(body_x + 8, ly + 1, lnum, (ln == cur_line) ? THEME_ACCENT_CYAN : THEME_TEXT_MUTED, 1);

            char line_buf[256];
            int lpos = 0;
            while (buf[pos] && buf[pos] != '\n' && lpos < 254) {
                line_buf[lpos++] = buf[pos++];
            }
            while (buf[pos] && buf[pos] != '\n') pos++;
            if (buf[pos] == '\n') pos++;
            line_buf[lpos] = '\0';

            uint32_t lcol = THEME_TEXT_PRIMARY;
            if (line_buf[0] == '#') lcol = THEME_ACCENT_CYAN;
            else if (line_buf[0] == '/' && line_buf[1] == '/') lcol = THEME_TEXT_MUTED;
            else if (line_buf[0] == ';') lcol = THEME_TEXT_MUTED;
            else if (line_buf[0] == '[') lcol = THEME_ACCENT_AMBER;

            dui_text(body_x + gutter_w + 10, ly + 1, line_buf, lcol, 1);

            // Blinking caret on active cursor line
            if (ln == cur_line && !active->is_binary && !g_bp_editing_name) {
                bool blink_on = !in || (((in->frame_ms / 450) % 2) == 0);
                if (blink_on) {
                    char prefix[256];
                    int cl = cur_col < 254 ? cur_col : 254;
                    for (int k = 0; k < cl; k++) prefix[k] = line_buf[k];
                    prefix[cl] = '\0';
                    int cx_caret = body_x + gutter_w + 10 + dui_text_width(prefix, 1);
                    dui_rect(cx_caret, ly, 2, line_h - 3, THEME_ACCENT_CYAN, 255);
                }
            }
        }
        dui_pop_clip();

        // Status Footer with Lira Mode Pill
        int fy = ey + eh - 28;
        const char* badge_lbl = g_bp_dirty ? "Unsaved*" : (active->is_binary ? "Binary RO" : (active->on_disk ? "FAT32" : "RAM VFS"));
        uint32_t badge_col = g_bp_dirty ? THEME_ACCENT_AMBER : (active->is_binary ? THEME_ACCENT_IRIS : (active->on_disk ? THEME_ACCENT_GREEN : THEME_ACCENT_CYAN));
        dui_status_pill(ex + 8, fy - 2, 96, 22, badge_lbl, badge_col, THEME_SURFACE_INPUT, g_bp_dirty || active->on_disk);

        char footer[128];
        char nb1[16], nb2[16], nb3[16];
        bp_int_to_str(cur_line + 1, nb1, sizeof(nb1));
        bp_int_to_str(cur_col + 1, nb2, sizeof(nb2));
        bp_int_to_str(active->size, nb3, sizeof(nb3));
        bp_str_copy(footer, "Ln ", sizeof(footer));
        bp_str_cat(footer, nb1, sizeof(footer));
        bp_str_cat(footer, ", Col ", sizeof(footer));
        bp_str_cat(footer, nb2, sizeof(footer));
        bp_str_cat(footer, "  |  ", sizeof(footer));
        bp_str_cat(footer, nb3, sizeof(footer));
        bp_str_cat(footer, " B  |  ", sizeof(footer));
        bp_str_cat(footer, g_bp_status, sizeof(footer));

        int hint_space = (ew >= 340) ? 112 : 12;
        dui_text_ellipsis(ex + 112, fy + 1, ew - 118 - hint_space, footer, g_bp_dirty ? THEME_ACCENT_AMBER : THEME_TEXT_SECONDARY, 1);
        if (ew >= 340) {
            dui_text(ex + ew - 104, fy + 1, "Ctrl+S Save", THEME_TEXT_MUTED, 1);
        }
    }
}

extern "C" {

void blankpad_register(void) {
    wm_register_app(APP_FILES, "Files & Notes", 760, 500, THEME_ACCENT_CYAN, blankpad_render, NULL);
}

void launch_blankpad(void) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    wm_open_app(APP_FILES);
}

void launch_files_app(void) {
    if (!g_vfs_initialized) vfs_init(gSystemTable);
    wm_open_app(APP_FILES);
}

} // extern "C"
