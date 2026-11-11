#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/blankDUI.h"
#include "../ui/theme.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"
#include "../kernel/net.h"

static void ls_str_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int ls_u32_to_str(uint32_t v, char* out) {
    char tmp[16];
    int p = 0;
    if (v == 0) {
        tmp[p++] = '0';
    } else {
        while (v > 0) {
            tmp[p++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    int n = 0;
    while (p > 0) out[n++] = tmp[--p];
    out[n] = '\0';
    return n;
}

static void ls_format_res(uint32_t w, uint32_t h, char* out, int max_len) {
    char buf[32];
    int pos = ls_u32_to_str(w, buf);
    buf[pos++] = 'x';
    pos += ls_u32_to_str(h, buf + pos);
    buf[pos] = '\0';
    ls_str_copy(out, buf, max_len);
}

static int ls_count_fs_volumes(EFI_SYSTEM_TABLE* st) {
    if (!st || !st->BootServices) return 0;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(
        ByProtocol, &fs_guid, NULL, &count, &handles);
    if (handles) st->BootServices->FreePool(handles);
    if (EFI_ERROR(status)) return 0;
    return (int)count;
}

extern "C" void launch_loading_screen(EFI_SYSTEM_TABLE* SystemTable) {
    int sw = (int)(screen_width > 0 ? screen_width : 1024);
    int sh = (int)(screen_height > 0 ? screen_height : 768);

    char gop_val[24];
    ls_format_res((uint32_t)sw, (uint32_t)sh, gop_val, sizeof(gop_val));

    const char* hid_raw = input_get_pointer_driver_name();
    char hid_val[24];
    ls_str_copy(hid_val, (hid_raw && hid_raw[0]) ? hid_raw : "HID Ready", sizeof(hid_val));

    NetStatus ns;
    net_get_status(&ns);
    char net_val[24];
    if (ns.nic_present) {
        ls_str_copy(net_val, ns.link_up ? "Link Up" : "NIC Ready", sizeof(net_val));
    } else {
        ls_str_copy(net_val, "Offline", sizeof(net_val));
    }

    int vol_cnt = ls_count_fs_volumes(SystemTable);
    char fs_val[24];
    if (vol_cnt > 0) {
        int n = ls_u32_to_str((uint32_t)vol_cnt, fs_val);
        const char* sfx = (vol_cnt == 1) ? " Vol" : " Vols";
        ls_str_copy(fs_val + n, sfx, (int)sizeof(fs_val) - n);
    } else {
        ls_str_copy(fs_val, "No Vol", sizeof(fs_val));
    }

    const int total_frames = 70; // ~1.4s at 20ms/frame
    BlankInputState in;

    for (int frame = 0; frame <= total_frames; frame++) {
        input_poll(&in);
        audio_tick();

        if (in.left_pressed ||
            (in.has_key && (in.key_char == '\n' || in.key_char == '\r' ||
                            in.key_char == ' ' || in.scan_code == BLANK_SCAN_ESC))) {
            break;
        }

        float t = (float)frame / (float)total_frames;
        float progress = horizon_ease_out_cubic(t);

        // Obsidian void backdrop
        dui_rect(0, 0, sw, sh, THEME_BG_VOID, 255);

        int cx = sw / 2;
        int cy = sh / 2 - 28;

        // Ambient Aurora halo behind Horizon orb
        int pulse_r = 128 + (frame % 24) / 3;
        dui_circle(cx, cy - 36, pulse_r + 56, THEME_ACCENT_IRIS, 12);
        dui_circle(cx, cy - 36, pulse_r, THEME_ACCENT_CYAN, 18);
        dui_circle(cx, cy - 36, 72, THEME_ACCENT_TEAL, 22);

        // Glowing Horizon orb + eased progress ring
        dui_circle(cx, cy - 36, 36, THEME_SURFACE_BASE, 235);
        dui_progress_ring(cx, cy - 36, 48, 5, progress, THEME_SURFACE_ELEVATED, THEME_ACCENT_CYAN);
        dui_circle(cx, cy - 36, 20, THEME_ACCENT_CYAN, 48);
        dui_circle(cx, cy - 36, 11, THEME_ACCENT_IRIS, 190);
        dui_circle(cx, cy - 36, 5, THEME_TEXT_PRIMARY, 245);

        // Title & Subtitle
        const char* title = "BlankOS Horizon";
        int tw = dui_text_width(title, 2);
        dui_text(cx - tw / 2, cy + 32, title, THEME_TEXT_PRIMARY, 2);

        const char* sub = "UEFI Spatial Glass Kernel";
        int sub_w = dui_text_width(sub, 1);
        dui_text(cx - sub_w / 2, cy + 68, sub, THEME_TEXT_SECONDARY, 1);

        // Sleek horizontal progress bar
        int bar_w = 280;
        int bar_h = 5;
        int bar_x = cx - bar_w / 2;
        int bar_y = cy + 100;
        dui_rect_rounded(bar_x, bar_y, bar_w, bar_h, 2, THEME_SURFACE_CARD, 255);
        int fill_w = (int)((float)bar_w * progress);
        if (fill_w >= 4) {
            dui_gradient_rounded(bar_x, bar_y, fill_w, bar_h, 2, THEME_ACCENT_CYAN, THEME_ACCENT_TEAL, 255);
        }

        // Hardware Check Readout Pills (4 compact glass chips)
        const char* labels[4] = { "GOP Display", "UEFI HID", "SNP Net", "FAT32" };
        const char* vals[4]   = { gop_val, hid_val, net_val, fs_val };
        const float thresh[4] = { 0.12f, 0.32f, 0.54f, 0.76f };
        bool ready[4] = {
            (sw > 0 && sh > 0),
            (hid_val[0] != '\0'),
            ns.nic_present,
            (vol_cnt > 0)
        };

        int pill_w = 152;
        int pill_h = 46;
        int gap = 12;
        int row_w = 4 * pill_w + 3 * gap;
        int row_x = cx - row_w / 2;
        int row_y = cy + 132;

        for (int i = 0; i < 4; i++) {
            int px = row_x + i * (pill_w + gap);
            bool active = (progress >= thresh[i]);
            dui_glass_card(px, row_y, pill_w, pill_h, THEME_RADIUS_MD,
                           active ? THEME_SURFACE_CARD : THEME_SURFACE_BASE,
                           active ? 220 : 140, active);

            uint32_t dot_col = THEME_TEXT_MUTED;
            if (active) {
                dot_col = ready[i] ? THEME_ACCENT_GREEN : THEME_ACCENT_AMBER;
            }
            dui_circle(px + 14, row_y + 16, 4, dot_col, active ? 255 : 120);
            dui_text(px + 25, row_y + 8, labels[i], active ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);
            dui_text_ellipsis(px + 12, row_y + 26, pill_w - 24,
                              active ? vals[i] : "Checking...",
                              active ? THEME_TEXT_SECONDARY : THEME_TEXT_MUTED, 1);
        }

        const char* hint = "Press Space or click to skip";
        int hw = dui_text_width(hint, 1);
        dui_text(cx - hw / 2, sh - 38, hint, THEME_TEXT_MUTED, 1);

        blankUI_draw_cursor(in.mx, in.my, 0);
        swap_buffers();

        if (SystemTable && SystemTable->BootServices) {
            SystemTable->BootServices->Stall(20000);
        }
    }
}
