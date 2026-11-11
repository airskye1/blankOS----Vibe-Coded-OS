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
    extern uint32_t svga_get_vram_size(void);
}

static void pm_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void pm_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int dl = 0;
    while (dst[dl]) dl++;
    int i = 0;
    while (src[i] && (dl + i) < max_len - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = '\0';
}

static void pm_u64_str(uint64_t val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[24];
    int p = 0;
    while (val > 0 && p < 22) { tmp[p++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (p > 0 && j < max_len - 1) out[j++] = tmp[--p];
    out[j] = '\0';
}

static void pm_hex4(uint16_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = hex[(val >> 12) & 0xF];
    out[1] = hex[(val >> 8) & 0xF];
    out[2] = hex[(val >> 4) & 0xF];
    out[3] = hex[val & 0xF];
    out[4] = '\0';
}

static void pm_hex2(uint8_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = hex[(val >> 4) & 0xF];
    out[1] = hex[val & 0xF];
    out[2] = '\0';
}

static bool pm_pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static int  g_pm_tab = 0;        // 0=Overview, 1=Windows, 2=Network, 3=Hardware
static int  g_pm_win_scroll = 0;
static int  g_pm_pci_scroll = 0;

static uint64_t g_pm_last_sample_ms = 0;
static uint64_t g_pm_prev_rx = 0;
static uint64_t g_pm_prev_tx = 0;
static int      g_pm_net_hist[28] = {0};
static int      g_pm_net_rate_kbps = 0;

static void pm_sample_network(uint64_t now_ms, const NetStatus* ns) {
    if (!ns) return;
    if (g_pm_last_sample_ms == 0) {
        g_pm_last_sample_ms = now_ms;
        g_pm_prev_rx = ns->rx_bytes;
        g_pm_prev_tx = ns->tx_bytes;
        return;
    }
    if (now_ms >= g_pm_last_sample_ms + 500) {
        uint64_t dt = now_ms - g_pm_last_sample_ms;
        uint64_t d_rx = (ns->rx_bytes >= g_pm_prev_rx) ? (ns->rx_bytes - g_pm_prev_rx) : 0;
        uint64_t d_tx = (ns->tx_bytes >= g_pm_prev_tx) ? (ns->tx_bytes - g_pm_prev_tx) : 0;
        int kbps = (int)(((d_rx + d_tx) * 1000ULL) / (dt * 1024ULL));
        g_pm_net_rate_kbps = kbps;

        for (int i = 0; i < 27; i++) g_pm_net_hist[i] = g_pm_net_hist[i + 1];
        int bar = kbps;
        if (bar > 100) bar = 100;
        if (bar == 0 && (d_rx + d_tx) > 0) bar = 8;
        g_pm_net_hist[27] = bar;

        g_pm_prev_rx = ns->rx_bytes;
        g_pm_prev_tx = ns->tx_bytes;
        g_pm_last_sample_ms = now_ms;
    }
}

static void pm_draw_memory_card(int x, int y, int w, int h) {
    dui_bento_card(x, y, w, h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 232, false);

    uint64_t tot = memory_get_total_mb();
    uint64_t used = memory_get_used_mb();
    uint64_t avail = memory_get_free_mb();
    uint32_t allocs = memory_get_alloc_count();
    if (tot == 0) tot = 1;

    int pct = (int)((used * 100ULL) / tot);
    if (pct > 100) pct = 100;

    dui_text(x + 14, y + 10, "Physical Memory", THEME_TEXT_PRIMARY, 1);

    char pb[16];
    pm_u64_str((uint64_t)pct, pb, sizeof(pb));
    pm_cat(pb, "%", sizeof(pb));
    dui_status_pill(x + w - 82, y + 7, 68, 22, pb,
                    (pct > 80) ? THEME_ACCENT_ROSE : THEME_ACCENT_AMBER,
                    THEME_SURFACE_ELEVATED, true);

    // Memory segmented bar
    int bar_x = x + 14;
    int bar_y = y + 36;
    int bar_w = w - 28;
    int bar_h = 14;
    dui_rect_rounded(bar_x, bar_y, bar_w, bar_h, 7, THEME_SURFACE_INPUT, 240);
    int fill_w = (bar_w * pct) / 100;
    if (fill_w < 8 && used > 0) fill_w = 8;
    if (fill_w > bar_w) fill_w = bar_w;
    dui_gradient_rounded(bar_x, bar_y, fill_w, bar_h, 7, THEME_ACCENT_ROSE, THEME_ACCENT_AMBER, 245);

    char b1[20], b2[20], b3[20], b4[20], line[96];
    pm_u64_str(used, b1, sizeof(b1));
    pm_u64_str(avail, b2, sizeof(b2));
    pm_u64_str(tot, b3, sizeof(b3));
    pm_u64_str(allocs, b4, sizeof(b4));

    pm_copy(line, "Used: ", sizeof(line)); pm_cat(line, b1, sizeof(line));
    pm_cat(line, " MB  |  Available: ", sizeof(line)); pm_cat(line, b2, sizeof(line));
    pm_cat(line, " MB", sizeof(line));
    dui_text_ellipsis(x + 14, y + 60, w - 28, line, THEME_TEXT_SECONDARY, 1);

    pm_copy(line, "Total: ", sizeof(line)); pm_cat(line, b3, sizeof(line));
    pm_cat(line, " MB  |  Heap Allocs: ", sizeof(line)); pm_cat(line, b4, sizeof(line));
    dui_text_ellipsis(x + 14, y + 82, w - 28, line, THEME_TEXT_MUTED, 1);
}

static void pm_draw_network_card(int x, int y, int w, int h, const NetStatus* ns,
                                 int mx, int my, bool down, bool* click_avail) {
    dui_bento_card(x, y, w, h, THEME_RADIUS_LG, THEME_SURFACE_CARD, 232, ns->dhcp_bound);
    dui_text(x + 14, y + 10, "Network I/O", THEME_TEXT_PRIMARY, 1);

    const char* st_txt = !ns->nic_present ? "No NIC" : (ns->dhcp_bound ? ns->ip_str : (ns->link_up ? "Link Up" : "Down"));
    uint32_t st_col = ns->dhcp_bound ? THEME_ACCENT_GREEN : (ns->link_up ? THEME_ACCENT_AMBER : THEME_ACCENT_ROSE);
    dui_status_pill(x + w - 142, y + 7, 128, 22, st_txt, st_col, THEME_SURFACE_ELEVATED, ns->link_up);

    char b1[20], b2[20], line[96];
    pm_u64_str(ns->rx_packets, b1, sizeof(b1));
    pm_u64_str(ns->rx_bytes / 1024ULL, b2, sizeof(b2));
    pm_copy(line, "RX: ", sizeof(line)); pm_cat(line, b1, sizeof(line));
    pm_cat(line, " pkts (", sizeof(line)); pm_cat(line, b2, sizeof(line)); pm_cat(line, " KB)", sizeof(line));
    dui_text_ellipsis(x + 14, y + 36, w - 28, line, THEME_ACCENT_CYAN, 1);

    pm_u64_str(ns->tx_packets, b1, sizeof(b1));
    pm_u64_str(ns->tx_bytes / 1024ULL, b2, sizeof(b2));
    pm_copy(line, "TX: ", sizeof(line)); pm_cat(line, b1, sizeof(line));
    pm_cat(line, " pkts (", sizeof(line)); pm_cat(line, b2, sizeof(line)); pm_cat(line, " KB)", sizeof(line));
    dui_text_ellipsis(x + 14, y + 58, w - 28, line, THEME_ACCENT_TEAL, 1);

    dui_text_ellipsis(x + 14, y + 82, w - 110, ns->nic_present ? ns->adapter_name : "No Ethernet adapter", THEME_TEXT_MUTED, 1);

    if (click_avail && dui_button_tactile(x + w - 92, y + 76, 80, 24, "DHCP", THEME_SURFACE_ELEVATED, false, mx, my, down, *click_avail)) {
        *click_avail = false;
        play_system_sound((char*)"tool");
        if (net_dhcp_renew()) {
            play_system_sound((char*)"success");
        } else {
            play_system_sound((char*)"error");
        }
    }
}

static void pm_draw_windows_panel(int x, int y, int w, int h, int mx, int my, bool down, bool* click_avail, const BlankInputState* in) {
    dui_bento_card(x, y, w, h, THEME_RADIUS_LG, THEME_SURFACE_BASE, 225, false);

    int zc = wm_get_z_count();
    char hdr[48], nb[12];
    pm_u64_str((uint64_t)zc, nb, sizeof(nb));
    pm_copy(hdr, "Active Windows (", sizeof(hdr));
    pm_cat(hdr, nb, sizeof(hdr));
    pm_cat(hdr, ")", sizeof(hdr));
    dui_text(x + 14, y + 9, hdr, THEME_TEXT_PRIMARY, 1);
    dui_rect(x + 1, y + 32, w - 2, 1, THEME_BORDER_SUBTLE, 160);

    int list_y = y + 36;
    int list_h = h - 42;
    int row_h = 34;
    int max_rows = list_h / row_h;
    if (max_rows < 1) max_rows = 1;

    if (in && in->scroll_y != 0 && pm_pt_in(mx, my, x, y, w, h)) {
        g_pm_win_scroll -= in->scroll_y;
    }
    if (g_pm_win_scroll > zc - max_rows) g_pm_win_scroll = zc - max_rows;
    if (g_pm_win_scroll < 0) g_pm_win_scroll = 0;

    BlankWindow* focused = wm_get_focused_window();

    dui_push_clip(x + 4, list_y, w - 8, list_h);
    for (int r = 0; r < max_rows && (g_pm_win_scroll + r) < zc; r++) {
        int z_idx = (zc - 1) - (g_pm_win_scroll + r); // newest/topmost first
        BlankWindow* target = wm_get_z_window(z_idx);
        if (!target || !target->open) continue;

        int ry = list_y + r * row_h;
        bool is_foc = (target == focused);
        dui_rect_rounded(x + 8, ry, w - 16, row_h - 4, 7,
                         is_foc ? THEME_SURFACE_ELEVATED : THEME_SURFACE_CARD, is_foc ? 235 : 190);

        dui_draw_squircle_icon(x + 14, ry + 4, 22, (int)target->app_id, false);
        dui_text_ellipsis(x + 44, ry + 7, w - 322, target->title, THEME_TEXT_PRIMARY, 1);

        const char* st = target->minimized ? "Minimized" : (is_foc ? "Focused" : "Background");
        uint32_t scol = target->minimized ? THEME_ACCENT_AMBER : (is_foc ? THEME_ACCENT_GREEN : THEME_TEXT_MUTED);
        dui_status_pill(x + w - 270, ry + 4, 98, 22, st, scol, THEME_SURFACE_ELEVATED, is_foc);

        if (dui_button_tactile(x + w - 164, ry + 3, 62, 24, "Focus", THEME_ACCENT_CYAN, false, mx, my, down, *click_avail)) {
            *click_avail = false;
            wm_focus_window(target);
            play_system_sound((char*)"open");
        }
        if (dui_button_tactile(x + w - 96, ry + 3, 80, 24, "Close Task", THEME_ACCENT_ROSE, true, mx, my, down, *click_avail)) {
            *click_avail = false;
            wm_close_window(target);
            play_system_sound((char*)"close");
            break;
        }
    }
    dui_pop_clip();
}

static void pm_draw_pci_panel(int x, int y, int w, int h, int mx, int my, const BlankInputState* in) {
    dui_bento_card(x, y, w, h, THEME_RADIUS_LG, THEME_SURFACE_BASE, 225, false);

    int cnt = pci_get_device_count();
    char hdr[64], nb[12];
    pm_u64_str((uint64_t)cnt, nb, sizeof(nb));
    pm_copy(hdr, "PCIe Hardware Bus (", sizeof(hdr));
    pm_cat(hdr, nb, sizeof(hdr));
    pm_cat(hdr, " devices)", sizeof(hdr));
    dui_text(x + 14, y + 9, hdr, THEME_TEXT_PRIMARY, 1);
    dui_text_ellipsis(x + 240, y + 9, w - 254, pci_get_cpu_brand(), THEME_TEXT_SECONDARY, 1);
    dui_rect(x + 1, y + 32, w - 2, 1, THEME_BORDER_SUBTLE, 160);

    int list_y = y + 36;
    int list_h = h - 42;
    int row_h = 26;
    int max_rows = list_h / row_h;
    if (max_rows < 1) max_rows = 1;

    if (in && in->scroll_y != 0 && pm_pt_in(mx, my, x, y, w, h)) {
        g_pm_pci_scroll -= in->scroll_y;
    }
    if (g_pm_pci_scroll > cnt - max_rows) g_pm_pci_scroll = cnt - max_rows;
    if (g_pm_pci_scroll < 0) g_pm_pci_scroll = 0;

    dui_push_clip(x + 4, list_y, w - 8, list_h);
    for (int r = 0; r < max_rows && (g_pm_pci_scroll + r) < cnt; r++) {
        int idx = g_pm_pci_scroll + r;
        uint8_t bus = 0, dev = 0, fn = 0, cc = 0, sc = 0;
        uint16_t vid = 0, did = 0;
        uint32_t bar0 = 0;
        const char* vstr = NULL;
        const char* dstr = NULL;
        const char* drv = NULL;
        if (!pci_get_device_info(idx, &bus, &dev, &fn, &vid, &did, &cc, &sc, &bar0, &vstr, &dstr, &drv)) continue;

        int ry = list_y + r * row_h;
        if (r % 2 == 0) dui_rect_rounded(x + 8, ry, w - 16, row_h - 2, 5, THEME_SURFACE_CARD, 130);

        char bdf[16], hb[4], hd[4], hv[6], hdid[6], ids[16];
        pm_hex2(bus, hb); pm_hex2(dev, hd);
        pm_copy(bdf, hb, sizeof(bdf)); pm_cat(bdf, ":", sizeof(bdf)); pm_cat(bdf, hd, sizeof(bdf));
        char fch[3] = { '.', (char)('0' + (fn & 7)), '\0' };
        pm_cat(bdf, fch, sizeof(bdf));

        pm_hex4(vid, hv); pm_hex4(did, hdid);
        pm_copy(ids, hv, sizeof(ids)); pm_cat(ids, ":", sizeof(ids)); pm_cat(ids, hdid, sizeof(ids));

        dui_text(x + 14, ry + 4, bdf, THEME_ACCENT_AMBER, 1);
        dui_text(x + 84, ry + 4, ids, THEME_TEXT_MUTED, 1);
        dui_text_ellipsis(x + 172, ry + 4, w - 310, dstr ? dstr : "PCI Device", THEME_TEXT_PRIMARY, 1);
        dui_text_ellipsis(x + w - 130, ry + 4, 118, drv ? drv : "pci.sys", THEME_ACCENT_CYAN, 1);
    }
    dui_pop_clip();
}

static void task_manager_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    NetStatus ns;
    net_get_status(&ns);
    if (in) pm_sample_network(in->frame_ms, &ns);

    bool click_avail = (in && in->left_pressed);
    bool down = (in && in->left_down);
    int mx = in ? in->mx : -1;
    int my = in ? in->my : -1;

    dui_rect(cx, cy, cw, ch, THEME_BG_DEEP, 185);

    // Top Navigation Tabs
    int pad = 10;
    const char* tabs[4] = { "Overview", "Tasks", "Network", "Hardware" };
    int tab_w = 104;
    for (int t = 0; t < 4; t++) {
        bool active = (g_pm_tab == t);
        if (dui_button_tactile(cx + pad + t * (tab_w + 8), cy + 10, tab_w, 28, tabs[t],
                               active ? THEME_ACCENT_ROSE : THEME_SURFACE_ELEVATED, active, mx, my, down, click_avail)) {
            click_avail = false;
            g_pm_tab = t;
            play_system_sound((char*)"tab");
        }
    }

    char cpu_hdr[64];
    pm_copy(cpu_hdr, pci_get_cpu_vendor(), sizeof(cpu_hdr));
    pm_cat(cpu_hdr, "  |  VRAM ", sizeof(cpu_hdr));
    char vb[16];
    pm_u64_str((uint64_t)(svga_get_vram_size() / (1024U * 1024U)), vb, sizeof(vb));
    pm_cat(cpu_hdr, vb, sizeof(cpu_hdr));
    pm_cat(cpu_hdr, " MB", sizeof(cpu_hdr));
    int chw = dui_text_width(cpu_hdr, 1);
    if (cw > 520) {
        dui_text_ellipsis(cx + cw - pad - chw, cy + 15, 220, cpu_hdr, THEME_TEXT_MUTED, 1);
    }

    int body_y = cy + 46;
    int body_h = ch - 56;
    int inner_w = cw - pad * 2;

    if (g_pm_tab == 0) {
        // Overview: Memory + Network top cards, Active Windows middle, PCI Hardware bottom
        int top_h = 108;
        int col_w = (inner_w - 10) / 2;
        pm_draw_memory_card(cx + pad, body_y, col_w, top_h);
        pm_draw_network_card(cx + pad + col_w + 10, body_y, col_w, top_h, &ns, mx, my, down, &click_avail);

        int mid_y = body_y + top_h + 10;
        int rem_h = body_h - top_h - 20;
        int win_h = (rem_h * 52) / 100;
        int pci_h = rem_h - win_h;
        pm_draw_windows_panel(cx + pad, mid_y, inner_w, win_h, mx, my, down, &click_avail, in);
        pm_draw_pci_panel(cx + pad, mid_y + win_h + 10, inner_w, pci_h, mx, my, in);
    } else if (g_pm_tab == 1) {
        pm_draw_windows_panel(cx + pad, body_y, inner_w, body_h, mx, my, down, &click_avail, in);
    } else if (g_pm_tab == 2) {
        pm_draw_network_card(cx + pad, body_y, inner_w, 110, &ns, mx, my, down, &click_avail);

        int gy = body_y + 120;
        int gh = body_h - 120;
        dui_bento_card(cx + pad, gy, inner_w, gh, THEME_RADIUS_LG, THEME_SURFACE_BASE, 225, ns.link_up);
        dui_dot_grid_overlay(cx + pad + 4, gy + 4, inner_w - 8, 52, 14, THEME_BORDER_SPECULAR, 24);

        char rate_str[48], rb[16];
        pm_u64_str((uint64_t)g_pm_net_rate_kbps, rb, sizeof(rb));
        pm_copy(rate_str, "Live Throughput: ", sizeof(rate_str));
        pm_cat(rate_str, rb, sizeof(rate_str));
        pm_cat(rate_str, " KB/s", sizeof(rate_str));
        dui_text(cx + pad + 14, gy + 12, rate_str, THEME_ACCENT_CYAN, 1);
        dui_status_pill(cx + pad + inner_w - 118, gy + 8, 104, 22,
                        (g_pm_net_rate_kbps > 0) ? "Active I/O" : "Idle",
                        (g_pm_net_rate_kbps > 0) ? THEME_ACCENT_GREEN : THEME_TEXT_MUTED,
                        THEME_SURFACE_ELEVATED, g_pm_net_rate_kbps > 0);

        char net_det[128];
        pm_copy(net_det, "MAC ", sizeof(net_det)); pm_cat(net_det, ns.mac_str, sizeof(net_det));
        pm_cat(net_det, "  |  GW ", sizeof(net_det)); pm_cat(net_det, ns.gw_str, sizeof(net_det));
        pm_cat(net_det, "  |  DNS ", sizeof(net_det)); pm_cat(net_det, ns.dns_str, sizeof(net_det));
        dui_text_ellipsis(cx + pad + 14, gy + 36, inner_w - 28, net_det, THEME_TEXT_SECONDARY, 1);

        // Bar sparkline for RX/TX history
        int chart_x = cx + pad + 14;
        int chart_y = gy + 68;
        int chart_w = inner_w - 28;
        int chart_h = gh - 84;
        if (chart_h > 30) {
            dui_rect_rounded(chart_x, chart_y, chart_w, chart_h, 8, THEME_SURFACE_INPUT, 235);
            int slot_w = (chart_w - 16) / 28;
            if (slot_w < 4) slot_w = 4;
            for (int i = 0; i < 28; i++) {
                int bh = (g_pm_net_hist[i] * (chart_h - 16)) / 100;
                if (bh < 3) bh = 3;
                int bx = chart_x + 8 + i * slot_w;
                int by = chart_y + chart_h - 8 - bh;
                dui_rect_rounded(bx, by, slot_w - 3, bh, 2,
                                 (g_pm_net_hist[i] > 50) ? THEME_ACCENT_ROSE : THEME_ACCENT_CYAN, 225);
            }
        }
    } else {
        pm_draw_pci_panel(cx + pad, body_y, inner_w, body_h, mx, my, in);
    }
}

extern "C" {

void task_manager_register(void) {
    wm_register_app(APP_MONITOR, "System Pulse", 720, 500, THEME_ACCENT_ROSE, task_manager_render, NULL);
}

void launch_task_manager(void) {
    wm_open_app(APP_MONITOR);
}

} // extern "C"
