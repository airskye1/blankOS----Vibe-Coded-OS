// BlankOS Terminal Emulator - Catppuccin Mocha themed graphical shell (blanksh)
// Freestanding C++ for UEFI. 100% Real Hardware, VFS, Crypto, and PCIe Integration.

#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern "C" {

// External drawing primitives
extern void swap_buffers();
extern void dui_draw_wallpaper();
extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
extern int  dui_text_width(const char* text, int scale);
extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
extern void blankUI_draw_menubar();
extern void blankUI_draw_dock();
extern void blankUI_draw_cursor(int x, int y);
extern int  screen_width;
extern int  screen_height;
extern int  g_wallpaper_theme;
extern bool set_screen_resolution(uint32_t target_w, uint32_t target_h);
extern void play_system_sound(char* sound_name);
extern void set_master_volume(float volume);
extern float get_master_volume(void);
extern void launch_updater(EFI_SYSTEM_TABLE *SystemTable);
extern void launch_blankpad(void);
extern void launch_task_manager(void);
extern void launch_blankreg_edit(void);
extern bool g_is_live_cd;

// Real Kernel & Driver Hooks
extern int pci_get_device_count(void);
extern bool pci_get_device_info(int index, uint8_t* bus, uint8_t* dev, uint8_t* func, uint16_t* vendor, uint16_t* device, uint8_t* class_code, uint8_t* subclass, uint32_t* bar0, const char** vendor_str, const char** dev_str, const char** drv_str);
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

extern void sha256_hash(char* input_string, uint8_t* output_hash);
extern const char* net_get_adapter_name(void);
extern const char* net_get_mac_str(void);
extern const char* net_get_ipv4_str(void);
extern uint32_t net_get_tx_packets(void);
extern uint32_t net_get_rx_packets(void);

extern int blankReg_get_count(void);
extern bool blankReg_get_entry(int index, char* key_out, int* type_out, int* int_out, char* str_out);
extern void os_restart(void);
extern void os_shutdown(void);

struct RTC_Time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
};
extern void get_rtc_time(RTC_Time *time);

// Catppuccin Mocha palette
static const uint32_t COL_BASE      = 0x1E1E2E;
static const uint32_t COL_MANTLE    = 0x181825;
static const uint32_t COL_SURFACE0  = 0x313244;
static const uint32_t COL_TEXT      = 0xCDD6F4;
static const uint32_t COL_SUBTEXT   = 0xA6ADC8;
static const uint32_t COL_GREEN     = 0xA6E3A1;
static const uint32_t COL_RED       = 0xF38BA8;
static const uint32_t COL_YELLOW    = 0xF9E2AF;
static const uint32_t COL_BLUE      = 0x89B4FA;
static const uint32_t COL_LAVENDER  = 0xB4BEFE;

static const uint32_t COL_TL_CLOSE    = 0xFF5F57;
static const uint32_t COL_TL_MINIMIZE = 0xFEBC2E;
static const uint32_t COL_TL_MAXIMIZE = 0x28C840;

static int str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static int str_starts_with(const char* str, const char* prefix) {
    if (!str || !prefix) return 0;
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

static void str_copy(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void str_cat(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void int_to_str(int val, char* buf) {
    if (val == 0) {
        buf[0] = '0'; buf[1] = '\0'; return;
    }
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    char tmp[16];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    for (int k = i - 1; k >= 0; k--) buf[j++] = tmp[k];
    buf[j] = '\0';
}

static void u16_to_hex4(uint16_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = hex[(val >> 12) & 0xF];
    out[1] = hex[(val >> 8) & 0xF];
    out[2] = hex[(val >> 4) & 0xF];
    out[3] = hex[val & 0xF];
    out[4] = '\0';
}

#define TERM_MAX_LINES     40
#define TERM_LINE_LEN      120
#define TERM_INPUT_LEN     120
#define TERM_TITLE_BAR_H   32
#define TERM_PADDING       12

typedef struct {
    char     text[TERM_LINE_LEN];
    uint32_t color;
} TermLine;

static TermLine g_lines[TERM_MAX_LINES];
static int      g_line_count = 0;

static void term_push_line(const char* text, uint32_t color) {
    if (g_line_count >= TERM_MAX_LINES) {
        for (int i = 0; i < TERM_MAX_LINES - 1; i++) {
            g_lines[i] = g_lines[i + 1];
        }
        g_line_count = TERM_MAX_LINES - 1;
    }
    int i = 0;
    while (text && text[i] && i < TERM_LINE_LEN - 1) {
        g_lines[g_line_count].text[i] = text[i];
        i++;
    }
    g_lines[g_line_count].text[i] = '\0';
    g_lines[g_line_count].color = color;
    g_line_count++;
}

static void term_clear(void) {
    g_line_count = 0;
}

static void term_execute_cmd(EFI_SYSTEM_TABLE* SystemTable, const char* input, int* win_x, int* win_y, int win_w, int win_h) {
    while (*input == ' ') input++;
    if (*input == '\0') return;

    char prompt_echo[TERM_INPUT_LEN + 16];
    str_copy(prompt_echo, "blankOS $ ");
    str_cat(prompt_echo, input);
    term_push_line(prompt_echo, COL_SUBTEXT);

    if (str_eq(input, "help")) {
        term_push_line("BlankOS Sequoia Shell (blanksh) — Real Hardware & VFS Commands:", COL_YELLOW);
        term_push_line("  ls                  - List real files in VFS & FAT32 volume", COL_TEXT);
        term_push_line("  cat <file>          - Print contents of a file (e.g. cat README.md)", COL_TEXT);
        term_push_line("  write <file> <text> - Write text to file on VFS & FAT32 disk", COL_TEXT);
        term_push_line("  rm <file>           - Delete file from VFS & FAT32 disk", COL_TEXT);
        term_push_line("  lspci               - Enumerate real PCIe hardware bus & drivers", COL_TEXT);
        term_push_line("  mem / free          - Show real UEFI physical RAM & heap usage", COL_TEXT);
        term_push_line("  cpu                 - Read hardware CPUID vendor & brand string", COL_TEXT);
        term_push_line("  ifconfig / net      - Show PCIe NIC, MAC address, IPv4 & packets", COL_TEXT);
        term_push_line("  disks               - Probe physical UEFI BlockIO storage drives", COL_TEXT);
        term_push_line("  sha256 <text>       - Compute real FIPS 180-4 SHA-256 hex digest", COL_TEXT);
        term_push_line("  reg                 - Dump active blankReg system registry keys", COL_TEXT);
        term_push_line("  res <wxh>           - Switch GOP mode (1024x768, 1280x720, 1920x1080)", COL_TEXT);
        term_push_line("  theme <0-3>         - Switch desktop wallpaper theme", COL_TEXT);
        term_push_line("  files / top / regedit - Launch BlankPad, Activity Monitor, RegEdit", COL_TEXT);
        term_push_line("  date / reboot / shutdown / clear / exit", COL_TEXT);
        return;
    }

    if (str_eq(input, "clear")) {
        term_clear();
        return;
    }

    if (str_starts_with(input, "echo ")) {
        term_push_line(input + 5, COL_TEXT);
        return;
    }

    if (str_eq(input, "ls")) {
        vfs_init(SystemTable);
        int cnt = vfs_get_file_count();
        term_push_line("FILENAME                 SIZE (BYTES)   STORAGE", COL_LAVENDER);
        for (int i = 0; i < cnt; i++) {
            const char* fn = vfs_get_file_name(i);
            int sz = vfs_get_file_size(i);
            char row[100];
            str_copy(row, "  ");
            str_cat(row, fn);
            int cur = 0; while (row[cur]) cur++;
            while (cur < 25) row[cur++] = ' ';
            row[cur] = '\0';
            char sz_s[16];
            int_to_str(sz, sz_s);
            str_cat(row, sz_s);
            str_cat(row, " B          FAT32 / VFS");
            term_push_line(row, COL_BLUE);
        }
        return;
    }

    if (str_starts_with(input, "cat ")) {
        const char* fname = input + 4;
        while (*fname == ' ') fname++;
        static char file_buf[2048];
        if (vfs_read_file(fname, file_buf, 2047)) {
            char line[TERM_LINE_LEN];
            int lp = 0;
            for (int i = 0; file_buf[i]; i++) {
                if (file_buf[i] == '\r') continue;
                if (file_buf[i] == '\n' || lp >= TERM_LINE_LEN - 2) {
                    line[lp] = '\0';
                    term_push_line(line, COL_TEXT);
                    lp = 0;
                } else {
                    line[lp++] = file_buf[i];
                }
            }
            if (lp > 0) {
                line[lp] = '\0';
                term_push_line(line, COL_TEXT);
            }
        } else {
            term_push_line("cat: file not found in VFS or FAT32 volume.", COL_RED);
        }
        return;
    }

    if (str_starts_with(input, "write ")) {
        const char* p = input + 6;
        while (*p == ' ') p++;
        char fname[48];
        int fi = 0;
        while (*p && *p != ' ' && fi < 46) fname[fi++] = *p++;
        fname[fi] = '\0';
        while (*p == ' ') p++;
        if (fname[0] && *p) {
            if (vfs_write_file(fname, p)) {
                term_push_line("[OK] Wrote file to VFS & flushed to FAT32 volume.", COL_GREEN);
            } else {
                term_push_line("[ERR] Failed to write file.", COL_RED);
            }
        } else {
            term_push_line("Usage: write <filename> <text content>", COL_YELLOW);
        }
        return;
    }

    if (str_starts_with(input, "rm ")) {
        const char* fname = input + 3;
        while (*fname == ' ') fname++;
        if (vfs_delete_file(fname)) {
            term_push_line("[OK] Deleted file from VFS & FAT32 volume.", COL_GREEN);
        } else {
            term_push_line("rm: file not found.", COL_RED);
        }
        return;
    }

    if (str_eq(input, "lspci")) {
        int count = pci_get_device_count();
        char hdr[64];
        str_copy(hdr, "Enumerated PCIe Hardware Functions: ");
        char nbuf[12];
        int_to_str(count, nbuf);
        str_cat(hdr, nbuf);
        term_push_line(hdr, COL_LAVENDER);
        for (int i = 0; i < count && i < 16; i++) {
            uint8_t bus, dev, func, cls, sub;
            uint16_t ven, did;
            uint32_t bar0;
            const char *vstr, *dstr, *drv;
            if (pci_get_device_info(i, &bus, &dev, &func, &ven, &did, &cls, &sub, &bar0, &vstr, &dstr, &drv)) {
                char line[116];
                str_copy(line, "PCI ");
                char b_s[8];
                int_to_str(bus, b_s); str_cat(line, b_s); str_cat(line, ":");
                int_to_str(dev, b_s); str_cat(line, b_s); str_cat(line, ".");
                int_to_str(func, b_s); str_cat(line, b_s); str_cat(line, " [");
                char h4[6];
                u16_to_hex4(ven, h4); str_cat(line, h4); str_cat(line, ":");
                u16_to_hex4(did, h4); str_cat(line, h4); str_cat(line, "] ");
                str_cat(line, dstr);
                str_cat(line, " ("); str_cat(line, drv); str_cat(line, ")");
                term_push_line(line, COL_TEXT);
            }
        }
        return;
    }

    if (str_eq(input, "mem") || str_eq(input, "free")) {
        char line[96];
        char nbuf[16];
        str_copy(line, "Total Physical RAM (UEFI Memory Map): ");
        int_to_str((int)memory_get_total_mb(), nbuf);
        str_cat(line, nbuf); str_cat(line, " MB");
        term_push_line(line, COL_LAVENDER);

        str_copy(line, "Used RAM: ");
        int_to_str((int)memory_get_used_mb(), nbuf);
        str_cat(line, nbuf);
        str_cat(line, " MB  |  Free RAM: ");
        int_to_str((int)memory_get_free_mb(), nbuf);
        str_cat(line, nbuf);
        str_cat(line, " MB  |  Heap Allocs: ");
        int_to_str((int)memory_get_alloc_count(), nbuf);
        str_cat(line, nbuf);
        term_push_line(line, COL_GREEN);
        return;
    }

    if (str_eq(input, "cpu") || str_eq(input, "cpuid") || str_eq(input, "sysinfo")) {
        char line[108];
        str_copy(line, "CPU Vendor: ");
        str_cat(line, pci_get_cpu_vendor());
        term_push_line(line, COL_LAVENDER);
        str_copy(line, "CPU Brand:  ");
        str_cat(line, pci_get_cpu_brand());
        term_push_line(line, COL_GREEN);
        str_copy(line, "Display:    ");
        char num[12];
        int_to_str(screen_width, num); str_cat(line, num); str_cat(line, "x");
        int_to_str(screen_height, num); str_cat(line, num);
        str_cat(line, " @ 32-bit BGRA (BDRM + SVGA II)");
        term_push_line(line, COL_TEXT);
        return;
    }

    if (str_eq(input, "ifconfig") || str_eq(input, "net")) {
        char line[108];
        str_copy(line, "eth0: ");
        str_cat(line, net_get_adapter_name());
        term_push_line(line, COL_LAVENDER);
        str_copy(line, "  ether ");
        str_cat(line, net_get_mac_str());
        str_cat(line, "  inet ");
        str_cat(line, net_get_ipv4_str());
        str_cat(line, "  netmask 255.255.255.0");
        term_push_line(line, COL_GREEN);
        char nbuf[16];
        str_copy(line, "  RX packets: ");
        int_to_str((int)net_get_rx_packets(), nbuf); str_cat(line, nbuf);
        str_cat(line, "  TX packets: ");
        int_to_str((int)net_get_tx_packets(), nbuf); str_cat(line, nbuf);
        term_push_line(line, COL_TEXT);
        return;
    }

    if (str_starts_with(input, "sha256 ")) {
        const char* arg = input + 7;
        uint8_t digest[32];
        sha256_hash((char*)arg, digest);
        const char* hex = "0123456789abcdef";
        char hex_out[80];
        str_copy(hex_out, "SHA256: ");
        int pos = 8;
        for (int i = 0; i < 32; i++) {
            hex_out[pos++] = hex[(digest[i] >> 4) & 0xF];
            hex_out[pos++] = hex[digest[i] & 0xF];
        }
        hex_out[pos] = '\0';
        term_push_line(hex_out, COL_GREEN);
        return;
    }

    if (str_eq(input, "reg")) {
        int cnt = blankReg_get_count();
        term_push_line("blankReg Kernel Configuration Database:", COL_LAVENDER);
        for (int i = 0; i < cnt && i < 14; i++) {
            char k[64], s[128];
            int t = 0, iv = 0;
            if (blankReg_get_entry(i, k, &t, &iv, s)) {
                char line[110];
                str_copy(line, "  ");
                str_cat(line, k);
                str_cat(line, " = ");
                if (t == 0) {
                    char nb[16]; int_to_str(iv, nb); str_cat(line, nb);
                } else {
                    str_cat(line, s);
                }
                term_push_line(line, COL_TEXT);
            }
        }
        return;
    }

    if (str_starts_with(input, "res ")) {
        const char* arg = input + 4;
        while (*arg == ' ') arg++;
        uint32_t tw = 0, th = 0;
        if (str_eq(arg, "1024x768")) { tw = 1024; th = 768; }
        else if (str_eq(arg, "1280x720")) { tw = 1280; th = 720; }
        else if (str_eq(arg, "1440x900")) { tw = 1440; th = 900; }
        else if (str_eq(arg, "1920x1080")) { tw = 1920; th = 1080; }

        if (tw != 0 && th != 0) {
            if (set_screen_resolution(tw, th)) {
                *win_x = (screen_width - win_w) / 2;
                *win_y = (screen_height - win_h) / 2;
                term_push_line("[OK] Screen resolution updated live.", COL_GREEN);
            } else {
                term_push_line("[ERROR] Mode not supported by UEFI GOP firmware.", COL_RED);
            }
        } else {
            term_push_line("Usage: res 1024x768 | 1280x720 | 1440x900 | 1920x1080", COL_YELLOW);
        }
        return;
    }

    if (str_starts_with(input, "theme ")) {
        const char* arg = input + 6;
        while (*arg == ' ') arg++;
        if (*arg >= '0' && *arg <= '3') {
            g_wallpaper_theme = *arg - '0';
            play_system_sound((char*)"click");
            term_push_line("[OK] Wallpaper theme applied.", COL_GREEN);
        } else {
            term_push_line("Usage: theme 0 (Sequoia) | 1 (Sunset) | 2 (Neon) | 3 (Slate)", COL_YELLOW);
        }
        return;
    }

    if (str_starts_with(input, "volume ")) {
        const char* arg = input + 7;
        while (*arg == ' ') arg++;
        int val = 0;
        while (*arg >= '0' && *arg <= '9') {
            val = val * 10 + (*arg - '0');
            arg++;
        }
        if (val > 100) val = 100;
        set_master_volume((float)val / 100.0f);
        play_system_sound((char*)"click");
        term_push_line("[OK] Master volume updated.", COL_GREEN);
        return;
    }

    if (str_starts_with(input, "play ")) {
        const char* sound = input + 5;
        while (*sound == ' ') sound++;
        play_system_sound((char*)sound);
        term_push_line("[OK] Synthesized audio waveform played.", COL_GREEN);
        return;
    }

    if (str_eq(input, "disks")) {
        EFI_GUID blockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
        UINTN numBlockIo = 0;
        EFI_HANDLE *blockIoHandles = NULL;
        SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &blockIoGuid, NULL, &numBlockIo, &blockIoHandles);
        if (numBlockIo == 0 || !blockIoHandles) {
            term_push_line("No Block I/O handles located.", COL_YELLOW);
        } else {
            term_push_line("Detected Physical Storage Drives:", COL_LAVENDER);
            int disk_num = 0;
            for (UINTN i = 0; i < numBlockIo; i++) {
                EFI_BLOCK_IO_PROTOCOL *blk = NULL;
                SystemTable->BootServices->HandleProtocol(blockIoHandles[i], &blockIoGuid, (void**)&blk);
                if (blk && blk->Media && !blk->Media->LogicalPartition) {
                    char dinfo[80];
                    str_copy(dinfo, "  Drive ");
                    char num[16];
                    int_to_str(disk_num++, num);
                    str_cat(dinfo, num);
                    str_cat(dinfo, ": ");
                    uint64_t mb = ((blk->Media->LastBlock + 1) * blk->Media->BlockSize) / (1024 * 1024);
                    int_to_str((int)mb, num);
                    str_cat(dinfo, num);
                    str_cat(dinfo, " MB | BlockSize: ");
                    int_to_str((int)blk->Media->BlockSize, num);
                    str_cat(dinfo, num);
                    str_cat(dinfo, " B");
                    term_push_line(dinfo, COL_TEXT);
                }
            }
            SystemTable->BootServices->FreePool(blockIoHandles);
        }
        return;
    }

    if (str_eq(input, "update")) {
        launch_updater(SystemTable);
        return;
    }
    if (str_eq(input, "files") || str_eq(input, "blankpad")) {
        launch_blankpad();
        return;
    }
    if (str_eq(input, "top") || str_eq(input, "monitor")) {
        launch_task_manager();
        return;
    }
    if (str_eq(input, "regedit")) {
        launch_blankreg_edit();
        return;
    }
    if (str_eq(input, "reboot")) {
        os_restart();
        return;
    }
    if (str_eq(input, "shutdown")) {
        os_shutdown();
        return;
    }

    if (str_eq(input, "whoami")) {
        term_push_line("admin (Ring-0 UEFI Kernel Administrator)", COL_GREEN);
        return;
    }

    if (str_eq(input, "uname")) {
        term_push_line("BlankOS Sequoia 2.4.0-x86_64 UEFI Bare-Metal Monolithic", COL_TEXT);
        return;
    }

    if (str_eq(input, "date")) {
        RTC_Time rt;
        get_rtc_time(&rt);
        char dt[64];
        int p = 0;
        uint32_t yr = rt.year ? rt.year : 2026;
        dt[p++] = '0' + ((yr / 1000) % 10);
        dt[p++] = '0' + ((yr / 100) % 10);
        dt[p++] = '0' + ((yr / 10) % 10);
        dt[p++] = '0' + (yr % 10);
        dt[p++] = '-';
        dt[p++] = '0' + (rt.month / 10);
        dt[p++] = '0' + (rt.month % 10);
        dt[p++] = '-';
        dt[p++] = '0' + (rt.day / 10);
        dt[p++] = '0' + (rt.day % 10);
        dt[p++] = ' ';
        dt[p++] = '0' + (rt.hour / 10);
        dt[p++] = '0' + (rt.hour % 10);
        dt[p++] = ':';
        dt[p++] = '0' + (rt.minute / 10);
        dt[p++] = '0' + (rt.minute % 10);
        dt[p++] = ':';
        dt[p++] = '0' + (rt.second / 10);
        dt[p++] = '0' + (rt.second % 10);
        dt[p] = '\0';
        str_cat(dt, " UTC (Hardware RTC)");
        term_push_line(dt, COL_GREEN);
        return;
    }

    char err[TERM_INPUT_LEN + 32];
    str_copy(err, "blanksh: command not found: ");
    str_cat(err, input);
    term_push_line(err, COL_RED);
}

void launch_terminal(EFI_SYSTEM_TABLE* SystemTable) {
    if (!SystemTable) return;

    EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int ww = 720;
    int wh = 440;
    int wx = (screen_width  - ww) / 2;
    int wy = (screen_height - wh) / 2;

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;

    char input_buf[TERM_INPUT_LEN];
    int  input_len = 0;
    input_buf[0] = '\0';

    bool running = true;
    int  blink_counter = 0;
    bool cursor_visible = true;

    if (g_line_count == 0) {
        term_push_line("BlankOS Sequoia Terminal (blanksh v2.4.0 — Real Hardware & VFS)", COL_LAVENDER);
        term_push_line("Type 'help', 'lspci', 'mem', 'cpu', 'ls', 'cat README.md', or 'ifconfig'.", COL_SUBTEXT);
        term_push_line("", COL_TEXT);
    }

    while (running) {
        EFI_INPUT_KEY Key;
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
            if (Key.ScanCode == 0x17) {
                running = false;
                break;
            }

            if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') {
                if (str_eq(input_buf, "exit")) {
                    running = false;
                    break;
                }
                term_execute_cmd(SystemTable, input_buf, &wx, &wy, ww, wh);
                input_len = 0;
                input_buf[0] = '\0';
            } else if (Key.UnicodeChar == 0x08 || Key.ScanCode == 0x08) {
                if (input_len > 0) {
                    input_len--;
                    input_buf[input_len] = '\0';
                }
            } else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                if (input_len < TERM_INPUT_LEN - 2) {
                    input_buf[input_len++] = (char)Key.UnicodeChar;
                    input_buf[input_len] = '\0';
                }
            }
        }

        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;
                if (dx || dy) {
                    cursor_x += dx;
                    cursor_y += dy;
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_x > screen_width  - 1) cursor_x = screen_width  - 1;
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                }

                if (State.LeftButton) {
                    int btn_cx = wx + 16;
                    int btn_cy = wy + TERM_TITLE_BAR_H / 2;
                    int dx2 = cursor_x - btn_cx;
                    int dy2 = cursor_y - btn_cy;
                    if (dx2 * dx2 + dy2 * dy2 <= 8 * 8) {
                        running = false;
                        break;
                    }
                }
            }
        }

        blink_counter++;
        if (blink_counter >= 30) {
            cursor_visible = !cursor_visible;
            blink_counter = 0;
        }

        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();

        dui_shadow(wx, wy, ww, wh, 10, 8, 0x000000, 70);
        dui_rect_rounded(wx, wy, ww, wh, 10, COL_BASE, 250);
        dui_rect_outline(wx, wy, ww, wh, COL_SURFACE0, 1);

        dui_rect(wx, wy, ww, TERM_TITLE_BAR_H, COL_MANTLE, 255);
        dui_circle(wx + 16, wy + TERM_TITLE_BAR_H / 2, 5, COL_TL_CLOSE, 255);
        dui_circle(wx + 34, wy + TERM_TITLE_BAR_H / 2, 5, COL_TL_MINIMIZE, 255);
        dui_circle(wx + 52, wy + TERM_TITLE_BAR_H / 2, 5, COL_TL_MAXIMIZE, 255);

        const char* title = "Terminal — blanksh (Ring-0 UEFI Shell)";
        dui_text(wx + (ww - dui_text_width(title, 1)) / 2, wy + 10, title, COL_SUBTEXT, 1);

        int text_x = wx + TERM_PADDING;
        int text_y = wy + TERM_TITLE_BAR_H + 10;
        int line_h = 16;

        int max_visible_lines = (wh - TERM_TITLE_BAR_H - 40) / line_h;
        int start_line = 0;
        if (g_line_count > max_visible_lines) {
            start_line = g_line_count - max_visible_lines;
        }

        for (int i = start_line; i < g_line_count; i++) {
            dui_text(text_x, text_y, g_lines[i].text, g_lines[i].color, 1);
            text_y += line_h;
        }

        dui_text(text_x, text_y, "blankOS $ ", COL_GREEN, 1);
        int prompt_w = dui_text_width("blankOS $ ", 1);
        dui_text(text_x + prompt_w, text_y, input_buf, COL_TEXT, 1);

        if (cursor_visible) {
            int cur_x = text_x + prompt_w + dui_text_width(input_buf, 1);
            dui_rect(cur_x, text_y, 7, 12, COL_TEXT, 255);
        }

        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();
    }
}

} // extern "C"
