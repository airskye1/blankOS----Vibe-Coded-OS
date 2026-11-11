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
#include "../kernel/net.h"
#include "../kernel/audio.h"

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
    extern int g_wallpaper_theme;

    extern const char* kernel_get_boot_log(void);
    extern void os_restart(void);
    extern void os_shutdown(void);

    extern int pci_get_device_count(void);
    extern bool pci_get_device_info(int index, uint8_t* bus, uint8_t* dev, uint8_t* func,
                                    uint16_t* vendor, uint16_t* device, uint8_t* class_code,
                                    uint8_t* subclass, uint32_t* bar0, const char** vendor_str,
                                    const char** dev_str, const char** drv_str);
    extern const char* pci_get_cpu_vendor(void);
    extern const char* pci_get_cpu_brand(void);

    extern uint64_t memory_get_total_mb(void);
    extern uint64_t memory_get_used_mb(void);
    extern uint64_t memory_get_free_mb(void);
    extern uint32_t memory_get_alloc_count(void);

    extern void vfs_init(EFI_SYSTEM_TABLE *st);
    extern int vfs_get_file_count(void);
    extern const char* vfs_get_file_name(int idx);
    extern int vfs_get_file_size(int idx);
    extern bool vfs_read_file(const char* name, char* out_buf, int max_len);
    extern bool vfs_write_file(const char* name, const char* content);
    extern bool vfs_delete_file(const char* name);

    extern void blankReg_set_int(const char* key, int val);

    typedef struct {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t month;
        uint32_t year;
    } RTC_Time;
    extern void get_rtc_time(RTC_Time *time);
}

#define TERM_MAX_LINES   256
#define TERM_LINE_LEN    112
#define TERM_HIST_MAX    24
#define TERM_INPUT_MAX   160

typedef struct {
    char text[TERM_LINE_LEN];
    uint32_t color;
} TermLine;

static TermLine g_term_lines[TERM_MAX_LINES];
static int      g_term_count = 0;
static int      g_term_scroll = 0;
static char     g_term_input[TERM_INPUT_MAX] = "";
static int      g_term_input_len = 0;
static char     g_term_hist[TERM_HIST_MAX][TERM_INPUT_MAX];
static int      g_term_hist_count = 0;
static int      g_term_hist_pos = -1;
static int      g_term_http_id = -1;
static bool     g_term_bootstrapped = false;

static int tm_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void tm_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void tm_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int dl = tm_len(dst);
    int i = 0;
    while (src[i] && (dl + i) < max_len - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = '\0';
}

static bool tm_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static bool tm_starts(const char* s, const char* pfx) {
    if (!s || !pfx) return false;
    for (int i = 0; pfx[i]; i++) {
        if (s[i] != pfx[i]) return false;
    }
    return true;
}

static void tm_u64_str(uint64_t val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[24];
    int p = 0;
    while (val > 0 && p < 22) {
        tmp[p++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (p > 0 && j < max_len - 1) out[j++] = tmp[--p];
    out[j] = '\0';
}

static void tm_i64_str(int64_t val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val < 0) {
        out[0] = '-';
        tm_u64_str((uint64_t)(-val), out + 1, max_len - 1);
    } else {
        tm_u64_str((uint64_t)val, out, max_len);
    }
}

static void tm_hex4(uint16_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = hex[(val >> 12) & 0xF];
    out[1] = hex[(val >> 8) & 0xF];
    out[2] = hex[(val >> 4) & 0xF];
    out[3] = hex[val & 0xF];
    out[4] = '\0';
}

static void tm_hex2(uint8_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = hex[(val >> 4) & 0xF];
    out[1] = hex[val & 0xF];
    out[2] = '\0';
}

static void term_push_line(const char* text, uint32_t color) {
    if (!text) return;
    int len = tm_len(text);
    if (len == 0) {
        if (g_term_count >= TERM_MAX_LINES) {
            for (int i = 0; i < TERM_MAX_LINES - 1; i++) g_term_lines[i] = g_term_lines[i + 1];
            g_term_count = TERM_MAX_LINES - 1;
        }
        g_term_lines[g_term_count].text[0] = '\0';
        g_term_lines[g_term_count].color = color;
        g_term_count++;
        return;
    }

    int pos = 0;
    while (pos < len) {
        if (g_term_count >= TERM_MAX_LINES) {
            for (int i = 0; i < TERM_MAX_LINES - 1; i++) g_term_lines[i] = g_term_lines[i + 1];
            g_term_count = TERM_MAX_LINES - 1;
        }
        int chunk = len - pos;
        if (chunk > TERM_LINE_LEN - 1) chunk = TERM_LINE_LEN - 1;
        for (int k = 0; k < chunk; k++) {
            char c = text[pos + k];
            g_term_lines[g_term_count].text[k] = (c >= 32 && c <= 126) ? c : ' ';
        }
        g_term_lines[g_term_count].text[chunk] = '\0';
        g_term_lines[g_term_count].color = color;
        g_term_count++;
        pos += chunk;
    }
    g_term_scroll = g_term_count;
}

static void term_push_multiline(const char* text, uint32_t color, int max_lines) {
    if (!text) return;
    char line[TERM_LINE_LEN];
    int lp = 0;
    int emitted = 0;
    for (int i = 0; text[i]; i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n' || lp >= TERM_LINE_LEN - 1) {
            line[lp] = '\0';
            term_push_line(line, color);
            lp = 0;
            emitted++;
            if (max_lines > 0 && emitted >= max_lines) return;
            if (c != '\n') line[lp++] = (c >= 32 && c <= 126) ? c : '.';
        } else {
            line[lp++] = (c >= 32 && c <= 126) ? c : '.';
        }
    }
    if (lp > 0) {
        line[lp] = '\0';
        term_push_line(line, color);
    }
}

static const char* skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static int next_token(const char* s, char* out, int max_len, const char** rest) {
    s = skip_ws(s);
    int i = 0;
    while (s && *s && *s != ' ' && *s != '\t') {
        if (i < max_len - 1) out[i++] = *s;
        s++;
    }
    out[i] = '\0';
    if (rest) *rest = skip_ws(s);
    return i;
}

static void term_bootstrap(void) {
    if (g_term_bootstrapped) return;
    g_term_bootstrapped = true;
    g_term_count = 0;
    term_push_line("BlankOS Horizon Shell (blanksh v2.4)", THEME_ACCENT_GREEN);
    term_push_line("Type 'help' for filesystem, hardware, and network commands.", THEME_TEXT_SECONDARY);
    term_push_line("", THEME_TEXT_PRIMARY);
}

static void term_exec(const char* raw_cmd) {
    const char* cmd = skip_ws(raw_cmd);
    char prompt_echo[TERM_INPUT_MAX + 24];
    tm_copy(prompt_echo, "horizon$ ", sizeof(prompt_echo));
    tm_cat(prompt_echo, raw_cmd, sizeof(prompt_echo));
    term_push_line(prompt_echo, THEME_ACCENT_CYAN);

    if (!cmd || !cmd[0]) return;

    // Save history
    if (g_term_hist_count == 0 || !tm_eq(g_term_hist[g_term_hist_count - 1], cmd)) {
        if (g_term_hist_count < TERM_HIST_MAX) {
            tm_copy(g_term_hist[g_term_hist_count++], cmd, TERM_INPUT_MAX);
        } else {
            for (int i = 0; i < TERM_HIST_MAX - 1; i++) {
                tm_copy(g_term_hist[i], g_term_hist[i + 1], TERM_INPUT_MAX);
            }
            tm_copy(g_term_hist[TERM_HIST_MAX - 1], cmd, TERM_INPUT_MAX);
        }
    }
    g_term_hist_pos = -1;

    char verb[32];
    const char* args = NULL;
    next_token(cmd, verb, sizeof(verb), &args);

    if (tm_eq(verb, "help")) {
        term_push_line("Available Commands:", THEME_ACCENT_GREEN);
        term_push_line("  ls                        List VFS & FAT32 files", THEME_TEXT_PRIMARY);
        term_push_line("  cat <file>                Display file contents", THEME_TEXT_PRIMARY);
        term_push_line("  write <file> <text>       Write text to FAT32 / VFS", THEME_TEXT_PRIMARY);
        term_push_line("  rm <file>                 Delete a file", THEME_TEXT_PRIMARY);
        term_push_line("  mem | pci | sysinfo       Hardware, RAM, & PCIe telemetry", THEME_TEXT_PRIMARY);
        term_push_line("  netstat | dhcp            UEFI SNP NIC status & DHCP lease", THEME_TEXT_PRIMARY);
        term_push_line("  ping <host>               ICMP Echo Request (RTT ms)", THEME_TEXT_PRIMARY);
        term_push_line("  httpget <url> [save_as]   Live HTTP/1.1 GET transfer", THEME_TEXT_PRIMARY);
        term_push_line("  downloads                 List \\DOWNLOADS\\ transfers", THEME_TEXT_PRIMARY);
        term_push_line("  theme <0-4>               Switch Horizon Aurora wallpaper", THEME_TEXT_PRIMARY);
        term_push_line("  dmesg | clear             Kernel boot log / clear terminal", THEME_TEXT_PRIMARY);
        term_push_line("  reboot | shutdown         ACPI / UEFI power control", THEME_TEXT_PRIMARY);
    } else if (tm_eq(verb, "clear")) {
        g_term_count = 0;
        g_term_scroll = 0;
    } else if (tm_eq(verb, "ls")) {
        vfs_init(gSystemTable);
        int cnt = vfs_get_file_count();
        char hdr[64], nbuf[16];
        tm_u64_str((uint64_t)cnt, nbuf, sizeof(nbuf));
        tm_copy(hdr, "VFS / FAT32 Entries (", sizeof(hdr));
        tm_cat(hdr, nbuf, sizeof(hdr));
        tm_cat(hdr, "):", sizeof(hdr));
        term_push_line(hdr, THEME_ACCENT_CYAN);
        for (int i = 0; i < cnt; i++) {
            const char* fn = vfs_get_file_name(i);
            int sz = vfs_get_file_size(i);
            char szb[16], line[112];
            tm_u64_str((uint64_t)sz, szb, sizeof(szb));
            tm_copy(line, "  ", sizeof(line));
            tm_cat(line, fn, sizeof(line));
            tm_cat(line, "  (", sizeof(line));
            tm_cat(line, szb, sizeof(line));
            tm_cat(line, " bytes)", sizeof(line));
            term_push_line(line, THEME_TEXT_PRIMARY);
        }
    } else if (tm_eq(verb, "cat")) {
        char fname[64];
        if (next_token(args, fname, sizeof(fname), NULL) == 0) {
            term_push_line("Usage: cat <filename>", THEME_ACCENT_AMBER);
        } else {
            static char fbuf[4096];
            if (vfs_read_file(fname, fbuf, sizeof(fbuf))) {
                term_push_multiline(fbuf, THEME_TEXT_PRIMARY, 80);
            } else {
                term_push_line("cat: file not found", THEME_ACCENT_ROSE);
            }
        }
    } else if (tm_eq(verb, "write")) {
        char fname[64];
        const char* body = NULL;
        if (next_token(args, fname, sizeof(fname), &body) == 0 || !body || !body[0]) {
            term_push_line("Usage: write <filename> <text>", THEME_ACCENT_AMBER);
        } else if (vfs_write_file(fname, body)) {
            term_push_line("Written to VFS & FAT32.", THEME_ACCENT_GREEN);
        } else {
            term_push_line("write: failed", THEME_ACCENT_ROSE);
        }
    } else if (tm_eq(verb, "rm")) {
        char fname[64];
        if (next_token(args, fname, sizeof(fname), NULL) == 0) {
            term_push_line("Usage: rm <filename>", THEME_ACCENT_AMBER);
        } else if (vfs_delete_file(fname)) {
            term_push_line("File removed.", THEME_ACCENT_GREEN);
        } else {
            term_push_line("rm: file not found", THEME_ACCENT_ROSE);
        }
    } else if (tm_eq(verb, "mem")) {
        uint64_t tot = memory_get_total_mb();
        uint64_t used = memory_get_used_mb();
        uint64_t free_mb = memory_get_free_mb();
        uint32_t allocs = memory_get_alloc_count();
        char b1[20], b2[20], b3[20], b4[20], line[112];
        tm_u64_str(tot, b1, sizeof(b1));
        tm_u64_str(used, b2, sizeof(b2));
        tm_u64_str(free_mb, b3, sizeof(b3));
        tm_u64_str(allocs, b4, sizeof(b4));
        tm_copy(line, "Physical RAM: Total ", sizeof(line));
        tm_cat(line, b1, sizeof(line));
        tm_cat(line, " MB | Used ", sizeof(line));
        tm_cat(line, b2, sizeof(line));
        tm_cat(line, " MB | Free ", sizeof(line));
        tm_cat(line, b3, sizeof(line));
        tm_cat(line, " MB", sizeof(line));
        term_push_line(line, THEME_ACCENT_GREEN);

        tm_copy(line, "Kernel Heap Allocations: ", sizeof(line));
        tm_cat(line, b4, sizeof(line));
        term_push_line(line, THEME_TEXT_SECONDARY);
    } else if (tm_eq(verb, "pci")) {
        int cnt = pci_get_device_count();
        char nb[12], hdr[64];
        tm_u64_str((uint64_t)cnt, nb, sizeof(nb));
        tm_copy(hdr, "PCIe Devices (", sizeof(hdr));
        tm_cat(hdr, nb, sizeof(hdr));
        tm_cat(hdr, "):", sizeof(hdr));
        term_push_line(hdr, THEME_ACCENT_CYAN);

        for (int i = 0; i < cnt; i++) {
            uint8_t bus = 0, dev = 0, fn = 0, cc = 0, sc = 0;
            uint16_t vid = 0, did = 0;
            uint32_t bar0 = 0;
            const char* vstr = NULL;
            const char* dstr = NULL;
            const char* drv = NULL;
            if (pci_get_device_info(i, &bus, &dev, &fn, &vid, &did, &cc, &sc, &bar0, &vstr, &dstr, &drv)) {
                char hb[4], hd[4], hv[6], hdid[6], line[112];
                tm_hex2(bus, hb); tm_hex2(dev, hd);
                tm_hex4(vid, hv); tm_hex4(did, hdid);
                tm_copy(line, "  ", sizeof(line));
                tm_cat(line, hb, sizeof(line));
                tm_cat(line, ":", sizeof(line));
                tm_cat(line, hd, sizeof(line));
                tm_cat(line, ".", sizeof(line));
                char fch[2] = { (char)('0' + (fn & 7)), '\0' };
                tm_cat(line, fch, sizeof(line));
                tm_cat(line, " [", sizeof(line));
                tm_cat(line, hv, sizeof(line));
                tm_cat(line, ":", sizeof(line));
                tm_cat(line, hdid, sizeof(line));
                tm_cat(line, "] ", sizeof(line));
                tm_cat(line, dstr ? dstr : "Device", sizeof(line));
                term_push_line(line, THEME_TEXT_PRIMARY);
            }
        }
    } else if (tm_eq(verb, "sysinfo")) {
        char line[112], b1[16], b2[16];
        term_push_line("BlankOS Horizon x86_64 UEFI", THEME_ACCENT_GREEN);
        tm_copy(line, "  CPU    : ", sizeof(line));
        tm_cat(line, pci_get_cpu_brand(), sizeof(line));
        tm_cat(line, " (", sizeof(line));
        tm_cat(line, pci_get_cpu_vendor(), sizeof(line));
        tm_cat(line, ")", sizeof(line));
        term_push_line(line, THEME_TEXT_PRIMARY);

        tm_u64_str(screen_width, b1, sizeof(b1));
        tm_u64_str(screen_height, b2, sizeof(b2));
        tm_copy(line, "  Display: ", sizeof(line));
        tm_cat(line, b1, sizeof(line));
        tm_cat(line, " x ", sizeof(line));
        tm_cat(line, b2, sizeof(line));
        term_push_line(line, THEME_TEXT_PRIMARY);

        tm_u64_str(memory_get_used_mb(), b1, sizeof(b1));
        tm_u64_str(memory_get_total_mb(), b2, sizeof(b2));
        tm_copy(line, "  Memory : ", sizeof(line));
        tm_cat(line, b1, sizeof(line));
        tm_cat(line, " / ", sizeof(line));
        tm_cat(line, b2, sizeof(line));
        tm_cat(line, " MB", sizeof(line));
        term_push_line(line, THEME_TEXT_PRIMARY);

        RTC_Time rtc;
        get_rtc_time(&rtc);
        char y[8], m[8], d[8], hh[8], mm[8];
        tm_u64_str(rtc.year, y, sizeof(y));
        tm_u64_str(rtc.month, m, sizeof(m));
        tm_u64_str(rtc.day, d, sizeof(d));
        tm_u64_str(rtc.hour, hh, sizeof(hh));
        tm_u64_str(rtc.minute, mm, sizeof(mm));
        tm_copy(line, "  RTC    : ", sizeof(line));
        tm_cat(line, y, sizeof(line)); tm_cat(line, "-", sizeof(line));
        tm_cat(line, m, sizeof(line)); tm_cat(line, "-", sizeof(line));
        tm_cat(line, d, sizeof(line)); tm_cat(line, " ", sizeof(line));
        tm_cat(line, hh, sizeof(line)); tm_cat(line, ":", sizeof(line));
        if (rtc.minute < 10) tm_cat(line, "0", sizeof(line));
        tm_cat(line, mm, sizeof(line));
        term_push_line(line, THEME_TEXT_SECONDARY);
    } else if (tm_eq(verb, "netstat")) {
        NetStatus ns;
        net_get_status(&ns);
        char line[112], b1[20], b2[20];
        if (!ns.nic_present) {
            term_push_line("NIC: No UEFI SNP adapter present.", THEME_ACCENT_ROSE);
        } else {
            tm_copy(line, "Adapter : ", sizeof(line));
            tm_cat(line, ns.adapter_name, sizeof(line));
            tm_cat(line, ns.link_up ? " [LINK UP]" : " [LINK DOWN]", sizeof(line));
            term_push_line(line, ns.link_up ? THEME_ACCENT_GREEN : THEME_ACCENT_AMBER);

            tm_copy(line, "MAC     : ", sizeof(line)); tm_cat(line, ns.mac_str, sizeof(line));
            term_push_line(line, THEME_TEXT_PRIMARY);

            tm_copy(line, "IPv4    : ", sizeof(line));
            tm_cat(line, ns.dhcp_bound ? ns.ip_str : "Unbound (DHCP needed)", sizeof(line));
            tm_cat(line, " | GW ", sizeof(line)); tm_cat(line, ns.gw_str, sizeof(line));
            tm_cat(line, " | DNS ", sizeof(line)); tm_cat(line, ns.dns_str, sizeof(line));
            term_push_line(line, THEME_TEXT_PRIMARY);

            tm_u64_str(ns.rx_packets, b1, sizeof(b1));
            tm_u64_str(ns.tx_packets, b2, sizeof(b2));
            tm_copy(line, "Traffic : RX ", sizeof(line)); tm_cat(line, b1, sizeof(line));
            tm_cat(line, " pkts (", sizeof(line));
            tm_u64_str(ns.rx_bytes, b1, sizeof(b1)); tm_cat(line, b1, sizeof(line));
            tm_cat(line, " B) | TX ", sizeof(line)); tm_cat(line, b2, sizeof(line));
            tm_cat(line, " pkts (", sizeof(line));
            tm_u64_str(ns.tx_bytes, b2, sizeof(b2)); tm_cat(line, b2, sizeof(line));
            tm_cat(line, " B)", sizeof(line));
            term_push_line(line, THEME_TEXT_SECONDARY);
        }
    } else if (tm_eq(verb, "dhcp")) {
        term_push_line("Requesting DHCPv4 lease...", THEME_TEXT_SECONDARY);
        if (net_dhcp_renew()) {
            NetStatus ns;
            net_get_status(&ns);
            char line[96];
            tm_copy(line, "DHCP Bound: IP ", sizeof(line));
            tm_cat(line, ns.ip_str, sizeof(line));
            tm_cat(line, " | GW ", sizeof(line));
            tm_cat(line, ns.gw_str, sizeof(line));
            term_push_line(line, THEME_ACCENT_GREEN);
        } else {
            term_push_line("DHCP failed or link down.", THEME_ACCENT_ROSE);
        }
    } else if (tm_eq(verb, "ping")) {
        char host[96];
        if (next_token(args, host, sizeof(host), NULL) == 0) {
            term_push_line("Usage: ping <host_or_ipv4>", THEME_ACCENT_AMBER);
        } else {
            int rtt = net_ping(host, 800);
            if (rtt >= 0) {
                char rb[16], line[96];
                tm_i64_str(rtt, rb, sizeof(rb));
                tm_copy(line, "Reply from ", sizeof(line));
                tm_cat(line, host, sizeof(line));
                tm_cat(line, ": time=", sizeof(line));
                tm_cat(line, rb, sizeof(line));
                tm_cat(line, " ms", sizeof(line));
                term_push_line(line, THEME_ACCENT_GREEN);
            } else {
                term_push_line("Ping timed out or unreachable.", THEME_ACCENT_ROSE);
            }
        }
    } else if (tm_eq(verb, "httpget")) {
        if (g_term_http_id >= 0) {
            term_push_line("An HTTP transfer is already active (Ctrl+C to abort).", THEME_ACCENT_AMBER);
        } else {
            char url_tok[128], save_tok[96];
            const char* rest = NULL;
            if (next_token(args, url_tok, sizeof(url_tok), &rest) == 0) {
                term_push_line("Usage: httpget <url> [save_as]", THEME_ACCENT_AMBER);
            } else {
                char full_url[144];
                if (tm_starts(url_tok, "http://") || tm_starts(url_tok, "https://")) {
                    tm_copy(full_url, url_tok, sizeof(full_url));
                } else {
                    tm_copy(full_url, "http://", sizeof(full_url));
                    tm_cat(full_url, url_tok, sizeof(full_url));
                }
                const char* save_ptr = (next_token(rest, save_tok, sizeof(save_tok), NULL) > 0) ? save_tok : NULL;
                int id = net_http_start(full_url, save_ptr);
                if (id < 0) {
                    term_push_line("httpget: unable to initiate request (check link/DHCP).", THEME_ACCENT_ROSE);
                } else {
                    g_term_http_id = id;
                    char msg[128];
                    tm_copy(msg, "Fetching ", sizeof(msg));
                    tm_cat(msg, full_url, sizeof(msg));
                    term_push_line(msg, THEME_TEXT_SECONDARY);
                }
            }
        }
    } else if (tm_eq(verb, "downloads")) {
        int cnt = net_download_count();
        if (cnt == 0) {
            term_push_line("No downloads recorded.", THEME_TEXT_MUTED);
        } else {
            for (int i = 0; i < cnt; i++) {
                const DownloadEntry* de = net_get_download(i);
                if (!de) continue;
                char sz[16], line[112];
                tm_i64_str(de->downloaded_bytes, sz, sizeof(sz));
                tm_copy(line, "  ", sizeof(line));
                tm_cat(line, de->filename, sizeof(line));
                tm_cat(line, " | ", sizeof(line));
                tm_cat(line, sz, sizeof(line));
                tm_cat(line, " B | ", sizeof(line));
                tm_cat(line, de->status_text, sizeof(line));
                term_push_line(line, (de->state == HTTP_DONE) ? THEME_ACCENT_GREEN : THEME_TEXT_SECONDARY);
            }
        }
    } else if (tm_eq(verb, "theme")) {
        char arg[16];
        if (next_token(args, arg, sizeof(arg), NULL) == 0 || arg[0] < '0' || arg[0] > '4' || arg[1] != '\0') {
            term_push_line("Usage: theme <0-4>", THEME_ACCENT_AMBER);
        } else {
            int tid = arg[0] - '0';
            dui_set_wallpaper_id(tid);
            g_wallpaper_theme = tid;
            blankReg_set_int("UI/WallpaperTheme", tid);
            term_push_line("Wallpaper theme updated.", THEME_ACCENT_GREEN);
        }
    } else if (tm_eq(verb, "dmesg")) {
        const char* log = kernel_get_boot_log();
        if (log && log[0]) term_push_multiline(log, THEME_TEXT_SECONDARY, 120);
        else term_push_line("(boot log empty)", THEME_TEXT_MUTED);
    } else if (tm_eq(verb, "reboot")) {
        os_restart();
    } else if (tm_eq(verb, "shutdown")) {
        os_shutdown();
    } else {
        char msg[96];
        tm_copy(msg, "Unknown command: ", sizeof(msg));
        tm_cat(msg, verb, sizeof(msg));
        tm_cat(msg, " (try 'help')", sizeof(msg));
        term_push_line(msg, THEME_ACCENT_ROSE);
    }
}

static bool term_pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void terminal_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    term_bootstrap();

    // Advance non-blocking HTTP transfer if one was started by `httpget`
    if (g_term_http_id >= 0) {
        const HttpTransfer* tr = net_http_get(g_term_http_id);
        if (!tr) {
            g_term_http_id = -1;
        } else if (tr->state == HTTP_DONE) {
            char line[112], sc[12], bytes[16];
            tm_i64_str(tr->status_code, sc, sizeof(sc));
            tm_i64_str(tr->bytes_received, bytes, sizeof(bytes));
            tm_copy(line, "HTTP ", sizeof(line));
            tm_cat(line, sc, sizeof(line));
            tm_cat(line, " | ", sizeof(line));
            tm_cat(line, bytes, sizeof(line));
            tm_cat(line, " bytes", sizeof(line));
            if (tr->content_type[0]) {
                tm_cat(line, " | ", sizeof(line));
                tm_cat(line, tr->content_type, sizeof(line));
            }
            term_push_line(line, THEME_ACCENT_GREEN);
            if (tr->save_to_disk) {
                tm_copy(line, "Saved to \\DOWNLOADS\\", sizeof(line));
                tm_cat(line, tr->save_filename, sizeof(line));
                term_push_line(line, THEME_ACCENT_CYAN);
            } else if (tr->body_buf && tr->bytes_received > 0) {
                term_push_multiline(tr->body_buf, THEME_TEXT_PRIMARY, 18);
            }
            net_http_close(g_term_http_id);
            g_term_http_id = -1;
            play_system_sound((char*)"success");
        } else if (tr->state == HTTP_ERROR) {
            term_push_line(tr->error_msg[0] ? tr->error_msg : "HTTP transfer error", THEME_ACCENT_ROSE);
            net_http_close(g_term_http_id);
            g_term_http_id = -1;
            play_system_sound((char*)"error");
        }
    }

    dui_rect(cx, cy, cw, ch, THEME_BG_DEEP, 210);

    bool click_avail = (in && in->left_pressed);
    bool down = (in && in->left_down);
    int mx = in ? in->mx : -1;
    int my = in ? in->my : -1;

    // Lira Bento Console Header Toolbar
    int hdr_x = cx + 8;
    int hdr_y = cy + 8;
    int hdr_w = cw - 16;
    int hdr_h = 40;
    dui_bento_card(hdr_x, hdr_y, hdr_w, hdr_h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 225, g_term_http_id >= 0);

    dui_draw_sys_icon(hdr_x + 12, hdr_y + 13, 14, 7, THEME_ACCENT_GREEN);
    dui_text(hdr_x + 32, hdr_y + 11, "blanksh", THEME_TEXT_PRIMARY, 1);

    NetStatus ns;
    net_get_status(&ns);
    const char* net_lbl = ns.dhcp_bound ? ns.ip_str : (ns.link_up ? "Link Up" : "Offline");
    uint32_t net_dot = ns.dhcp_bound ? THEME_ACCENT_GREEN : (ns.link_up ? THEME_ACCENT_AMBER : THEME_ACCENT_ROSE);
    if (hdr_w >= 340) {
        dui_status_pill(hdr_x + 100, hdr_y + 8, 132, 24, net_lbl, net_dot, THEME_SURFACE_INPUT, ns.link_up || ns.dhcp_bound);
    }
    if (hdr_w >= 510) {
        if (g_term_http_id >= 0) {
            dui_status_pill(hdr_x + 240, hdr_y + 8, 106, 24, "HTTP Live", THEME_ACCENT_AMBER, THEME_SURFACE_INPUT, true);
        } else {
            dui_status_pill(hdr_x + 240, hdr_y + 8, 102, 24, "Ready", THEME_ACCENT_CYAN, THEME_SURFACE_INPUT, false);
        }
    }

    if (dui_button_tactile(hdr_x + hdr_w - 134, hdr_y + 8, 56, 24, "Help", THEME_SURFACE_ELEVATED, false, mx, my, down, click_avail)) {
        click_avail = false;
        term_exec("help");
        play_system_sound((char*)"tool");
    }
    if (dui_button_tactile(hdr_x + hdr_w - 72, hdr_y + 8, 62, 24, "Clear", THEME_SURFACE_ELEVATED, false, mx, my, down, click_avail)) {
        click_avail = false;
        g_term_count = 0;
        g_term_scroll = 0;
        play_system_sound((char*)"close");
    }

    // Console Bento Output Container
    int body_x = cx + 8;
    int body_y = hdr_y + hdr_h + 8;
    int body_w = cw - 16;
    int body_h = ch - hdr_h - 24;
    if (body_h < 60) body_h = 60;

    dui_bento_card(body_x, body_y, body_w, body_h, THEME_RADIUS_LG, THEME_BG_VOID, 235, true);
    dui_dot_grid_overlay(body_x + 8, body_y + 8, body_w - 16, body_h - 16, 22, THEME_BORDER_SUBTLE, 24);

    int pad_x = 14;
    int text_y = body_y + 10;
    int text_h = body_h - 20;
    int line_h = 19;
    int vis_rows = text_h / line_h;
    if (vis_rows < 2) vis_rows = 2;

    // Reserve bottom row for prompt or HTTP progress
    int scroll_rows = vis_rows - 1;
    int max_scroll = g_term_count - scroll_rows;
    if (max_scroll < 0) max_scroll = 0;

    if (in && in->scroll_y != 0 && term_pt_in(mx, my, body_x, body_y, body_w, body_h)) {
        g_term_scroll -= in->scroll_y * 2;
    }

    // Keyboard input
    if (in && in->has_key) {
        if (in->ctrl_held && (in->key_char == 'c' || in->key_char == 'C')) {
            if (g_term_http_id >= 0) {
                net_http_close(g_term_http_id);
                g_term_http_id = -1;
                term_push_line("^C (HTTP transfer cancelled)", THEME_ACCENT_AMBER);
            } else {
                g_term_input_len = 0;
                g_term_input[0] = '\0';
                term_push_line("^C", THEME_ACCENT_AMBER);
            }
        } else if (in->ctrl_held && (in->key_char == 'l' || in->key_char == 'L')) {
            g_term_count = 0;
            g_term_scroll = 0;
        } else if (in->scan_code == BLANK_SCAN_PGUP) {
            g_term_scroll -= scroll_rows;
        } else if (in->scan_code == BLANK_SCAN_PGDN) {
            g_term_scroll += scroll_rows;
        } else if (g_term_http_id < 0) {
            if (in->scan_code == BLANK_SCAN_UP) {
                if (g_term_hist_count > 0) {
                    if (g_term_hist_pos < 0) g_term_hist_pos = g_term_hist_count - 1;
                    else if (g_term_hist_pos > 0) g_term_hist_pos--;
                    tm_copy(g_term_input, g_term_hist[g_term_hist_pos], sizeof(g_term_input));
                    g_term_input_len = tm_len(g_term_input);
                }
            } else if (in->scan_code == BLANK_SCAN_DOWN) {
                if (g_term_hist_pos >= 0) {
                    if (g_term_hist_pos + 1 < g_term_hist_count) {
                        g_term_hist_pos++;
                        tm_copy(g_term_input, g_term_hist[g_term_hist_pos], sizeof(g_term_input));
                        g_term_input_len = tm_len(g_term_input);
                    } else {
                        g_term_hist_pos = -1;
                        g_term_input[0] = '\0';
                        g_term_input_len = 0;
                    }
                }
            } else if (in->key_char == '\b') {
                if (g_term_input_len > 0) g_term_input[--g_term_input_len] = '\0';
            } else if (in->key_char == '\n') {
                char run_buf[TERM_INPUT_MAX];
                tm_copy(run_buf, g_term_input, sizeof(run_buf));
                g_term_input[0] = '\0';
                g_term_input_len = 0;
                term_exec(run_buf);
                max_scroll = g_term_count - scroll_rows;
                if (max_scroll < 0) max_scroll = 0;
                g_term_scroll = max_scroll;
            } else if (in->key_char >= 32 && in->key_char <= 126 && g_term_input_len < TERM_INPUT_MAX - 2) {
                g_term_input[g_term_input_len++] = in->key_char;
                g_term_input[g_term_input_len] = '\0';
            }
        }
    }

    if (g_term_scroll > max_scroll) g_term_scroll = max_scroll;
    if (g_term_scroll < 0) g_term_scroll = 0;

    dui_push_clip(body_x + 4, text_y, body_w - 8, text_h);
    int drawn_rows = 0;
    for (int i = 0; i < scroll_rows && (g_term_scroll + i) < g_term_count; i++) {
        TermLine* tl = &g_term_lines[g_term_scroll + i];
        int ly = text_y + i * line_h;
        dui_text(body_x + pad_x, ly, tl->text, tl->color, 1);
        drawn_rows++;
    }

    int py = text_y + drawn_rows * line_h;
    if (g_term_http_id >= 0) {
        const HttpTransfer* tr = net_http_get(g_term_http_id);
        char prog[80], nb[16];
        tm_i64_str(tr ? tr->bytes_received : 0, nb, sizeof(nb));
        tm_copy(prog, "[HTTP] Transferring... ", sizeof(prog));
        tm_cat(prog, nb, sizeof(prog));
        tm_cat(prog, " B (Ctrl+C to abort)", sizeof(prog));
        dui_text(body_x + pad_x, py, prog, THEME_ACCENT_AMBER, 1);
    } else {
        const char* pfx = "horizon$ ";
        dui_text(body_x + pad_x, py, pfx, THEME_ACCENT_GREEN, 1);
        int px_start = body_x + pad_x + dui_text_width(pfx, 1);
        dui_text(px_start, py, g_term_input, THEME_TEXT_PRIMARY, 1);

        bool blink = !in || (((in->frame_ms / 450) % 2) == 0);
        if (blink) {
            int cx_cur = px_start + dui_text_width(g_term_input, 1) + 1;
            dui_rect(cx_cur, py + 1, 8, 15, THEME_ACCENT_GREEN, 235);
        }
    }
    dui_pop_clip();
}

extern "C" {

void terminal_register(void) {
    wm_register_app(APP_TERMINAL, "Horizon Terminal", 720, 460, THEME_ACCENT_GREEN, terminal_render, NULL);
}

void launch_terminal(EFI_SYSTEM_TABLE* st) {
    (void)st;
    term_bootstrap();
    wm_open_app(APP_TERMINAL);
}

} // extern "C"
