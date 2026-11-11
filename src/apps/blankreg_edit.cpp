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
    extern int g_wallpaper_theme;

    extern void blankReg_init(void);
    extern int  blankReg_get_count(void);
    extern bool blankReg_get_entry(int index, char* key_out, int* type_out, int* int_out, char* str_out);
    extern void blankReg_set_int(const char* key, int val);
    extern void blankReg_set_string(const char* key, const char* val);
    extern bool blankReg_delete_key(const char* key);
    extern bool blankReg_save(EFI_SYSTEM_TABLE *st);
    extern bool blankReg_load(EFI_SYSTEM_TABLE *st);
}

#define REG_MAX_ROWS 128

static int br_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void br_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void br_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int dlen = br_len(dst);
    int i = 0;
    while (src[i] && (dlen + i) < max_len - 1) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static char br_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static bool br_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static bool br_starts_nocase(const char* s, const char* pfx) {
    if (!s || !pfx) return false;
    for (int i = 0; pfx[i]; i++) {
        if (!s[i] || br_upper(s[i]) != br_upper(pfx[i])) return false;
    }
    return true;
}

static bool br_contains_nocase(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && br_upper(hay[i + j]) == br_upper(needle[j])) j++;
        if (!needle[j]) return true;
    }
    return false;
}

static void br_itoa(int val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    bool neg = false;
    uint32_t u = 0;
    if (val < 0) { neg = true; u = (uint32_t)(-val); }
    else u = (uint32_t)val;
    char tmp[16];
    int p = 0;
    while (u > 0 && p < 14) { tmp[p++] = (char)('0' + (u % 10)); u /= 10; }
    int j = 0;
    if (neg && j < max_len - 1) out[j++] = '-';
    while (p > 0 && j < max_len - 1) out[j++] = tmp[--p];
    out[j] = '\0';
}

static bool br_parse_int(const char* s, int* out_val) {
    if (!s || !s[0] || !out_val) return false;
    int i = 0;
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }
    else if (s[i] == '+') { i++; }
    if (!s[i]) return false;
    int acc = 0;
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9') return false;
        acc = acc * 10 + (s[i] - '0');
        i++;
    }
    *out_val = neg ? -acc : acc;
    return true;
}

static bool br_pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

typedef struct {
    char key[64];
    int  type; // 0 = INT, 1 = STR
    int  int_val;
    char str_val[128];
} RegRowView;

static RegRowView g_reg_rows[REG_MAX_ROWS];
static int  g_reg_total = 0;
static int  g_reg_ns = 0;          // 0=All, 1=System/, 2=UI/, 3=Audio/, 4=Hardware/, 5=Network/, 6=Display/
static int  g_reg_scroll = 0;
static int  g_reg_sel_idx = -1;    // index into g_reg_rows
static int  g_reg_focus_field = 0; // 0=none, 1=search, 2=edit_val, 3=new_key, 4=new_val
static char g_reg_search[48] = "";
static char g_reg_edit_val[128] = "";
static int  g_reg_edit_type = 0;
static bool g_reg_new_mode = false;
static char g_reg_new_key[64] = "";
static char g_reg_new_val[128] = "";
static int  g_reg_new_type = 1;
static char g_reg_status[80] = "Hive Loaded (\\EFI\\BOOT\\REGISTRY.INI)";
static bool g_reg_bootstrapped = false;

static void reg_load_snapshot(void) {
    blankReg_init();
    int cnt = blankReg_get_count();
    if (cnt > REG_MAX_ROWS) cnt = REG_MAX_ROWS;
    g_reg_total = 0;
    for (int i = 0; i < cnt; i++) {
        RegRowView* r = &g_reg_rows[g_reg_total];
        if (blankReg_get_entry(i, r->key, &r->type, &r->int_val, r->str_val)) {
            g_reg_total++;
        }
    }
    if (g_reg_sel_idx >= g_reg_total) g_reg_sel_idx = g_reg_total - 1;
    if (g_reg_sel_idx < 0 && g_reg_total > 0) g_reg_sel_idx = 0;
}

static void reg_sync_inspector_from_sel(void) {
    if (g_reg_sel_idx >= 0 && g_reg_sel_idx < g_reg_total) {
        RegRowView* r = &g_reg_rows[g_reg_sel_idx];
        g_reg_edit_type = r->type;
        if (r->type == 0) br_itoa(r->int_val, g_reg_edit_val, sizeof(g_reg_edit_val));
        else br_copy(g_reg_edit_val, r->str_val, sizeof(g_reg_edit_val));
    } else {
        g_reg_edit_val[0] = '\0';
    }
}

static void reg_apply_selected_value(void) {
    if (g_reg_sel_idx < 0 || g_reg_sel_idx >= g_reg_total) return;
    const char* key = g_reg_rows[g_reg_sel_idx].key;

    if (g_reg_edit_type == 0) {
        int iv = 0;
        if (!br_parse_int(g_reg_edit_val, &iv)) {
            br_copy(g_reg_status, "Invalid integer for DWORD key", sizeof(g_reg_status));
            play_system_sound((const char*)"error");
            return;
        }
        blankReg_set_int(key, iv);
        if (br_eq(key, "UI/WallpaperTheme")) {
            g_wallpaper_theme = iv;
            dui_set_wallpaper_id(iv);
        }
    } else {
        blankReg_set_string(key, g_reg_edit_val);
    }

    reg_load_snapshot();
    for (int i = 0; i < g_reg_total; i++) {
        if (br_eq(g_reg_rows[i].key, key)) {
            g_reg_sel_idx = i;
            break;
        }
    }
    reg_sync_inspector_from_sel();
    br_copy(g_reg_status, "Updated: ", sizeof(g_reg_status));
    br_cat(g_reg_status, key, sizeof(g_reg_status));
    play_system_sound((const char*)"success");
}

static void reg_create_new_key(void) {
    if (!g_reg_new_key[0]) {
        br_copy(g_reg_status, "Enter a key name first", sizeof(g_reg_status));
        play_system_sound((const char*)"error");
        return;
    }
    if (g_reg_new_type == 0) {
        int iv = 0;
        if (!br_parse_int(g_reg_new_val, &iv)) iv = 0;
        blankReg_set_int(g_reg_new_key, iv);
    } else {
        blankReg_set_string(g_reg_new_key, g_reg_new_val);
    }
    char target[64];
    br_copy(target, g_reg_new_key, sizeof(target));
    g_reg_new_mode = false;
    g_reg_focus_field = 2;
    g_reg_ns = 0;
    reg_load_snapshot();
    for (int i = 0; i < g_reg_total; i++) {
        if (br_eq(g_reg_rows[i].key, target)) {
            g_reg_sel_idx = i;
            break;
        }
    }
    reg_sync_inspector_from_sel();
    br_copy(g_reg_status, "Added key: ", sizeof(g_reg_status));
    br_cat(g_reg_status, target, sizeof(g_reg_status));
    play_system_sound((const char*)"success");
}

static void reg_handle_text_key(char* buf, int max_len, const BlankInputState* in) {
    int l = br_len(buf);
    if (in->key_char == '\b') {
        if (l > 0) buf[l - 1] = '\0';
    } else if (in->key_char >= 32 && in->key_char <= 126 && l < max_len - 2) {
        buf[l] = in->key_char;
        buf[l + 1] = '\0';
    }
}

static void blankreg_edit_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    if (!g_reg_bootstrapped) {
        g_reg_bootstrapped = true;
        reg_load_snapshot();
        reg_sync_inspector_from_sel();
    }

    bool click_avail = (in && in->left_pressed);
    int mx = in ? in->mx : -1;
    int my = in ? in->my : -1;

    dui_rect(cx, cy, cw, ch, THEME_BG_DEEP, 185);

    const char* ns_labels[7] = { "All Keys", "System", "UI", "Audio", "Hardware", "Network", "Display" };
    const char* ns_pfxs[7]   = { "", "System/", "UI/", "Audio/", "Hardware/", "Network/", "Display/" };

    // Left Namespace Sidebar
    int sb_w = 152;
    int sb_x = cx + 8;
    int sb_y = cy + 8;
    int sb_h = ch - 16;
    dui_glass_card(sb_x, sb_y, sb_w, sb_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 210, false);
    dui_text(sb_x + 12, sb_y + 11, "Namespaces", THEME_TEXT_PRIMARY, 1);

    for (int n = 0; n < 7; n++) {
        int ny = sb_y + 38 + n * 34;
        bool active = (g_reg_ns == n);
        bool hov = br_pt_in(mx, my, sb_x + 6, ny, sb_w - 12, 30);

        int count = 0;
        for (int i = 0; i < g_reg_total; i++) {
            if (n == 0 || br_starts_nocase(g_reg_rows[i].key, ns_pfxs[n])) count++;
        }

        dui_rect_rounded(sb_x + 6, ny, sb_w - 12, 30, 7,
                         active ? THEME_SURFACE_ELEVATED : (hov ? THEME_SURFACE_HOVER : THEME_SURFACE_BASE),
                         active ? 240 : (hov ? 200 : 100));
        if (active) dui_rect_rounded(sb_x + 6, ny + 6, 3, 18, 2, THEME_ACCENT_IRIS, 255);
        dui_text(sb_x + 16, ny + 7, ns_labels[n], active ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);

        char cbuf[8];
        br_itoa(count, cbuf, sizeof(cbuf));
        int cw_txt = dui_text_width(cbuf, 1);
        dui_text(sb_x + sb_w - 14 - cw_txt, ny + 7, cbuf, active ? THEME_ACCENT_IRIS : THEME_TEXT_MUTED, 1);

        if (hov && click_avail) {
            click_avail = false;
            g_reg_ns = n;
            g_reg_scroll = 0;
            play_system_sound((const char*)"click");
        }
    }

    // Main Right Pane
    int rx = sb_x + sb_w + 8;
    int ry = cy + 8;
    int rw = cw - sb_w - 24;
    int rh = ch - 16;

    // Top Toolbar
    int tb_h = 40;
    dui_glass_card(rx, ry, rw, tb_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);

    // Search filter box
    int srch_x = rx + 8;
    int srch_y = ry + 7;
    int srch_w = rw - 248;
    if (srch_w < 100) srch_w = 100;
    dui_rect_rounded(srch_x, srch_y, srch_w, 26, 6, THEME_SURFACE_INPUT, 240);
    dui_rect_rounded_outline(srch_x, srch_y, srch_w, 26, 6,
                             (g_reg_focus_field == 1) ? THEME_ACCENT_IRIS : THEME_BORDER_SUBTLE, 200);
    if (g_reg_search[0]) {
        dui_text_ellipsis(srch_x + 8, srch_y + 5, srch_w - 14, g_reg_search, THEME_TEXT_PRIMARY, 1);
    } else {
        dui_text_ellipsis(srch_x + 8, srch_y + 5, srch_w - 14, "Filter keys...", THEME_TEXT_MUTED, 1);
    }
    if (click_avail && br_pt_in(mx, my, srch_x, srch_y, srch_w, 26)) {
        click_avail = false;
        g_reg_focus_field = 1;
    }

    if (dui_button(rx + rw - 232, ry + 7, 70, 26, "+ New", THEME_ACCENT_IRIS, false, mx, my, click_avail)) {
        click_avail = false;
        g_reg_new_mode = !g_reg_new_mode;
        if (g_reg_new_mode) {
            g_reg_new_key[0] = '\0';
            g_reg_new_val[0] = '\0';
            g_reg_focus_field = 3;
        }
        play_system_sound((const char*)"click");
    }
    if (dui_button(rx + rw - 156, ry + 7, 68, 26, "Reload", THEME_SURFACE_ELEVATED, false, mx, my, click_avail)) {
        click_avail = false;
        if (gSystemTable) blankReg_load(gSystemTable);
        reg_load_snapshot();
        reg_sync_inspector_from_sel();
        br_copy(g_reg_status, "Reloaded from REGISTRY.INI", sizeof(g_reg_status));
        play_system_sound((const char*)"click");
    }
    if (dui_button(rx + rw - 82, ry + 7, 74, 26, "Save Hive", THEME_ACCENT_IRIS, true, mx, my, click_avail)) {
        click_avail = false;
        bool ok = blankReg_save(gSystemTable);
        br_copy(g_reg_status, ok ? "Saved to \\EFI\\BOOT\\REGISTRY.INI" : "Saved in RAM (FAT32 RO)", sizeof(g_reg_status));
        play_system_sound((const char*)(ok ? "success" : "notify"));
    }

    // Optional Inline "New Key" Bar
    int tbl_y = ry + tb_h + 8;
    if (g_reg_new_mode) {
        int nh = 44;
        dui_glass_card(rx, tbl_y, rw, nh, THEME_RADIUS_MD, THEME_SURFACE_ELEVATED, 235, true);
        int kw = (rw - 190) / 2;
        if (kw < 80) kw = 80;

        // Key input
        dui_rect_rounded(rx + 8, tbl_y + 9, kw, 26, 6, THEME_SURFACE_INPUT, 240);
        dui_rect_rounded_outline(rx + 8, tbl_y + 9, kw, 26, 6, (g_reg_focus_field == 3) ? THEME_ACCENT_IRIS : THEME_BORDER_SUBTLE, 200);
        dui_text_ellipsis(rx + 14, tbl_y + 14, kw - 12, g_reg_new_key[0] ? g_reg_new_key : "Key (e.g. User/Note)",
                          g_reg_new_key[0] ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);
        if (click_avail && br_pt_in(mx, my, rx + 8, tbl_y + 9, kw, 26)) { click_avail = false; g_reg_focus_field = 3; }

        // Type toggle
        if (dui_button(rx + 14 + kw, tbl_y + 9, 46, 26, g_reg_new_type ? "STR" : "INT", THEME_SURFACE_CARD, false, mx, my, click_avail)) {
            click_avail = false;
            g_reg_new_type = 1 - g_reg_new_type;
            play_system_sound((const char*)"click");
        }

        // Value input
        int vx = rx + 66 + kw;
        dui_rect_rounded(vx, tbl_y + 9, kw, 26, 6, THEME_SURFACE_INPUT, 240);
        dui_rect_rounded_outline(vx, tbl_y + 9, kw, 26, 6, (g_reg_focus_field == 4) ? THEME_ACCENT_IRIS : THEME_BORDER_SUBTLE, 200);
        dui_text_ellipsis(vx + 6, tbl_y + 14, kw - 12, g_reg_new_val[0] ? g_reg_new_val : "Value",
                          g_reg_new_val[0] ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);
        if (click_avail && br_pt_in(mx, my, vx, tbl_y + 9, kw, 26)) { click_avail = false; g_reg_focus_field = 4; }

        if (dui_button(rx + rw - 110, tbl_y + 9, 52, 26, "Add", THEME_ACCENT_GREEN, true, mx, my, click_avail)) {
            click_avail = false;
            reg_create_new_key();
        }
        if (dui_button(rx + rw - 52, tbl_y + 9, 44, 26, "Esc", THEME_SURFACE_CARD, false, mx, my, click_avail)) {
            click_avail = false;
            g_reg_new_mode = false;
        }
        tbl_y += nh + 8;
    }

    // Filter visible rows
    int vis_idx[REG_MAX_ROWS];
    int vis_cnt = 0;
    for (int i = 0; i < g_reg_total; i++) {
        if (g_reg_ns != 0 && !br_starts_nocase(g_reg_rows[i].key, ns_pfxs[g_reg_ns])) continue;
        if (g_reg_search[0] && !br_contains_nocase(g_reg_rows[i].key, g_reg_search) &&
            !br_contains_nocase(g_reg_rows[i].str_val, g_reg_search)) continue;
        vis_idx[vis_cnt++] = i;
    }

    // Bottom Inspector Height = 96px
    int insp_h = 96;
    int tbl_h = (ry + rh - insp_h - 8) - tbl_y;
    if (tbl_h < 100) tbl_h = 100;

    dui_glass_card(rx, tbl_y, rw, tbl_h, THEME_RADIUS_MD, THEME_SURFACE_BASE, 215, false);

    // Table header
    dui_rect(rx + 1, tbl_y + 28, rw - 2, 1, THEME_BORDER_SUBTLE, 180);
    dui_text(rx + 12, tbl_y + 7, "Key Path", THEME_TEXT_SECONDARY, 1);
    dui_text(rx + rw - 220, tbl_y + 7, "Type", THEME_TEXT_SECONDARY, 1);
    dui_text(rx + rw - 165, tbl_y + 7, "Value", THEME_TEXT_SECONDARY, 1);

    int row_h = 28;
    int list_y = tbl_y + 32;
    int list_h = tbl_h - 36;
    int max_rows = list_h / row_h;
    if (max_rows < 1) max_rows = 1;

    if (in && in->scroll_y != 0 && br_pt_in(mx, my, rx, tbl_y, rw, tbl_h)) {
        g_reg_scroll -= in->scroll_y;
    }
    if (g_reg_scroll > vis_cnt - max_rows) g_reg_scroll = vis_cnt - max_rows;
    if (g_reg_scroll < 0) g_reg_scroll = 0;

    dui_push_clip(rx + 4, list_y, rw - 8, list_h);
    for (int r = 0; r < max_rows && (g_reg_scroll + r) < vis_cnt; r++) {
        int idx = vis_idx[g_reg_scroll + r];
        RegRowView* row = &g_reg_rows[idx];
        int y0 = list_y + r * row_h;
        bool sel = (idx == g_reg_sel_idx);
        bool hov = br_pt_in(mx, my, rx + 6, y0, rw - 12, row_h - 2);

        if (sel || hov) {
            dui_rect_rounded(rx + 6, y0, rw - 12, row_h - 2, 6,
                             sel ? THEME_SURFACE_ELEVATED : THEME_SURFACE_HOVER, sel ? 240 : 180);
        }
        if (sel) {
            dui_rect_rounded(rx + 6, y0 + 5, 3, row_h - 12, 2, THEME_ACCENT_IRIS, 255);
        }

        dui_text_ellipsis(rx + 14, y0 + 5, rw - 245, row->key, sel ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY, 1);

        uint32_t tcol = (row->type == 0) ? THEME_ACCENT_IRIS : THEME_ACCENT_TEAL;
        dui_rect_rounded(rx + rw - 222, y0 + 4, 46, 18, 4, tcol, 200);
        dui_text(rx + rw - 216, y0 + 5, (row->type == 0) ? "INT" : "STR", THEME_TEXT_ON_ACCENT, 1);

        char vdisp[128];
        if (row->type == 0) br_itoa(row->int_val, vdisp, sizeof(vdisp));
        else br_copy(vdisp, row->str_val, sizeof(vdisp));
        dui_text_ellipsis(rx + rw - 165, y0 + 5, 152, vdisp, THEME_TEXT_PRIMARY, 1);

        if (hov && click_avail) {
            click_avail = false;
            g_reg_sel_idx = idx;
            reg_sync_inspector_from_sel();
            g_reg_focus_field = 2;
            play_system_sound((const char*)"click");
        }
    }
    dui_pop_clip();

    // Bottom Inspector / Editor Card
    int iy = ry + rh - insp_h;
    dui_glass_card(rx, iy, rw, insp_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 232, g_reg_focus_field == 2);

    if (g_reg_sel_idx >= 0 && g_reg_sel_idx < g_reg_total) {
        RegRowView* sel_r = &g_reg_rows[g_reg_sel_idx];
        dui_text_ellipsis(rx + 12, iy + 10, rw - 190, sel_r->key, THEME_ACCENT_IRIS, 1);

        if (dui_button(rx + rw - 166, iy + 7, 74, 24, (g_reg_edit_type == 0) ? "Type: INT" : "Type: STR",
                       THEME_SURFACE_ELEVATED, false, mx, my, click_avail)) {
            click_avail = false;
            g_reg_edit_type = 1 - g_reg_edit_type;
            play_system_sound((const char*)"click");
        }
        if (dui_button(rx + rw - 86, iy + 7, 74, 24, "Delete", THEME_ACCENT_ROSE, false, mx, my, click_avail)) {
            click_avail = false;
            char delk[64];
            br_copy(delk, sel_r->key, sizeof(delk));
            blankReg_delete_key(delk);
            reg_load_snapshot();
            reg_sync_inspector_from_sel();
            br_copy(g_reg_status, "Deleted: ", sizeof(g_reg_status));
            br_cat(g_reg_status, delk, sizeof(g_reg_status));
            play_system_sound((const char*)"click");
        }

        // Editable Value Box + Apply
        int vx = rx + 12;
        int vy = iy + 38;
        int vw = rw - 108;
        dui_rect_rounded(vx, vy, vw, 28, 6, THEME_SURFACE_INPUT, 245);
        dui_rect_rounded_outline(vx, vy, vw, 28, 6, (g_reg_focus_field == 2) ? THEME_ACCENT_IRIS : THEME_BORDER_SUBTLE, 210);
        dui_text_ellipsis(vx + 8, vy + 6, vw - 16, g_reg_edit_val, THEME_TEXT_PRIMARY, 1);
        if (click_avail && br_pt_in(mx, my, vx, vy, vw, 28)) {
            click_avail = false;
            g_reg_focus_field = 2;
        }

        if (dui_button(rx + rw - 88, vy, 76, 28, "Apply", THEME_ACCENT_IRIS, true, mx, my, click_avail)) {
            click_avail = false;
            reg_apply_selected_value();
        }
    } else {
        dui_text(rx + 12, iy + 36, "Select a registry key above to inspect or modify its value.", THEME_TEXT_MUTED, 1);
    }

    dui_text_ellipsis(rx + 12, iy + 73, rw - 24, g_reg_status, THEME_TEXT_SECONDARY, 1);

    // Keyboard routing
    if (in && in->has_key) {
        if (g_reg_focus_field == 1) {
            if (in->scan_code == BLANK_SCAN_ESC) g_reg_search[0] = '\0';
            else reg_handle_text_key(g_reg_search, sizeof(g_reg_search), in);
        } else if (g_reg_focus_field == 2) {
            if (in->key_char == '\n') reg_apply_selected_value();
            else reg_handle_text_key(g_reg_edit_val, sizeof(g_reg_edit_val), in);
        } else if (g_reg_focus_field == 3 && g_reg_new_mode) {
            if (in->key_char == '\t' || in->key_char == '\n') g_reg_focus_field = 4;
            else reg_handle_text_key(g_reg_new_key, sizeof(g_reg_new_key), in);
        } else if (g_reg_focus_field == 4 && g_reg_new_mode) {
            if (in->key_char == '\n') reg_create_new_key();
            else reg_handle_text_key(g_reg_new_val, sizeof(g_reg_new_val), in);
        }
    }
}

extern "C" {

void blankreg_edit_register(void) {
    wm_register_app(APP_REGEDIT, "Registry Studio", 680, 480, THEME_ACCENT_IRIS, blankreg_edit_render, NULL);
}

void launch_blankreg_edit(void) {
    blankReg_init();
    wm_open_app(APP_REGEDIT);
}

} // extern "C"
