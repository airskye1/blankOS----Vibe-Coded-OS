#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/theme.h"
#include "../ui/blankDUI.h"
#include "../ui/window_manager.h"
#include "../kernel/input.h"
#include "../kernel/net.h"
#include "../kernel/audio.h"

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
    extern bool load_and_run_elf(EFI_SYSTEM_TABLE *SystemTable, CHAR16* filename);
    extern void os_send_notification(const char* title, const char* message);
}

namespace {

#define MAX_STORE_PKGS 32

typedef struct {
    char id[32];
    char name[48];
    char category[24];
    char version[16];
    char desc[96];
    char disk_path[96];
    uint32_t accent;
    bool is_elf;
    bool installed;
    bool is_builtin_repo;
    uint32_t disk_bytes;
    AppId mapped_app; // APP_COUNT if not a built-in WM app
} StorePkg;

static bool s_registered = false;
static bool s_scanned = false;
static StorePkg s_pkgs[MAX_STORE_PKGS];
static int s_pkg_count = 0;

static int s_active_tab = 0; // 0=All Packages, 1=Installed, 2=Downloads
static int s_scroll_idx = 0;

static char s_url_buf[256] = "http://10.0.2.2:8000/hello.pkg";
static int s_url_len = 30;
static bool s_url_focused = false;
static int s_active_xfer_id = -1;

static char s_status_msg[112] = "Ready - UEFI SimpleFileSystem & HTTP/1.1 Package Hub";
static uint32_t s_status_color = THEME_TEXT_SECONDARY;

static bool pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void s_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void s_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int i = 0;
    while (dst[i] && i < max_len - 1) i++;
    int j = 0;
    while (src[j] && i < max_len - 1) dst[i++] = src[j++];
    dst[i] = '\0';
}

static int s_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static char to_lower_c(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static bool s_ieq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (to_lower_c(a[i]) != to_lower_c(b[i])) return false;
        i++;
    }
    return a[i] == b[i];
}

static bool s_ends_with_i(const char* s, const char* suffix) {
    int ls = s_len(s);
    int lf = s_len(suffix);
    if (ls < lf) return false;
    return s_ieq(s + (ls - lf), suffix);
}

static void u64_to_str(uint64_t val, char* buf, int max_len) {
    if (!buf || max_len < 2) return;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[24];
    int i = 0;
    while (val > 0 && i < 22) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (i > 0 && j < max_len - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void ascii_to_wide(const char* src, CHAR16* dst, int max_chars) {
    int i = 0;
    while (src && src[i] && i < max_chars - 1) {
        dst[i] = (CHAR16)(uint8_t)src[i];
        i++;
    }
    dst[i] = 0;
}

static void wide_to_ascii(const CHAR16* src, char* dst, int max_chars) {
    int i = 0;
    while (src && src[i] && i < max_chars - 1) {
        dst[i] = (char)(src[i] & 0x7F);
        i++;
    }
    dst[i] = '\0';
}

static void set_status(const char* msg, uint32_t col) {
    s_copy(s_status_msg, msg, sizeof(s_status_msg));
    s_status_color = col;
}

// Open a directory on a volume (creating subdirectories if write_mode is true)
static EFI_FILE_PROTOCOL* open_dir_on_root(EFI_FILE_PROTOCOL* root, const char* dir_path, bool create_if_missing) {
    if (!root || !dir_path || !dir_path[0]) return NULL;
    char seg[64];
    EFI_FILE_PROTOCOL* cur = root;
    bool cur_is_root = true;

    int p = 0;
    while (dir_path[p]) {
        while (dir_path[p] == '\\' || dir_path[p] == '/') p++;
        if (!dir_path[p]) break;
        int sl = 0;
        while (dir_path[p] && dir_path[p] != '\\' && dir_path[p] != '/' && sl < 62) {
            seg[sl++] = dir_path[p++];
        }
        seg[sl] = '\0';

        CHAR16 wseg[64];
        ascii_to_wide(seg, wseg, 64);

        EFI_FILE_PROTOCOL* next = NULL;
        UINT64 mode = create_if_missing ? (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)
                                        : EFI_FILE_MODE_READ;
        EFI_STATUS st = cur->Open(cur, &next, wseg, mode, EFI_FILE_DIRECTORY);
        if (!cur_is_root) cur->Close(cur);
        if (EFI_ERROR(st) || !next) return NULL;
        cur = next;
        cur_is_root = false;
    }
    return cur_is_root ? NULL : cur;
}

static bool fs_read_file_bytes(const char* rel_path, uint8_t* out_buf, uint32_t max_cap, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!gSystemTable || !gSystemTable->BootServices || !rel_path) return false;

    EFI_GUID sfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(gSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &sfspGuid, NULL, &count, &handles)) || !handles) {
        return false;
    }

    CHAR16 wpath[128];
    ascii_to_wide(rel_path, wpath, 128);

    bool ok = false;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(gSystemTable->BootServices->HandleProtocol(handles[i], &sfspGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL* root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_PROTOCOL* file = NULL;
        if (root->Open(root, &file, wpath, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS && file) {
            UINTN rsz = max_cap;
            if (file->Read(file, &rsz, out_buf) == EFI_SUCCESS) {
                if (out_size) *out_size = (uint32_t)rsz;
                ok = true;
            }
            file->Close(file);
        }
        root->Close(root);
        if (ok) break;
    }
    gSystemTable->BootServices->FreePool(handles);
    return ok;
}

static bool fs_write_file_bytes(const char* dir_rel, const char* rel_path, const void* data, uint32_t len) {
    if (!gSystemTable || !gSystemTable->BootServices || !rel_path || !data) return false;

    EFI_GUID sfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(gSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &sfspGuid, NULL, &count, &handles)) || !handles) {
        return false;
    }

    CHAR16 wpath[128];
    ascii_to_wide(rel_path, wpath, 128);

    bool ok = false;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(gSystemTable->BootServices->HandleProtocol(handles[i], &sfspGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL* root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        if (dir_rel && dir_rel[0]) {
            EFI_FILE_PROTOCOL* d = open_dir_on_root(root, dir_rel, true);
            if (!d) {
                root->Close(root);
                continue;
            }
            d->Close(d);
        }

        // Remove existing file first so shorter writes don't leave trailing bytes
        EFI_FILE_PROTOCOL* old_f = NULL;
        if (root->Open(root, &old_f, wpath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS && old_f) {
            old_f->Delete(old_f);
        }

        EFI_FILE_PROTOCOL* file = NULL;
        if (root->Open(root, &file, wpath,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) == EFI_SUCCESS && file) {
            UINTN wlen = len;
            if (file->Write(file, &wlen, (void*)data) == EFI_SUCCESS && wlen == len) {
                file->Flush(file);
                ok = true;
            }
            file->Close(file);
        }
        root->Close(root);
        if (ok) break;
    }
    gSystemTable->BootServices->FreePool(handles);
    return ok;
}

static bool fs_delete_file(const char* rel_path) {
    if (!gSystemTable || !gSystemTable->BootServices || !rel_path) return false;

    EFI_GUID sfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(gSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &sfspGuid, NULL, &count, &handles)) || !handles) {
        return false;
    }

    CHAR16 wpath[128];
    ascii_to_wide(rel_path, wpath, 128);

    bool ok = false;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(gSystemTable->BootServices->HandleProtocol(handles[i], &sfspGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL* root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_PROTOCOL* file = NULL;
        if (root->Open(root, &file, wpath, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS && file) {
            if (file->Delete(file) == EFI_SUCCESS) {
                ok = true;
            }
        }
        root->Close(root);
        if (ok) break;
    }
    gSystemTable->BootServices->FreePool(handles);
    return ok;
}

static void parse_pkg_metadata(const uint8_t* buf, uint32_t sz, StorePkg* pkg) {
    if (!buf || sz == 0 || !pkg) return;
    uint32_t i = 0;
    char line[128];
    while (i < sz) {
        int lp = 0;
        while (i < sz && buf[i] != '\n' && buf[i] != '\r' && lp < 126) {
            if (buf[i] >= 32 && buf[i] < 127) line[lp++] = (char)buf[i];
            i++;
        }
        line[lp] = '\0';
        while (i < sz && (buf[i] == '\n' || buf[i] == '\r')) i++;

        if (line[0] == 'N' && line[1] == 'A' && line[2] == 'M' && line[3] == 'E' && line[4] == '=') {
            s_copy(pkg->name, line + 5, sizeof(pkg->name));
        } else if (line[0] == 'V' && line[1] == 'E' && line[2] == 'R' && line[3] == 'S' && line[4] == 'I' &&
                   line[5] == 'O' && line[6] == 'N' && line[7] == '=') {
            s_copy(pkg->version, line + 8, sizeof(pkg->version));
        } else if (line[0] == 'D' && line[1] == 'E' && line[2] == 'S' && line[3] == 'C' && line[4] == '=') {
            s_copy(pkg->desc, line + 5, sizeof(pkg->desc));
        } else if (line[0] == 'C' && line[1] == 'A' && line[2] == 'T' && line[3] == '=') {
            s_copy(pkg->category, line + 4, sizeof(pkg->category));
        }
    }
}

static int find_pkg_by_path(const char* path) {
    for (int i = 0; i < s_pkg_count; i++) {
        if (s_ieq(s_pkgs[i].disk_path, path)) return i;
    }
    return -1;
}

static void add_builtin_repo(void) {
    struct RepoSeed {
        const char* id;
        const char* name;
        const char* cat;
        const char* ver;
        const char* desc;
        const char* path;
        uint32_t accent;
        AppId app;
    };
    const RepoSeed seeds[6] = {
        { "BLANKPAD", "BlankPad & Files", "Productivity", "v2.4",
          "Dual-pane FAT32 file explorer and rich notes studio.", "EFI\\APPS\\BLANKPAD.PKG", THEME_ACCENT_CYAN, APP_FILES },
        { "ACTMON", "System Pulse", "Developer", "v2.4",
          "Live CPUID, RDTSC, memory, and PCIe bus telemetry.", "EFI\\APPS\\ACTMON.PKG", THEME_ACCENT_GREEN, APP_MONITOR },
        { "BLANKREG", "BlankReg Studio", "Utilities", "v2.1",
          "Hierarchical kernel registry editor with INI persistence.", "EFI\\APPS\\BLANKREG.PKG", THEME_ACCENT_AMBER, APP_REGEDIT },
        { "TERMINAL", "Horizon Terminal", "Developer", "v2.0",
          "Bare-metal shell with VFS, ping, HTTP, and system tools.", "EFI\\APPS\\TERMINAL.PKG", THEME_ACCENT_IRIS, APP_TERMINAL },
        { "WEATHER", "Atmosphere Weather", "Utilities", "v2.0",
          "Live HTTP climate & atmospheric telemetry dashboard.", "EFI\\APPS\\WEATHER.PKG", THEME_ACCENT_TEAL, APP_WEATHER },
        { "CALC", "Precision Calc", "Productivity", "v2.0",
          "Floating-point & scientific Horizon desktop calculator.", "EFI\\APPS\\CALC.PKG", THEME_ACCENT_ROSE, APP_CALCULATOR }
    };

    for (int i = 0; i < 6 && s_pkg_count < MAX_STORE_PKGS; i++) {
        StorePkg* p = &s_pkgs[s_pkg_count++];
        s_copy(p->id, seeds[i].id, sizeof(p->id));
        s_copy(p->name, seeds[i].name, sizeof(p->name));
        s_copy(p->category, seeds[i].cat, sizeof(p->category));
        s_copy(p->version, seeds[i].ver, sizeof(p->version));
        s_copy(p->desc, seeds[i].desc, sizeof(p->desc));
        s_copy(p->disk_path, seeds[i].path, sizeof(p->disk_path));
        p->accent = seeds[i].accent;
        p->is_elf = false;
        p->installed = false;
        p->is_builtin_repo = true;
        p->disk_bytes = 0;
        p->mapped_app = seeds[i].app;
    }
}

static void scan_dir_packages(EFI_FILE_PROTOCOL* root, const char* dir_rel) {
    EFI_FILE_PROTOCOL* dir = open_dir_on_root(root, dir_rel, false);
    if (!dir) return;

    dir->SetPosition(dir, 0);
    uint8_t info_buf[512];
    int guard = 0;
    while (guard++ < 64) {
        UINTN bsz = sizeof(info_buf);
        EFI_STATUS st = dir->Read(dir, &bsz, info_buf);
        if (EFI_ERROR(st) || bsz == 0) break;

        EFI_FILE_INFO* info = (EFI_FILE_INFO*)info_buf;
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;

        char fname[64];
        wide_to_ascii(info->FileName, fname, sizeof(fname));
        if (!fname[0] || fname[0] == '.') continue;

        bool is_elf = s_ends_with_i(fname, ".elf");
        bool is_pkg = s_ends_with_i(fname, ".pkg");
        if (!is_elf && !is_pkg) continue;

        char full_path[96];
        s_copy(full_path, dir_rel, sizeof(full_path));
        s_cat(full_path, "\\", sizeof(full_path));
        s_cat(full_path, fname, sizeof(full_path));

        int existing = find_pkg_by_path(full_path);
        if (existing >= 0) {
            s_pkgs[existing].installed = true;
            s_pkgs[existing].disk_bytes = (uint32_t)info->FileSize;
            continue;
        }

        if (s_pkg_count >= MAX_STORE_PKGS) continue;
        StorePkg* p = &s_pkgs[s_pkg_count++];
        s_copy(p->id, fname, sizeof(p->id));
        // Strip extension for clean display name
        s_copy(p->name, fname, sizeof(p->name));
        int nl = s_len(p->name);
        if (nl > 4 && p->name[nl - 4] == '.') p->name[nl - 4] = '\0';

        s_copy(p->category, is_elf ? "ELF Binary" : "Package (.PKG)", sizeof(p->category));
        s_copy(p->version, is_elf ? "ELF64" : "v1.0", sizeof(p->version));
        s_copy(p->desc, is_elf ? "Native x86_64 ELF executable discovered on disk."
                               : "BlankOS package manifest discovered on disk.", sizeof(p->desc));
        s_copy(p->disk_path, full_path, sizeof(p->disk_path));
        p->accent = is_elf ? THEME_ACCENT_TEAL : THEME_ACCENT_IRIS;
        p->is_elf = is_elf;
        p->installed = true;
        p->is_builtin_repo = false;
        p->disk_bytes = (uint32_t)info->FileSize;
        p->mapped_app = APP_COUNT;

        if (is_pkg && info->FileSize > 0 && info->FileSize <= 2048) {
            uint8_t meta[512];
            uint32_t msz = 0;
            if (fs_read_file_bytes(full_path, meta, sizeof(meta), &msz) && msz > 0) {
                parse_pkg_metadata(meta, msz, p);
            }
        }
    }
    dir->Close(dir);
}

static void scan_all_packages(void) {
    s_scanned = true;
    s_pkg_count = 0;
    add_builtin_repo();

    if (!gSystemTable || !gSystemTable->BootServices) return;
    EFI_GUID sfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(gSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &sfspGuid, NULL, &count, &handles)) || !handles) {
        return;
    }

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(gSystemTable->BootServices->HandleProtocol(handles[i], &sfspGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL* root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        scan_dir_packages(root, "EFI\\APPS");
        scan_dir_packages(root, "APPS");
        root->Close(root);
    }
    gSystemTable->BootServices->FreePool(handles);
}

static void extract_filename_from_url(const char* url, char* out, int max_len) {
    const char* last_slash = NULL;
    for (int i = 0; url && url[i]; i++) {
        if (url[i] == '/') last_slash = &url[i];
    }
    if (last_slash && last_slash[1]) {
        s_copy(out, last_slash + 1, max_len);
        // Strip query string if any
        for (int i = 0; out[i]; i++) {
            if (out[i] == '?' || out[i] == '#') { out[i] = '\0'; break; }
        }
    } else {
        s_copy(out, "package.pkg", max_len);
    }
    if (!out[0]) s_copy(out, "package.pkg", max_len);
}

static void start_http_fetch(void) {
    if (!s_url_buf[0]) {
        set_status("Enter an http:// URL to fetch a package or manifest", THEME_ACCENT_AMBER);
        return;
    }
    if (s_active_xfer_id >= 0) {
        net_http_close(s_active_xfer_id);
        s_active_xfer_id = -1;
    }
    char fname[64];
    extract_filename_from_url(s_url_buf, fname, sizeof(fname));
    s_active_xfer_id = net_http_start(s_url_buf, fname);
    if (s_active_xfer_id >= 0) {
        play_system_sound((char*)"click");
        set_status("Fetching package over HTTP/1.1...", THEME_ACCENT_TEAL);
    } else {
        play_system_sound((char*)"error");
        set_status("Failed to start HTTP transfer (check URL / NIC)", THEME_ACCENT_ROSE);
    }
}

static bool install_pkg_entry(StorePkg* p) {
    if (!p || p->is_elf) return false;
    char payload[512];
    for (int i = 0; i < 512; i++) payload[i] = 0;
    s_copy(payload, "BLOE_PKG_V2\nID=", sizeof(payload));
    s_cat(payload, p->id, sizeof(payload));
    s_cat(payload, "\nNAME=", sizeof(payload));
    s_cat(payload, p->name, sizeof(payload));
    s_cat(payload, "\nVERSION=", sizeof(payload));
    s_cat(payload, p->version, sizeof(payload));
    s_cat(payload, "\nCAT=", sizeof(payload));
    s_cat(payload, p->category, sizeof(payload));
    s_cat(payload, "\nDESC=", sizeof(payload));
    s_cat(payload, p->desc, sizeof(payload));
    s_cat(payload, "\nARCH=x86_64-uefi\nVERIFIED=1\n", sizeof(payload));

    uint32_t plen = (uint32_t)s_len(payload);
    if (fs_write_file_bytes("EFI\\APPS", p->disk_path, payload, plen)) {
        p->installed = true;
        p->disk_bytes = plen;
        play_system_sound((char*)"success");
        os_send_notification("Software Hub", "Package installed to \\EFI\\APPS\\");
        set_status("Package written to \\EFI\\APPS\\ on FAT32", THEME_ACCENT_GREEN);
        return true;
    }
    play_system_sound((char*)"error");
    set_status("Write failed: volume is read-only (Live ISO9660)", THEME_ACCENT_ROSE);
    return false;
}

static void launch_pkg_entry(StorePkg* p) {
    if (!p) return;
    if (p->is_elf) {
        if (!gSystemTable) return;
        CHAR16 wpath[128];
        ascii_to_wide(p->disk_path, wpath, 128);
        play_system_sound((char*)"click");
        bool ok = load_and_run_elf(gSystemTable, wpath);
        input_refresh_bounds((int)screen_width, (int)screen_height);
        if (ok) {
            set_status("ELF application exited cleanly", THEME_ACCENT_GREEN);
        } else {
            play_system_sound((char*)"error");
            set_status("Failed to load ELF64 binary", THEME_ACCENT_ROSE);
        }
    } else if (p->mapped_app != APP_COUNT) {
        wm_open_app(p->mapped_app);
    } else {
        wm_request_open_file(p->disk_path);
    }
}

static void render_downloads_tab(int px, int py, int pw, int ph, const BlankInputState* in) {
    // URL Input + Fetch Bar
    dui_glass_card(px, py, pw, 114, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);
    dui_text(px + 14, py + 12, "HTTP PACKAGE & MANIFEST FETCHER (SAVES TO \\DOWNLOADS\\)", THEME_ACCENT_TEAL, 1);

    int inp_x = px + 14;
    int inp_y = py + 36;
    int inp_w = pw - 124;
    int inp_h = 34;

    if (in->left_pressed) {
        s_url_focused = pt_in(in->mx, in->my, inp_x, inp_y, inp_w, inp_h);
    }
    dui_rect_rounded(inp_x, inp_y, inp_w, inp_h, THEME_RADIUS_SM, THEME_SURFACE_INPUT, 240);
    dui_rect_rounded_outline(inp_x, inp_y, inp_w, inp_h, THEME_RADIUS_SM,
                             s_url_focused ? THEME_BORDER_FOCUS : THEME_BORDER_SUBTLE, 220);
    dui_text_ellipsis(inp_x + 10, inp_y + 9, inp_w - 20,
                      s_url_len > 0 ? s_url_buf : "http://...",
                      s_url_len > 0 ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);

    if (s_url_focused && in->has_key) {
        if (in->key_char == '\b') {
            if (s_url_len > 0) s_url_buf[--s_url_len] = '\0';
        } else if (in->key_char == '\n') {
            start_http_fetch();
        } else if (in->key_char >= 32 && in->key_char < 127 && s_url_len < 240) {
            s_url_buf[s_url_len++] = in->key_char;
            s_url_buf[s_url_len] = '\0';
        }
    }

    if (dui_button(px + pw - 100, inp_y, 86, inp_h, "Fetch", THEME_ACCENT_TEAL, true,
                   in->mx, in->my, in->left_pressed)) {
        start_http_fetch();
    }

    // Curated HTTP Presets
    const char* preset_labels[3] = { "Local :8000 .PKG", "Version Manifest", "HTTP Info Test" };
    const char* preset_urls[3] = {
        "http://10.0.2.2:8000/hello.pkg",
        "http://10.0.2.2:8000/version.json",
        "http://info.cern.ch/"
    };
    for (int i = 0; i < 3; i++) {
        int bx = px + 14 + i * 144;
        if (dui_button(bx, py + 78, 136, 26, preset_labels[i], THEME_SURFACE_ELEVATED, false,
                       in->mx, in->my, in->left_pressed)) {
            s_copy(s_url_buf, preset_urls[i], sizeof(s_url_buf));
            s_url_len = s_len(s_url_buf);
            s_url_focused = true;
            play_system_sound((char*)"click");
        }
    }

    // Poll active transfer if running
    int list_y = py + 124;
    if (s_active_xfer_id >= 0) {
        const HttpTransfer* tr = net_http_get(s_active_xfer_id);
        if (tr) {
            dui_glass_card(px, list_y, pw, 60, THEME_RADIUS_MD, THEME_SURFACE_ELEVATED, 235, true);
            const char* st_txt = "Idle";
            if (tr->state == HTTP_RESOLVING_DNS) st_txt = "Resolving DNS...";
            else if (tr->state == HTTP_CONNECTING) st_txt = "Connecting TCP...";
            else if (tr->state == HTTP_SENDING) st_txt = "Sending HTTP GET...";
            else if (tr->state == HTTP_RECEIVING) st_txt = "Downloading...";
            else if (tr->state == HTTP_DONE) st_txt = "Completed";
            else if (tr->state == HTTP_ERROR) st_txt = tr->error_msg[0] ? tr->error_msg : "Transfer error";

            dui_text_ellipsis(px + 14, list_y + 10, pw - 140, tr->save_filename, THEME_TEXT_PRIMARY, 1);
            dui_text_ellipsis(px + 14, list_y + 30, pw - 140, st_txt, THEME_ACCENT_CYAN, 1);

            float prog = 0.25f;
            if (tr->content_length > 0) {
                prog = (float)tr->bytes_received / (float)tr->content_length;
                if (prog > 1.0f) prog = 1.0f;
            } else if (tr->state == HTTP_DONE) {
                prog = 1.0f;
            }
            dui_progress_ring(px + pw - 36, list_y + 30, 18, 4, prog, THEME_SURFACE_INPUT, THEME_ACCENT_TEAL);

            if (tr->state == HTTP_DONE) {
                play_system_sound((char*)"download");
                os_send_notification("Software Hub", "Download saved to \\DOWNLOADS\\");
                set_status("HTTP fetch complete - saved to \\DOWNLOADS\\", THEME_ACCENT_GREEN);
                net_http_close(s_active_xfer_id);
                s_active_xfer_id = -1;
                scan_all_packages();
            } else if (tr->state == HTTP_ERROR) {
                play_system_sound((char*)"error");
                set_status(tr->error_msg[0] ? tr->error_msg : "HTTP download failed", THEME_ACCENT_ROSE);
                net_http_close(s_active_xfer_id);
                s_active_xfer_id = -1;
            }
            list_y += 70;
        } else {
            s_active_xfer_id = -1;
        }
    }

    // Real Download History from net.h
    int dl_cnt = net_download_count();
    int box_h = ph - (list_y - py);
    if (box_h < 80) box_h = 80;
    dui_glass_card(px, list_y, pw, box_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
    dui_text(px + 14, list_y + 12, "DOWNLOADED FILES (\\DOWNLOADS\\)", THEME_TEXT_SECONDARY, 1);

    if (dl_cnt == 0) {
        dui_text(px + 14, list_y + 44, "No HTTP downloads in this session yet.", THEME_TEXT_MUTED, 1);
        dui_text(px + 14, list_y + 66, "Enter an http:// URL above and click Fetch to download real files.", THEME_TEXT_MUTED, 1);
        return;
    }

    int max_rows = (box_h - 46) / 48;
    if (max_rows < 1) max_rows = 1;
    for (int i = 0; i < dl_cnt && i < max_rows; i++) {
        const DownloadEntry* de = net_get_download(i);
        if (!de) continue;
        int ry = list_y + 38 + i * 48;
        dui_rect_rounded(px + 10, ry, pw - 20, 42, THEME_RADIUS_SM, THEME_SURFACE_INPUT, 210);
        dui_text_ellipsis(px + 20, ry + 6, pw - 240, de->filename, THEME_TEXT_PRIMARY, 1);
        dui_text_ellipsis(px + 20, ry + 24, pw - 240, de->status_text, THEME_TEXT_SECONDARY, 1);

        if (de->state == HTTP_DONE && de->saved_ok) {
            if (dui_button(px + pw - 206, ry + 7, 114, 28, "Install App", THEME_ACCENT_TEAL, true,
                           in->mx, in->my, in->left_pressed)) {
                uint32_t max_copy = 4 * 1024 * 1024;
                uint8_t* copy_buf = NULL;
                if (gSystemTable && gSystemTable->BootServices &&
                    gSystemTable->BootServices->AllocatePool(EfiLoaderData, max_copy, (void**)&copy_buf) == EFI_SUCCESS && copy_buf) {
                    uint32_t csz = 0;
                    char src_rel[192];
                    s_copy(src_rel, "DOWNLOADS\\", sizeof(src_rel));
                    s_cat(src_rel, de->filename, sizeof(src_rel));
                    if (fs_read_file_bytes(src_rel, copy_buf, max_copy, &csz) && csz > 0) {
                        char dst_rel[128];
                        s_copy(dst_rel, "EFI\\APPS\\", sizeof(dst_rel));
                        s_cat(dst_rel, de->filename, sizeof(dst_rel));
                        if (fs_write_file_bytes("EFI\\APPS", dst_rel, copy_buf, csz)) {
                            play_system_sound((char*)"success");
                            set_status("Installed downloaded package to \\EFI\\APPS\\", THEME_ACCENT_GREEN);
                            scan_all_packages();
                        } else {
                            play_system_sound((char*)"error");
                            set_status("Could not write to \\EFI\\APPS\\", THEME_ACCENT_ROSE);
                        }
                    } else {
                        set_status("Downloaded file empty or unreadable", THEME_ACCENT_AMBER);
                    }
                    gSystemTable->BootServices->FreePool(copy_buf);
                } else {
                    set_status("Out of UEFI pool memory during install", THEME_ACCENT_ROSE);
                }
            }
            if (dui_button(px + pw - 84, ry + 7, 66, 28, "Open", THEME_SURFACE_ELEVATED, false,
                           in->mx, in->my, in->left_pressed)) {
                wm_request_open_file(de->disk_path);
            }
        }
    }
}

static void store_on_frame(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    if (!s_scanned) scan_all_packages();

    int pad = 16;
    int px = cx + pad;
    int pw = cw - pad * 2;
    int top_y = cy + 12;

    // Header Row: Filter Tabs + Rescan Button
    const char* tabs[3] = { "All Packages", "Installed", "Downloads" };
    const int ticons[3] = { 17, 11, 8 };
    int tx = px;
    for (int t = 0; t < 3; t++) {
        int tw = 128;
        if (dui_icon_pill(tx, top_y, tw, 32, tabs[t], ticons[t], s_active_tab == t,
                          in->mx, in->my, in->left_pressed)) {
            s_active_tab = t;
            s_scroll_idx = 0;
            play_system_sound((char*)"click");
        }
        tx += tw + 8;
    }

    if (dui_icon_pill(px + pw - 122, top_y, 122, 32, "Rescan Disk", 16, false,
                      in->mx, in->my, in->left_pressed)) {
        scan_all_packages();
        play_system_sound((char*)"click");
        set_status("Scanned \\EFI\\APPS\\ and \\APPS\\ on UEFI volumes", THEME_ACCENT_CYAN);
    }

    // Status strip at bottom
    int foot_h = 26;
    int body_y = top_y + 44;
    int body_h = ch - (body_y - cy) - foot_h - 8;
    if (body_h < 100) body_h = 100;

    if (s_active_tab == 2) {
        render_downloads_tab(px, body_y, pw, body_h, in);
    } else {
        // Filter matching packages
        int vis_indices[MAX_STORE_PKGS];
        int vis_count = 0;
        for (int i = 0; i < s_pkg_count; i++) {
            if (s_active_tab == 1 && !s_pkgs[i].installed) continue;
            vis_indices[vis_count++] = i;
        }

        // Compact 2-column grid of Spatial Glass package cards
        int card_h = 112;
        int gap = 12;
        int cols = (pw >= 520) ? 2 : 1;
        int card_w = (cols == 2) ? (pw - gap) / 2 : pw;
        int rows_vis = (body_h - 32) / (card_h + gap);
        if (rows_vis < 1) rows_vis = 1;
        int total_rows = (vis_count + cols - 1) / cols;
        int max_scroll = total_rows - rows_vis;
        if (max_scroll < 0) max_scroll = 0;

        if (pt_in(in->mx, in->my, px, body_y, pw, body_h) && in->scroll_y != 0) {
            s_scroll_idx -= in->scroll_y;
        }
        if (s_scroll_idx < 0) s_scroll_idx = 0;
        if (s_scroll_idx > max_scroll) s_scroll_idx = max_scroll;

        if (vis_count == 0) {
            dui_glass_card(px, body_y, pw, 120, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
            dui_text(px + 20, body_y + 32, "No installed .PKG or .ELF packages found on disk.", THEME_TEXT_SECONDARY, 1);
            dui_text(px + 20, body_y + 56, "Switch to 'All Packages' to install or 'Downloads' to fetch via HTTP.", THEME_TEXT_MUTED, 1);
        } else {
            int start_idx = s_scroll_idx * cols;
            int max_draw = rows_vis * cols;
            for (int v = 0; v < max_draw && (start_idx + v) < vis_count; v++) {
                StorePkg* p = &s_pkgs[vis_indices[start_idx + v]];
                int r = v / cols;
                int c = v % cols;
                int kx = px + c * (card_w + gap);
                int ky = body_y + r * (card_h + gap);

                dui_glass_card(kx, ky, card_w, card_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);

                // App badge
                dui_rect_rounded(kx + 12, ky + 14, 42, 42, 10, p->accent, 235);
                if (p->mapped_app != APP_COUNT) {
                    dui_draw_squircle_icon(kx + 15, ky + 17, 36, (int)p->mapped_app, false);
                } else {
                    dui_draw_sys_icon(kx + 22, ky + 24, 22, p->is_elf ? 7 : 8, 0xFFFFFF);
                }

                // Title, badge & version
                dui_text_ellipsis(kx + 64, ky + 14, card_w - 156, p->name, THEME_TEXT_PRIMARY, 1);
                char meta[48];
                s_copy(meta, p->category, sizeof(meta));
                s_cat(meta, " | ", sizeof(meta));
                s_cat(meta, p->version, sizeof(meta));
                if (p->installed && p->disk_bytes > 0) {
                    char bstr[12];
                    u64_to_str(p->disk_bytes, bstr, sizeof(bstr));
                    s_cat(meta, " (", sizeof(meta));
                    s_cat(meta, bstr, sizeof(meta));
                    s_cat(meta, "B)", sizeof(meta));
                }
                dui_text_ellipsis(kx + 64, ky + 34, card_w - 156, meta, THEME_TEXT_SECONDARY, 1);
                dui_text_ellipsis(kx + 12, ky + 68, card_w - 24, p->desc, THEME_TEXT_MUTED, 1);
                dui_text_ellipsis(kx + 12, ky + 88, card_w - 160, p->disk_path, THEME_ACCENT_TEAL, 1);

                int bx = kx + card_w - 86;
                if (p->installed) {
                    if (dui_button(bx, ky + 14, 74, 28, "Launch", THEME_ACCENT_TEAL, true,
                                   in->mx, in->my, in->left_pressed)) {
                        launch_pkg_entry(p);
                    }
                    if (dui_button(bx, ky + 48, 74, 24, "Remove", THEME_SURFACE_ELEVATED, false,
                                   in->mx, in->my, in->left_pressed)) {
                        if (fs_delete_file(p->disk_path)) {
                            p->installed = false;
                            p->disk_bytes = 0;
                            play_system_sound((char*)"click");
                            set_status("Removed package file from disk", THEME_TEXT_SECONDARY);
                            if (!p->is_builtin_repo) scan_all_packages();
                        } else {
                            play_system_sound((char*)"error");
                            set_status("Cannot delete: read-only media or locked file", THEME_ACCENT_ROSE);
                        }
                    }
                } else {
                    if (dui_button(bx, ky + 14, 74, 28, "Install", THEME_ACCENT_IRIS, true,
                                   in->mx, in->my, in->left_pressed)) {
                        install_pkg_entry(p);
                    }
                }
            }

            // Scroll controls if needed
            if (max_scroll > 0) {
                int sy = body_y + body_h - 28;
                if (dui_button(px, sy, 84, 24, "Scroll Up", THEME_SURFACE_ELEVATED, false,
                               in->mx, in->my, in->left_pressed) && s_scroll_idx > 0) {
                    s_scroll_idx--;
                }
                if (dui_button(px + 92, sy, 94, 24, "Scroll Dn", THEME_SURFACE_ELEVATED, false,
                               in->mx, in->my, in->left_pressed) && s_scroll_idx < max_scroll) {
                    s_scroll_idx++;
                }
            }
        }
    }

    // Bottom status bar
    dui_line(cx + 8, cy + ch - foot_h - 4, cx + cw - 8, cy + ch - foot_h - 4, THEME_BORDER_SUBTLE, 180);
    dui_text_ellipsis(px, cy + ch - foot_h + 2, pw - 120, s_status_msg, s_status_color, 1);

    int inst_cnt = 0;
    for (int i = 0; i < s_pkg_count; i++) if (s_pkgs[i].installed) inst_cnt++;
    char cbuf[32], n1[12], n2[12];
    u64_to_str((uint64_t)inst_cnt, n1, sizeof(n1));
    u64_to_str((uint64_t)s_pkg_count, n2, sizeof(n2));
    s_copy(cbuf, n1, sizeof(cbuf));
    s_cat(cbuf, "/", sizeof(cbuf));
    s_cat(cbuf, n2, sizeof(cbuf));
    s_cat(cbuf, " Active", sizeof(cbuf));
    dui_text(px + pw - 96, cy + ch - foot_h + 2, cbuf, THEME_TEXT_MUTED, 1);
}

static void store_on_close(BlankWindow* win) {
    (void)win;
    s_url_focused = false;
}

} // namespace

extern "C" {

void store_register(void) {
    if (s_registered) return;
    s_registered = true;
    wm_register_app(APP_STORE, "Software Hub", 780, 540, THEME_ACCENT_TEAL,
                    store_on_frame, store_on_close);
}

void launch_app_store(EFI_SYSTEM_TABLE* st) {
    if (st && !gSystemTable) gSystemTable = st;
    store_register();
    wm_open_app(APP_STORE);
}

} // extern "C"
