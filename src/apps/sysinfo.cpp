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
    extern int g_wallpaper_theme;
    extern bool g_show_desktop_widgets;
    extern bool g_show_dynamic_island;

    extern bool set_screen_resolution(uint32_t target_w, uint32_t target_h);
    extern const char* kernel_get_boot_log(void);
    extern uint64_t memory_get_total_mb(void);
    extern uint64_t memory_get_used_mb(void);
    extern uint32_t svga_get_vram_size(void);

    extern int pci_get_device_count(void);
    extern const char* pci_get_cpu_vendor(void);
    extern const char* pci_get_cpu_brand(void);
    extern bool pci_get_device_info(int index, uint8_t* bus, uint8_t* dev, uint8_t* func,
                                    uint16_t* vendor, uint16_t* device, uint8_t* class_code,
                                    uint8_t* subclass, uint32_t* bar0, const char** vendor_str,
                                    const char** dev_str, const char** drv_str);

    extern void blankReg_set_int(const char* key, int val);
    extern bool blankReg_save(EFI_SYSTEM_TABLE *st);
    extern void os_send_notification(const char* title, const char* message);
}

namespace {

static bool s_registered = false;
static int s_active_tab = 0; // 0=Overview, 1=Display&Theme, 2=Audio, 3=Network, 4=Hardware&PCI
static int s_bootlog_scroll = 0;
static int s_pci_scroll = 0;
static bool s_vol_dragging = false;
static char s_status_msg[96] = {0};
static uint32_t s_status_color = THEME_TEXT_SECONDARY;
static char s_ping_result[64] = "Not tested yet";
static uint32_t s_ping_color = THEME_TEXT_MUTED;
static char s_fw_vendor[48] = {0};
static bool s_fw_probed = false;

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
    while (src[j] && i < max_len - 1) {
        dst[i++] = src[j++];
    }
    dst[i] = '\0';
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
    while (i > 0 && j < max_len - 1) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

static void hex_to_str(uint32_t val, int digits, char* out) {
    const char* hex = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = hex[val & 0xF];
        val >>= 4;
    }
    out[digits] = '\0';
}

static void probe_fw_vendor(void) {
    if (s_fw_probed) return;
    s_fw_probed = true;
    s_fw_vendor[0] = '\0';
    if (gSystemTable && gSystemTable->FirmwareVendor) {
        int i = 0;
        while (gSystemTable->FirmwareVendor[i] && i < 46) {
            s_fw_vendor[i] = (char)(gSystemTable->FirmwareVendor[i] & 0x7F);
            i++;
        }
        s_fw_vendor[i] = '\0';
    }
    if (!s_fw_vendor[0]) {
        s_copy(s_fw_vendor, "UEFI x86_64 Compliant", sizeof(s_fw_vendor));
    }
}

static void set_status(const char* msg, uint32_t color) {
    s_copy(s_status_msg, msg, sizeof(s_status_msg));
    s_status_color = color;
}

static void render_overview(int px, int py, int pw, int ph, const BlankInputState* in) {
    probe_fw_vendor();
    dui_text(px, py, "System Overview", THEME_TEXT_PRIMARY, 2);
    dui_text(px, py + 26, "BlankOS Horizon v3.0.0 (x86_64-uefi) - Bare-Metal Spatial Workspace", THEME_TEXT_SECONDARY, 1);

    int card_y = py + 52;
    int left_w = (pw * 62) / 100;
    int right_w = pw - left_w - 12;

    // Left Hardware Summary Card
    dui_glass_card(px, card_y, left_w, 152, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);
    const char* cpu_brand = pci_get_cpu_brand();
    const char* cpu_vendor = pci_get_cpu_vendor();

    dui_text(px + 14, card_y + 12, "PROCESSOR", THEME_ACCENT_IRIS, 1);
    dui_text_ellipsis(px + 14, card_y + 30, left_w - 28,
                      (cpu_brand && cpu_brand[0]) ? cpu_brand : "x86_64 Processor",
                      THEME_TEXT_PRIMARY, 1);

    char vline[64];
    s_copy(vline, "Vendor: ", sizeof(vline));
    s_cat(vline, (cpu_vendor && cpu_vendor[0]) ? cpu_vendor : "Genuine x86_64", sizeof(vline));
    dui_text_ellipsis(px + 14, card_y + 50, left_w - 28, vline, THEME_TEXT_SECONDARY, 1);

    char fwline[72];
    s_copy(fwline, "Firmware: ", sizeof(fwline));
    s_cat(fwline, s_fw_vendor, sizeof(fwline));
    dui_text_ellipsis(px + 14, card_y + 74, left_w - 28, fwline, THEME_TEXT_SECONDARY, 1);

    char gpuline[72];
    char wb[12], hb[12], vb[12];
    u64_to_str(screen_width, wb, sizeof(wb));
    u64_to_str(screen_height, hb, sizeof(hb));
    u64_to_str(svga_get_vram_size() / (1024 * 1024), vb, sizeof(vb));
    s_copy(gpuline, "Display: ", sizeof(gpuline));
    s_cat(gpuline, wb, sizeof(gpuline));
    s_cat(gpuline, "x", sizeof(gpuline));
    s_cat(gpuline, hb, sizeof(gpuline));
    s_cat(gpuline, " @ 32bpp  |  VRAM: ", sizeof(gpuline));
    s_cat(gpuline, vb, sizeof(gpuline));
    s_cat(gpuline, " MB", sizeof(gpuline));
    dui_text_ellipsis(px + 14, card_y + 98, left_w - 28, gpuline, THEME_ACCENT_CYAN, 1);

    char pciline[64];
    char pb[12];
    u64_to_str((uint64_t)pci_get_device_count(), pb, sizeof(pb));
    s_copy(pciline, "Enumerated PCIe Functions: ", sizeof(pciline));
    s_cat(pciline, pb, sizeof(pciline));
    dui_text(px + 14, card_y + 122, pciline, THEME_TEXT_SECONDARY, 1);

    // Right Memory Ring Card
    int rx = px + left_w + 12;
    dui_glass_card(rx, card_y, right_w, 152, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);
    uint64_t used_mb = memory_get_used_mb();
    uint64_t total_mb = memory_get_total_mb();
    if (total_mb == 0) total_mb = 512;
    float mem_frac = (float)used_mb / (float)total_mb;
    if (mem_frac > 1.0f) mem_frac = 1.0f;
    if (mem_frac < 0.02f && used_mb > 0) mem_frac = 0.02f;

    dui_text(rx + 14, card_y + 12, "MEMORY", THEME_ACCENT_TEAL, 1);
    int ring_cx = rx + right_w / 2;
    int ring_cy = card_y + 72;
    dui_progress_ring(ring_cx, ring_cy, 30, 6, mem_frac, THEME_SURFACE_ELEVATED, THEME_ACCENT_IRIS);

    char pct_str[12];
    uint64_t pct = (used_mb * 100) / total_mb;
    u64_to_str(pct, pct_str, sizeof(pct_str));
    s_cat(pct_str, "%", sizeof(pct_str));
    int tw = dui_text_width(pct_str, 1);
    dui_text(ring_cx - tw / 2, ring_cy - 8, pct_str, THEME_TEXT_PRIMARY, 1);

    char ram_str[48];
    char ub[12], tb[12];
    u64_to_str(used_mb, ub, sizeof(ub));
    u64_to_str(total_mb, tb, sizeof(tb));
    s_copy(ram_str, ub, sizeof(ram_str));
    s_cat(ram_str, " / ", sizeof(ram_str));
    s_cat(ram_str, tb, sizeof(ram_str));
    s_cat(ram_str, " MB", sizeof(ram_str));
    int rtw = dui_text_width(ram_str, 1);
    dui_text(rx + (right_w - rtw) / 2, card_y + 122, ram_str, THEME_TEXT_SECONDARY, 1);

    // Boot Log Snippet Card
    int log_y = card_y + 166;
    int log_h = ph - (log_y - py);
    if (log_h < 100) log_h = 100;

    dui_glass_card(px, log_y, pw, log_h, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 235, false);
    dui_text(px + 14, log_y + 10, "KERNEL BOOT LOG", THEME_ACCENT_CYAN, 1);

    // Parse boot log lines
    const char* log_raw = kernel_get_boot_log();
    const char* line_ptrs[64];
    int line_lens[64];
    int total_lines = 0;
    if (log_raw) {
        const char* p = log_raw;
        while (*p && total_lines < 64) {
            line_ptrs[total_lines] = p;
            int len = 0;
            while (p[len] && p[len] != '\n') len++;
            line_lens[total_lines] = len;
            total_lines++;
            p += len;
            if (*p == '\n') p++;
        }
    }

    int vis_lines = (log_h - 42) / 20;
    if (vis_lines < 1) vis_lines = 1;
    int max_scroll = total_lines - vis_lines;
    if (max_scroll < 0) max_scroll = 0;

    if (pt_in(in->mx, in->my, px, log_y, pw, log_h) && in->scroll_y != 0) {
        s_bootlog_scroll -= in->scroll_y;
    }
    if (dui_button(px + pw - 96, log_y + 6, 40, 22, "Up", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
        s_bootlog_scroll--;
        play_system_sound((char*)"click");
    }
    if (dui_button(px + pw - 50, log_y + 6, 40, 22, "Dn", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
        s_bootlog_scroll++;
        play_system_sound((char*)"click");
    }
    if (s_bootlog_scroll < 0) s_bootlog_scroll = 0;
    if (s_bootlog_scroll > max_scroll) s_bootlog_scroll = max_scroll;

    for (int i = 0; i < vis_lines && (s_bootlog_scroll + i) < total_lines; i++) {
        int idx = s_bootlog_scroll + i;
        char lbuf[112];
        int clen = line_lens[idx];
        if (clen > 110) clen = 110;
        for (int c = 0; c < clen; c++) lbuf[c] = line_ptrs[idx][c];
        lbuf[clen] = '\0';
        dui_text_ellipsis(px + 14, log_y + 36 + i * 20, pw - 28, lbuf, THEME_TEXT_SECONDARY, 1);
    }
}

static void render_display_theme(int px, int py, int pw, int ph, const BlankInputState* in) {
    (void)ph;
    dui_text(px, py, "Display & Horizon Theme", THEME_TEXT_PRIMARY, 2);
    dui_text(px, py + 26, "Aurora wallpapers, UEFI GOP resolution, and desktop glass overlays", THEME_TEXT_SECONDARY, 1);

    // 5 Horizon Wallpaper Theme Cards
    dui_text(px, py + 54, "HORIZON WALLPAPER PALETTE", THEME_ACCENT_IRIS, 1);
    const char* theme_names[5] = { "Aurora Ocean", "Emerald Mist", "Cosmic Iris", "Crimson Dusk", "Deep Teal" };
    const uint32_t theme_top[5] = { 0x1E3A8A, 0x065F46, 0x3730A3, 0x7F1D1D, 0x0F766E };
    const uint32_t theme_bot[5] = { 0x38BDF8, 0x34D399, 0x818CF8, 0xFB7185, 0x2DD4BF };

    int gap = 10;
    int card_w = (pw - gap * 4) / 5;
    if (card_w < 84) card_w = 84;
    int card_h = 92;
    int cur_wp = dui_get_wallpaper_id();

    for (int t = 0; t < 5; t++) {
        int cx0 = px + t * (card_w + gap);
        int cy0 = py + 76;
        bool active = (cur_wp == t);
        bool hov = pt_in(in->mx, in->my, cx0, cy0, card_w, card_h);

        dui_glass_card(cx0, cy0, card_w, card_h, THEME_RADIUS_MD,
                       hov ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD, 225, active);
        dui_gradient_rounded(cx0 + 8, cy0 + 8, card_w - 16, 44, 6, theme_top[t], theme_bot[t], 255);
        dui_text_ellipsis(cx0 + 8, cy0 + 58, card_w - 16, theme_names[t],
                          active ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);
        if (active) {
            dui_rect_rounded_outline(cx0, cy0, card_w, card_h, THEME_RADIUS_MD, THEME_ACCENT_IRIS, 240);
            dui_circle(cx0 + card_w - 16, cy0 + 18, 6, THEME_ACCENT_GREEN, 255);
        }

        if (hov && in->left_pressed) {
            dui_set_wallpaper_id(t);
            g_wallpaper_theme = t;
            blankReg_set_int("UI/WallpaperTheme", t);
            if (gSystemTable) blankReg_save(gSystemTable);
            play_system_sound((char*)"click");
            set_status("Wallpaper theme updated & saved to REGISTRY.INI", THEME_ACCENT_GREEN);
        }
    }

    // GOP Resolution Switcher
    int res_y = py + 188;
    dui_glass_card(px, res_y, pw, 108, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
    dui_text(px + 16, res_y + 12, "DISPLAY RESOLUTION (UEFI GOP)", THEME_ACCENT_CYAN, 1);

    char cur_res[48];
    char wb[12], hb[12];
    u64_to_str(screen_width, wb, sizeof(wb));
    u64_to_str(screen_height, hb, sizeof(hb));
    s_copy(cur_res, "Current: ", sizeof(cur_res));
    s_cat(cur_res, wb, sizeof(cur_res));
    s_cat(cur_res, " x ", sizeof(cur_res));
    s_cat(cur_res, hb, sizeof(cur_res));
    dui_text(px + pw - 160, res_y + 12, cur_res, THEME_TEXT_SECONDARY, 1);

    const uint32_t rw[2] = { 1024, 1280 };
    const uint32_t rh[2] = { 768, 800 };
    const char* rlbl[2] = { "1024 x 768 (XGA)", "1280 x 800 (WXGA)" };

    for (int r = 0; r < 2; r++) {
        int bw = (pw - 44) / 2;
        int bx = px + 16 + r * (bw + 12);
        int by = res_y + 44;
        bool is_cur = ((uint32_t)screen_width == rw[r] && (uint32_t)screen_height == rh[r]);
        if (dui_button(bx, by, bw, 38, rlbl[r], is_cur ? THEME_ACCENT_IRIS : THEME_SURFACE_ELEVATED,
                       is_cur, in->mx, in->my, in->left_pressed)) {
            if (set_screen_resolution(rw[r], rh[r])) {
                blankReg_set_int("Display/Width", (int)rw[r]);
                blankReg_set_int("Display/Height", (int)rh[r]);
                if (gSystemTable) blankReg_save(gSystemTable);
                set_status("GOP display mode switched & saved", THEME_ACCENT_GREEN);
            } else {
                play_system_sound((char*)"error");
                set_status("Requested GOP mode not advertised by display", THEME_ACCENT_AMBER);
            }
        }
    }

    // Desktop Telemetry Widget & Pulse Capsule Toggles
    int tog_y = res_y + 122;
    dui_glass_card(px, tog_y, pw, 108, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
    dui_text(px + 16, tog_y + 12, "DESKTOP OVERLAYS", THEME_ACCENT_TEAL, 1);

    // Row 1: Desktop Telemetry Widget
    int r1y = tog_y + 38;
    bool hov_w = pt_in(in->mx, in->my, px + 16, r1y, pw - 32, 28);
    dui_text(px + 16, r1y + 6, "Desktop Telemetry Widget", THEME_TEXT_PRIMARY, 1);
    dui_toggle_switch(px + pw - 62, r1y + 4, g_show_desktop_widgets, THEME_ACCENT_IRIS);
    if (hov_w && in->left_pressed) {
        g_show_desktop_widgets = !g_show_desktop_widgets;
        blankReg_set_int("UI/DesktopWidgets", g_show_desktop_widgets ? 1 : 0);
        if (gSystemTable) blankReg_save(gSystemTable);
        play_system_sound((char*)"click");
        set_status(g_show_desktop_widgets ? "Desktop Telemetry Widget enabled" : "Desktop Telemetry Widget hidden", THEME_ACCENT_CYAN);
    }

    // Row 2: Horizon Pulse Capsule
    int r2y = tog_y + 70;
    bool hov_d = pt_in(in->mx, in->my, px + 16, r2y, pw - 32, 28);
    dui_text(px + 16, r2y + 6, "Top Bar Pulse Capsule", THEME_TEXT_PRIMARY, 1);
    dui_toggle_switch(px + pw - 62, r2y + 4, g_show_dynamic_island, THEME_ACCENT_IRIS);
    if (hov_d && in->left_pressed) {
        g_show_dynamic_island = !g_show_dynamic_island;
        blankReg_set_int("UI/DynamicIsland", g_show_dynamic_island ? 1 : 0);
        if (gSystemTable) blankReg_save(gSystemTable);
        play_system_sound((char*)"click");
        set_status("Top bar Pulse Capsule setting saved", THEME_ACCENT_CYAN);
    }

    if (s_status_msg[0]) {
        dui_text_ellipsis(px, tog_y + 120, pw, s_status_msg, s_status_color, 1);
    }
}

static void render_audio(int px, int py, int pw, int ph, const BlankInputState* in) {
    (void)ph;
    dui_text(px, py, "Audio & Chime Synthesizer", THEME_TEXT_PRIMARY, 2);
    dui_text(px, py + 26, "Non-blocking PIT chime sequencer & Intel HDA hardware probe", THEME_TEXT_SECONDARY, 1);

    int card_y = py + 54;
    dui_glass_card(px, card_y, pw, 154, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);

    dui_draw_sys_icon(px + 16, card_y + 14, 18, 2, is_audio_muted() ? THEME_ACCENT_ROSE : THEME_ACCENT_IRIS);
    dui_text_ellipsis(px + 42, card_y + 14, pw - 160, get_audio_device_name(), THEME_TEXT_PRIMARY, 1);

    bool muted = is_audio_muted();
    if (dui_button(px + pw - 106, card_y + 10, 90, 28, muted ? "Unmute" : "Mute",
                   muted ? THEME_ACCENT_ROSE : THEME_SURFACE_ELEVATED, muted,
                   in->mx, in->my, in->left_pressed)) {
        toggle_audio_mute();
        if (!is_audio_muted()) play_system_sound((char*)"click");
    }

    int vol = get_master_volume();
    char vol_lbl[32];
    char vb[12];
    u64_to_str((uint64_t)vol, vb, sizeof(vb));
    s_copy(vol_lbl, "Master Volume: ", sizeof(vol_lbl));
    s_cat(vol_lbl, vb, sizeof(vol_lbl));
    s_cat(vol_lbl, "%", sizeof(vol_lbl));
    dui_text(px + 16, card_y + 54, vol_lbl, muted ? THEME_ACCENT_ROSE : THEME_TEXT_SECONDARY, 1);

    // Interactive Master Volume Slider
    int sl_x = px + 16;
    int sl_y = card_y + 84;
    int sl_w = pw - 32;
    dui_rect_rounded(sl_x, sl_y, sl_w, 10, 5, THEME_SURFACE_INPUT, 240);
    int fill_w = (sl_w * vol) / 100;
    if (fill_w > 0) {
        dui_rect_rounded(sl_x, sl_y, fill_w, 10, 5, muted ? THEME_TEXT_MUTED : THEME_ACCENT_IRIS, 255);
    }
    dui_circle(sl_x + fill_w, sl_y + 5, 9, 0xFFFFFF, 255);
    dui_circle(sl_x + fill_w, sl_y + 5, 5, muted ? THEME_TEXT_MUTED : THEME_ACCENT_IRIS, 255);

    if (in->left_pressed && pt_in(in->mx, in->my, sl_x - 8, sl_y - 10, sl_w + 16, 30)) {
        s_vol_dragging = true;
    }
    if (s_vol_dragging) {
        if (in->left_down) {
            int nv = ((in->mx - sl_x) * 100) / sl_w;
            nv = horizon_clampi(nv, 0, 100);
            set_master_volume(nv);
        } else {
            s_vol_dragging = false;
            blankReg_set_int("Audio/MasterVolume", get_master_volume());
            if (gSystemTable) blankReg_save(gSystemTable);
            play_system_sound((char*)"click");
            set_status("Master volume saved to REGISTRY.INI", THEME_ACCENT_GREEN);
        }
    }

    // Quick preset volume pills
    const int presets[4] = { 25, 50, 75, 100 };
    const char* plabels[4] = { "25%", "50%", "75%", "100%" };
    for (int i = 0; i < 4; i++) {
        int bx = px + 16 + i * 74;
        int by = card_y + 112;
        bool act = (vol == presets[i] && !muted);
        if (dui_button(bx, by, 66, 26, plabels[i], act ? THEME_ACCENT_IRIS : THEME_SURFACE_ELEVATED,
                       act, in->mx, in->my, in->left_pressed)) {
            set_master_volume(presets[i]);
            blankReg_set_int("Audio/MasterVolume", presets[i]);
            if (gSystemTable) blankReg_save(gSystemTable);
            play_system_sound((char*)"click");
        }
    }

    // Sound Test Card
    int test_y = card_y + 168;
    dui_glass_card(px, test_y, pw, 134, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
    dui_text(px + 16, test_y + 14, "CHIME & TONE DIAGNOSTICS", THEME_ACCENT_TEAL, 1);
    dui_text(px + 16, test_y + 34, "Trigger non-blocking speaker chime sequences:", THEME_TEXT_SECONDARY, 1);

    if (dui_button(px + 16, test_y + 66, 150, 38, "Test Chime", THEME_ACCENT_IRIS, true, in->mx, in->my, in->left_pressed)) {
        if (is_audio_muted()) toggle_audio_mute();
        if (get_master_volume() == 0) set_master_volume(75);
        play_system_sound((char*)"startup");
        set_status("Playing 4-note Horizon startup chime", THEME_ACCENT_CYAN);
    }
    if (dui_button(px + 178, test_y + 66, 110, 38, "Notify", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"notify");
    }
    if (dui_button(px + 298, test_y + 66, 110, 38, "Success", THEME_ACCENT_GREEN, false, in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"success");
    }
    if (dui_button(px + 418, test_y + 66, 100, 38, "Alert", THEME_ACCENT_ROSE, false, in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"error");
    }

    if (s_status_msg[0]) {
        dui_text_ellipsis(px, test_y + 148, pw, s_status_msg, s_status_color, 1);
    }
}

static void render_network(int px, int py, int pw, int ph, const BlankInputState* in) {
    (void)ph;
    NetStatus ns;
    net_get_status(&ns);

    dui_text(px, py, "Network & UEFI SNP Stack", THEME_TEXT_PRIMARY, 2);
    dui_text(px, py + 26, "Ethernet, ARP, DHCP, DNS, ICMP & TCP/HTTP/1.1 interface telemetry", THEME_TEXT_SECONDARY, 1);

    int card_y = py + 54;
    dui_glass_card(px, card_y, pw, 210, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);

    // Adapter Header + Link Badge
    dui_draw_sys_icon(px + 16, card_y + 16, 20, 1, ns.link_up ? THEME_ACCENT_GREEN : THEME_ACCENT_ROSE);
    dui_text_ellipsis(px + 44, card_y + 16, pw - 160,
                      ns.nic_present ? ns.adapter_name : "No SNP Network Interface",
                      THEME_TEXT_PRIMARY, 1);

    uint32_t badge_col = ns.link_up ? THEME_ACCENT_GREEN : THEME_ACCENT_ROSE;
    dui_rect_rounded(px + pw - 104, card_y + 12, 88, 24, 12, badge_col, 40);
    dui_rect_rounded_outline(px + pw - 104, card_y + 12, 88, 24, 12, badge_col, 180);
    dui_text(px + pw - 90, card_y + 16, ns.link_up ? "Link Up" : "Link Dn", badge_col, 1);

    int col1 = px + 16;
    int col2 = px + pw / 2 + 8;

    dui_text(col1, card_y + 54, "MAC Address:", THEME_TEXT_MUTED, 1);
    dui_text(col1 + 110, card_y + 54, ns.mac_str[0] ? ns.mac_str : "--:--:--:--:--:--", THEME_TEXT_PRIMARY, 1);

    dui_text(col2, card_y + 54, "DHCP Lease:", THEME_TEXT_MUTED, 1);
    dui_text(col2 + 100, card_y + 54, ns.dhcp_bound ? "Bound" : "Unbound",
             ns.dhcp_bound ? THEME_ACCENT_GREEN : THEME_ACCENT_AMBER, 1);

    dui_text(col1, card_y + 82, "IPv4 Address:", THEME_TEXT_MUTED, 1);
    dui_text(col1 + 110, card_y + 82, ns.ip_str[0] ? ns.ip_str : "0.0.0.0", THEME_ACCENT_CYAN, 1);

    dui_text(col2, card_y + 82, "Gateway:", THEME_TEXT_MUTED, 1);
    dui_text(col2 + 100, card_y + 82, ns.gw_str[0] ? ns.gw_str : "0.0.0.0", THEME_TEXT_PRIMARY, 1);

    dui_text(col1, card_y + 110, "DNS Server:", THEME_TEXT_MUTED, 1);
    dui_text(col1 + 110, card_y + 110, ns.dns_str[0] ? ns.dns_str : "0.0.0.0", THEME_TEXT_PRIMARY, 1);

    // Packet Counters
    dui_rect_rounded(px + 14, card_y + 142, pw - 28, 52, THEME_RADIUS_SM, THEME_SURFACE_INPUT, 230);
    char rx_str[64], tx_str[64];
    char rpk[20], rby[20], tpk[20], tby[20];
    u64_to_str(ns.rx_packets, rpk, sizeof(rpk));
    u64_to_str(ns.rx_bytes / 1024, rby, sizeof(rby));
    u64_to_str(ns.tx_packets, tpk, sizeof(tpk));
    u64_to_str(ns.tx_bytes / 1024, tby, sizeof(tby));

    s_copy(rx_str, "RX Packets: ", sizeof(rx_str));
    s_cat(rx_str, rpk, sizeof(rx_str));
    s_cat(rx_str, " (", sizeof(rx_str));
    s_cat(rx_str, rby, sizeof(rx_str));
    s_cat(rx_str, " KB)", sizeof(rx_str));

    s_copy(tx_str, "TX Packets: ", sizeof(tx_str));
    s_cat(tx_str, tpk, sizeof(tx_str));
    s_cat(tx_str, " (", sizeof(tx_str));
    s_cat(tx_str, tby, sizeof(tx_str));
    s_cat(tx_str, " KB)", sizeof(tx_str));

    dui_text(px + 26, card_y + 158, rx_str, THEME_ACCENT_TEAL, 1);
    dui_text(col2, card_y + 158, tx_str, THEME_ACCENT_IRIS, 1);

    // Actions Card (Renew DHCP + Ping Gateway)
    int act_y = card_y + 224;
    dui_glass_card(px, act_y, pw, 128, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
    dui_text(px + 16, act_y + 14, "NETWORK DIAGNOSTICS", THEME_ACCENT_CYAN, 1);

    if (dui_button(px + 16, act_y + 44, 150, 36, "Renew DHCP", THEME_ACCENT_IRIS, true, in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"click");
        if (net_dhcp_renew()) {
            play_system_sound((char*)"success");
            os_send_notification("Network", "DHCP lease renewed successfully.");
            set_status("DHCP lease renewed", THEME_ACCENT_GREEN);
        } else {
            play_system_sound((char*)"error");
            set_status("DHCP renewal failed or timed out", THEME_ACCENT_ROSE);
        }
    }

    if (dui_button(px + 178, act_y + 44, 156, 36, "Ping Gateway", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"click");
        if (ns.gateway_ip == 0 || !ns.gw_str[0]) {
            s_copy(s_ping_result, "No gateway IP bound", sizeof(s_ping_result));
            s_ping_color = THEME_ACCENT_AMBER;
        } else {
            int rtt = net_ping(ns.gw_str, 1000);
            if (rtt >= 0) {
                char rb[16];
                u64_to_str((uint64_t)rtt, rb, sizeof(rb));
                s_copy(s_ping_result, "Gateway ", sizeof(s_ping_result));
                s_cat(s_ping_result, ns.gw_str, sizeof(s_ping_result));
                s_cat(s_ping_result, " RTT: ", sizeof(s_ping_result));
                s_cat(s_ping_result, rb, sizeof(s_ping_result));
                s_cat(s_ping_result, " ms", sizeof(s_ping_result));
                s_ping_color = THEME_ACCENT_GREEN;
                play_system_sound((char*)"notify");
            } else {
                s_copy(s_ping_result, "Gateway ping timed out (1000 ms)", sizeof(s_ping_result));
                s_ping_color = THEME_ACCENT_ROSE;
                play_system_sound((char*)"error");
            }
        }
    }

    dui_text(px + 16, act_y + 96, "Ping Result:", THEME_TEXT_MUTED, 1);
    dui_text_ellipsis(px + 114, act_y + 96, pw - 130, s_ping_result, s_ping_color, 1);
}

static void render_hardware_pci(int px, int py, int pw, int ph, const BlankInputState* in) {
    int total = pci_get_device_count();
    dui_text(px, py, "Hardware & PCIe Bus", THEME_TEXT_PRIMARY, 2);

    char sub[64];
    char cb[12];
    u64_to_str((uint64_t)total, cb, sizeof(cb));
    s_copy(sub, "Enumerated ", sizeof(sub));
    s_cat(sub, cb, sizeof(sub));
    s_cat(sub, " PCI/PCIe functions via 0xCF8/0xCFC", sizeof(sub));
    dui_text(px, py + 26, sub, THEME_TEXT_SECONDARY, 1);

    int tbl_y = py + 52;
    int tbl_h = ph - 56;
    if (tbl_h < 120) tbl_h = 120;

    dui_glass_card(px, tbl_y, pw, tbl_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 225, false);

    // Table Header
    dui_rect_rounded(px + 6, tbl_y + 6, pw - 12, 28, 6, THEME_SURFACE_INPUT, 240);
    dui_text(px + 14, tbl_y + 12, "BDF", THEME_ACCENT_IRIS, 1);
    dui_text(px + 86, tbl_y + 12, "VID:DID", THEME_ACCENT_IRIS, 1);
    dui_text(px + 178, tbl_y + 12, "Device / Vendor", THEME_ACCENT_IRIS, 1);
    dui_text(px + pw - 146, tbl_y + 12, "Driver", THEME_ACCENT_IRIS, 1);

    int row_h = 30;
    int vis_rows = (tbl_h - 76) / row_h;
    if (vis_rows < 1) vis_rows = 1;
    int max_scroll = total - vis_rows;
    if (max_scroll < 0) max_scroll = 0;

    if (pt_in(in->mx, in->my, px, tbl_y, pw, tbl_h) && in->scroll_y != 0) {
        s_pci_scroll -= in->scroll_y;
    }

    int foot_y = tbl_y + tbl_h - 32;
    if (dui_button(px + 12, foot_y, 76, 24, "Prev", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
        s_pci_scroll -= 3;
        play_system_sound((char*)"click");
    }
    if (dui_button(px + 96, foot_y, 76, 24, "Next", THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
        s_pci_scroll += 3;
        play_system_sound((char*)"click");
    }

    if (s_pci_scroll < 0) s_pci_scroll = 0;
    if (s_pci_scroll > max_scroll) s_pci_scroll = max_scroll;

    for (int i = 0; i < vis_rows && (s_pci_scroll + i) < total; i++) {
        int idx = s_pci_scroll + i;
        uint8_t bus = 0, dev = 0, func = 0, cls = 0, subc = 0;
        uint16_t vid = 0, did = 0;
        uint32_t bar0 = 0;
        const char* vstr = NULL;
        const char* dstr = NULL;
        const char* drv = NULL;

        if (!pci_get_device_info(idx, &bus, &dev, &func, &vid, &did, &cls, &subc, &bar0, &vstr, &dstr, &drv)) {
            continue;
        }

        int ry = tbl_y + 40 + i * row_h;
        if (i % 2 == 1) {
            dui_rect_rounded(px + 6, ry, pw - 12, row_h - 2, 4, THEME_SURFACE_INPUT, 120);
        }

        char bdf[16], bhex[4], dhex[4], fhex[4];
        hex_to_str(bus, 2, bhex);
        hex_to_str(dev, 2, dhex);
        hex_to_str(func, 1, fhex);
        s_copy(bdf, bhex, sizeof(bdf));
        s_cat(bdf, ":", sizeof(bdf));
        s_cat(bdf, dhex, sizeof(bdf));
        s_cat(bdf, ".", sizeof(bdf));
        s_cat(bdf, fhex, sizeof(bdf));
        dui_text(px + 14, ry + 6, bdf, THEME_TEXT_SECONDARY, 1);

        char ids[16], vh[6], dh[6];
        hex_to_str(vid, 4, vh);
        hex_to_str(did, 4, dh);
        s_copy(ids, vh, sizeof(ids));
        s_cat(ids, ":", sizeof(ids));
        s_cat(ids, dh, sizeof(ids));
        dui_text(px + 86, ry + 6, ids, THEME_ACCENT_CYAN, 1);

        char name_buf[64];
        s_copy(name_buf, (vstr && vstr[0]) ? vstr : "Unknown", sizeof(name_buf));
        s_cat(name_buf, " ", sizeof(name_buf));
        s_cat(name_buf, (dstr && dstr[0]) ? dstr : "PCI Device", sizeof(name_buf));
        dui_text_ellipsis(px + 178, ry + 6, pw - 336, name_buf, THEME_TEXT_PRIMARY, 1);

        dui_text_ellipsis(px + pw - 146, ry + 6, 134,
                          (drv && drv[0]) ? drv : "Generic", THEME_ACCENT_TEAL, 1);
    }

    char pg[32];
    char s1[12], s2[12];
    u64_to_str((uint64_t)(s_pci_scroll + 1), s1, sizeof(s1));
    u64_to_str((uint64_t)total, s2, sizeof(s2));
    s_copy(pg, "Showing from #", sizeof(pg));
    s_cat(pg, s1, sizeof(pg));
    s_cat(pg, " of ", sizeof(pg));
    s_cat(pg, s2, sizeof(pg));
    dui_text(px + pw - 180, foot_y + 4, pg, THEME_TEXT_MUTED, 1);
}

static void sysinfo_on_frame(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    int sidebar_w = 188;
    if (cw < 520) sidebar_w = 152;

    // Sidebar glass backdrop + hairline divider
    dui_rect(cx, cy, sidebar_w, ch, THEME_BG_DEEP, 185);
    dui_line(cx + sidebar_w, cy, cx + sidebar_w, cy + ch, THEME_BORDER_SUBTLE, 200);

    dui_text(cx + 16, cy + 16, "HORIZON OS", THEME_ACCENT_IRIS, 1);
    dui_text(cx + 16, cy + 34, "Control & Hardware", THEME_TEXT_MUTED, 1);

    const char* tab_names[5] = {
        "Overview",
        "Display & Theme",
        "Audio",
        "Network",
        "Hardware & PCI"
    };
    const int tab_icons[5] = { 18, 17, 2, 1, 7 };

    for (int i = 0; i < 5; i++) {
        int ty = cy + 64 + i * 40;
        if (dui_icon_pill(cx + 10, ty, sidebar_w - 20, 34, tab_names[i], tab_icons[i],
                          s_active_tab == i, in->mx, in->my, in->left_pressed)) {
            s_active_tab = i;
            s_status_msg[0] = '\0';
            play_system_sound((char*)"click");
        }
    }

    // Quick cross-app utility links at sidebar bottom
    if (ch > 360) {
        if (dui_button(cx + 12, cy + ch - 78, sidebar_w - 24, 30, "Activity Monitor",
                       THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
            wm_open_app(APP_MONITOR);
        }
        if (dui_button(cx + 12, cy + ch - 42, sidebar_w - 24, 30, "Registry Studio",
                       THEME_SURFACE_ELEVATED, false, in->mx, in->my, in->left_pressed)) {
            wm_open_app(APP_REGEDIT);
        }
    }

    int px = cx + sidebar_w + 18;
    int py = cy + 16;
    int pw = cw - sidebar_w - 34;
    int ph = ch - 28;
    if (pw < 120 || ph < 120) return;

    if (s_active_tab == 0) render_overview(px, py, pw, ph, in);
    else if (s_active_tab == 1) render_display_theme(px, py, pw, ph, in);
    else if (s_active_tab == 2) render_audio(px, py, pw, ph, in);
    else if (s_active_tab == 3) render_network(px, py, pw, ph, in);
    else render_hardware_pci(px, py, pw, ph, in);
}

static void sysinfo_on_close(BlankWindow* win) {
    (void)win;
    s_vol_dragging = false;
}

} // namespace

extern "C" {

void sysinfo_register(void) {
    if (s_registered) return;
    s_registered = true;
    wm_register_app(APP_SETTINGS, "System Settings", 760, 520, THEME_ACCENT_IRIS,
                    sysinfo_on_frame, sysinfo_on_close);
}

void launch_sysinfo(void) {
    sysinfo_register();
    wm_open_app(APP_SETTINGS);
}

} // extern "C"
