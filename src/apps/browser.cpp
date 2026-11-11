// BlankOS Horizon Web Browser
// Non-blocking multi-tab HTTP/1.1 browser in Horizon Spatial Glass
// Backed by real UEFI SNP/ARP/IPv4/UDP/TCP/DHCP/DNS/HTTP stack & FAT32 storage

#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../ui/theme.h"
#include "../ui/blankDUI.h"
#include "../ui/window_manager.h"
#include "../kernel/input.h"
#include "../kernel/net.h"
#include "../kernel/audio.h"
#include "../kernel/js_engine.h"

extern "C" {
    extern EFI_SYSTEM_TABLE* gSystemTable;
    extern void* kmalloc(uint64_t size);
    extern void kfree(void* ptr);
    extern uint64_t memory_get_used_mb(void);
    extern uint64_t memory_get_total_mb(void);
    extern int pci_get_device_count(void);
    extern bool pci_get_device_info(
        int index,
        uint8_t* bus,
        uint8_t* dev,
        uint8_t* func,
        uint16_t* vendor,
        uint16_t* device,
        uint8_t* class_code,
        uint8_t* subclass,
        uint32_t* bar0,
        const char** vendor_str,
        const char** dev_str,
        const char** drv_str
    );
    extern const char* pci_get_cpu_brand(void);
    extern const char* pci_get_cpu_vendor(void);
    extern void os_send_notification(const char* title, const char* message);
}

// ============================================================================
// Constants & Limits
// ============================================================================

#define MAX_TABS          6
#define MAX_HIST          12
#define MAX_ACTIVE_DL_TID 8
#define MAX_DISK_DL_FILES 24
#define MAX_PING_SAMPLES  10

#define MAX_HTML_SPANS    2048
#define TEXT_POOL_CAP     65536
#define HREF_POOL_CAP     16384
#define RAW_BODY_MAX      131072

#define TAB_BAR_H         34
#define TOOLBAR_H         42
#define BOOKMARKS_H       28
#define SIDEBAR_W         186

#define SPAN_BOLD         0x01
#define SPAN_LINK         0x02
#define SPAN_CODE         0x04
#define SPAN_PRE          0x08
#define SPAN_HR           0x10
#define SPAN_BULLET       0x20
#define SPAN_QUOTE        0x40
#define SPAN_IMG_CHIP     0x80
#define SPAN_BTN          0x100
#define SPAN_INPUT        0x200

typedef enum {
    PAGE_START = 0,
    PAGE_PULSE = 1,
    PAGE_DOWNLOADS = 2,
    PAGE_HTTP = 3,
    PAGE_HTTPS_NOTICE = 4,
    PAGE_ERROR = 5
} BrowserPageMode;

typedef struct {
    int x, y, w, h;
    int text_off;
    int href_off;
    int dom_id;
    uint8_t scale;
    uint16_t flags;
    uint32_t color;
} HtmlSpan;

typedef struct {
    bool active;
    char url[512];
    char title[64];
    BrowserPageMode page_mode;

    char history[MAX_HIST][512];
    int hist_count;
    int hist_pos;

    int transfer_id;
    HttpState last_net_state;
    int status_code;
    char content_type[64];
    int bytes_received;
    int content_length;
    int speed_bps;
    char error_msg[128];

    char* raw_body;
    int raw_len;
    bool view_source;

    HtmlSpan* spans;
    int span_count;
    char* text_pool;
    int text_pool_used;
    char* href_pool;
    int href_pool_used;
    int layout_width;
    bool layout_dirty;
    bool layout_source_mode;

    JsRuntime* js_rt;
    bool js_scripts_ran;
    int initial_dom_count;
    int active_input_dom_id;

    int scroll_y;
    int content_height;
} BrowserTab;

typedef struct {
    char name[96];
    char disk_path[192];
    uint64_t size;
} DiskDownloadFile;

typedef struct {
    char target[48];
    int rtt_ms; // -1 = timeout
} PingSample;

// ============================================================================
// Global Browser State (Single Managed Window Instance)
// ============================================================================

static BrowserTab g_tabs[MAX_TABS];
static int g_tab_count = 0;
static int g_active_tab = 0;

static char g_omni_buf[512] = {0};
static int g_omni_len = 0;
static bool g_omni_focused = false;
static bool g_omni_sel_all = false;

static bool g_sidebar_open = true;
static bool g_dl_popover_open = false;
static bool g_scrollbar_drag = false;
static int g_scrollbar_grab_off = 0;

static bool g_console_open = false;
static char g_console_input[192] = {0};
static int g_console_input_len = 0;
static bool g_console_input_focused = false;
static char g_js_alert_banner[192] = {0};
static uint32_t g_js_alert_ticks = 0;

static bool g_ctx_open = false;
static int g_ctx_x = 0;
static int g_ctx_y = 0;
static char g_ctx_href[512] = {0};

static int g_active_dl_tids[MAX_ACTIVE_DL_TID] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static DiskDownloadFile g_disk_files[MAX_DISK_DL_FILES];
static int g_disk_file_count = 0;
static bool g_disk_scan_needed = true;

static PingSample g_ping_samples[MAX_PING_SAMPLES];
static int g_ping_count = 0;
static char g_last_ping_status[64] = "Idle (click a probe target)";

// ============================================================================
// Freestanding String, Number, and URL Helpers (Zero stdlib)
// ============================================================================

static int str_len(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void str_append(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int pos = str_len(dst);
    int i = 0;
    while (src[i] && pos < max_len - 1) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

static char to_lower_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static bool is_space_ch(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static bool str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static bool str_ieq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (to_lower_ch(a[i]) != to_lower_ch(b[i])) return false;
        i++;
    }
    return to_lower_ch(a[i]) == to_lower_ch(b[i]);
}

static bool str_starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return false;
        i++;
    }
    return true;
}

static bool str_istarts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    int i = 0;
    while (prefix[i]) {
        if (!s[i]) return false;
        if (to_lower_ch(s[i]) != to_lower_ch(prefix[i])) return false;
        i++;
    }
    return true;
}

static bool str_icontains(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j]) {
            if (to_lower_ch(hay[i + j]) != to_lower_ch(needle[j])) break;
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static void int_to_str(int64_t val, char* out, int max_len) {
    if (!out || max_len <= 0) return;
    if (val == 0) {
        str_copy(out, "0", max_len);
        return;
    }
    char tmp[32];
    int p = 0;
    bool neg = false;
    uint64_t uval;
    if (val < 0) {
        neg = true;
        uval = (uint64_t)(-val);
    } else {
        uval = (uint64_t)val;
    }
    while (uval > 0 && p < 30) {
        tmp[p++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    int o = 0;
    if (neg && o < max_len - 1) out[o++] = '-';
    while (p > 0 && o < max_len - 1) {
        out[o++] = tmp[--p];
    }
    out[o] = '\0';
}

static void u64_to_str(uint64_t val, char* out, int max_len) {
    if (!out || max_len <= 0) return;
    if (val == 0) {
        str_copy(out, "0", max_len);
        return;
    }
    char tmp[32];
    int p = 0;
    while (val > 0 && p < 30) {
        tmp[p++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int o = 0;
    while (p > 0 && o < max_len - 1) {
        out[o++] = tmp[--p];
    }
    out[o] = '\0';
}

static void u16_to_hex4(uint16_t val, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    out[2] = hex[(val >> 12) & 0xF];
    out[3] = hex[(val >> 8) & 0xF];
    out[4] = hex[(val >> 4) & 0xF];
    out[5] = hex[val & 0xF];
    out[6] = '\0';
}

static void format_bytes(uint64_t bytes, char* out, int max_len) {
    char num[24];
    if (bytes < 1024ULL) {
        u64_to_str(bytes, num, 24);
        str_copy(out, num, max_len);
        str_append(out, " B", max_len);
    } else if (bytes < 1024ULL * 1024ULL) {
        uint64_t kb10 = (bytes * 10ULL) / 1024ULL;
        u64_to_str(kb10 / 10ULL, num, 24);
        str_copy(out, num, max_len);
        if (kb10 < 1000ULL) {
            char frac[4] = { '.', (char)('0' + (kb10 % 10ULL)), '\0', '\0' };
            str_append(out, frac, max_len);
        }
        str_append(out, " KB", max_len);
    } else {
        uint64_t mb10 = (bytes * 10ULL) / (1024ULL * 1024ULL);
        u64_to_str(mb10 / 10ULL, num, 24);
        str_copy(out, num, max_len);
        char frac[4] = { '.', (char)('0' + (mb10 % 10ULL)), '\0', '\0' };
        str_append(out, frac, max_len);
        str_append(out, " MB", max_len);
    }
}

static bool pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

static void mem_zero(void* dst, int bytes) {
    uint8_t* p = (uint8_t*)dst;
    for (int i = 0; i < bytes; i++) p[i] = 0;
}

static void mem_copy(void* dst, const void* src, int bytes) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < bytes; i++) d[i] = s[i];
}

// ============================================================================
// URL Normalization, Filename Derivation, & Relative Resolution
// ============================================================================

static void normalize_input_url(const char* raw, char* out, int max_len) {
    if (!raw || !out || max_len <= 0) return;
    int s = 0;
    while (raw[s] && is_space_ch(raw[s])) s++;
    int e = str_len(raw);
    while (e > s && is_space_ch(raw[e - 1])) e--;

    char trimmed[512];
    int ti = 0;
    while (s < e && ti < 511) trimmed[ti++] = raw[s++];
    trimmed[ti] = '\0';

    if (ti == 0 || str_ieq(trimmed, "start") || str_ieq(trimmed, "home")) {
        str_copy(out, "blank://start", max_len);
        return;
    }
    if (str_ieq(trimmed, "pulse") || str_ieq(trimmed, "net") || str_ieq(trimmed, "status")) {
        str_copy(out, "blank://pulse", max_len);
        return;
    }
    if (str_ieq(trimmed, "downloads") || str_ieq(trimmed, "dl")) {
        str_copy(out, "blank://downloads", max_len);
        return;
    }
    if (str_ieq(trimmed, "js") || str_ieq(trimmed, "js-demo") || str_ieq(trimmed, "sandbox")) {
        str_copy(out, "blank://js-demo", max_len);
        return;
    }

    if (str_istarts_with(trimmed, "http://") ||
        str_istarts_with(trimmed, "https://") ||
        str_istarts_with(trimmed, "blank://") ||
        str_istarts_with(trimmed, "javascript:")) {
        str_copy(out, trimmed, max_len);
        return;
    }

    str_copy(out, "http://", max_len);
    str_append(out, trimmed, max_len);
}

static void derive_filename_from_url(const char* url, char* out, int max_len) {
    if (!url || !out || max_len <= 0) return;
    const char* p = url;
    if (str_istarts_with(p, "http://")) p += 7;
    else if (str_istarts_with(p, "https://")) p += 8;

    // Skip host
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;

    const char* last_seg = "";
    if (*p == '/') {
        const char* cur = p + 1;
        last_seg = cur;
        while (*cur && *cur != '?' && *cur != '#') {
            if (*cur == '/') last_seg = cur + 1;
            cur++;
        }
    }

    char raw_name[96];
    int ri = 0;
    while (last_seg[ri] && last_seg[ri] != '?' && last_seg[ri] != '#' && ri < 90) {
        char c = last_seg[ri];
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\\' || c == '/') {
            c = '_';
        }
        raw_name[ri++] = c;
    }
    raw_name[ri] = '\0';

    if (ri == 0) {
        str_copy(out, "index.html", max_len);
    } else {
        str_copy(out, raw_name, max_len);
    }
}

static void resolve_url(const char* base_url, const char* rel, char* out, int max_len) {
    if (!rel || !out || max_len <= 0) return;
    while (*rel && is_space_ch(*rel)) rel++;

    if (str_istarts_with(rel, "http://") ||
        str_istarts_with(rel, "https://") ||
        str_istarts_with(rel, "blank://") ||
        str_istarts_with(rel, "javascript:")) {
        str_copy(out, rel, max_len);
        return;
    }

    // Extract scheme + authority from base_url
    char scheme[16] = "http://";
    const char* bp = base_url;
    if (str_istarts_with(bp, "https://")) {
        str_copy(scheme, "https://", 16);
        bp += 8;
    } else if (str_istarts_with(bp, "http://")) {
        bp += 7;
    } else {
        str_copy(out, rel, max_len);
        return;
    }

    if (str_starts_with(rel, "//")) {
        str_copy(out, "http:", max_len);
        str_append(out, rel, max_len);
        return;
    }

    char authority[160];
    int ai = 0;
    while (*bp && *bp != '/' && *bp != '?' && *bp != '#' && ai < 158) {
        authority[ai++] = *bp++;
    }
    authority[ai] = '\0';

    char base_path[384];
    int pi = 0;
    if (*bp == '/') {
        while (*bp && *bp != '?' && *bp != '#' && pi < 380) {
            base_path[pi++] = *bp++;
        }
    }
    if (pi == 0) base_path[pi++] = '/';
    base_path[pi] = '\0';

    if (rel[0] == '#') {
        str_copy(out, base_url, max_len);
        return;
    }

    if (rel[0] == '?') {
        str_copy(out, scheme, max_len);
        str_append(out, authority, max_len);
        str_append(out, base_path, max_len);
        str_append(out, rel, max_len);
        return;
    }

    if (rel[0] == '/') {
        str_copy(out, scheme, max_len);
        str_append(out, authority, max_len);
        str_append(out, rel, max_len);
        return;
    }

    // Relative path: strip filename from base_path to get directory
    int last_slash = 0;
    for (int k = 0; base_path[k]; k++) {
        if (base_path[k] == '/') last_slash = k;
    }
    base_path[last_slash + 1] = '\0';

    // Handle ./ and ../ prefixes cleanly
    const char* rp = rel;
    while (rp[0]) {
        if (rp[0] == '.' && rp[1] == '/') {
            rp += 2;
        } else if (rp[0] == '.' && rp[1] == '.' && rp[2] == '/') {
            rp += 3;
            int len = str_len(base_path);
            if (len > 1) {
                base_path[len - 1] = '\0'; // drop trailing '/'
                int ls = 0;
                for (int k = 0; base_path[k]; k++) {
                    if (base_path[k] == '/') ls = k;
                }
                base_path[ls + 1] = '\0';
            }
        } else {
            break;
        }
    }

    str_copy(out, scheme, max_len);
    str_append(out, authority, max_len);
    str_append(out, base_path, max_len);
    str_append(out, rp, max_len);
}

// ============================================================================
// Real UEFI FAT32 \DOWNLOADS\ Scanner
// ============================================================================

static void scan_fat32_downloads_dir(void) {
    g_disk_scan_needed = false;
    g_disk_file_count = 0;
    if (!gSystemTable || !gSystemTable->BootServices) return;

    EFI_GUID sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
    if (gSystemTable->BootServices->LocateProtocol(&sfsGuid, NULL, (void**)&Sfs) != EFI_SUCCESS || !Sfs) {
        return;
    }

    EFI_FILE_HANDLE Root = NULL;
    if (Sfs->OpenVolume(Sfs, &Root) != EFI_SUCCESS || !Root) return;

    EFI_FILE_HANDLE Dir = NULL;
    if (Root->Open(Root, &Dir, (CHAR16*)L"\\DOWNLOADS", EFI_FILE_MODE_READ, 0) != EFI_SUCCESS || !Dir) {
        Root->Close(Root);
        return;
    }

    Dir->SetPosition(Dir, 0);
    uint8_t info_buf[384];
    while (g_disk_file_count < MAX_DISK_DL_FILES) {
        UINTN sz = sizeof(info_buf);
        if (Dir->Read(Dir, &sz, info_buf) != EFI_SUCCESS || sz == 0) break;
        EFI_FILE_INFO* info = (EFI_FILE_INFO*)info_buf;
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;
        if (info->FileName[0] == L'.') continue;

        DiskDownloadFile* f = &g_disk_files[g_disk_file_count];
        int k = 0;
        while (info->FileName[k] && k < 94) {
            f->name[k] = (char)(info->FileName[k] & 0x7F);
            k++;
        }
        f->name[k] = '\0';
        str_copy(f->disk_path, "\\DOWNLOADS\\", 192);
        str_append(f->disk_path, f->name, 192);
        f->size = info->FileSize;
        g_disk_file_count++;
    }

    Dir->Close(Dir);
    Root->Close(Root);
}

// ============================================================================
// Tab Allocation, Cleanup & Navigation
// ============================================================================

static void free_tab_doc_buffers(BrowserTab* t) {
    if (!t) return;
    if (t->raw_body) { kfree(t->raw_body); t->raw_body = NULL; }
    if (t->spans) { kfree(t->spans); t->spans = NULL; }
    if (t->text_pool) { kfree(t->text_pool); t->text_pool = NULL; }
    if (t->href_pool) { kfree(t->href_pool); t->href_pool = NULL; }
    if (t->js_rt) { js_runtime_destroy(t->js_rt); t->js_rt = NULL; }
    t->js_scripts_ran = false;
    t->active_input_dom_id = -1;
    t->raw_len = 0;
    t->span_count = 0;
    t->text_pool_used = 0;
    t->href_pool_used = 0;
}

static bool ensure_tab_layout_buffers(BrowserTab* t) {
    if (!t->spans) t->spans = (HtmlSpan*)kmalloc(sizeof(HtmlSpan) * MAX_HTML_SPANS);
    if (!t->text_pool) t->text_pool = (char*)kmalloc(TEXT_POOL_CAP);
    if (!t->href_pool) t->href_pool = (char*)kmalloc(HREF_POOL_CAP);
    return (t->spans != NULL && t->text_pool != NULL && t->href_pool != NULL);
}

static void sync_omnibar_from_active_tab(void) {
    if (g_tab_count <= 0 || g_active_tab < 0 || g_active_tab >= g_tab_count) return;
    str_copy(g_omni_buf, g_tabs[g_active_tab].url, 512);
    g_omni_len = str_len(g_omni_buf);
}

static void start_file_download(const char* url, const char* custom_filename) {
    if (!url || !url[0]) return;
    if (str_istarts_with(url, "blank://")) return;

    char fname[128];
    if (custom_filename && custom_filename[0]) {
        str_copy(fname, custom_filename, 128);
    } else {
        derive_filename_from_url(url, fname, 128);
    }

    int tid = net_http_start(url, fname);
    if (tid >= 0) {
        for (int i = 0; i < MAX_ACTIVE_DL_TID; i++) {
            if (g_active_dl_tids[i] < 0) {
                g_active_dl_tids[i] = tid;
                break;
            }
        }
        g_dl_popover_open = true;
        play_system_sound((char*)"click");
    } else {
        os_send_notification("Download Busy", "Max concurrent HTTP transfers reached.");
        play_system_sound((char*)"error");
    }
}

static void cancel_tab_transfer(BrowserTab* t) {
    if (!t) return;
    if (t->transfer_id >= 0) {
        net_http_close(t->transfer_id);
        t->transfer_id = -1;
    }
    t->last_net_state = HTTP_IDLE;
}

static void js_handle_outputs(BrowserTab* t) {
    if (!t || !t->js_rt) return;
    char abuf[192];
    if (js_take_alert(t->js_rt, abuf, sizeof(abuf))) {
        str_copy(g_js_alert_banner, abuf, sizeof(g_js_alert_banner));
        g_js_alert_ticks = 220;
    }
    char nurl[256];
    if (js_take_navigate(t->js_rt, nurl, sizeof(nurl))) {
        navigate_tab(g_active_tab, nurl, true);
    }
}

static bool hydrate_is_tracked_tag(const char* tname) {
    return str_eq(tname, "button") || str_eq(tname, "input") ||
           str_eq(tname, "h1") || str_eq(tname, "h2") || str_eq(tname, "h3") || str_eq(tname, "h4") ||
           str_eq(tname, "p") || str_eq(tname, "div") || str_eq(tname, "span") ||
           str_eq(tname, "li") || str_eq(tname, "a") || str_eq(tname, "ul") || str_eq(tname, "ol");
}

static void hydrate_tab_dom_and_scripts(BrowserTab* t) {
    if (!t || t->js_scripts_ran || !t->raw_body || t->raw_len <= 0) return;
    t->js_scripts_ran = true;

    if (!t->js_rt) t->js_rt = js_runtime_create();
    if (!t->js_rt) return;
    js_runtime_reset(t->js_rt, t->url, t->title);

    int parent_stack[16];
    int sp = 0;
    parent_stack[sp++] = js_dom_get_body_id(t->js_rt);

    char* script_buf = (char*)kmalloc(32768);
    int script_len = 0;
    bool in_script = false;

    const char* html = t->raw_body;
    int len = t->raw_len;
    int i = 0;

    while (i < len) {
        if (html[i] == '<') {
            if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')) i++;
                i += 3;
                continue;
            }
            int tag_start = i + 1;
            int j = tag_start;
            char q = 0;
            while (j < len && (q || html[j] != '>')) {
                if ((html[j] == '"' || html[j] == '\'') && !q) q = html[j];
                else if (q && html[j] == q) q = 0;
                j++;
            }
            if (j >= len) break;

            int tag_len = j - tag_start;
            char tag_buf[512];
            int cp = (tag_len < 511) ? tag_len : 511;
            for (int k = 0; k < cp; k++) tag_buf[k] = html[tag_start + k];
            tag_buf[cp] = '\0';

            int tp = 0;
            while (tag_buf[tp] && is_space_ch(tag_buf[tp])) tp++;
            bool is_close = false;
            if (tag_buf[tp] == '/') { is_close = true; tp++; }
            char tname[32]; int ni = 0;
            while (tag_buf[tp] && !is_space_ch(tag_buf[tp]) && tag_buf[tp] != '/' && ni < 30) {
                tname[ni++] = to_lower_ch(tag_buf[tp++]);
            }
            tname[ni] = '\0';

            if (in_script) {
                if (is_close && str_eq(tname, "script")) {
                    in_script = false;
                    if (script_buf && script_len > 0) {
                        script_buf[script_len] = '\0';
                        js_eval(t->js_rt, script_buf, NULL, 0);
                        js_handle_outputs(t);
                    }
                    script_len = 0;
                    i = j + 1;
                    continue;
                }
                if (script_buf && script_len + (j - i + 1) < 32760) {
                    for (int k = i; k <= j; k++) script_buf[script_len++] = html[k];
                }
                i = j + 1;
                continue;
            }

            i = j + 1;
            if (str_eq(tname, "script") && !is_close) {
                in_script = true;
                script_len = 0;
                continue;
            }
            if (is_close) {
                if (hydrate_is_tracked_tag(tname) && sp > 1) sp--;
                continue;
            }
            if (hydrate_is_tracked_tag(tname)) {
                char id_val[48] = {0}, cls_val[64] = {0}, onclk[256] = {0}, val_attr[256] = {0}, href_val[192] = {0};
                extract_attr(tag_buf + tp, "id", id_val, sizeof(id_val));
                extract_attr(tag_buf + tp, "class", cls_val, sizeof(cls_val));
                extract_attr(tag_buf + tp, "onclick", onclk, sizeof(onclk));
                extract_attr(tag_buf + tp, "value", val_attr, sizeof(val_attr));
                extract_attr(tag_buf + tp, "href", href_val, sizeof(href_val));

                if (id_val[0] || str_eq(tname, "button") || str_eq(tname, "input") || onclk[0] ||
                    str_eq(tname, "ul") || str_eq(tname, "ol") || str_eq(tname, "li") || str_eq(tname, "p") ||
                    str_eq(tname, "h1") || str_eq(tname, "h2") || str_eq(tname, "h3")) {
                    int nid = js_dom_create_element(t->js_rt, tname, parent_stack[sp - 1]);
                    if (nid >= 0) {
                        JsDomNode* dn = js_dom_get_node(t->js_rt, nid);
                        if (dn) {
                            str_copy(dn->elem_id, id_val, sizeof(dn->elem_id));
                            str_copy(dn->class_name, cls_val, sizeof(dn->class_name));
                            str_copy(dn->onclick_src, onclk, sizeof(dn->onclick_src));
                            str_copy(dn->href, href_val, sizeof(dn->href));
                            if (val_attr[0]) str_copy(dn->text, val_attr, sizeof(dn->text));
                        }
                        if (!str_eq(tname, "input") && cp > 0 && tag_buf[cp - 1] != '/' && sp < 15) {
                            parent_stack[sp++] = nid;
                        }
                    }
                }
            }
            continue;
        }

        if (in_script) {
            if (script_buf && script_len < 32766) script_buf[script_len++] = html[i];
            i++;
            continue;
        }

        // Capture plain text into innermost tracked element (collapsed whitespace)
        if (sp > 1) {
            JsDomNode* cur_el = js_dom_get_node(t->js_rt, parent_stack[sp - 1]);
            if (cur_el) {
                int cur_l = str_len(cur_el->text);
                char ch = html[i];
                if (is_space_ch(ch)) {
                    if (cur_l > 0 && cur_el->text[cur_l - 1] != ' ' && cur_l < 508) {
                        cur_el->text[cur_l++] = ' ';
                        cur_el->text[cur_l] = '\0';
                    }
                } else if (ch >= 32 && ch <= 126 && cur_l < 508) {
                    cur_el->text[cur_l++] = ch;
                    cur_el->text[cur_l] = '\0';
                }
            }
        }
        i++;
    }

    if (script_buf) kfree(script_buf);

    // Handle document.write(...) output appended directly into raw_body
    char* wbuf = (char*)kmalloc(2048);
    if (wbuf && js_take_doc_write(t->js_rt, wbuf, 2048)) {
        str_append(t->raw_body, "<hr/><p>", RAW_BODY_MAX);
        str_append(t->raw_body, wbuf, RAW_BODY_MAX);
        str_append(t->raw_body, "</p>", RAW_BODY_MAX);
        t->raw_len = str_len(t->raw_body);
    }
    if (wbuf) kfree(wbuf);

    const char* js_t = js_dom_get_title(t->js_rt);
    if (js_t && js_t[0]) str_copy(t->title, js_t, 64);

    t->initial_dom_count = js_dom_get_node_count(t->js_rt);
    js_dom_clear_dirty(t->js_rt);
}

static void navigate_tab(int idx, const char* raw_url, bool push_history) {
    if (idx < 0 || idx >= g_tab_count || !raw_url) return;
    BrowserTab* t = &g_tabs[idx];

    char url[512];
    normalize_input_url(raw_url, url, 512);

    // Execute javascript:... URL directly against current tab DOM without wiping page
    if (str_istarts_with(url, "javascript:")) {
        if (!t->js_rt) t->js_rt = js_runtime_create();
        if (t->js_rt) {
            js_eval(t->js_rt, url + 11, NULL, 0);
            js_handle_outputs(t);
            if (js_dom_is_dirty(t->js_rt)) {
                js_dom_clear_dirty(t->js_rt);
                t->layout_dirty = true;
            }
        }
        if (idx == g_active_tab) sync_omnibar_from_active_tab();
        return;
    }

    cancel_tab_transfer(t);
    str_copy(t->url, url, 512);
    t->scroll_y = 0;
    t->content_height = 0;
    t->status_code = 0;
    t->bytes_received = 0;
    t->content_length = -1;
    t->speed_bps = 0;
    t->error_msg[0] = '\0';
    t->content_type[0] = '\0';
    t->view_source = false;
    t->raw_len = 0;
    if (t->raw_body) t->raw_body[0] = '\0';
    t->span_count = 0;
    t->layout_dirty = true;
    t->js_scripts_ran = false;
    t->initial_dom_count = 0;
    t->active_input_dom_id = -1;

    if (str_ieq(url, "blank://start") || str_ieq(url, "blank://home")) {
        str_copy(t->url, "blank://start", 512);
        str_copy(t->title, "Start Page", 64);
        t->page_mode = PAGE_START;
    } else if (str_ieq(url, "blank://pulse")) {
        str_copy(t->url, "blank://pulse", 512);
        str_copy(t->title, "Network Pulse", 64);
        t->page_mode = PAGE_PULSE;
    } else if (str_ieq(url, "blank://downloads")) {
        str_copy(t->url, "blank://downloads", 512);
        str_copy(t->title, "Downloads", 64);
        t->page_mode = PAGE_DOWNLOADS;
        g_disk_scan_needed = true;
    } else if (str_ieq(url, "blank://js-demo")) {
        static const char* js_demo_html =
            "<!doctype html><html><head><title>BlankJS Live Sandbox</title></head><body>"
            "<h1>BlankJS Interactive Sandbox</h1>"
            "<p id=\"status\">Live ECMAScript + DOM engine running natively in BlankOS Horizon.</p>"
            "<p id=\"counter\">Button clicks: 0</p>"
            "<p id=\"live-clock\">Timer tick: waiting...</p>"
            "<button id=\"btn-inc\" onclick=\"incCounter()\">+1 Click Counter</button>"
            "<button id=\"btn-accent\" onclick=\"cycleAccent()\">Cycle Accent Color</button>"
            "<button id=\"btn-add\" onclick=\"addLogItem()\">Append DOM Item</button>"
            "<br/><input id=\"note-box\" value=\"Hello BlankJS!\" />"
            "<button id=\"btn-say\" onclick=\"submitNote()\">Add Note to List</button>"
            "<hr/><h3>Dynamic DOM List</h3>"
            "<ul id=\"dyn-list\"><li>Initial static HTML node</li></ul>"
            "<script>"
            "let count = 0; let ticks = 0; let accentIdx = 0;"
            "const palette = ['#38BDF8', '#2DD4BF', '#34D399', '#818CF8', '#FBBF24'];"
            "console.log('BlankJS Sandbox booted! Open Console (JS) to inspect.');"
            "function incCounter() { count++; document.getElementById('counter').textContent = `Button clicks: ${count} (doubled = ${count * 2})`; playSound('tool'); }"
            "function cycleAccent() { accentIdx = (accentIdx + 1) % palette.length; const col = palette[accentIdx]; const st = document.getElementById('status'); st.style.color = col; st.textContent = `Live style.color updated to ${col}`; playSound('send'); }"
            "function addLogItem() { const ul = document.getElementById('dyn-list'); const li = document.createElement('li'); li.textContent = `Dynamic DOM node #${count + 1} via createElement()`; ul.appendChild(li); playSound('reply'); }"
            "function submitNote() { const inp = document.getElementById('note-box'); const ul = document.getElementById('dyn-list'); const li = document.createElement('li'); li.textContent = `Note: ${inp.value}`; ul.appendChild(li); console.log('Note added:', inp.value); playSound('success'); }"
            "setInterval(() => { ticks++; const clk = document.getElementById('live-clock'); if (clk) clk.textContent = `Live setInterval tick #${ticks}`; }, 1000);"
            "</script></body></html>";
        str_copy(t->url, "blank://js-demo", 512);
        str_copy(t->title, "BlankJS Sandbox", 64);
        t->page_mode = PAGE_HTTP;
        if (!t->raw_body) t->raw_body = (char*)kmalloc(RAW_BODY_MAX);
        if (t->raw_body) {
            str_copy(t->raw_body, js_demo_html, RAW_BODY_MAX);
            t->raw_len = str_len(t->raw_body);
            hydrate_tab_dom_and_scripts(t);
        }
    } else if (str_istarts_with(url, "https://")) {
        str_copy(t->title, "TLS Not Loaded", 64);
        t->page_mode = PAGE_HTTPS_NOTICE;
    } else {
        // Real non-blocking HTTP/1.1 transfer via net.h
        t->page_mode = PAGE_HTTP;
        const char* host_lbl = url + 7; // after "http://"
        int hl = 0;
        char short_host[40];
        while (host_lbl[hl] && host_lbl[hl] != '/' && hl < 38) {
            short_host[hl] = host_lbl[hl];
            hl++;
        }
        short_host[hl] = '\0';
        str_copy(t->title, hl > 0 ? short_host : "Loading...", 64);

        t->transfer_id = net_http_start(url, NULL);
        if (t->transfer_id < 0) {
            t->page_mode = PAGE_ERROR;
            str_copy(t->title, "Connection Busy", 64);
            str_copy(t->error_msg, "All HTTP transfer slots are busy. Wait or close a transfer.", 128);
        } else {
            const HttpTransfer* ht = net_http_get(t->transfer_id);
            if (ht) {
                t->last_net_state = ht->state;
                if (ht->state == HTTP_ERROR) {
                    if (ht->is_https) {
                        t->page_mode = PAGE_HTTPS_NOTICE;
                        str_copy(t->title, "TLS Required", 64);
                    } else {
                        t->page_mode = PAGE_ERROR;
                        str_copy(t->title, "Network Error", 64);
                        str_copy(t->error_msg, ht->error_msg, 128);
                    }
                    net_http_close(t->transfer_id);
                    t->transfer_id = -1;
                }
            }
        }
    }

    if (push_history) {
        if (t->hist_count == 0 || !str_eq(t->history[t->hist_pos], t->url)) {
            if (t->hist_pos < MAX_HIST - 1) {
                t->hist_pos++;
                t->hist_count = t->hist_pos + 1;
                str_copy(t->history[t->hist_pos], t->url, 512);
            } else {
                for (int k = 0; k < MAX_HIST - 1; k++) {
                    str_copy(t->history[k], t->history[k + 1], 512);
                }
                str_copy(t->history[MAX_HIST - 1], t->url, 512);
                t->hist_pos = MAX_HIST - 1;
                t->hist_count = MAX_HIST;
            }
        }
    }

    if (idx == g_active_tab) {
        sync_omnibar_from_active_tab();
        g_omni_focused = false;
        g_omni_sel_all = false;
    }
}

static int create_new_tab(const char* url, bool switch_to) {
    if (g_tab_count >= MAX_TABS) return -1;
    int idx = g_tab_count++;
    mem_zero(&g_tabs[idx], sizeof(BrowserTab));
    g_tabs[idx].active = true;
    g_tabs[idx].transfer_id = -1;
    g_tabs[idx].content_length = -1;
    g_tabs[idx].hist_count = 0;
    g_tabs[idx].hist_pos = -1;
    navigate_tab(idx, url ? url : "blank://start", true);
    if (switch_to) {
        g_active_tab = idx;
        sync_omnibar_from_active_tab();
    }
    return idx;
}

static void close_tab_at(int idx) {
    if (idx < 0 || idx >= g_tab_count) return;
    cancel_tab_transfer(&g_tabs[idx]);
    free_tab_doc_buffers(&g_tabs[idx]);

    if (g_tab_count == 1) {
        mem_zero(&g_tabs[0], sizeof(BrowserTab));
        g_tabs[0].active = true;
        g_tabs[0].transfer_id = -1;
        g_tabs[0].content_length = -1;
        g_tabs[0].hist_count = 0;
        g_tabs[0].hist_pos = -1;
        g_active_tab = 0;
        navigate_tab(0, "blank://start", true);
        return;
    }

    for (int i = idx; i < g_tab_count - 1; i++) {
        g_tabs[i] = g_tabs[i + 1];
    }
    mem_zero(&g_tabs[g_tab_count - 1], sizeof(BrowserTab));
    g_tabs[g_tab_count - 1].transfer_id = -1;
    g_tab_count--;

    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
    else if (g_active_tab > idx) g_active_tab--;
    sync_omnibar_from_active_tab();
}

static void ensure_tabs_initialized(void) {
    if (g_tab_count == 0) {
        create_new_tab("blank://start", true);
    }
}

// ============================================================================
// HTML Entity Decoder, Title Extractor, & Flow Layout Engine
// ============================================================================

static char decode_html_entity(const char* s, int* consumed) {
    *consumed = 1;
    if (!s || s[0] != '&') return s ? s[0] : '\0';

    int end = 1;
    while (end < 12 && s[end] && s[end] != ';' && !is_space_ch(s[end]) && s[end] != '<') {
        end++;
    }
    if (s[end] != ';') return '&';

    char ent[16];
    int len = end - 1;
    if (len <= 0 || len >= 15) return '&';
    for (int i = 0; i < len; i++) ent[i] = s[1 + i];
    ent[len] = '\0';
    *consumed = end + 1;

    if (str_ieq(ent, "amp")) return '&';
    if (str_ieq(ent, "lt")) return '<';
    if (str_ieq(ent, "gt")) return '>';
    if (str_ieq(ent, "quot")) return '"';
    if (str_ieq(ent, "apos")) return '\'';
    if (str_ieq(ent, "nbsp")) return ' ';
    if (str_ieq(ent, "mdash") || str_ieq(ent, "ndash")) return '-';
    if (str_ieq(ent, "bull") || str_ieq(ent, "middot")) return '*';
    if (str_ieq(ent, "copy")) return 'C';
    if (str_ieq(ent, "reg")) return 'R';
    if (str_ieq(ent, "laquo") || str_ieq(ent, "raquo")) return '"';

    if (ent[0] == '#') {
        int val = 0;
        if (ent[1] == 'x' || ent[1] == 'X') {
            for (int k = 2; ent[k]; k++) {
                char c = ent[k];
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
                else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
                else break;
                val = val * 16 + d;
            }
        } else {
            for (int k = 1; ent[k]; k++) {
                if (ent[k] < '0' || ent[k] > '9') break;
                val = val * 10 + (ent[k] - '0');
            }
        }
        if (val == 160) return ' ';
        if (val >= 32 && val <= 126) return (char)val;
        if (val == 8211 || val == 8212) return '-';
        if (val == 8216 || val == 8217) return '\'';
        if (val == 8220 || val == 8221) return '"';
        if (val == 8226) return '*';
        return ' ';
    }

    *consumed = 1;
    return '&';
}

static void extract_html_title(const char* html, int len, char* out_title, int max_len) {
    if (!html || len <= 0 || !out_title) return;
    for (int i = 0; i + 7 < len; i++) {
        if (html[i] == '<' && str_istarts_with(html + i, "<title")) {
            int j = i + 6;
            while (j < len && html[j] != '>') j++;
            if (j >= len) break;
            j++;
            int o = 0;
            bool last_sp = true;
            while (j < len && o < max_len - 1) {
                if (html[j] == '<' && str_istarts_with(html + j, "</title")) break;
                if (html[j] == '&') {
                    int cons = 1;
                    char dec = decode_html_entity(html + j, &cons);
                    if (is_space_ch(dec)) {
                        if (!last_sp) { out_title[o++] = ' '; last_sp = true; }
                    } else if (dec >= 32 && dec <= 126) {
                        out_title[o++] = dec;
                        last_sp = false;
                    }
                    j += cons;
                } else {
                    char c = html[j++];
                    if (is_space_ch(c)) {
                        if (!last_sp) { out_title[o++] = ' '; last_sp = true; }
                    } else if (c >= 32 && c <= 126) {
                        out_title[o++] = c;
                        last_sp = false;
                    }
                }
            }
            while (o > 0 && out_title[o - 1] == ' ') o--;
            out_title[o] = '\0';
            return;
        }
    }
}

static int pool_push_str(char* pool, int* used, int cap, const char* str) {
    if (!pool || !used || !str) return -1;
    int len = str_len(str);
    if (*used + len + 1 > cap) return -1;
    int off = *used;
    for (int i = 0; i < len; i++) pool[off + i] = str[i];
    pool[off + len] = '\0';
    *used += len + 1;
    return off;
}

static bool extract_attr(const char* tag_inner, const char* attr_name, char* out, int max_len) {
    if (!tag_inner || !attr_name || !out || max_len <= 0) return false;
    out[0] = '\0';
    int nlen = str_len(attr_name);
    for (int i = 0; tag_inner[i]; i++) {
        // Check word boundary before attr_name
        if (i > 0 && !is_space_ch(tag_inner[i - 1])) continue;
        if (!str_istarts_with(tag_inner + i, attr_name)) continue;
        int p = i + nlen;
        while (tag_inner[p] && is_space_ch(tag_inner[p])) p++;
        if (tag_inner[p] != '=') continue;
        p++;
        while (tag_inner[p] && is_space_ch(tag_inner[p])) p++;
        if (!tag_inner[p]) return false;

        char quote = 0;
        if (tag_inner[p] == '"' || tag_inner[p] == '\'') {
            quote = tag_inner[p++];
        }
        int o = 0;
        while (tag_inner[p] && o < max_len - 1) {
            if (quote) {
                if (tag_inner[p] == quote) break;
            } else {
                if (is_space_ch(tag_inner[p])) break;
            }
            if (tag_inner[p] == '&') {
                int cons = 1;
                char dec = decode_html_entity(tag_inner + p, &cons);
                out[o++] = dec;
                p += cons;
            } else {
                out[o++] = tag_inner[p++];
            }
        }
        out[o] = '\0';
        return (o > 0);
    }
    return false;
}

typedef struct {
    BrowserTab* tab;
    int pad_x;
    int view_w;
    int cur_x;
    int cur_y;
    int line_h;
    int line_start_x;
    int max_x;
    bool line_has_content;
    int blank_lines;

    int bold_depth;
    int code_depth;
    int pre_depth;
    int quote_depth;
    int list_depth;
    int heading_level;

    bool in_link;
    char link_href[384];
    int last_href_off;
    char last_href_str[384];
} LayoutCtx;

static int scale_line_height(uint8_t scale) {
    if (scale >= 3) return 38;
    if (scale == 2) return 28;
    return 21;
}

static void layout_newline(LayoutCtx* ctx, int extra_gap) {
    if (ctx->line_has_content) {
        ctx->cur_y += ctx->line_h + extra_gap;
        ctx->line_has_content = false;
        ctx->blank_lines = (extra_gap > 0) ? 1 : 0;
    } else if (extra_gap > 0 && ctx->blank_lines == 0) {
        ctx->cur_y += extra_gap;
        ctx->blank_lines = 1;
    }
    ctx->line_start_x = ctx->pad_x + ctx->list_depth * 18 + (ctx->quote_depth > 0 ? 14 : 0);
    ctx->cur_x = ctx->line_start_x;
}

static void layout_emit_span(LayoutCtx* ctx, const char* text, uint8_t scale, uint16_t flags, uint32_t color, int w_override) {
    BrowserTab* t = ctx->tab;
    if (t->span_count >= MAX_HTML_SPANS) return;

    int text_off = pool_push_str(t->text_pool, &t->text_pool_used, TEXT_POOL_CAP, text);
    if (text_off < 0) return;

    int href_off = -1;
    if ((flags & SPAN_LINK) && ctx->link_href[0]) {
        if (ctx->last_href_off >= 0 && str_eq(ctx->last_href_str, ctx->link_href)) {
            href_off = ctx->last_href_off;
        } else {
            href_off = pool_push_str(t->href_pool, &t->href_pool_used, HREF_POOL_CAP, ctx->link_href);
            if (href_off >= 0) {
                ctx->last_href_off = href_off;
                str_copy(ctx->last_href_str, ctx->link_href, 384);
            }
        }
    }

    int w = (w_override >= 0) ? w_override : dui_text_width(text, (int)scale);
    int lh = scale_line_height(scale);
    if (lh > ctx->line_h) ctx->line_h = lh;

    HtmlSpan* sp = &t->spans[t->span_count++];
    sp->x = ctx->cur_x;
    sp->y = ctx->cur_y;
    sp->w = w;
    sp->h = lh;
    sp->text_off = text_off;
    sp->href_off = href_off;
    sp->dom_id = -1;
    sp->scale = scale;
    sp->flags = flags;
    sp->color = color;

    ctx->cur_x += w;
    ctx->line_has_content = true;
    ctx->blank_lines = 0;
}

static void layout_push_word(LayoutCtx* ctx, const char* word, bool leading_space) {
    if (!word || !word[0]) return;

    uint8_t scale = 1;
    if (ctx->heading_level == 1) scale = 3;
    else if (ctx->heading_level == 2) scale = 2;

    uint16_t flags = 0;
    uint32_t color = THEME_TEXT_PRIMARY;
    if (ctx->bold_depth > 0 || ctx->heading_level > 0) flags |= SPAN_BOLD;
    if (ctx->code_depth > 0) {
        flags |= SPAN_CODE;
        color = THEME_ACCENT_TEAL;
    }
    if (ctx->pre_depth > 0) {
        flags |= SPAN_PRE;
        color = THEME_ACCENT_TEAL;
    }
    if (ctx->quote_depth > 0) {
        flags |= SPAN_QUOTE;
        if (!(flags & SPAN_LINK) && ctx->heading_level == 0) color = THEME_TEXT_SECONDARY;
    }
    if (ctx->heading_level == 1) color = THEME_TEXT_PRIMARY;
    else if (ctx->heading_level == 2) color = THEME_ACCENT_CYAN;
    else if (ctx->heading_level >= 3) color = THEME_ACCENT_IRIS;
    if (ctx->in_link) {
        flags |= SPAN_LINK;
        color = THEME_ACCENT_CYAN;
    }

    int space_w = (leading_space && ctx->line_has_content) ? dui_text_width(" ", (int)scale) : 0;
    int word_w = dui_text_width(word, (int)scale);
    int avail_line_w = ctx->max_x - ctx->line_start_x;
    if (avail_line_w < 60) avail_line_w = 60;

    if (ctx->line_has_content && ctx->cur_x + space_w + word_w > ctx->max_x) {
        layout_newline(ctx, 0);
        space_w = 0;
    }

    if (space_w > 0) {
        ctx->cur_x += space_w;
    }

    // If a single token (like a long URL) is wider than the entire line, split it cleanly
    if (word_w > avail_line_w) {
        int pos = 0;
        int len = str_len(word);
        while (pos < len) {
            char chunk[128];
            int ci = 0;
            while (pos < len && ci < 126) {
                chunk[ci] = word[pos];
                chunk[ci + 1] = '\0';
                if (ci > 0 && ctx->cur_x + dui_text_width(chunk, (int)scale) > ctx->max_x) {
                    chunk[ci] = '\0';
                    break;
                }
                ci++;
                pos++;
            }
            if (ci == 0) {
                chunk[0] = word[pos++];
                chunk[1] = '\0';
            }
            layout_emit_span(ctx, chunk, scale, flags, color, -1);
            if (pos < len) layout_newline(ctx, 0);
        }
        return;
    }

    layout_emit_span(ctx, word, scale, flags, color, word_w);
}

static bool content_looks_like_html(const BrowserTab* t) {
    if (!t || !t->raw_body || t->raw_len <= 0) return false;
    if (t->view_source) return false;
    if (t->content_type[0]) {
        if (str_icontains(t->content_type, "text/html") || str_icontains(t->content_type, "xhtml")) return true;
        if (str_icontains(t->content_type, "text/plain") || str_icontains(t->content_type, "json")) return false;
    }
    return (str_icontains(t->raw_body, "<html") ||
            str_icontains(t->raw_body, "<!doctype") ||
            str_icontains(t->raw_body, "<body") ||
            str_icontains(t->raw_body, "<p>") ||
            str_icontains(t->raw_body, "<h1") ||
            str_icontains(t->raw_body, "<a "));
}

static int find_dom_node_for_layout(JsRuntime* rt, const char* id_attr, const char* tag, int* tag_nth, int max_initial) {
    if (!rt) return -1;
    int cnt = js_dom_get_node_count(rt);
    if (max_initial <= 0 || max_initial > cnt) max_initial = cnt;
    if (id_attr && id_attr[0]) {
        for (int i = 0; i < cnt; i++) {
            JsDomNode* n = js_dom_get_node(rt, i);
            if (n && n->elem_id[0] && str_eq(n->elem_id, id_attr)) return i;
        }
    }
    if (tag && tag[0] && tag_nth) {
        int seen = 0;
        int want = *tag_nth;
        for (int i = 0; i < max_initial; i++) {
            JsDomNode* n = js_dom_get_node(rt, i);
            if (n && str_eq(n->tag, tag)) {
                if (seen == want) {
                    (*tag_nth)++;
                    return i;
                }
                seen++;
            }
        }
    }
    return -1;
}

static void layout_emit_dynamic_node(LayoutCtx* ctx, JsRuntime* rt, int nid, bool* rendered_dyn) {
    if (!ctx || !rt || nid < 0 || nid >= JS_MAX_DOM_NODES || rendered_dyn[nid]) return;
    JsDomNode* dn = js_dom_get_node(rt, nid);
    if (!dn || dn->hidden) return;
    rendered_dyn[nid] = true;

    if (str_eq(dn->tag, "li")) {
        layout_newline(ctx, 2);
        int save_x = ctx->cur_x;
        ctx->cur_x = (ctx->line_start_x > ctx->pad_x + 14) ? (ctx->line_start_x - 14) : ctx->pad_x;
        layout_emit_span(ctx, "*", 1, SPAN_BULLET, THEME_ACCENT_CYAN, 12);
        ctx->cur_x = (save_x > ctx->line_start_x) ? save_x : ctx->line_start_x;
        ctx->line_has_content = false;
    } else if (str_eq(dn->tag, "button")) {
        const char* lbl = dn->text[0] ? dn->text : "Button";
        int bw = dui_text_width(lbl, 1) + 28;
        if (bw < 64) bw = 64;
        if (ctx->line_has_content && ctx->cur_x + bw + 8 > ctx->max_x) layout_newline(ctx, 4);
        else if (ctx->line_has_content) ctx->cur_x += 8;
        uint32_t col = dn->color ? dn->color : THEME_ACCENT_CYAN;
        layout_emit_span(ctx, lbl, 1, SPAN_BTN, col, bw);
        HtmlSpan* last = &ctx->tab->spans[ctx->tab->span_count - 1];
        last->dom_id = dn->id;
        last->h = 30;
        if (30 > ctx->line_h) ctx->line_h = 30;
        return;
    } else if (str_eq(dn->tag, "input")) {
        int iw = 220;
        if (ctx->line_has_content && ctx->cur_x + iw + 8 > ctx->max_x) layout_newline(ctx, 4);
        else if (ctx->line_has_content) ctx->cur_x += 8;
        layout_emit_span(ctx, dn->text, 1, SPAN_INPUT, THEME_TEXT_PRIMARY, iw);
        HtmlSpan* last = &ctx->tab->spans[ctx->tab->span_count - 1];
        last->dom_id = dn->id;
        last->h = 30;
        if (30 > ctx->line_h) ctx->line_h = 30;
        return;
    } else {
        layout_newline(ctx, 3);
    }

    if (dn->text[0]) {
        uint8_t sc = 1;
        if (str_eq(dn->tag, "h1")) sc = 3;
        else if (str_eq(dn->tag, "h2")) sc = 2;
        uint32_t col = dn->color ? dn->color : ((sc >= 2) ? THEME_ACCENT_CYAN : THEME_TEXT_PRIMARY);
        ctx->line_h = scale_line_height(sc);
        // Word-wrap dynamic node text
        const char* p = dn->text;
        char w[128];
        int wi = 0;
        bool lead = false;
        while (*p && ctx->tab->span_count < MAX_HTML_SPANS - 4) {
            if (*p == ' ' || *p == '\t' || *p == '\n') {
                if (wi > 0) {
                    w[wi] = '\0';
                    int sw = (lead && ctx->line_has_content) ? dui_text_width(" ", sc) : 0;
                    int ww = dui_text_width(w, sc);
                    if (ctx->line_has_content && ctx->cur_x + sw + ww > ctx->max_x) { layout_newline(ctx, 0); sw = 0; }
                    ctx->cur_x += sw;
                    layout_emit_span(ctx, w, sc, (sc >= 2) ? SPAN_BOLD : 0, col, ww);
                    wi = 0;
                    lead = true;
                } else if (ctx->line_has_content) lead = true;
            } else if (wi < 124) {
                w[wi++] = *p;
            }
            p++;
        }
        if (wi > 0) {
            w[wi] = '\0';
            int sw = (lead && ctx->line_has_content) ? dui_text_width(" ", sc) : 0;
            int ww = dui_text_width(w, sc);
            if (ctx->line_has_content && ctx->cur_x + sw + ww > ctx->max_x) { layout_newline(ctx, 0); sw = 0; }
            ctx->cur_x += sw;
            layout_emit_span(ctx, w, sc, (sc >= 2) ? SPAN_BOLD : 0, col, ww);
        }
    }

    int child = dn->first_child;
    int guard = 0;
    while (child >= 0 && guard++ < JS_MAX_DOM_NODES) {
        layout_emit_dynamic_node(ctx, rt, child, rendered_dyn);
        JsDomNode* ch = js_dom_get_node(rt, child);
        child = ch ? ch->next_sibling : -1;
    }
    if (!str_eq(dn->tag, "span")) layout_newline(ctx, 2);
}

static void layout_tab_document(BrowserTab* t, int view_w) {
    if (!t) return;
    t->layout_dirty = false;
    t->layout_width = view_w;
    t->layout_source_mode = t->view_source;
    t->span_count = 0;
    t->text_pool_used = 0;
    t->href_pool_used = 0;
    t->content_height = 60;

    if (!t->raw_body || t->raw_len <= 0) return;
    if (!ensure_tab_layout_buffers(t)) return;

    LayoutCtx ctx;
    mem_zero(&ctx, sizeof(ctx));
    ctx.tab = t;
    ctx.pad_x = 24;
    ctx.view_w = view_w;
    ctx.cur_x = ctx.pad_x;
    ctx.cur_y = 18;
    ctx.line_h = 21;
    ctx.line_start_x = ctx.pad_x;
    ctx.max_x = view_w - 28;
    if (ctx.max_x < 160) ctx.max_x = 160;
    ctx.last_href_off = -1;

    // Plain-text / JSON / View-Source mode
    if (!content_looks_like_html(t)) {
        ctx.pre_depth = 1;
        char line_buf[240];
        line_buf[0] = '\0';
        int li = 0;
        for (int i = 0; i <= t->raw_len; i++) {
            char c = (i < t->raw_len) ? t->raw_body[i] : '\n';
            if (c == '\r') continue;
            if (c == '\t') {
                for (int sp = 0; sp < 4 && li < 236; sp++) line_buf[li++] = ' ';
                line_buf[li] = '\0';
                continue;
            }
            if (c == '\n' || li >= 236 || (li > 0 && ctx.cur_x + dui_text_width(line_buf, 1) > ctx.max_x)) {
                line_buf[li] = '\0';
                if (li > 0) {
                    layout_emit_span(&ctx, line_buf, 1, SPAN_PRE,
                                     t->view_source ? THEME_TEXT_SECONDARY : THEME_TEXT_PRIMARY, -1);
                }
                layout_newline(&ctx, 0);
                if (c == '\n') {
                    if (!ctx.line_has_content) ctx.cur_y += 14;
                    li = 0;
                    line_buf[0] = '\0';
                } else {
                    li = 0;
                    if (c >= 32 && c <= 126) line_buf[li++] = c;
                    line_buf[li] = '\0';
                }
                if (t->span_count >= MAX_HTML_SPANS - 2) break;
            } else if (c >= 32 && c <= 126) {
                line_buf[li++] = c;
                line_buf[li] = '\0';
            }
        }
        t->content_height = ctx.cur_y + 40;
        return;
    }

    // HTML Streaming Tokenizer + Live BlankJS DOM Synchronization
    bool in_head = false;
    bool in_script = false;
    bool in_style = false;
    bool in_title = false;
    bool in_noscript = false;
    bool in_svg = false;
    bool pending_space = false;

    int btn_nth = 0;
    int inp_nth = 0;
    int dom_stack[16];
    int dom_sp = 0;
    int skip_text_depth = 0;
    int hidden_depth = 0;
    bool rendered_dyn[JS_MAX_DOM_NODES];
    mem_zero(rendered_dyn, sizeof(rendered_dyn));

    char word[256];
    int wi = 0;

    auto flush_word = [&]() {
        if (wi > 0) {
            word[wi] = '\0';
            if (skip_text_depth == 0 && hidden_depth == 0) {
                layout_push_word(&ctx, word, pending_space);
            }
            wi = 0;
            pending_space = false;
        }
    };

    const char* html = t->raw_body;
    int len = t->raw_len;
    int i = 0;
    while (i < len && t->span_count < MAX_HTML_SPANS - 4) {
        char c = html[i];

        if (c == '<') {
            // HTML comment <!-- ... -->
            if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')) i++;
                i += 3;
                continue;
            }

            // Read tag until '>'
            int tag_start = i + 1;
            int j = tag_start;
            char q = 0;
            while (j < len && (q || html[j] != '>')) {
                if ((html[j] == '"' || html[j] == '\'') && !q) q = html[j];
                else if (q && html[j] == q) q = 0;
                j++;
            }
            if (j >= len) break;

            int tag_len = j - tag_start;
            char tag_buf[512];
            int cp = (tag_len < 511) ? tag_len : 511;
            for (int k = 0; k < cp; k++) tag_buf[k] = html[tag_start + k];
            tag_buf[cp] = '\0';
            i = j + 1;

            // Extract tag name
            int tp = 0;
            while (tag_buf[tp] && is_space_ch(tag_buf[tp])) tp++;
            bool is_close = false;
            if (tag_buf[tp] == '/') {
                is_close = true;
                tp++;
            }
            char tname[32];
            int ni = 0;
            while (tag_buf[tp] && !is_space_ch(tag_buf[tp]) && tag_buf[tp] != '/' && ni < 30) {
                tname[ni++] = to_lower_ch(tag_buf[tp++]);
            }
            tname[ni] = '\0';

            if (str_eq(tname, "script")) {
                flush_word();
                in_script = !is_close;
            } else if (str_eq(tname, "style")) {
                flush_word();
                in_style = !is_close;
            } else if (str_eq(tname, "noscript")) {
                flush_word();
                in_noscript = !is_close;
            } else if (str_eq(tname, "svg")) {
                flush_word();
                in_svg = !is_close;
            } else if (str_eq(tname, "head")) {
                flush_word();
                in_head = !is_close;
            } else if (str_eq(tname, "title")) {
                in_title = !is_close;
            }

            if (in_script || in_style || in_noscript || in_svg || in_head || in_title) {
                continue;
            }

            // On closing tracked containers, flush dynamically appended DOM children inside parent
            if (is_close) {
                if (skip_text_depth > 0 && (str_eq(tname, "button") || str_eq(tname, "p") || str_eq(tname, "h1") ||
                    str_eq(tname, "h2") || str_eq(tname, "h3") || str_eq(tname, "h4") || str_eq(tname, "span") ||
                    str_eq(tname, "li"))) {
                    skip_text_depth--;
                }
                if (hidden_depth > 0) hidden_depth--;
                if (dom_sp > 0 && (str_eq(tname, "ul") || str_eq(tname, "ol") || str_eq(tname, "div") ||
                    str_eq(tname, "section") || str_eq(tname, "p") || str_eq(tname, "main"))) {
                    int closing_id = dom_stack[--dom_sp];
                    if (t->js_rt && closing_id >= 0) {
                        JsDomNode* pnode = js_dom_get_node(t->js_rt, closing_id);
                        if (pnode) {
                            int ch = pnode->first_child;
                            int g = 0;
                            while (ch >= 0 && g++ < JS_MAX_DOM_NODES) {
                                if (ch >= t->initial_dom_count && !rendered_dyn[ch]) {
                                    layout_emit_dynamic_node(&ctx, t->js_rt, ch, rendered_dyn);
                                }
                                JsDomNode* cn = js_dom_get_node(t->js_rt, ch);
                                ch = cn ? cn->next_sibling : -1;
                            }
                        }
                    }
                }
            }

            // Match opening tags against hydrated BlankJS DOM nodes
            JsDomNode* matched_dom = NULL;
            if (!is_close && t->js_rt) {
                char id_attr[48] = {0};
                extract_attr(tag_buf + tp, "id", id_attr, sizeof(id_attr));
                int found_id = -1;
                if (str_eq(tname, "button")) {
                    found_id = find_dom_node_for_layout(t->js_rt, id_attr, "button", &btn_nth, t->initial_dom_count);
                } else if (str_eq(tname, "input")) {
                    found_id = find_dom_node_for_layout(t->js_rt, id_attr, "input", &inp_nth, t->initial_dom_count);
                } else if (id_attr[0]) {
                    found_id = find_dom_node_for_layout(t->js_rt, id_attr, NULL, NULL, t->initial_dom_count);
                }
                if (found_id >= 0) matched_dom = js_dom_get_node(t->js_rt, found_id);

                if (str_eq(tname, "ul") || str_eq(tname, "ol") || str_eq(tname, "div") ||
                    str_eq(tname, "section") || str_eq(tname, "p") || str_eq(tname, "main")) {
                    if (dom_sp < 15) dom_stack[dom_sp++] = found_id;
                }
                if (matched_dom && matched_dom->hidden) {
                    hidden_depth++;
                }
            }

            if (hidden_depth > 0 && !is_close) {
                continue;
            }

            // Interactive HTML Form Controls (<button>, <input>) backed by BlankJS DOM
            if (str_eq(tname, "button")) {
                flush_word();
                if (!is_close) {
                    const char* lbl = (matched_dom && matched_dom->text[0]) ? matched_dom->text : "Button";
                    int bw = dui_text_width(lbl, 1) + 28;
                    if (bw < 68) bw = 68;
                    if (ctx.line_has_content && ctx.cur_x + bw + 8 > ctx.max_x) {
                        layout_newline(&ctx, 6);
                    } else if (ctx.line_has_content) {
                        ctx.cur_x += 8;
                    }
                    uint32_t col = (matched_dom && matched_dom->color) ? matched_dom->color : THEME_ACCENT_CYAN;
                    layout_emit_span(&ctx, lbl, 1, SPAN_BTN, col, bw);
                    if (t->span_count > 0) {
                        HtmlSpan* bsp = &t->spans[t->span_count - 1];
                        bsp->dom_id = matched_dom ? matched_dom->id : -1;
                        bsp->h = 30;
                    }
                    if (30 > ctx.line_h) ctx.line_h = 30;
                    skip_text_depth++;
                }
                pending_space = false;
                continue;
            } else if (str_eq(tname, "input")) {
                flush_word();
                if (!is_close) {
                    const char* val = matched_dom ? matched_dom->text : "";
                    int iw = 220;
                    if (ctx.line_has_content && ctx.cur_x + iw + 8 > ctx.max_x) {
                        layout_newline(&ctx, 6);
                    } else if (ctx.line_has_content) {
                        ctx.cur_x += 8;
                    }
                    layout_emit_span(&ctx, val, 1, SPAN_INPUT, THEME_TEXT_PRIMARY, iw);
                    if (t->span_count > 0) {
                        HtmlSpan* isp = &t->spans[t->span_count - 1];
                        isp->dom_id = matched_dom ? matched_dom->id : -1;
                        isp->h = 30;
                    }
                    if (30 > ctx.line_h) ctx.line_h = 30;
                }
                pending_space = false;
                continue;
            }

            // Inline & Block element handling
            if (str_eq(tname, "br")) {
                flush_word();
                if (ctx.line_has_content) layout_newline(&ctx, 0);
                else ctx.cur_y += 16;
                pending_space = false;
            } else if (str_eq(tname, "hr")) {
                flush_word();
                layout_newline(&ctx, 8);
                layout_emit_span(&ctx, "", 1, SPAN_HR, THEME_BORDER_SPECULAR, ctx.max_x - ctx.line_start_x);
                layout_newline(&ctx, 10);
                pending_space = false;
            } else if (str_eq(tname, "h1") || str_eq(tname, "h2") || str_eq(tname, "h3") || str_eq(tname, "h4")) {
                flush_word();
                int lvl = tname[1] - '0';
                if (!is_close) {
                    layout_newline(&ctx, 10);
                    ctx.heading_level = lvl;
                    ctx.line_h = scale_line_height((lvl == 1) ? 3 : ((lvl == 2) ? 2 : 1));
                } else {
                    ctx.heading_level = 0;
                    layout_newline(&ctx, 8);
                }
                pending_space = false;
            } else if (str_eq(tname, "p") || str_eq(tname, "div") || str_eq(tname, "section") ||
                       str_eq(tname, "article") || str_eq(tname, "header") || str_eq(tname, "footer") ||
                       str_eq(tname, "main") || str_eq(tname, "nav") || str_eq(tname, "tr")) {
                flush_word();
                int gap = (str_eq(tname, "p")) ? 8 : 2;
                layout_newline(&ctx, gap);
                pending_space = false;
            } else if (str_eq(tname, "ul") || str_eq(tname, "ol")) {
                flush_word();
                if (!is_close) {
                    layout_newline(&ctx, 4);
                    if (ctx.list_depth < 4) ctx.list_depth++;
                } else {
                    if (ctx.list_depth > 0) ctx.list_depth--;
                    layout_newline(&ctx, 6);
                }
                pending_space = false;
            } else if (str_eq(tname, "li")) {
                flush_word();
                if (!is_close) {
                    layout_newline(&ctx, 2);
                    int save_x = ctx.cur_x;
                    ctx.cur_x = ctx.line_start_x - 14;
                    if (ctx.cur_x < ctx.pad_x) ctx.cur_x = ctx.pad_x;
                    layout_emit_span(&ctx, "*", 1, SPAN_BULLET, THEME_ACCENT_CYAN, 12);
                    ctx.cur_x = (save_x > ctx.line_start_x) ? save_x : ctx.line_start_x;
                    ctx.line_has_content = false;
                } else {
                    layout_newline(&ctx, 2);
                }
                pending_space = false;
            } else if (str_eq(tname, "blockquote")) {
                flush_word();
                if (!is_close) {
                    layout_newline(&ctx, 6);
                    if (ctx.quote_depth < 3) ctx.quote_depth++;
                    ctx.line_start_x = ctx.pad_x + ctx.list_depth * 18 + 14;
                    ctx.cur_x = ctx.line_start_x;
                } else {
                    if (ctx.quote_depth > 0) ctx.quote_depth--;
                    layout_newline(&ctx, 6);
                }
                pending_space = false;
            } else if (str_eq(tname, "pre")) {
                flush_word();
                if (!is_close) {
                    layout_newline(&ctx, 6);
                    ctx.pre_depth++;
                } else {
                    if (ctx.pre_depth > 0) ctx.pre_depth--;
                    layout_newline(&ctx, 8);
                }
                pending_space = false;
            } else if (str_eq(tname, "code") || str_eq(tname, "kbd") || str_eq(tname, "samp")) {
                flush_word();
                if (!is_close) ctx.code_depth++;
                else if (ctx.code_depth > 0) ctx.code_depth--;
            } else if (str_eq(tname, "b") || str_eq(tname, "strong")) {
                flush_word();
                if (!is_close) ctx.bold_depth++;
                else if (ctx.bold_depth > 0) ctx.bold_depth--;
            } else if (str_eq(tname, "a")) {
                flush_word();
                if (!is_close) {
                    char raw_href[384];
                    if (extract_attr(tag_buf + tp, "href", raw_href, 384)) {
                        resolve_url(t->url, raw_href, ctx.link_href, 384);
                        ctx.in_link = true;
                    }
                } else {
                    ctx.in_link = false;
                    ctx.link_href[0] = '\0';
                }
            } else if (str_eq(tname, "img") && !is_close) {
                flush_word();
                char alt[80];
                if (extract_attr(tag_buf + tp, "alt", alt, 80) && alt[0]) {
                    char chip[96];
                    str_copy(chip, "[IMG: ", 96);
                    str_append(chip, alt, 96);
                    str_append(chip, "]", 96);
                    layout_push_word(&ctx, chip, pending_space);
                    pending_space = true;
                }
            } else if (str_eq(tname, "td") || str_eq(tname, "th")) {
                flush_word();
                if (!is_close && ctx.line_has_content) {
                    layout_push_word(&ctx, "|", true);
                    pending_space = true;
                }
            }

            // If JS modified a leaf element with an id="..." (e.g. <p id="counter">), emit live text & custom color!
            if (!is_close && matched_dom && matched_dom->elem_id[0] && matched_dom->first_child < 0 &&
                (str_eq(tname, "p") || str_eq(tname, "h1") || str_eq(tname, "h2") || str_eq(tname, "h3") ||
                 str_eq(tname, "h4") || str_eq(tname, "span") || str_eq(tname, "li"))) {
                if (matched_dom->text[0]) {
                    int prev_cnt = t->span_count;
                    const char* p = matched_dom->text;
                    char w[256];
                    int k = 0;
                    bool lead = false;
                    while (*p && t->span_count < MAX_HTML_SPANS - 4) {
                        if (*p == ' ' || *p == '\t' || *p == '\n') {
                            if (k > 0) {
                                w[k] = '\0';
                                layout_push_word(&ctx, w, lead);
                                k = 0;
                                lead = true;
                            } else if (ctx.line_has_content) lead = true;
                        } else if (k < 250) {
                            w[k++] = *p;
                        }
                        p++;
                    }
                    if (k > 0) {
                        w[k] = '\0';
                        layout_push_word(&ctx, w, lead);
                    }
                    if (matched_dom->color != 0) {
                        for (int s = prev_cnt; s < t->span_count; s++) {
                            t->spans[s].color = matched_dom->color;
                        }
                    }
                    skip_text_depth++;
                }
            }
            continue;
        }

        if (in_script || in_style || in_noscript || in_svg || in_head || in_title) {
            i++;
            continue;
        }

        // Handle <pre> verbatim formatting
        if (ctx.pre_depth > 0) {
            char ch = c;
            if (c == '&') {
                int cons = 1;
                ch = decode_html_entity(html + i, &cons);
                i += cons;
            } else {
                i++;
            }
            if (ch == '\r') continue;
            if (ch == '\n') {
                flush_word();
                if (ctx.line_has_content) layout_newline(&ctx, 0);
                else ctx.cur_y += 18;
                pending_space = false;
                continue;
            }
            if (ch == '\t') {
                flush_word();
                if (skip_text_depth == 0 && hidden_depth == 0) {
                    layout_emit_span(&ctx, "    ", 1, SPAN_PRE, THEME_ACCENT_TEAL, -1);
                }
                continue;
            }
            if (ch >= 32 && ch <= 126) {
                if (wi < 120) {
                    word[wi++] = ch;
                } else {
                    flush_word();
                    word[wi++] = ch;
                }
            }
            continue;
        }

        // Normal HTML whitespace & inline text
        char ch = c;
        if (c == '&') {
            int cons = 1;
            ch = decode_html_entity(html + i, &cons);
            i += cons;
        } else {
            i++;
        }

        if (is_space_ch(ch)) {
            if (wi > 0) {
                flush_word();
                pending_space = true;
            } else if (ctx.line_has_content) {
                pending_space = true;
            }
        } else if (ch >= 32 && ch <= 126) {
            if (wi < 250) {
                word[wi++] = ch;
            } else {
                flush_word();
                word[wi++] = ch;
            }
        }
    }

    flush_word();

    // Emit any remaining dynamically created DOM elements appended directly to document.body
    if (t->js_rt) {
        int tot_dom = js_dom_get_node_count(t->js_rt);
        for (int nid = t->initial_dom_count; nid < tot_dom; nid++) {
            if (!rendered_dyn[nid]) {
                JsDomNode* dn = js_dom_get_node(t->js_rt, nid);
                if (dn && dn->parent_id == js_dom_get_body_id(t->js_rt)) {
                    layout_emit_dynamic_node(&ctx, t->js_rt, nid, rendered_dyn);
                }
            }
        }
    }

    t->content_height = ctx.cur_y + 48;
}

// ============================================================================
// Non-Blocking HTTP & Download Polling
// ============================================================================

static void poll_network_transfers(BlankWindow* win) {
    // 1. Poll open tabs
    for (int i = 0; i < g_tab_count; i++) {
        BrowserTab* t = &g_tabs[i];
        if (t->transfer_id < 0) continue;

        const HttpTransfer* ht = net_http_get(t->transfer_id);
        if (!ht) {
            t->transfer_id = -1;
            continue;
        }

        t->last_net_state = ht->state;
        t->status_code = ht->status_code;
        t->bytes_received = ht->bytes_received;
        t->content_length = ht->content_length;
        t->speed_bps = ht->speed_bps;
        str_copy(t->content_type, ht->content_type, 64);

        if (ht->url[0] && !str_eq(t->url, ht->url)) {
            str_copy(t->url, ht->url, 512);
            if (i == g_active_tab) sync_omnibar_from_active_tab();
        }

        if (ht->state == HTTP_DONE) {
            int copy_len = ht->bytes_received;
            if (copy_len > RAW_BODY_MAX - 1) copy_len = RAW_BODY_MAX - 1;
            if (copy_len > 0 && ht->body_buf) {
                if (!t->raw_body) t->raw_body = (char*)kmalloc(RAW_BODY_MAX);
                if (t->raw_body) {
                    mem_copy(t->raw_body, ht->body_buf, copy_len);
                    t->raw_body[copy_len] = '\0';
                    t->raw_len = copy_len;
                }
            } else {
                t->raw_len = 0;
                if (t->raw_body) t->raw_body[0] = '\0';
            }

            // Extract HTML <title>
            if (t->raw_body && t->raw_len > 0) {
                char parsed_title[64];
                parsed_title[0] = '\0';
                extract_html_title(t->raw_body, t->raw_len, parsed_title, 64);
                if (parsed_title[0]) str_copy(t->title, parsed_title, 64);
            }

            if (t->status_code >= 400 && t->raw_len == 0) {
                t->page_mode = PAGE_ERROR;
                str_copy(t->error_msg, "Server returned an empty HTTP error response.", 128);
            } else {
                t->page_mode = PAGE_HTTP;
                if (content_looks_like_html(t)) {
                    hydrate_tab_dom_and_scripts(t);
                }
                t->layout_dirty = true;
            }

            net_http_close(t->transfer_id);
            t->transfer_id = -1;
            t->last_net_state = HTTP_DONE;

            if (i == g_active_tab && win) {
                char wtitle[64];
                str_copy(wtitle, "Horizon Web - ", 64);
                str_append(wtitle, t->title, 64);
                str_copy(win->title, wtitle, 64);
            }
        } else if (ht->state == HTTP_ERROR) {
            str_copy(t->error_msg, ht->error_msg[0] ? ht->error_msg : "HTTP request failed", 128);
            if (ht->is_https) {
                t->page_mode = PAGE_HTTPS_NOTICE;
                str_copy(t->title, "TLS Required", 64);
            } else {
                t->page_mode = PAGE_ERROR;
                str_copy(t->title, "Load Failed", 64);
            }
            net_http_close(t->transfer_id);
            t->transfer_id = -1;
            t->last_net_state = HTTP_ERROR;
        }
    }

    // 2. Poll active disk downloads so we release HTTP slots & notify on completion
    for (int i = 0; i < MAX_ACTIVE_DL_TID; i++) {
        int tid = g_active_dl_tids[i];
        if (tid < 0) continue;
        const HttpTransfer* ht = net_http_get(tid);
        if (!ht) {
            g_active_dl_tids[i] = -1;
            continue;
        }
        if (ht->state == HTTP_DONE || ht->state == HTTP_ERROR) {
            if (ht->state == HTTP_DONE) {
                play_system_sound((char*)"download");
                os_send_notification("Download Complete", ht->save_filename[0] ? ht->save_filename : "Saved to \\DOWNLOADS\\");
                g_disk_scan_needed = true;
            } else {
                play_system_sound((char*)"error");
                os_send_notification("Download Failed", ht->error_msg[0] ? ht->error_msg : "Transfer error");
            }
            net_http_close(tid);
            g_active_dl_tids[i] = -1;
        }
    }
}

// ============================================================================
// Built-in Page Renderers (100% Real Data, Zero Mock Data)
// ============================================================================

static void render_start_page(BrowserTab* t, int vx, int vy, int vw, int vh, const BlankInputState* in, bool allow_click) {
    int sy = vy - t->scroll_y;
    int pad = 28;
    int inner_w = vw - pad * 2;
    if (inner_w < 240) inner_w = 240;
    int x0 = vx + pad;

    NetStatus ns;
    net_get_status(&ns);

    // Lira Asymmetric 2fr + 1fr Bento Hero Row (no neon halos, crisp dot lattice + catchlight)
    int hero_h = 118;
    int gap = 14;
    int side_w = (inner_w >= 620) ? (inner_w - gap) / 3 : 0;
    int hero_w = (side_w > 0) ? (inner_w - gap - side_w) : inner_w;

    dui_bento_card(x0, sy + 18, hero_w, hero_h, THEME_RADIUS_CARD, THEME_SURFACE_CARD, 224, true);
    dui_dot_grid_overlay(x0 + 6, sy + 24, hero_w - 12, hero_h - 12, 24, 0x94A3B8, 12);
    dui_rect_rounded(x0 + 20, sy + 38, 42, 42, THEME_RADIUS_MD, THEME_ACCENT_CYAN, 36);
    dui_rect_rounded_outline(x0 + 20, sy + 38, 42, 42, THEME_RADIUS_MD, THEME_ACCENT_CYAN, 110);
    dui_draw_sys_icon(x0 + 29, sy + 47, 24, 6, THEME_ACCENT_CYAN);
    dui_text(x0 + 76, sy + 36, "BlankOS Horizon Web", THEME_TEXT_PRIMARY, 2);
    dui_text_ellipsis(x0 + 76, sy + 68, hero_w - 96, "Freestanding HTTP/1.1 + BlankJS DOM Engine on UEFI SNP", THEME_TEXT_SECONDARY, 1);

    // Live Network Status Pill inside Hero Card
    uint32_t pill_col = ns.dhcp_bound ? THEME_ACCENT_GREEN : (ns.link_up ? THEME_ACCENT_AMBER : THEME_ACCENT_ROSE);
    const char* net_lbl = ns.dhcp_bound ? ns.ip_str : (ns.link_up ? "Link Up" : (ns.nic_present ? "Link Down" : "Offline"));
    int pw = dui_text_width(net_lbl, 1) + 28;
    int px = x0 + hero_w - pw - 16;
    if (px > x0 + 260) {
        dui_status_pill(px, sy + 28, pw, 24, net_lbl, pill_col, THEME_SURFACE_INPUT, ns.dhcp_bound);
    }

    if (side_w > 0) {
        int sx0 = x0 + hero_w + gap;
        bool hov_js_hero = allow_click && pt_in(in->mx, in->my, sx0, sy + 18, side_w, hero_h) && pt_in(in->mx, in->my, vx, vy, vw, vh);
        dui_bento_card(sx0, sy + 18, side_w, hero_h, THEME_RADIUS_CARD,
                       hov_js_hero ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 224, hov_js_hero);
        dui_status_pill(sx0 + 16, sy + 30, 108, 24, "BlankJS Live", THEME_ACCENT_IRIS, THEME_SURFACE_INPUT, true);
        dui_text(sx0 + 16, sy + 62, "JS + DOM Sandbox", THEME_TEXT_PRIMARY, 1);
        dui_text_ellipsis(sx0 + 16, sy + 84, side_w - 32, "Interactive timers & Console ->", THEME_ACCENT_CYAN, 1);
        if (hov_js_hero && in->left_pressed) {
            play_system_sound((char*)"open");
            navigate_tab(g_active_tab, "blank://js-demo", true);
            return;
        }
    }

    // Speed Dial Bento Grid (2x3)
    int grid_y = sy + 154;
    dui_text(x0, grid_y, "QUICK LAUNCH & VERIFIED DESTINATIONS", THEME_TEXT_MUTED, 1);

    typedef struct {
        const char* title;
        const char* sub;
        const char* url;
        int icon_id;
        uint32_t accent;
    } SpeedTile;

    static const SpeedTile tiles[6] = {
        { "BlankJS Lab",     "Interactive DOM & REPL", "blank://js-demo",        14, THEME_ACCENT_IRIS },
        { "Example Domain",  "http://example.com/",    "http://example.com/",     6, THEME_ACCENT_CYAN },
        { "CERN Info Web",   "http://info.cern.ch/",   "http://info.cern.ch/",    6, THEME_ACCENT_TEAL },
        { "NeverSSL Portal", "http://neverssl.com/",   "http://neverssl.com/",   18, THEME_ACCENT_GREEN },
        { "Network Pulse",   "Live SNP, TCP & Ping",   "blank://pulse",           1, THEME_ACCENT_AMBER },
        { "Downloads",       "FAT32 \\DOWNLOADS\\",    "blank://downloads",       8, THEME_ACCENT_ROSE }
    };

    int cols = (inner_w >= 540) ? 3 : 2;
    int card_w = (inner_w - gap * (cols - 1)) / cols;
    int card_h = 84;

    for (int i = 0; i < 6; i++) {
        int r = i / cols;
        int c = i % cols;
        int cx0 = x0 + c * (card_w + gap);
        int cy0 = grid_y + 26 + r * (card_h + gap);
        bool hov = allow_click && pt_in(in->mx, in->my, cx0, cy0, card_w, card_h) && pt_in(in->mx, in->my, vx, vy, vw, vh);

        dui_bento_card(cx0, cy0, card_w, card_h, THEME_RADIUS_LG,
                       hov ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, hov ? 236 : 210, hov);
        dui_rect_rounded(cx0 + 14, cy0 + 22, 38, 38, THEME_RADIUS_MD, tiles[i].accent, 34);
        dui_rect_rounded_outline(cx0 + 14, cy0 + 22, 38, 38, THEME_RADIUS_MD, tiles[i].accent, 95);
        dui_draw_sys_icon(cx0 + 23, cy0 + 31, 20, tiles[i].icon_id, tiles[i].accent);
        dui_text_ellipsis(cx0 + 62, cy0 + 20, card_w - 76, tiles[i].title, THEME_TEXT_PRIMARY, 1);
        dui_text_ellipsis(cx0 + 62, cy0 + 44, card_w - 76, tiles[i].sub, THEME_TEXT_SECONDARY, 1);

        if (hov && in->left_pressed) {
            play_system_sound((char*)"open");
            navigate_tab(g_active_tab, tiles[i].url, true);
            return;
        }
    }

    int rows = (6 + cols - 1) / cols;
    int foot_y = grid_y + 26 + rows * (card_h + gap) + 12;

    // Active / Recent Downloads Summary Strip
    int dl_cnt = net_download_count();
    dui_bento_card(x0, foot_y, inner_w, 64, THEME_RADIUS_LG, THEME_SURFACE_INPUT, 215, false);
    dui_draw_sys_icon(x0 + 18, foot_y + 22, 20, 8, THEME_ACCENT_CYAN);
    if (dl_cnt == 0) {
        dui_text(x0 + 48, foot_y + 14, "Real FAT32 Download Engine + BlankJS Runtime Ready", THEME_TEXT_PRIMARY, 1);
        dui_text_ellipsis(x0 + 48, foot_y + 34, inner_w - 200, "Press Ctrl+J for BlankJS Console or click 'Save' to stream to \\DOWNLOADS\\", THEME_TEXT_SECONDARY, 1);
    } else {
        const DownloadEntry* last_dl = net_get_download(dl_cnt - 1);
        char msg[128];
        str_copy(msg, "Latest: ", 128);
        str_append(msg, last_dl ? last_dl->filename : "file", 128);
        str_append(msg, " (", 128);
        str_append(msg, last_dl ? last_dl->status_text : "", 128);
        str_append(msg, ")", 128);
        dui_text_ellipsis(x0 + 48, foot_y + 14, inner_w - 200, msg, THEME_TEXT_PRIMARY, 1);
        dui_text(x0 + 48, foot_y + 34, "Click 'Downloads' to inspect or launch saved files", THEME_TEXT_SECONDARY, 1);
    }
    if (dui_button_tactile(x0 + inner_w - 134, foot_y + 16, 118, 32, "Downloads", THEME_ACCENT_CYAN, false,
                           in->mx, in->my, in->left_down, allow_click && in->left_pressed && pt_in(in->mx, in->my, vx, vy, vw, vh))) {
        play_system_sound((char*)"open");
        navigate_tab(g_active_tab, "blank://downloads", true);
        return;
    }

    t->content_height = (foot_y - sy) + 96;
}

static void render_pulse_page(BrowserTab* t, int vx, int vy, int vw, int vh, const BlankInputState* in, bool allow_click) {
    int sy = vy - t->scroll_y;
    int pad = 24;
    int inner_w = vw - pad * 2;
    if (inner_w < 240) inner_w = 240;
    int x0 = vx + pad;

    NetStatus ns;
    net_get_status(&ns);

    // Header
    dui_text(x0, sy + 18, "Network & Hardware Pulse", THEME_TEXT_PRIMARY, 2);
    dui_text(x0, sy + 48, "Live UEFI Simple Network Protocol, IPv4, ICMP & PCIe Bus Telemetry", THEME_TEXT_SECONDARY, 1);

    // Network Status Card
    int net_y = sy + 80;
    int net_h = 166;
    dui_glass_card(x0, net_y, inner_w, net_h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 220, true);
    dui_draw_sys_icon(x0 + 18, net_y + 16, 20, 1, ns.link_up ? THEME_ACCENT_GREEN : THEME_ACCENT_ROSE);
    dui_text_ellipsis(x0 + 46, net_y + 17, inner_w - 180, ns.nic_present ? ns.adapter_name : "No UEFI SNP Network Adapter", THEME_TEXT_PRIMARY, 1);

    if (dui_button(x0 + inner_w - 118, net_y + 12, 102, 28, "Renew DHCP", THEME_ACCENT_CYAN, true,
                   in->mx, in->my, allow_click && in->left_pressed && pt_in(in->mx, in->my, vx, vy, vw, vh))) {
        bool ok = net_dhcp_renew();
        play_system_sound(ok ? (char*)"notify" : "error");
        str_copy(g_last_ping_status, ok ? "DHCP lease renewed successfully" : "DHCP renew timed out", 64);
    }

    char sub_str[20], rx_str[32], tx_str[32], rxb_str[24], txb_str[24];
    net_format_ip(ns.subnet_mask, sub_str, 20);
    u64_to_str(ns.rx_packets, rx_str, 32);
    u64_to_str(ns.tx_packets, tx_str, 32);
    format_bytes(ns.rx_bytes, rxb_str, 24);
    format_bytes(ns.tx_bytes, txb_str, 24);

    int col2 = x0 + inner_w / 2;
    char line[96];

    str_copy(line, "MAC: ", 96); str_append(line, ns.mac_str[0] ? ns.mac_str : "--", 96);
    dui_text(x0 + 18, net_y + 52, line, THEME_TEXT_SECONDARY, 1);

    str_copy(line, "Link: ", 96); str_append(line, ns.link_up ? "UP" : "DOWN", 96);
    str_append(line, ns.dhcp_bound ? "  |  DHCP Bound" : "  |  No DHCP", 96);
    dui_text(col2, net_y + 52, line, ns.link_up ? THEME_ACCENT_GREEN : THEME_ACCENT_ROSE, 1);

    str_copy(line, "IPv4: ", 96); str_append(line, ns.ip_str, 96);
    dui_text(x0 + 18, net_y + 76, line, THEME_TEXT_PRIMARY, 1);

    str_copy(line, "Mask: ", 96); str_append(line, sub_str, 96);
    dui_text(col2, net_y + 76, line, THEME_TEXT_SECONDARY, 1);

    str_copy(line, "Gateway: ", 96); str_append(line, ns.gw_str, 96);
    dui_text(x0 + 18, net_y + 100, line, THEME_TEXT_SECONDARY, 1);

    str_copy(line, "DNS: ", 96); str_append(line, ns.dns_str, 96);
    dui_text(col2, net_y + 100, line, THEME_TEXT_SECONDARY, 1);

    str_copy(line, "RX: ", 96); str_append(line, rx_str, 96); str_append(line, " pkts (", 96); str_append(line, rxb_str, 96); str_append(line, ")", 96);
    dui_text(x0 + 18, net_y + 128, line, THEME_ACCENT_TEAL, 1);

    str_copy(line, "TX: ", 96); str_append(line, tx_str, 96); str_append(line, " pkts (", 96); str_append(line, txb_str, 96); str_append(line, ")", 96);
    dui_text(col2, net_y + 128, line, THEME_ACCENT_IRIS, 1);

    // Real ICMP Ping Tool Card
    int ping_y = net_y + net_h + 14;
    int ping_h = 132;
    dui_glass_card(x0, ping_y, inner_w, ping_h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 215, false);
    dui_text(x0 + 18, ping_y + 14, "ICMP Echo Latency Probe (Real net_ping)", THEME_TEXT_PRIMARY, 1);
    dui_text_ellipsis(x0 + 18, ping_y + 36, inner_w - 280, g_last_ping_status, THEME_ACCENT_CYAN, 1);

    bool in_vp = allow_click && pt_in(in->mx, in->my, vx, vy, vw, vh);
    auto run_ping = [&](const char* target) {
        int rtt = net_ping(target, 1000);
        if (g_ping_count < MAX_PING_SAMPLES) {
            str_copy(g_ping_samples[g_ping_count].target, target, 48);
            g_ping_samples[g_ping_count].rtt_ms = rtt;
            g_ping_count++;
        } else {
            for (int k = 0; k < MAX_PING_SAMPLES - 1; k++) g_ping_samples[k] = g_ping_samples[k + 1];
            str_copy(g_ping_samples[MAX_PING_SAMPLES - 1].target, target, 48);
            g_ping_samples[MAX_PING_SAMPLES - 1].rtt_ms = rtt;
        }
        if (rtt >= 0) {
            char rbuf[16];
            int_to_str(rtt, rbuf, 16);
            str_copy(g_last_ping_status, target, 64);
            str_append(g_last_ping_status, ": ", 64);
            str_append(g_last_ping_status, rbuf, 64);
            str_append(g_last_ping_status, " ms RTT", 64);
            play_system_sound((char*)"notify");
        } else {
            str_copy(g_last_ping_status, target, 64);
            str_append(g_last_ping_status, ": Timeout / Unreachable", 64);
            play_system_sound((char*)"error");
        }
    };

    if (dui_button(x0 + inner_w - 246, ping_y + 12, 110, 28, "Ping Gateway", THEME_SURFACE_ELEVATED, false,
                   in->mx, in->my, in_vp && in->left_pressed)) {
        run_ping(ns.gw_str[0] && !str_eq(ns.gw_str, "0.0.0.0") ? ns.gw_str : "10.0.2.2");
    }
    if (dui_button(x0 + inner_w - 128, ping_y + 12, 112, 28, "Ping 1.1.1.1", THEME_ACCENT_TEAL, false,
                   in->mx, in->my, in_vp && in->left_pressed)) {
        run_ping("1.1.1.1");
    }

    // Real Ping Sample Bars (empty until probed)
    int bar_area_y = ping_y + 64;
    if (g_ping_count == 0) {
        dui_rect_rounded(x0 + 18, bar_area_y, inner_w - 36, 48, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 200);
        dui_text(x0 + 32, bar_area_y + 16, "No ICMP samples yet. Click 'Ping Gateway' or 'Ping 1.1.1.1' to measure real RTT.", THEME_TEXT_MUTED, 1);
    } else {
        int slot_w = (inner_w - 36) / MAX_PING_SAMPLES;
        for (int i = 0; i < g_ping_count; i++) {
            int bx = x0 + 18 + i * slot_w + 4;
            int bw = slot_w - 8;
            int rtt = g_ping_samples[i].rtt_ms;
            int bh = (rtt < 0) ? 10 : horizon_clampi(10 + rtt / 3, 10, 44);
            uint32_t bc = (rtt < 0) ? THEME_ACCENT_ROSE : ((rtt < 60) ? THEME_ACCENT_GREEN : THEME_ACCENT_AMBER);
            dui_rect_rounded(bx, bar_area_y + 44 - bh, bw, bh, 4, bc, 220);
            char lbl[16];
            if (rtt < 0) str_copy(lbl, "TMO", 16);
            else { int_to_str(rtt, lbl, 16); str_append(lbl, "ms", 16); }
            dui_text(bx, bar_area_y + 48, lbl, THEME_TEXT_SECONDARY, 1);
        }
    }

    // Real System Hardware & PCIe Card
    int hw_y = ping_y + ping_h + 14;
    int pci_cnt = pci_get_device_count();
    int show_pci = (pci_cnt > 8) ? 8 : pci_cnt;
    int hw_h = 112 + show_pci * 22;

    dui_glass_card(x0, hw_y, inner_w, hw_h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 215, false);
    dui_text(x0 + 18, hw_y + 14, "System Hardware & PCIe Enumeration", THEME_TEXT_PRIMARY, 1);

    char ram_u[20], ram_t[20], pci_s[16];
    u64_to_str(memory_get_used_mb(), ram_u, 20);
    u64_to_str(memory_get_total_mb(), ram_t, 20);
    int_to_str(pci_cnt, pci_s, 16);

    str_copy(line, "CPU: ", 96); str_append(line, pci_get_cpu_brand(), 96);
    dui_text_ellipsis(x0 + 18, hw_y + 38, inner_w - 36, line, THEME_ACCENT_CYAN, 1);

    str_copy(line, "RAM: ", 96); str_append(line, ram_u, 96); str_append(line, " MB / ", 96); str_append(line, ram_t, 96);
    str_append(line, " MB   |   PCI Devices: ", 96); str_append(line, pci_s, 96);
    dui_text(x0 + 18, hw_y + 60, line, THEME_TEXT_SECONDARY, 1);

    dui_line(x0 + 18, hw_y + 84, x0 + inner_w - 18, hw_y + 84, THEME_BORDER_SUBTLE, 200);
    for (int i = 0; i < show_pci; i++) {
        uint8_t bus = 0, dev = 0, func = 0, cc = 0, sc = 0;
        uint16_t vid = 0, did = 0;
        const char* vstr = "";
        const char* dstr = "";
        const char* drv = "";
        if (pci_get_device_info(i, &bus, &dev, &func, &vid, &did, &cc, &sc, NULL, &vstr, &dstr, &drv)) {
            char vhex[8], dhex[8];
            u16_to_hex4(vid, vhex);
            u16_to_hex4(did, dhex);
            char row[128];
            str_copy(row, vhex, 128);
            str_append(row, ":", 128);
            str_append(row, dhex + 2, 128);
            str_append(row, "  ", 128);
            str_append(row, vstr ? vstr : "", 128);
            str_append(row, " - ", 128);
            str_append(row, dstr ? dstr : "", 128);
            dui_text_ellipsis(x0 + 18, hw_y + 92 + i * 22, inner_w - 36, row, THEME_TEXT_SECONDARY, 1);
        }
    }

    t->content_height = (hw_y - sy) + hw_h + 36;
}

static void render_downloads_page(BrowserTab* t, int vx, int vy, int vw, int vh, const BlankInputState* in, bool allow_click) {
    if (g_disk_scan_needed) scan_fat32_downloads_dir();

    int sy = vy - t->scroll_y;
    int pad = 24;
    int inner_w = vw - pad * 2;
    if (inner_w < 240) inner_w = 240;
    int x0 = vx + pad;
    bool in_vp = allow_click && pt_in(in->mx, in->my, vx, vy, vw, vh);

    dui_text(x0, sy + 18, "Downloads & \\DOWNLOADS\\ Storage", THEME_TEXT_PRIMARY, 2);
    dui_text(x0, sy + 48, "Live HTTP transfers stream directly to FAT32 \\DOWNLOADS\\", THEME_TEXT_SECONDARY, 1);

    if (dui_button(x0 + inner_w - 224, sy + 16, 102, 28, "Refresh Disk", THEME_SURFACE_ELEVATED, false,
                   in->mx, in->my, in_vp && in->left_pressed)) {
        scan_fat32_downloads_dir();
        play_system_sound((char*)"click");
    }
    if (dui_button(x0 + inner_w - 114, sy + 16, 114, 28, "Clear Done", THEME_ACCENT_ROSE, false,
                   in->mx, in->my, in_vp && in->left_pressed)) {
        net_clear_finished_downloads();
        play_system_sound((char*)"click");
    }

    int cur_y = sy + 82;
    int dl_cnt = net_download_count();
    dui_text(x0, cur_y, "SESSION TRANSFERS", THEME_TEXT_MUTED, 1);
    cur_y += 22;

    if (dl_cnt == 0) {
        dui_glass_card(x0, cur_y, inner_w, 56, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 200, false);
        dui_text(x0 + 18, cur_y + 19, "No active or recent session downloads.", THEME_TEXT_SECONDARY, 1);
        cur_y += 72;
    } else {
        for (int i = dl_cnt - 1; i >= 0; i--) {
            const DownloadEntry* de = net_get_download(i);
            if (!de) continue;
            dui_glass_card(x0, cur_y, inner_w, 72, THEME_RADIUS_MD, THEME_SURFACE_CARD, 215, de->state == HTTP_RECEIVING);

            dui_draw_sys_icon(x0 + 16, cur_y + 16, 20, 8,
                              de->state == HTTP_DONE ? THEME_ACCENT_GREEN :
                              (de->state == HTTP_ERROR ? THEME_ACCENT_ROSE : THEME_ACCENT_CYAN));
            dui_text_ellipsis(x0 + 46, cur_y + 12, inner_w - 180, de->filename, THEME_TEXT_PRIMARY, 1);

            char info[96];
            char b1[24], b2[24], spd[24];
            format_bytes((uint64_t)de->downloaded_bytes, b1, 24);
            str_copy(info, b1, 96);
            if (de->total_bytes > 0) {
                format_bytes((uint64_t)de->total_bytes, b2, 24);
                str_append(info, " / ", 96);
                str_append(info, b2, 96);
            }
            if (de->speed_bps > 0 && de->state == HTTP_RECEIVING) {
                format_bytes((uint64_t)de->speed_bps, spd, 24);
                str_append(info, "  |  ", 96);
                str_append(info, spd, 96);
                str_append(info, "/s", 96);
            }
            str_append(info, "  |  ", 96);
            str_append(info, de->status_text, 96);
            dui_text_ellipsis(x0 + 46, cur_y + 34, inner_w - 180, info, THEME_TEXT_SECONDARY, 1);

            // Progress bar
            int bar_w = inner_w - 170;
            if (bar_w > 60) {
                dui_rect_rounded(x0 + 46, cur_y + 56, bar_w, 5, 3, THEME_SURFACE_INPUT, 230);
                float frac = 0.0f;
                if (de->state == HTTP_DONE) frac = 1.0f;
                else if (de->total_bytes > 0) frac = horizon_clampf((float)de->downloaded_bytes / (float)de->total_bytes, 0.05f, 0.98f);
                else if (de->state == HTTP_RECEIVING) frac = 0.45f;
                else if (de->state != HTTP_ERROR && de->state != HTTP_IDLE) frac = 0.2f;
                if (frac > 0.0f) {
                    dui_rect_rounded(x0 + 46, cur_y + 56, (int)(bar_w * frac), 5, 3,
                                     de->state == HTTP_DONE ? THEME_ACCENT_GREEN :
                                     (de->state == HTTP_ERROR ? THEME_ACCENT_ROSE : THEME_ACCENT_CYAN), 255);
                }
            }

            if (de->state == HTTP_DONE && de->saved_ok) {
                if (dui_button(x0 + inner_w - 110, cur_y + 20, 96, 30, "Open File", THEME_ACCENT_CYAN, true,
                               in->mx, in->my, in_vp && in->left_pressed)) {
                    wm_request_open_file(de->disk_path);
                    return;
                }
            }
            cur_y += 82;
        }
    }

    // Real FAT32 \DOWNLOADS\ Disk Directory
    cur_y += 8;
    dui_text(x0, cur_y, "FAT32 VOLUME (\\DOWNLOADS\\)", THEME_TEXT_MUTED, 1);
    cur_y += 22;

    if (g_disk_file_count == 0) {
        dui_glass_card(x0, cur_y, inner_w, 56, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 200, false);
        dui_text(x0 + 18, cur_y + 19, "No files found in \\DOWNLOADS\\ on FAT32 disk.", THEME_TEXT_SECONDARY, 1);
        cur_y += 72;
    } else {
        for (int i = 0; i < g_disk_file_count; i++) {
            DiskDownloadFile* f = &g_disk_files[i];
            dui_glass_card(x0, cur_y, inner_w, 52, THEME_RADIUS_MD, THEME_SURFACE_CARD, 205, false);
            dui_draw_sys_icon(x0 + 16, cur_y + 16, 18, 5, THEME_ACCENT_TEAL);
            dui_text_ellipsis(x0 + 44, cur_y + 10, inner_w - 180, f->name, THEME_TEXT_PRIMARY, 1);
            char sz[24];
            format_bytes(f->size, sz, 24);
            dui_text(x0 + 44, cur_y + 30, sz, THEME_TEXT_SECONDARY, 1);

            if (dui_button(x0 + inner_w - 110, cur_y + 11, 96, 30, "Open File", THEME_SURFACE_ELEVATED, false,
                           in->mx, in->my, in_vp && in->left_pressed)) {
                wm_request_open_file(f->disk_path);
                return;
            }
            cur_y += 60;
        }
    }

    t->content_height = (cur_y - sy) + 32;
}

static void render_https_notice_page(BrowserTab* t, int vx, int vy, int vw, int vh, const BlankInputState* in, bool allow_click) {
    int pad = 32;
    int card_w = vw - pad * 2;
    if (card_w > 580) card_w = 580;
    int card_x = vx + (vw - card_w) / 2;
    int card_y = vy + 36;
    int card_h = 240;
    bool in_vp = allow_click && pt_in(in->mx, in->my, vx, vy, vw, vh);

    dui_bento_card(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, THEME_SURFACE_CARD, 230, true);
    dui_rect_rounded(card_x + 24, card_y + 24, 44, 44, 12, THEME_ACCENT_AMBER, 38);
    dui_draw_sys_icon(card_x + 34, card_y + 34, 24, 19, THEME_ACCENT_AMBER);

    dui_text(card_x + 82, card_y + 26, "HTTPS / TLS Stack Not Loaded", THEME_TEXT_PRIMARY, 2);
    dui_text(card_x + 82, card_y + 56, "Bare-metal UEFI TCP/HTTP/1.1 mode is active.", THEME_TEXT_SECONDARY, 1);

    dui_rect_rounded(card_x + 24, card_y + 88, card_w - 48, 70, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 220);
    dui_text(card_x + 38, card_y + 100, "BlankOS Horizon Web does not load a TLS 1.3 / X.509 certificate store yet.", THEME_TEXT_SECONDARY, 1);
    dui_text_ellipsis(card_x + 38, card_y + 124, card_w - 76, t->url, THEME_ACCENT_AMBER, 1);

    // Build http:// fallback URL
    char http_url[512];
    if (str_istarts_with(t->url, "https://")) {
        str_copy(http_url, "http://", 512);
        str_append(http_url, t->url + 8, 512);
    } else {
        str_copy(http_url, t->url, 512);
    }

    if (dui_button_tactile(card_x + 24, card_y + 180, 176, 34, "Try HTTP Instead", THEME_ACCENT_CYAN, true,
                           in->mx, in->my, in->left_down, in_vp && in->left_pressed)) {
        play_system_sound((char*)"open");
        navigate_tab(g_active_tab, http_url, true);
        return;
    }

    if (t->hist_pos > 0) {
        if (dui_button_tactile(card_x + 212, card_y + 180, 110, 34, "Go Back", THEME_SURFACE_ELEVATED, false,
                               in->mx, in->my, in->left_down, in_vp && in->left_pressed)) {
            play_system_sound((char*)"tab");
            t->hist_pos--;
            navigate_tab(g_active_tab, t->history[t->hist_pos], false);
            return;
        }
    }

    t->content_height = 300;
}

static void render_error_page(BrowserTab* t, int vx, int vy, int vw, int vh, const BlankInputState* in, bool allow_click) {
    int pad = 32;
    int card_w = vw - pad * 2;
    if (card_w > 560) card_w = 560;
    int card_x = vx + (vw - card_w) / 2;
    int card_y = vy + 36;
    int card_h = 224;
    bool in_vp = allow_click && pt_in(in->mx, in->my, vx, vy, vw, vh);

    dui_bento_card(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, THEME_SURFACE_CARD, 230, true);
    dui_rect_rounded(card_x + 24, card_y + 24, 44, 44, 12, THEME_ACCENT_ROSE, 38);
    dui_draw_sys_icon(card_x + 34, card_y + 34, 24, 12, THEME_ACCENT_ROSE);

    dui_text(card_x + 82, card_y + 26, "Unable to Load Page", THEME_TEXT_PRIMARY, 2);
    dui_text_ellipsis(card_x + 82, card_y + 56, card_w - 104, t->url, THEME_TEXT_SECONDARY, 1);

    dui_rect_rounded(card_x + 24, card_y + 88, card_w - 48, 58, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 220);
    dui_text_ellipsis(card_x + 38, card_y + 108, card_w - 76,
                      t->error_msg[0] ? t->error_msg : "Network connection or HTTP response error.", THEME_ACCENT_ROSE, 1);

    if (dui_button_tactile(card_x + 24, card_y + 168, 120, 34, "Retry", THEME_ACCENT_CYAN, true,
                           in->mx, in->my, in->left_down, in_vp && in->left_pressed)) {
        play_system_sound((char*)"tool");
        navigate_tab(g_active_tab, t->url, false);
        return;
    }
    if (dui_button_tactile(card_x + 156, card_y + 168, 150, 34, "Network Pulse", THEME_SURFACE_ELEVATED, false,
                           in->mx, in->my, in->left_down, in_vp && in->left_pressed)) {
        play_system_sound((char*)"open");
        navigate_tab(g_active_tab, "blank://pulse", true);
        return;
    }

    t->content_height = 280;
}

static void render_http_document(BrowserTab* t, int vx, int vy, int vw, int vh, const BlankInputState* in, bool allow_click, char* hovered_href_out) {
    if (t->transfer_id >= 0 && t->raw_len == 0) {
        // Real non-blocking loading view while DNS/TCP/HTTP streams
        int card_w = (vw > 460) ? 420 : (vw - 40);
        int card_x = vx + (vw - card_w) / 2;
        int card_y = vy + 56;
        dui_bento_card(card_x, card_y, card_w, 156, THEME_RADIUS_CARD, THEME_SURFACE_CARD, 225, true);

        const char* stage = "Preparing Request...";
        float prog = 0.15f;
        if (t->last_net_state == HTTP_RESOLVING_DNS) { stage = "Resolving DNS..."; prog = 0.25f; }
        else if (t->last_net_state == HTTP_CONNECTING) { stage = "TCP Handshake..."; prog = 0.45f; }
        else if (t->last_net_state == HTTP_SENDING) { stage = "Sending HTTP/1.1 GET..."; prog = 0.60f; }
        else if (t->last_net_state == HTTP_RECEIVING) {
            stage = "Receiving Response...";
            if (t->content_length > 0) {
                prog = horizon_clampf(0.60f + 0.35f * ((float)t->bytes_received / (float)t->content_length), 0.60f, 0.96f);
            } else {
                prog = 0.78f;
            }
        }

        dui_progress_ring(card_x + 42, card_y + 52, 16, 3, prog, THEME_BORDER_SUBTLE, THEME_ACCENT_CYAN);
        dui_text(card_x + 74, card_y + 34, stage, THEME_TEXT_PRIMARY, 1);
        dui_text_ellipsis(card_x + 74, card_y + 56, card_w - 94, t->url, THEME_TEXT_SECONDARY, 1);

        char rx_buf[64];
        char bytes_s[24];
        format_bytes((uint64_t)t->bytes_received, bytes_s, 24);
        str_copy(rx_buf, "Received: ", 64);
        str_append(rx_buf, bytes_s, 64);
        if (t->speed_bps > 0) {
            char sp_s[24];
            format_bytes((uint64_t)t->speed_bps, sp_s, 24);
            str_append(rx_buf, " (", 64);
            str_append(rx_buf, sp_s, 64);
            str_append(rx_buf, "/s)", 64);
        }
        dui_text(card_x + 24, card_y + 104, rx_buf, THEME_ACCENT_TEAL, 1);

        t->content_height = 240;
        return;
    }

    if (t->layout_dirty || t->layout_width != vw || t->layout_source_mode != t->view_source) {
        layout_tab_document(t, vw);
    }

    if (t->span_count == 0) {
        dui_text(vx + 28, vy + 28, "(Empty HTTP response body)", THEME_TEXT_MUTED, 1);
        t->content_height = 100;
        return;
    }

    bool in_vp = pt_in(in->mx, in->my, vx, vy, vw, vh);

    for (int i = 0; i < t->span_count; i++) {
        HtmlSpan* sp = &t->spans[i];
        int sx = vx + sp->x;
        int sy = vy + sp->y - t->scroll_y;

        if (sy + sp->h < vy - 8) continue;
        if (sy > vy + vh + 8) break;

        if (sp->flags & SPAN_HR) {
            dui_line(sx, sy + 6, sx + sp->w, sy + 6, THEME_BORDER_SPECULAR, 180);
            continue;
        }

        if (sp->flags & SPAN_QUOTE) {
            dui_rect(vx + 22, sy, 3, sp->h, THEME_ACCENT_IRIS, 180);
        }

        if ((sp->flags & (SPAN_CODE | SPAN_PRE)) && !(sp->flags & SPAN_LINK)) {
            dui_rect_rounded(sx - 2, sy + 1, sp->w + 4, sp->h - 3, 4, THEME_SURFACE_INPUT, 190);
        }

        const char* txt = t->text_pool + sp->text_off;
        const char* href = (sp->href_off >= 0) ? (t->href_pool + sp->href_off) : NULL;

        if (sp->flags & SPAN_BTN) {
            bool press_ok = allow_click && in_vp && in->left_pressed;
            if (dui_button_tactile(sx, sy, sp->w, 28, txt, sp->color ? sp->color : THEME_ACCENT_CYAN, true,
                                   in->mx, in->my, in->left_down, press_ok)) {
                play_system_sound((char*)"tool");
                if (t->js_rt && sp->dom_id >= 0) {
                    js_dispatch_click(t->js_rt, sp->dom_id);
                    js_handle_outputs(t);
                    if (js_dom_is_dirty(t->js_rt)) {
                        js_dom_clear_dirty(t->js_rt);
                        t->layout_dirty = true;
                    }
                }
                return;
            }
            continue;
        }

        if (sp->flags & SPAN_INPUT) {
            bool foc = (sp->dom_id >= 0 && t->active_input_dom_id == sp->dom_id);
            bool hov = allow_click && in_vp && pt_in(in->mx, in->my, sx, sy, sp->w, 28);
            dui_rect_rounded(sx, sy, sp->w, 28, THEME_RADIUS_SM, THEME_SURFACE_INPUT, 240);
            dui_rect_rounded_outline(sx, sy, sp->w, 28, THEME_RADIUS_SM,
                                     foc ? THEME_BORDER_FOCUS : (hov ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE),
                                     foc ? 245 : 190);
            const char* show_txt = txt;
            if (t->js_rt && sp->dom_id >= 0) {
                JsDomNode* idn = js_dom_get_node(t->js_rt, sp->dom_id);
                if (idn) show_txt = idn->text;
            }
            dui_push_clip(sx + 6, sy + 2, sp->w - 12, 24);
            dui_text(sx + 8, sy + 6, show_txt[0] ? show_txt : "Type here...",
                     show_txt[0] ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);
            if (foc && ((in->frame_ms / 450) % 2 == 0)) {
                int cw_t = dui_text_width(show_txt, 1);
                if (cw_t > sp->w - 18) cw_t = sp->w - 18;
                dui_rect(sx + 9 + cw_t, sy + 6, 2, 16, THEME_ACCENT_CYAN, 255);
            }
            dui_pop_clip();
            if (hov && in->left_pressed) {
                t->active_input_dom_id = sp->dom_id;
                g_omni_focused = false;
                g_console_input_focused = false;
                play_system_sound((char*)"tab");
            }
            continue;
        }

        bool link_hov = false;
        if ((sp->flags & SPAN_LINK) && href && allow_click && in_vp) {
            if (pt_in(in->mx, in->my, sx - 2, sy, sp->w + 4, sp->h)) {
                link_hov = true;
                if (hovered_href_out) str_copy(hovered_href_out, href, 512);
            }
        }

        uint32_t col = link_hov ? 0x7DD3FC : sp->color;
        if (link_hov) {
            dui_rect_rounded(sx - 3, sy + 1, sp->w + 6, sp->h - 2, 4, THEME_ACCENT_CYAN, 38);
        }

        dui_text(sx, sy + 2, txt, col, (int)sp->scale);
        if (sp->flags & SPAN_BOLD) {
            dui_text(sx + 1, sy + 2, txt, col, (int)sp->scale);
        }

        if (sp->flags & SPAN_LINK) {
            int uy = sy + sp->h - 3;
            dui_line(sx, uy, sx + sp->w, uy, col, link_hov ? 255 : 170);

            if (link_hov && href) {
                if (in->left_pressed) {
                    if (in->ctrl_held) {
                        start_file_download(href, NULL);
                    } else {
                        play_system_sound((char*)"open");
                        navigate_tab(g_active_tab, href, true);
                    }
                    return;
                } else if (in->right_pressed) {
                    g_ctx_open = true;
                    g_ctx_x = in->mx;
                    g_ctx_y = in->my;
                    str_copy(g_ctx_href, href, 512);
                    return;
                }
            }
        }
    }
}

// ============================================================================
// Main Non-Blocking Browser Frame Renderer & Close Cleanup
// ============================================================================

static void browser_close(BlankWindow* win) {
    (void)win;
    for (int i = 0; i < g_tab_count; i++) {
        cancel_tab_transfer(&g_tabs[i]);
        free_tab_doc_buffers(&g_tabs[i]);
    }
    for (int i = 0; i < MAX_ACTIVE_DL_TID; i++) {
        if (g_active_dl_tids[i] >= 0) {
            net_http_close(g_active_dl_tids[i]);
            g_active_dl_tids[i] = -1;
        }
    }
    g_tab_count = 0;
    g_active_tab = 0;
    g_omni_focused = false;
    g_console_open = false;
    g_console_input_focused = false;
    g_js_alert_ticks = 0;
    g_dl_popover_open = false;
    g_ctx_open = false;
    g_scrollbar_drag = false;
}

static void browser_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* raw_in) {
    ensure_tabs_initialized();

    // 1. Consume pending cross-app URL requests (e.g. Spotlight or Store)
    const char* pending_url = wm_consume_pending_url();
    if (pending_url && pending_url[0]) {
        navigate_tab(g_active_tab, pending_url, true);
    }

    // 2. Non-blocking HTTP & download polling + BlankJS timers
    poll_network_transfers(win);
    if (g_js_alert_ticks > 0) g_js_alert_ticks--;

    BlankInputState local_in = *raw_in;
    BlankInputState* in = &local_in;

    // Auto-collapse sidebar on very narrow windows
    bool show_sidebar = g_sidebar_open && (cw >= 640);
    int sb_w = show_sidebar ? SIDEBAR_W : 0;
    int console_h = g_console_open ? 156 : 0;

    int tab_y = cy;
    int tool_y = cy + TAB_BAR_H;
    int bmk_y = tool_y + TOOLBAR_H;
    int vx = cx + sb_w;
    int vy = bmk_y + BOOKMARKS_H;
    int vw = cw - sb_w;
    int vh = ch - (TAB_BAR_H + TOOLBAR_H + BOOKMARKS_H) - console_h;
    if (vw < 120) vw = 120;
    if (vh < 80) vh = 80;

    BrowserTab* active = &g_tabs[g_active_tab];

    if (active->js_rt && active->page_mode == PAGE_HTTP) {
        if (js_tick_timers(active->js_rt, in->frame_ms)) {
            active->layout_dirty = true;
        }
        js_handle_outputs(active);
        if (js_dom_is_dirty(active->js_rt)) {
            js_dom_clear_dirty(active->js_rt);
            active->layout_dirty = true;
        }
    }

    // Update window title to match active tab cleanly
    if (win) {
        char wt[64];
        str_copy(wt, "Horizon Web - ", 64);
        str_append(wt, active->title, 64);
        str_copy(win->title, wt, 64);
    }

    // ========================================================================
    // Popover & Context Menu Pre-Interception
    // ========================================================================
    int dl_pop_w = 330;
    int dl_pop_h = 286;
    int dl_pop_x = cx + cw - dl_pop_w - 10;
    int dl_pop_y = tool_y + TOOLBAR_H + 4;

    int ctx_w = 186;
    int ctx_h = 112;
    if (g_ctx_open) {
        if (g_ctx_x + ctx_w > cx + cw - 8) g_ctx_x = cx + cw - ctx_w - 8;
        if (g_ctx_y + ctx_h > cy + ch - 8) g_ctx_y = cy + ch - ctx_h - 8;
        if (g_ctx_x < cx + 8) g_ctx_x = cx + 8;
        if (g_ctx_y < cy + 8) g_ctx_y = cy + 8;
    }

    bool overlay_blocks_bg = (g_dl_popover_open || g_ctx_open);
    if (in->has_key && in->scan_code == BLANK_SCAN_ESC) {
        if (g_ctx_open) {
            g_ctx_open = false;
            in->has_key = false;
        } else if (g_dl_popover_open) {
            g_dl_popover_open = false;
            in->has_key = false;
        } else if (g_console_input_focused) {
            g_console_input_focused = false;
            in->has_key = false;
        } else if (active->active_input_dom_id >= 0) {
            active->active_input_dom_id = -1;
            in->has_key = false;
        } else if (g_omni_focused) {
            g_omni_focused = false;
            g_omni_sel_all = false;
            sync_omnibar_from_active_tab();
            in->has_key = false;
        }
    }

    // ========================================================================
    // Keyboard Input Handling (Omnibar, Console REPL, <input> DOM, Scrolling)
    // ========================================================================
    if (in->has_key) {
        if (in->ctrl_held) {
            if (in->key_char == 'l') {
                g_omni_focused = true;
                g_omni_sel_all = true;
                g_console_input_focused = false;
                active->active_input_dom_id = -1;
                in->has_key = false;
            } else if (in->key_char == 'j') {
                g_console_open = !g_console_open;
                g_console_input_focused = g_console_open;
                if (g_console_open) g_omni_focused = false;
                play_system_sound((char*)"tab");
                in->has_key = false;
            } else if (in->key_char == 't') {
                create_new_tab("blank://start", true);
                play_system_sound((char*)"open");
                in->has_key = false;
            } else if (in->key_char == 'w') {
                close_tab_at(g_active_tab);
                play_system_sound((char*)"close");
                in->has_key = false;
            } else if (in->key_char == 'r') {
                navigate_tab(g_active_tab, active->url, false);
                play_system_sound((char*)"tool");
                in->has_key = false;
            } else if (in->key_char == 'a' && g_omni_focused) {
                g_omni_sel_all = true;
                in->has_key = false;
            }
        } else if (g_console_open && g_console_input_focused) {
            if (in->key_char == '\n') {
                g_console_input[g_console_input_len] = '\0';
                if (g_console_input_len > 0) {
                    if (!active->js_rt) active->js_rt = js_runtime_create();
                    if (active->js_rt) {
                        js_eval(active->js_rt, g_console_input, NULL, 0);
                        js_handle_outputs(active);
                        if (js_dom_is_dirty(active->js_rt)) {
                            js_dom_clear_dirty(active->js_rt);
                            active->layout_dirty = true;
                        }
                    }
                    g_console_input_len = 0;
                    g_console_input[0] = '\0';
                    play_system_sound((char*)"tool");
                }
            } else if (in->key_char == '\b' || in->scan_code == BLANK_SCAN_DELETE) {
                if (g_console_input_len > 0) g_console_input[--g_console_input_len] = '\0';
            } else if (in->key_char >= 32 && in->key_char < 127) {
                if (g_console_input_len < 252) {
                    g_console_input[g_console_input_len++] = in->key_char;
                    g_console_input[g_console_input_len] = '\0';
                }
            }
            in->has_key = false;
        } else if (active->active_input_dom_id >= 0 && active->js_rt && !g_omni_focused) {
            JsDomNode* idn = js_dom_get_node(active->js_rt, active->active_input_dom_id);
            if (idn) {
                int il = str_len(idn->text);
                if (in->key_char == '\b' || in->scan_code == BLANK_SCAN_DELETE) {
                    if (il > 0) { idn->text[--il] = '\0'; active->layout_dirty = true; }
                } else if (in->key_char >= 32 && in->key_char < 127 && il < 500) {
                    idn->text[il++] = in->key_char;
                    idn->text[il] = '\0';
                    active->layout_dirty = true;
                }
            }
            in->has_key = false;
        } else if (g_omni_focused) {
            if (in->key_char == '\n') {
                g_omni_buf[g_omni_len] = '\0';
                navigate_tab(g_active_tab, g_omni_buf, true);
                play_system_sound((char*)"open");
            } else if (in->key_char == '\b' || in->scan_code == BLANK_SCAN_DELETE) {
                if (g_omni_sel_all) {
                    g_omni_len = 0;
                    g_omni_buf[0] = '\0';
                    g_omni_sel_all = false;
                } else if (g_omni_len > 0) {
                    g_omni_buf[--g_omni_len] = '\0';
                }
            } else if (in->scan_code == BLANK_SCAN_LEFT || in->scan_code == BLANK_SCAN_RIGHT ||
                       in->scan_code == BLANK_SCAN_END || in->scan_code == BLANK_SCAN_HOME) {
                g_omni_sel_all = false;
            } else if (in->key_char >= 32 && in->key_char < 127) {
                if (g_omni_sel_all) {
                    g_omni_len = 0;
                    g_omni_sel_all = false;
                }
                if (g_omni_len < 510) {
                    g_omni_buf[g_omni_len++] = in->key_char;
                    g_omni_buf[g_omni_len] = '\0';
                }
            }
            in->has_key = false;
        } else {
            // Viewport keyboard scrolling
            int step = 36;
            int page_step = (vh > 80) ? (vh - 60) : 160;
            if (in->scan_code == BLANK_SCAN_UP) active->scroll_y -= step;
            else if (in->scan_code == BLANK_SCAN_DOWN) active->scroll_y += step;
            else if (in->scan_code == BLANK_SCAN_PGUP) active->scroll_y -= page_step;
            else if (in->scan_code == BLANK_SCAN_PGDN) active->scroll_y += page_step;
            else if (in->scan_code == BLANK_SCAN_HOME) active->scroll_y = 0;
            else if (in->scan_code == BLANK_SCAN_END) active->scroll_y = active->content_height;
        }
    }

    // Mouse Wheel Scrolling inside viewport
    active = &g_tabs[g_active_tab];
    if (!overlay_blocks_bg && in->scroll_y != 0 && pt_in(in->mx, in->my, vx, vy, vw, vh)) {
        active->scroll_y -= in->scroll_y * 38;
    }

    // Clamp scroll_y
    int max_scroll = active->content_height - vh;
    if (max_scroll < 0) max_scroll = 0;
    active->scroll_y = horizon_clampi(active->scroll_y, 0, max_scroll);

    // ========================================================================
    // 1. Top Glass Tab Bar
    // ========================================================================
    dui_rect(cx, tab_y, cw, TAB_BAR_H, THEME_BG_DEEP, 228);
    dui_line(cx, tab_y + TAB_BAR_H - 1, cx + cw, tab_y + TAB_BAR_H - 1, THEME_BORDER_SUBTLE, 200);

    // Sidebar Toggle Button (left edge of tab bar)
    int sb_btn_x = cx + 8;
    int sb_btn_y = tab_y + 5;
    bool hov_sb = !overlay_blocks_bg && pt_in(in->mx, in->my, sb_btn_x, sb_btn_y, 26, 24);
    dui_rect_rounded(sb_btn_x, sb_btn_y, 26, 24, THEME_RADIUS_SM,
                     (show_sidebar || hov_sb) ? THEME_SURFACE_ELEVATED : THEME_SURFACE_CARD, 210);
    dui_rect(sb_btn_x + 6, sb_btn_y + 6, 4, 12, show_sidebar ? THEME_ACCENT_CYAN : THEME_TEXT_SECONDARY, 255);
    dui_rect_rounded_outline(sb_btn_x + 5, sb_btn_y + 5, 16, 14, 2, THEME_TEXT_SECONDARY, 200);
    if (hov_sb && in->left_pressed) {
        g_sidebar_open = !g_sidebar_open;
        play_system_sound((char*)"tab");
    }

    // Tab Pills
    int tabs_x0 = cx + 42;
    int tabs_avail = cw - 92;
    int slot_w = (tabs_avail - 32) / (g_tab_count > 0 ? g_tab_count : 1);
    if (slot_w > 180) slot_w = 180;
    if (slot_w < 84) slot_w = 84;

    for (int i = 0; i < g_tab_count; i++) {
        int tx = tabs_x0 + i * (slot_w + 4);
        int ty = tab_y + 4;
        int th = 26;
        bool is_act = (i == g_active_tab);
        bool hov_tab = !overlay_blocks_bg && pt_in(in->mx, in->my, tx, ty, slot_w, th);
        bool hov_x = !overlay_blocks_bg && pt_in(in->mx, in->my, tx + slot_w - 20, ty + 4, 16, 18);

        if (is_act) {
            dui_rect_rounded(tx, ty, slot_w, th, 8, THEME_SURFACE_ELEVATED, 245);
            dui_rect_rounded_outline(tx, ty, slot_w, th, 8, THEME_BORDER_SPECULAR, 210);
            dui_rect(tx + 10, ty + th - 2, slot_w - 20, 2, THEME_ACCENT_CYAN, 255);
        } else {
            dui_rect_rounded(tx, ty, slot_w, th, 8, hov_tab ? THEME_SURFACE_HOVER : THEME_SURFACE_BASE, 195);
        }

        // Loading spinner or page icon
        BrowserTab* tb = &g_tabs[i];
        if (tb->transfer_id >= 0) {
            float spin = (float)((in->frame_ms / 40) % 10) * 0.1f;
            dui_progress_ring(tx + 14, ty + 13, 6, 2, spin, THEME_BORDER_SUBTLE, THEME_ACCENT_CYAN);
        } else {
            int ic = (tb->page_mode == PAGE_PULSE) ? 1 : ((tb->page_mode == PAGE_DOWNLOADS) ? 8 : 6);
            dui_draw_sys_icon(tx + 8, ty + 6, 14, ic, is_act ? THEME_ACCENT_CYAN : THEME_TEXT_SECONDARY);
        }

        dui_text_ellipsis(tx + 26, ty + 6, slot_w - 48, tb->title,
                          is_act ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);

        // Close [x]
        if (hov_x) dui_rect_rounded(tx + slot_w - 20, ty + 4, 16, 18, 4, THEME_ACCENT_ROSE, 60);
        dui_draw_sys_icon(tx + slot_w - 17, ty + 8, 10, 12, hov_x ? THEME_ACCENT_ROSE : THEME_TEXT_MUTED);

        if (!overlay_blocks_bg && in->left_pressed) {
            if (hov_x) {
                close_tab_at(i);
                play_system_sound((char*)"close");
                break;
            } else if (hov_tab) {
                g_active_tab = i;
                sync_omnibar_from_active_tab();
                g_omni_focused = false;
                g_omni_sel_all = false;
                play_system_sound((char*)"tab");
            }
        }
    }

    // [+] New Tab Button
    if (g_tab_count < MAX_TABS) {
        int nx = tabs_x0 + g_tab_count * (slot_w + 4) + 2;
        int ny = tab_y + 5;
        bool hov_new = !overlay_blocks_bg && pt_in(in->mx, in->my, nx, ny, 24, 24);
        dui_rect_rounded(nx, ny, 24, 24, 7, hov_new ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 210);
        dui_draw_sys_icon(nx + 5, ny + 5, 14, 13, hov_new ? THEME_ACCENT_CYAN : THEME_TEXT_SECONDARY);
        if (hov_new && in->left_pressed) {
            create_new_tab("blank://start", true);
            play_system_sound((char*)"open");
        }
    }

    active = &g_tabs[g_active_tab];

    // ========================================================================
    // 2. Unified Glass Toolbar (Nav + Omnibar + Save + Source + DL Popover)
    // ========================================================================
    dui_rect(cx, tool_y, cw, TOOLBAR_H, THEME_SURFACE_BASE, 232);
    dui_line(cx, tool_y + TOOLBAR_H - 1, cx + cw, tool_y + TOOLBAR_H - 1, THEME_BORDER_SUBTLE, 180);

    bool can_back = (active->hist_pos > 0);
    bool can_fwd = (active->hist_pos + 1 < active->hist_count);
    bool is_loading = (active->transfer_id >= 0);

    int bx = cx + 10;
    int by = tool_y + 7;

    // Back <
    bool hov_back = !overlay_blocks_bg && can_back && pt_in(in->mx, in->my, bx, by, 28, 28);
    dui_rect_rounded(bx, by, 28, 28, 8, hov_back ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 200);
    dui_draw_sys_icon(bx + 6, by + 6, 16, 14, can_back ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED);
    if (hov_back && in->left_pressed) {
        active->hist_pos--;
        navigate_tab(g_active_tab, active->history[active->hist_pos], false);
        play_system_sound((char*)"tab");
    }

    // Forward >
    bx += 32;
    bool hov_fwd = !overlay_blocks_bg && can_fwd && pt_in(in->mx, in->my, bx, by, 28, 28);
    dui_rect_rounded(bx, by, 28, 28, 8, hov_fwd ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 200);
    dui_draw_sys_icon(bx + 6, by + 6, 16, 15, can_fwd ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED);
    if (hov_fwd && in->left_pressed) {
        active->hist_pos++;
        navigate_tab(g_active_tab, active->history[active->hist_pos], false);
        play_system_sound((char*)"tab");
    }

    // Reload / Stop
    bx += 32;
    bool hov_rel = !overlay_blocks_bg && pt_in(in->mx, in->my, bx, by, 28, 28);
    dui_rect_rounded(bx, by, 28, 28, 8, hov_rel ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 200);
    dui_draw_sys_icon(bx + 6, by + 6, 16, is_loading ? 12 : 16, is_loading ? THEME_ACCENT_ROSE : THEME_TEXT_PRIMARY);
    if (hov_rel && in->left_pressed) {
        if (is_loading) {
            cancel_tab_transfer(active);
            str_copy(active->title, "Stopped", 64);
        } else {
            navigate_tab(g_active_tab, active->url, false);
        }
        play_system_sound((char*)"tool");
    }

    // Right-side Toolbar Action Pills (Save, Src, DL)
    int right_edge = cx + cw - 10;

    // Downloads Popover Trigger Pill
    int dl_cnt = net_download_count();
    int active_dl = 0;
    for (int i = 0; i < dl_cnt; i++) {
        const DownloadEntry* de = net_get_download(i);
        if (de && de->state != HTTP_DONE && de->state != HTTP_ERROR && de->state != HTTP_IDLE) active_dl++;
    }
    int dl_btn_w = 58;
    int dl_btn_x = right_edge - dl_btn_w;
    bool hov_dl_btn = pt_in(in->mx, in->my, dl_btn_x, by, dl_btn_w, 28);
    dui_rect_rounded(dl_btn_x, by, dl_btn_w, 28, 8,
                     (g_dl_popover_open || hov_dl_btn) ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 220);
    if (g_dl_popover_open) dui_rect_rounded_outline(dl_btn_x, by, dl_btn_w, 28, 8, THEME_ACCENT_CYAN, 200);
    dui_draw_sys_icon(dl_btn_x + 8, by + 6, 16, 8, active_dl > 0 ? THEME_ACCENT_AMBER : THEME_ACCENT_CYAN);
    char dl_num[8];
    int_to_str(dl_cnt, dl_num, 8);
    dui_text(dl_btn_x + 30, by + 7, dl_num, THEME_TEXT_PRIMARY, 1);
    if (hov_dl_btn && in->left_pressed) {
        g_dl_popover_open = !g_dl_popover_open;
        g_ctx_open = false;
        play_system_sound((char*)"click");
        in->left_pressed = false;
    }

    // BlankJS Console Toggle Pill (Ctrl+J)
    int cur_rx = dl_btn_x - 6;
    {
        int js_w = 38;
        int js_x = cur_rx - js_w;
        bool hov_js = !overlay_blocks_bg && pt_in(in->mx, in->my, js_x, by, js_w, 28);
        dui_rect_rounded(js_x, by, js_w, 28, 8,
                         g_console_open ? THEME_ACCENT_IRIS : (hov_js ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD),
                         g_console_open ? 78 : 210);
        if (g_console_open) dui_rect_rounded_outline(js_x, by, js_w, 28, 8, THEME_ACCENT_IRIS, 210);
        dui_text(js_x + 11, by + 7, "JS", g_console_open ? THEME_ACCENT_IRIS : THEME_ACCENT_CYAN, 1);
        if (hov_js && in->left_pressed) {
            g_console_open = !g_console_open;
            g_console_input_focused = g_console_open;
            if (g_console_open) g_omni_focused = false;
            play_system_sound((char*)"tab");
        }
        cur_rx = js_x - 6;
    }

    // View Source Toggle Pill (for HTTP pages)
    if (active->page_mode == PAGE_HTTP && active->raw_len > 0) {
        int src_w = 42;
        int src_x = cur_rx - src_w;
        bool hov_src = !overlay_blocks_bg && pt_in(in->mx, in->my, src_x, by, src_w, 28);
        dui_rect_rounded(src_x, by, src_w, 28, 8,
                         active->view_source ? THEME_ACCENT_IRIS : (hov_src ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD),
                         active->view_source ? 70 : 210);
        if (active->view_source) dui_rect_rounded_outline(src_x, by, src_w, 28, 8, THEME_ACCENT_IRIS, 210);
        dui_text(src_x + 9, by + 7, "Src", active->view_source ? THEME_ACCENT_IRIS : THEME_TEXT_SECONDARY, 1);
        if (hov_src && in->left_pressed) {
            active->view_source = !active->view_source;
            active->layout_dirty = true;
            active->scroll_y = 0;
            play_system_sound((char*)"tab");
        }
        cur_rx = src_x - 6;
    }

    // Download Page / Save URL Button
    bool can_save_url = str_istarts_with(g_omni_buf, "http://");
    int save_w = 64;
    int save_x = cur_rx - save_w;
    bool hov_save = !overlay_blocks_bg && can_save_url && pt_in(in->mx, in->my, save_x, by, save_w, 28);
    dui_rect_rounded(save_x, by, save_w, 28, 8, hov_save ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 210);
    dui_text(save_x + 14, by + 7, "Save", can_save_url ? THEME_ACCENT_TEAL : THEME_TEXT_MUTED, 1);
    if (hov_save && in->left_pressed) {
        start_file_download(g_omni_buf, NULL);
    }
    cur_rx = save_x - 8;

    // Unified Omnibar
    int omni_x = bx + 36;
    int omni_w = cur_rx - omni_x;
    if (omni_w < 120) omni_w = 120;
    bool hov_omni = !overlay_blocks_bg && pt_in(in->mx, in->my, omni_x, by, omni_w, 28);

    dui_rect_rounded(omni_x, by, omni_w, 28, THEME_RADIUS_PILL, THEME_SURFACE_INPUT, 245);
    dui_rect_rounded_outline(omni_x, by, omni_w, 28, THEME_RADIUS_PILL,
                             g_omni_focused ? THEME_BORDER_FOCUS : (hov_omni ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE),
                             g_omni_focused ? 240 : 180);

    // Scheme Badge inside Omnibar
    const char* badge_lbl = "OS";
    uint32_t badge_col = THEME_ACCENT_IRIS;
    if (is_loading) {
        badge_lbl = "NET";
        badge_col = THEME_ACCENT_AMBER;
    } else if (str_ieq(active->url, "blank://js-demo") || str_istarts_with(g_omni_buf, "javascript:")) {
        badge_lbl = "JS";
        badge_col = THEME_ACCENT_IRIS;
    } else if (str_istarts_with(g_omni_buf, "https://")) {
        badge_lbl = "TLS!";
        badge_col = THEME_ACCENT_ROSE;
    } else if (str_istarts_with(g_omni_buf, "http://")) {
        badge_lbl = "HTTP";
        badge_col = THEME_ACCENT_TEAL;
    }
    dui_rect_rounded(omni_x + 6, by + 4, 42, 20, THEME_RADIUS_PILL, badge_col, 42);
    dui_text(omni_x + 11, by + 7, badge_lbl, badge_col, 1);

    int txt_x = omni_x + 56;
    int txt_max_w = omni_w - 68;
    dui_push_clip(txt_x - 2, by + 2, txt_max_w + 4, 24);
    if (g_omni_focused && g_omni_sel_all && g_omni_len > 0) {
        int sel_w = dui_text_width(g_omni_buf, 1) + 4;
        if (sel_w > txt_max_w) sel_w = txt_max_w;
        dui_rect_rounded(txt_x - 2, by + 4, sel_w, 20, 4, THEME_ACCENT_CYAN, 75);
    }
    if (g_omni_len > 0) {
        int full_w = dui_text_width(g_omni_buf, 1);
        int draw_tx = txt_x;
        if (g_omni_focused && full_w > txt_max_w) {
            draw_tx = txt_x + txt_max_w - full_w - 4;
        }
        dui_text(draw_tx, by + 7, g_omni_buf, THEME_TEXT_PRIMARY, 1);
        if (g_omni_focused && !g_omni_sel_all && ((in->frame_ms / 450) % 2 == 0)) {
            dui_rect(draw_tx + full_w + 1, by + 6, 2, 16, THEME_ACCENT_CYAN, 255);
        }
    } else {
        dui_text(txt_x, by + 7, "Enter http:// URL or blank://js-demo ...", THEME_TEXT_MUTED, 1);
        if (g_omni_focused && ((in->frame_ms / 450) % 2 == 0)) {
            dui_rect(txt_x + 1, by + 6, 2, 16, THEME_ACCENT_CYAN, 255);
        }
    }
    dui_pop_clip();

    if (!overlay_blocks_bg && in->left_pressed) {
        if (hov_omni) {
            g_console_input_focused = false;
            active->active_input_dom_id = -1;
            if (!g_omni_focused) {
                g_omni_focused = true;
                g_omni_sel_all = true;
            } else {
                g_omni_sel_all = false;
            }
        } else {
            g_omni_focused = false;
            g_omni_sel_all = false;
        }
    }

    // ========================================================================
    // 3. Bookmarks Strip + Thin Loading Progress Bar
    // ========================================================================
    dui_rect(cx, bmk_y, cw, BOOKMARKS_H, THEME_BG_DEEP, 210);
    dui_line(cx, bmk_y + BOOKMARKS_H - 1, cx + cw, bmk_y + BOOKMARKS_H - 1, THEME_BORDER_SUBTLE, 180);

    typedef struct { const char* label; const char* url; } Bookmark;
    static const Bookmark bmks[7] = {
        { "Start",       "blank://start" },
        { "JS Lab",      "blank://js-demo" },
        { "Net Pulse",   "blank://pulse" },
        { "Downloads",   "blank://downloads" },
        { "Example.com", "http://example.com/" },
        { "CERN Web",    "http://info.cern.ch/" },
        { "NeverSSL",    "http://neverssl.com/" }
    };

    int bkx = cx + 10;
    for (int i = 0; i < 7; i++) {
        int bw = dui_text_width(bmks[i].label, 1) + 18;
        if (bkx + bw > cx + cw - 10) break;
        bool act_b = str_ieq(active->url, bmks[i].url);
        bool hov_b = !overlay_blocks_bg && pt_in(in->mx, in->my, bkx, bmk_y + 3, bw, 22);
        if (act_b || hov_b) {
            dui_rect_rounded(bkx, bmk_y + 3, bw, 22, THEME_RADIUS_PILL,
                             act_b ? THEME_ACCENT_CYAN : THEME_SURFACE_HOVER, act_b ? 42 : 190);
        }
        dui_text(bkx + 9, bmk_y + 6, bmks[i].label, act_b ? THEME_ACCENT_CYAN : THEME_TEXT_SECONDARY, 1);
        if (hov_b && in->left_pressed) {
            navigate_tab(g_active_tab, bmks[i].url, true);
            play_system_sound((char*)"open");
        }
        bkx += bw + 6;
    }

    // Thin Progress Hairline
    if (is_loading) {
        float frac = 0.25f;
        if (active->last_net_state == HTTP_CONNECTING) frac = 0.45f;
        else if (active->last_net_state == HTTP_SENDING) frac = 0.60f;
        else if (active->last_net_state == HTTP_RECEIVING) {
            frac = (active->content_length > 0)
                ? horizon_clampf(0.60f + 0.36f * ((float)active->bytes_received / (float)active->content_length), 0.60f, 0.96f)
                : 0.78f;
        }
        dui_rect(cx, bmk_y + BOOKMARKS_H - 2, (int)(cw * frac), 2, THEME_ACCENT_CYAN, 255);
    }

    // ========================================================================
    // 4. Collapsible Glass Sidebar
    // ========================================================================
    if (show_sidebar) {
        dui_rect(cx, vy, sb_w, vh + console_h, THEME_BG_DEEP, 215);
        dui_line(cx + sb_w - 1, vy, cx + sb_w - 1, vy + vh + console_h, THEME_BORDER_SUBTLE, 200);

        dui_push_clip(cx, vy, sb_w, vh + console_h);
        int sy0 = vy + 12;

        dui_text(cx + 14, sy0, "SPACES", THEME_TEXT_MUTED, 1);
        sy0 += 20;
        const char* sp_lbls[4] = { "Start Page", "JS Sandbox", "Network Pulse", "Downloads" };
        const char* sp_urls[4] = { "blank://start", "blank://js-demo", "blank://pulse", "blank://downloads" };
        const int sp_icons[4]  = { 6, 14, 1, 8 };
        for (int i = 0; i < 4; i++) {
            bool act = str_ieq(active->url, sp_urls[i]);
            if (dui_icon_pill(cx + 10, sy0, sb_w - 20, 28, sp_lbls[i], sp_icons[i], act,
                              in->mx, in->my, !overlay_blocks_bg && in->left_pressed)) {
                navigate_tab(g_active_tab, sp_urls[i], true);
                play_system_sound((char*)"open");
            }
            sy0 += 32;
        }

        sy0 += 8;
        dui_text(cx + 14, sy0, "WEB BOOKMARKS", THEME_TEXT_MUTED, 1);
        sy0 += 20;
        const char* wb_lbls[4] = { "example.com", "info.cern.ch", "neverssl.com", "httpforever.com" };
        const char* wb_urls[4] = { "http://example.com/", "http://info.cern.ch/", "http://neverssl.com/", "http://httpforever.com/" };
        for (int i = 0; i < 4; i++) {
            bool act = str_ieq(active->url, wb_urls[i]);
            if (dui_icon_pill(cx + 10, sy0, sb_w - 20, 28, wb_lbls[i], 17, act,
                              in->mx, in->my, !overlay_blocks_bg && in->left_pressed)) {
                navigate_tab(g_active_tab, wb_urls[i], true);
                play_system_sound((char*)"click");
            }
            sy0 += 32;
        }

        // Bottom Network Mini-Card inside Sidebar
        if (vh + console_h >= 340) {
            NetStatus ns;
            net_get_status(&ns);
            int card_y = vy + vh + console_h - 66;
            dui_bento_card(cx + 10, card_y, sb_w - 20, 56, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 224, false);
            dui_circle(cx + 22, card_y + 18, 4, ns.dhcp_bound ? THEME_ACCENT_GREEN : (ns.link_up ? THEME_ACCENT_AMBER : THEME_ACCENT_ROSE), 255);
            dui_text(cx + 32, card_y + 11, ns.dhcp_bound ? "SNP Online" : (ns.link_up ? "Link Up" : "Offline"), THEME_TEXT_PRIMARY, 1);
            dui_text_ellipsis(cx + 20, card_y + 32, sb_w - 40, ns.dhcp_bound ? ns.ip_str : "No DHCP IP", THEME_TEXT_SECONDARY, 1);
        }
        dui_pop_clip();
    }

    // ========================================================================
    // 5. Clipped Content Viewport & Scrollbar
    // ========================================================================
    dui_rect(vx, vy, vw, vh, THEME_BG_VOID, 235);
    dui_push_clip(vx, vy, vw, vh);

    char hovered_href[512];
    hovered_href[0] = '\0';
    bool allow_vp_click = !overlay_blocks_bg && !g_scrollbar_drag;

    if (active->page_mode == PAGE_START) {
        render_start_page(active, vx, vy, vw, vh, in, allow_vp_click);
    } else if (active->page_mode == PAGE_PULSE) {
        render_pulse_page(active, vx, vy, vw, vh, in, allow_vp_click);
    } else if (active->page_mode == PAGE_DOWNLOADS) {
        render_downloads_page(active, vx, vy, vw, vh, in, allow_vp_click);
    } else if (active->page_mode == PAGE_HTTPS_NOTICE) {
        render_https_notice_page(active, vx, vy, vw, vh, in, allow_vp_click);
    } else if (active->page_mode == PAGE_ERROR) {
        render_error_page(active, vx, vy, vw, vh, in, allow_vp_click);
    } else {
        render_http_document(active, vx, vy, vw, vh, in, allow_vp_click, hovered_href);
    }

    // Scrollbar
    max_scroll = active->content_height - vh;
    if (max_scroll < 0) max_scroll = 0;
    active->scroll_y = horizon_clampi(active->scroll_y, 0, max_scroll);

    if (max_scroll > 0) {
        int track_x = vx + vw - 10;
        int track_y = vy + 6;
        int track_h = vh - 12;
        int thumb_h = (vh * track_h) / active->content_height;
        if (thumb_h < 28) thumb_h = 28;
        if (thumb_h > track_h) thumb_h = track_h;
        int thumb_y = track_y + ((track_h - thumb_h) * active->scroll_y) / max_scroll;

        bool hov_sb_track = !overlay_blocks_bg && pt_in(in->mx, in->my, track_x - 4, track_y, 14, track_h);
        if (hov_sb_track && in->left_pressed) {
            if (in->my >= thumb_y && in->my <= thumb_y + thumb_h) {
                g_scrollbar_drag = true;
                g_scrollbar_grab_off = in->my - thumb_y;
            } else {
                int target_ty = horizon_clampi(in->my - thumb_h / 2 - track_y, 0, track_h - thumb_h);
                active->scroll_y = (track_h > thumb_h) ? (target_ty * max_scroll) / (track_h - thumb_h) : 0;
            }
        }
        if (g_scrollbar_drag) {
            if (!in->left_down) {
                g_scrollbar_drag = false;
            } else if (track_h > thumb_h) {
                int new_ty = horizon_clampi(in->my - g_scrollbar_grab_off - track_y, 0, track_h - thumb_h);
                active->scroll_y = (new_ty * max_scroll) / (track_h - thumb_h);
                thumb_y = track_y + new_ty;
            }
        }

        dui_rect_rounded(track_x, track_y, 6, track_h, 3, THEME_SURFACE_INPUT, 170);
        dui_rect_rounded(track_x, thumb_y, 6, thumb_h, 3,
                         (hov_sb_track || g_scrollbar_drag) ? THEME_ACCENT_CYAN : THEME_BORDER_SPECULAR, 230);
    } else {
        g_scrollbar_drag = false;
    }

    // Floating Hover Link Preview Pill (bottom-left of viewport)
    if (hovered_href[0]) {
        int lw = dui_text_width(hovered_href, 1) + 20;
        if (lw > vw - 30) lw = vw - 30;
        int lx = vx + 10;
        int ly = vy + vh - 28;
        dui_rect_rounded(lx, ly, lw, 22, 6, THEME_SURFACE_ELEVATED, 238);
        dui_rect_rounded_outline(lx, ly, lw, 22, 6, THEME_BORDER_SPECULAR, 200);
        dui_text_ellipsis(lx + 10, ly + 4, lw - 20, hovered_href, THEME_ACCENT_CYAN, 1);
    }

    // Floating Alert Banner from window.alert()
    if (g_js_alert_ticks > 0 && g_js_alert_banner[0]) {
        int aw = dui_text_width(g_js_alert_banner, 1) + 96;
        if (aw > vw - 36) aw = vw - 36;
        if (aw < 220) aw = 220;
        int ax = vx + (vw - aw) / 2;
        int ay = vy + 14;
        dui_shadow(ax, ay, aw, 36, THEME_RADIUS_PILL, 6);
        dui_bento_card(ax, ay, aw, 36, THEME_RADIUS_PILL, THEME_SURFACE_ELEVATED, 245, true);
        dui_status_pill(ax + 8, ay + 6, 72, 24, "JS alert", THEME_ACCENT_AMBER, THEME_SURFACE_INPUT, true);
        dui_text_ellipsis(ax + 88, ay + 11, aw - 102, g_js_alert_banner, THEME_TEXT_PRIMARY, 1);
    }

    dui_pop_clip();

    // ========================================================================
    // 5B. BlankJS DevTools Console & Live REPL Drawer
    // ========================================================================
    if (g_console_open) {
        int csy = vy + vh;
        dui_rect(vx, csy, vw, console_h, THEME_BG_DEEP, 244);
        dui_line(vx, csy, vx + vw, csy, THEME_BORDER_SPECULAR, 210);

        // Drawer Header
        dui_rect(vx, csy, vw, 28, THEME_SURFACE_CARD, 220);
        dui_line(vx, csy + 27, vx + vw, csy + 27, THEME_BORDER_SUBTLE, 180);
        dui_text(vx + 12, csy + 7, "BlankJS Console  (Ctrl+J)", THEME_ACCENT_CYAN, 1);

        if (dui_button_tactile(vx + vw - 134, csy + 3, 62, 22, "Clear", THEME_SURFACE_ELEVATED, false,
                               in->mx, in->my, in->left_down, !overlay_blocks_bg && in->left_pressed)) {
            if (active->js_rt) js_console_clear(active->js_rt);
            play_system_sound((char*)"tab");
        }
        if (dui_button_tactile(vx + vw - 66, csy + 3, 56, 22, "Close", THEME_SURFACE_ELEVATED, false,
                               in->mx, in->my, in->left_down, !overlay_blocks_bg && in->left_pressed)) {
            g_console_open = false;
            g_console_input_focused = false;
            play_system_sound((char*)"close");
        }

        // Scrollback Console Output Lines (latest 5 lines)
        dui_push_clip(vx + 8, csy + 30, vw - 16, console_h - 68);
        int log_y = csy + 32;
        if (!active->js_rt || js_console_count(active->js_rt) == 0) {
            dui_text(vx + 12, log_y + 4, "Tip: type expressions like 2 + 2, document.title, or document.getElementById('status').textContent = 'Hi!'",
                     THEME_TEXT_MUTED, 1);
        } else {
            int tot = js_console_count(active->js_rt);
            int start = (tot > 5) ? (tot - 5) : 0;
            for (int li = start; li < tot; li++) {
                const JsConsoleLine* cl = js_console_get(active->js_rt, li);
                if (!cl) continue;
                uint32_t lcol = THEME_TEXT_PRIMARY;
                const char* pfx = "> ";
                if (cl->level == 1) { lcol = THEME_ACCENT_AMBER; pfx = "! "; }
                else if (cl->level == 2) { lcol = THEME_ACCENT_ROSE; pfx = "x "; }
                else if (cl->level == 3) { lcol = THEME_ACCENT_CYAN; pfx = "i "; }
                else if (cl->level == 4) { lcol = THEME_ACCENT_TEAL; pfx = "<- "; }
                char line_txt[160];
                str_copy(line_txt, pfx, sizeof(line_txt));
                str_append(line_txt, cl->text, sizeof(line_txt));
                dui_text_ellipsis(vx + 10, log_y, vw - 22, line_txt, lcol, 1);
                log_y += 17;
            }
        }
        dui_pop_clip();

        // REPL Prompt Bar at bottom of drawer
        int ry = csy + console_h - 34;
        int rw = vw - 86;
        if (rw < 120) rw = 120;
        bool hov_repl = !overlay_blocks_bg && pt_in(in->mx, in->my, vx + 10, ry, rw, 26);
        dui_rect_rounded(vx + 10, ry, rw, 26, THEME_RADIUS_PILL, THEME_SURFACE_INPUT, 245);
        dui_rect_rounded_outline(vx + 10, ry, rw, 26, THEME_RADIUS_PILL,
                                 g_console_input_focused ? THEME_BORDER_FOCUS : THEME_BORDER_SUBTLE, 210);
        dui_text(vx + 20, ry + 6, ">", THEME_ACCENT_CYAN, 1);
        dui_push_clip(vx + 32, ry + 2, rw - 42, 22);
        if (g_console_input_len > 0) {
            dui_text(vx + 34, ry + 6, g_console_input, THEME_TEXT_PRIMARY, 1);
            if (g_console_input_focused && ((in->frame_ms / 450) % 2 == 0)) {
                int cw_r = dui_text_width(g_console_input, 1);
                dui_rect(vx + 35 + cw_r, ry + 5, 2, 16, THEME_ACCENT_CYAN, 255);
            }
        } else {
            dui_text(vx + 34, ry + 6, "Evaluate JS expression...", THEME_TEXT_MUTED, 1);
            if (g_console_input_focused && ((in->frame_ms / 450) % 2 == 0)) {
                dui_rect(vx + 35, ry + 5, 2, 16, THEME_ACCENT_CYAN, 255);
            }
        }
        dui_pop_clip();

        if (hov_repl && in->left_pressed) {
            g_console_input_focused = true;
            g_omni_focused = false;
            active->active_input_dom_id = -1;
        }

        if (dui_button_tactile(vx + 16 + rw, ry, 62, 26, "Run", THEME_ACCENT_CYAN, true,
                               in->mx, in->my, in->left_down, !overlay_blocks_bg && in->left_pressed)) {
            g_console_input[g_console_input_len] = '\0';
            if (g_console_input_len > 0) {
                if (!active->js_rt) active->js_rt = js_runtime_create();
                if (active->js_rt) {
                    js_eval(active->js_rt, g_console_input, NULL, 0);
                    js_handle_outputs(active);
                    if (js_dom_is_dirty(active->js_rt)) {
                        js_dom_clear_dirty(active->js_rt);
                        active->layout_dirty = true;
                    }
                }
                g_console_input_len = 0;
                g_console_input[0] = '\0';
                play_system_sound((char*)"tool");
            }
        }
    }

    // ========================================================================
    // 6. Live Download Popover (Floating above viewport)
    // ========================================================================
    if (g_dl_popover_open) {
        dui_shadow(dl_pop_x, dl_pop_y, dl_pop_w, dl_pop_h, THEME_RADIUS_LG, 8);
        dui_bento_card(dl_pop_x, dl_pop_y, dl_pop_w, dl_pop_h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 245, true);

        dui_text(dl_pop_x + 16, dl_pop_y + 14, "Downloads (\\DOWNLOADS\\)", THEME_TEXT_PRIMARY, 1);

        if (dui_button(dl_pop_x + dl_pop_w - 134, dl_pop_y + 10, 62, 24, "All", THEME_SURFACE_ELEVATED, false,
                       in->mx, in->my, in->left_pressed)) {
            g_dl_popover_open = false;
            navigate_tab(g_active_tab, "blank://downloads", true);
            return;
        }
        if (dui_button(dl_pop_x + dl_pop_w - 68, dl_pop_y + 10, 56, 24, "Clear", THEME_SURFACE_ELEVATED, false,
                       in->mx, in->my, in->left_pressed)) {
            net_clear_finished_downloads();
        }

        if (dl_cnt == 0) {
            dui_text(dl_pop_x + 18, dl_pop_y + 104, "No downloads yet.", THEME_TEXT_SECONDARY, 1);
            dui_text(dl_pop_x + 18, dl_pop_y + 128, "Click 'Save' in toolbar or right-click a link.", THEME_TEXT_MUTED, 1);
        } else {
            int max_show = (dl_cnt > 4) ? 4 : dl_cnt;
            for (int i = 0; i < max_show; i++) {
                const DownloadEntry* de = net_get_download(dl_cnt - 1 - i);
                if (!de) continue;
                int ry = dl_pop_y + 46 + i * 58;
                dui_rect_rounded(dl_pop_x + 12, ry, dl_pop_w - 24, 52, 8, THEME_SURFACE_INPUT, 225);
                dui_text_ellipsis(dl_pop_x + 20, ry + 6, dl_pop_w - 122, de->filename, THEME_TEXT_PRIMARY, 1);

                char meta[64];
                char bs[20], spd[20];
                format_bytes((uint64_t)de->downloaded_bytes, bs, 20);
                str_copy(meta, bs, 64);
                if (de->speed_bps > 0 && de->state == HTTP_RECEIVING) {
                    format_bytes((uint64_t)de->speed_bps, spd, 20);
                    str_append(meta, " - ", 64);
                    str_append(meta, spd, 64);
                    str_append(meta, "/s", 64);
                } else {
                    str_append(meta, " - ", 64);
                    str_append(meta, de->status_text, 64);
                }
                dui_text_ellipsis(dl_pop_x + 20, ry + 24, dl_pop_w - 122, meta, THEME_TEXT_SECONDARY, 1);

                // Progress bar
                int pb_w = dl_pop_w - 124;
                dui_rect_rounded(dl_pop_x + 20, ry + 42, pb_w, 4, 2, THEME_BORDER_SUBTLE, 220);
                float frac = (de->state == HTTP_DONE) ? 1.0f :
                    ((de->total_bytes > 0) ? horizon_clampf((float)de->downloaded_bytes / (float)de->total_bytes, 0.08f, 0.96f) : 0.4f);
                dui_rect_rounded(dl_pop_x + 20, ry + 42, (int)(pb_w * frac), 4, 2,
                                 de->state == HTTP_DONE ? THEME_ACCENT_GREEN :
                                 (de->state == HTTP_ERROR ? THEME_ACCENT_ROSE : THEME_ACCENT_CYAN), 255);

                if (de->state == HTTP_DONE && de->saved_ok) {
                    if (dui_button(dl_pop_x + dl_pop_w - 94, ry + 11, 74, 28, "Open", THEME_ACCENT_CYAN, true,
                                   in->mx, in->my, in->left_pressed)) {
                        g_dl_popover_open = false;
                        wm_request_open_file(de->disk_path);
                        return;
                    }
                }
            }
        }

        if (in->left_pressed && !pt_in(in->mx, in->my, dl_pop_x, dl_pop_y, dl_pop_w, dl_pop_h)) {
            g_dl_popover_open = false;
        }
    }

    // ========================================================================
    // 7. Right-Click Link Context Menu Popover
    // ========================================================================
    if (g_ctx_open) {
        dui_shadow(g_ctx_x, g_ctx_y, ctx_w, ctx_h, THEME_RADIUS_MD, 6);
        dui_glass_card(g_ctx_x, g_ctx_y, ctx_w, ctx_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 245, true);

        const char* opts[3] = { "Open Link", "Open in New Tab", "Download Link" };
        for (int i = 0; i < 3; i++) {
            int ry = g_ctx_y + 8 + i * 32;
            bool hov = pt_in(in->mx, in->my, g_ctx_x + 6, ry, ctx_w - 12, 28);
            if (hov) dui_rect_rounded(g_ctx_x + 6, ry, ctx_w - 12, 28, 6, THEME_ACCENT_CYAN, 50);
            dui_text(g_ctx_x + 14, ry + 6, opts[i], (i == 2) ? THEME_ACCENT_TEAL : THEME_TEXT_PRIMARY, 1);
            if (hov && in->left_pressed) {
                g_ctx_open = false;
                if (i == 0) {
                    navigate_tab(g_active_tab, g_ctx_href, true);
                } else if (i == 1) {
                    if (g_tab_count < MAX_TABS) create_new_tab(g_ctx_href, true);
                    else navigate_tab(g_active_tab, g_ctx_href, true);
                } else {
                    start_file_download(g_ctx_href, NULL);
                }
                return;
            }
        }

        if (in->left_pressed && !pt_in(in->mx, in->my, g_ctx_x, g_ctx_y, ctx_w, ctx_h)) {
            g_ctx_open = false;
        }
    }
}

// ============================================================================
// Required Public Exports
// ============================================================================

extern "C" {

void browser_register(void) {
    wm_register_app(APP_BROWSER, "Horizon Web", 860, 580, THEME_ACCENT_CYAN, browser_render, browser_close);
}

void launch_browser(EFI_SYSTEM_TABLE* st) {
    (void)st;
    wm_open_app(APP_BROWSER);
}

} // extern "C"
