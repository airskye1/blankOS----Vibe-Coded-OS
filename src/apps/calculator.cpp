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

#define CALC_HISTORY_MAX 6

static int calc_strlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void calc_strcpy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void calc_strcat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int len = calc_strlen(dst);
    int i = 0;
    while (src[i] && (len + i) < max_len - 1) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = '\0';
}

static void calc_i64_to_str(int64_t val, char* buf, int max_len) {
    if (!buf || max_len < 2) return;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    bool neg = false;
    uint64_t uval = 0;
    if (val < 0) {
        neg = true;
        uval = (uint64_t)(-val);
    } else {
        uval = (uint64_t)val;
    }
    char tmp[32];
    int i = 0;
    while (uval > 0 && i < 28) {
        tmp[i++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    int pos = 0;
    if (neg && pos < max_len - 1) buf[pos++] = '-';
    while (i > 0 && pos < max_len - 1) buf[pos++] = tmp[--i];
    buf[pos] = '\0';
}

static void calc_double_to_str(double val, char* buf, int max_len) {
    if (!buf || max_len < 4) return;
    if (val != val) {
        calc_strcpy(buf, "Error", max_len);
        return;
    }
    if (val > -1e-11 && val < 1e-11) val = 0.0;
    int pos = 0;
    if (val < 0.0) {
        buf[pos++] = '-';
        val = -val;
    }
    if (val > 1e14) {
        calc_strcpy(buf, "Overflow", max_len);
        return;
    }

    int64_t ipart = (int64_t)val;
    double frac = val - (double)ipart;
    int64_t fpart = (int64_t)(frac * 1000000.0 + 0.5);
    if (fpart >= 1000000) {
        ipart += 1;
        fpart -= 1000000;
    }

    char ibuf[32];
    calc_i64_to_str(ipart, ibuf, sizeof(ibuf));
    for (int i = 0; ibuf[i] && pos < max_len - 1; i++) buf[pos++] = ibuf[i];

    if (fpart > 0 && pos < max_len - 2) {
        buf[pos++] = '.';
        char fbuf[8];
        int64_t div = 100000;
        for (int k = 0; k < 6; k++) {
            fbuf[k] = (char)('0' + ((fpart / div) % 10));
            div /= 10;
        }
        int last = 5;
        while (last > 0 && fbuf[last] == '0') last--;
        for (int k = 0; k <= last && pos < max_len - 1; k++) {
            buf[pos++] = fbuf[k];
        }
    }
    buf[pos] = '\0';
}

static double calc_str_to_double(const char* s) {
    if (!s || !s[0]) return 0.0;
    int i = 0;
    double sign = 1.0;
    if (s[i] == '-') { sign = -1.0; i++; }
    else if (s[i] == '+') { i++; }

    double res = 0.0;
    while (s[i] >= '0' && s[i] <= '9') {
        res = res * 10.0 + (double)(s[i] - '0');
        i++;
    }
    if (s[i] == '.') {
        i++;
        double place = 0.1;
        while (s[i] >= '0' && s[i] <= '9') {
            res += (double)(s[i] - '0') * place;
            place *= 0.1;
            i++;
        }
    }
    return res * sign;
}

typedef struct {
    char expr[64];
    char result[32];
} CalcHistoryItem;

static char   g_calc_display[32] = "0";
static char   g_calc_pending_expr[64] = "";
static double g_calc_accum = 0.0;
static char   g_calc_op = 0;
static bool   g_calc_reset_next = true;
static bool   g_calc_error = false;
static CalcHistoryItem g_calc_history[CALC_HISTORY_MAX];
static int    g_calc_hist_count = 0;
static char   g_calc_flash_btn[8] = "";
static int    g_calc_flash_ticks = 0;

static void calc_push_history(const char* expr, const char* res) {
    if (g_calc_hist_count < CALC_HISTORY_MAX) {
        calc_strcpy(g_calc_history[g_calc_hist_count].expr, expr, 64);
        calc_strcpy(g_calc_history[g_calc_hist_count].result, res, 32);
        g_calc_hist_count++;
    } else {
        for (int i = 0; i < CALC_HISTORY_MAX - 1; i++) {
            g_calc_history[i] = g_calc_history[i + 1];
        }
        calc_strcpy(g_calc_history[CALC_HISTORY_MAX - 1].expr, expr, 64);
        calc_strcpy(g_calc_history[CALC_HISTORY_MAX - 1].result, res, 32);
    }
}

static void calc_flash(const char* label) {
    calc_strcpy(g_calc_flash_btn, label, sizeof(g_calc_flash_btn));
    g_calc_flash_ticks = 7;
}

static void calc_clear_all(void) {
    calc_strcpy(g_calc_display, "0", sizeof(g_calc_display));
    g_calc_pending_expr[0] = '\0';
    g_calc_accum = 0.0;
    g_calc_op = 0;
    g_calc_reset_next = true;
    g_calc_error = false;
}

static void calc_input_digit(char d) {
    if (g_calc_error) calc_clear_all();
    if (g_calc_reset_next) {
        g_calc_display[0] = d;
        g_calc_display[1] = '\0';
        g_calc_reset_next = false;
    } else {
        int len = calc_strlen(g_calc_display);
        if (len == 1 && g_calc_display[0] == '0') {
            g_calc_display[0] = d;
        } else if (len < 16) {
            g_calc_display[len] = d;
            g_calc_display[len + 1] = '\0';
        }
    }
}

static void calc_input_dot(void) {
    if (g_calc_error) calc_clear_all();
    if (g_calc_reset_next) {
        calc_strcpy(g_calc_display, "0.", sizeof(g_calc_display));
        g_calc_reset_next = false;
        return;
    }
    for (int i = 0; g_calc_display[i]; i++) {
        if (g_calc_display[i] == '.') return;
    }
    calc_strcat(g_calc_display, ".", sizeof(g_calc_display));
}

static void calc_backspace(void) {
    if (g_calc_error || g_calc_reset_next) {
        calc_strcpy(g_calc_display, "0", sizeof(g_calc_display));
        g_calc_reset_next = true;
        g_calc_error = false;
        return;
    }
    int len = calc_strlen(g_calc_display);
    if (len <= 1 || (len == 2 && g_calc_display[0] == '-')) {
        calc_strcpy(g_calc_display, "0", sizeof(g_calc_display));
        g_calc_reset_next = true;
    } else {
        g_calc_display[len - 1] = '\0';
    }
}

static void calc_toggle_sign(void) {
    if (g_calc_error) return;
    double v = calc_str_to_double(g_calc_display);
    if (v == 0.0) return;
    calc_double_to_str(-v, g_calc_display, sizeof(g_calc_display));
}

static void calc_percent(void) {
    if (g_calc_error) return;
    double cur = calc_str_to_double(g_calc_display);
    if (g_calc_op == '+' || g_calc_op == '-') {
        cur = (g_calc_accum * cur) / 100.0;
    } else {
        cur = cur / 100.0;
    }
    calc_double_to_str(cur, g_calc_display, sizeof(g_calc_display));
}

static bool calc_eval_pending(double rhs, double* out) {
    if (g_calc_op == '+') { *out = g_calc_accum + rhs; return true; }
    if (g_calc_op == '-') { *out = g_calc_accum - rhs; return true; }
    if (g_calc_op == '*') { *out = g_calc_accum * rhs; return true; }
    if (g_calc_op == '/') {
        if (rhs > -1e-12 && rhs < 1e-12) return false;
        *out = g_calc_accum / rhs;
        return true;
    }
    *out = rhs;
    return true;
}

static void calc_apply_op(char next_op) {
    if (g_calc_error) return;
    double cur = calc_str_to_double(g_calc_display);
    if (g_calc_op != 0 && !g_calc_reset_next) {
        double res = 0.0;
        if (!calc_eval_pending(cur, &res)) {
            calc_strcpy(g_calc_display, "Error", sizeof(g_calc_display));
            g_calc_error = true;
            g_calc_op = 0;
            g_calc_pending_expr[0] = '\0';
            play_system_sound((const char*)"error");
            return;
        }
        g_calc_accum = res;
        calc_double_to_str(res, g_calc_display, sizeof(g_calc_display));
    } else {
        g_calc_accum = cur;
    }
    g_calc_op = next_op;
    g_calc_reset_next = true;

    char op_str[4] = { ' ', next_op, '\0', '\0' };
    calc_double_to_str(g_calc_accum, g_calc_pending_expr, sizeof(g_calc_pending_expr));
    calc_strcat(g_calc_pending_expr, op_str, sizeof(g_calc_pending_expr));
}

static void calc_equals(void) {
    if (g_calc_error) return;
    double rhs = calc_str_to_double(g_calc_display);
    if (g_calc_op == 0) return;

    char lhs_str[32], rhs_str[32], res_str[32], full_expr[64];
    calc_double_to_str(g_calc_accum, lhs_str, sizeof(lhs_str));
    calc_double_to_str(rhs, rhs_str, sizeof(rhs_str));

    double res = 0.0;
    if (!calc_eval_pending(rhs, &res)) {
        calc_strcpy(g_calc_display, "Error", sizeof(g_calc_display));
        g_calc_error = true;
        g_calc_op = 0;
        g_calc_pending_expr[0] = '\0';
        play_system_sound((const char*)"error");
        return;
    }

    calc_double_to_str(res, res_str, sizeof(res_str));
    char op_str[4] = { ' ', g_calc_op, ' ', '\0' };
    calc_strcpy(full_expr, lhs_str, sizeof(full_expr));
    calc_strcat(full_expr, op_str, sizeof(full_expr));
    calc_strcat(full_expr, rhs_str, sizeof(full_expr));

    calc_push_history(full_expr, res_str);
    calc_strcpy(g_calc_pending_expr, full_expr, sizeof(g_calc_pending_expr));
    calc_strcat(g_calc_pending_expr, " =", sizeof(g_calc_pending_expr));
    calc_strcpy(g_calc_display, res_str, sizeof(g_calc_display));

    g_calc_accum = res;
    g_calc_op = 0;
    g_calc_reset_next = true;
}

static bool calc_pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static bool calc_streq(const char* a, const char* b) {
    int i = 0;
    while (a && b && a[i] && b[i]) {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a && b && a[i] == b[i];
}

static void calculator_render(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    if (g_calc_flash_ticks > 0) g_calc_flash_ticks--;

    bool click_avail = (in && in->left_pressed);
    int mx = in ? in->mx : -1;
    int my = in ? in->my : -1;

    // Keyboard support
    if (in && in->has_key) {
        char kc = in->key_char;
        if (kc >= '0' && kc <= '9') {
            char lb[2] = { kc, '\0' };
            calc_input_digit(kc);
            calc_flash(lb);
            play_system_sound((const char*)"click");
        } else if (kc == '.') {
            calc_input_dot();
            calc_flash(".");
            play_system_sound((const char*)"click");
        } else if (kc == '+' || kc == '-' || kc == '*' || kc == '/') {
            char lb[2] = { kc, '\0' };
            calc_apply_op(kc);
            calc_flash(lb);
            play_system_sound((const char*)"click");
        } else if (kc == '%') {
            calc_percent();
            calc_flash("%");
            play_system_sound((const char*)"click");
        } else if (kc == '=' || kc == '\n') {
            calc_equals();
            calc_flash("=");
            play_system_sound((const char*)"click");
        } else if (kc == '\b') {
            calc_backspace();
            play_system_sound((const char*)"click");
        } else if (kc == 'c' || kc == 'C' || in->scan_code == BLANK_SCAN_ESC) {
            calc_clear_all();
            calc_flash("C");
            play_system_sound((const char*)"click");
        }
    }

    dui_rect(cx, cy, cw, ch, THEME_BG_DEEP, 185);

    int pad = 12;
    int card_w = cw - pad * 2;

    // 1. Expression History Strip
    int hist_y = cy + 10;
    int hist_h = 68;
    dui_glass_card(cx + pad, hist_y, card_w, hist_h, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 215, false);
    if (g_calc_hist_count == 0) {
        dui_text(cx + pad + 12, hist_y + 24, "History empty  |  Click or type digits", THEME_TEXT_MUTED, 1);
    } else {
        int show = g_calc_hist_count > 3 ? 3 : g_calc_hist_count;
        int start = g_calc_hist_count - show;
        for (int i = 0; i < show; i++) {
            int idx = start + i;
            int ry = hist_y + 6 + i * 20;
            char line[96];
            calc_strcpy(line, g_calc_history[idx].expr, sizeof(line));
            calc_strcat(line, " = ", sizeof(line));
            calc_strcat(line, g_calc_history[idx].result, sizeof(line));

            bool hov = calc_pt_in(mx, my, cx + pad + 4, ry, card_w - 8, 18);
            if (hov) {
                dui_rect_rounded(cx + pad + 6, ry, card_w - 12, 18, 4, THEME_SURFACE_HOVER, 160);
                if (click_avail) {
                    click_avail = false;
                    calc_strcpy(g_calc_display, g_calc_history[idx].result, sizeof(g_calc_display));
                    g_calc_reset_next = false;
                    g_calc_error = false;
                    play_system_sound((const char*)"click");
                }
            }
            int tw = dui_text_width(line, 1);
            int tx = cx + pad + card_w - 12 - tw;
            if (tx < cx + pad + 10) tx = cx + pad + 10;
            dui_text_ellipsis(tx, ry + 1, card_w - 20, line, hov ? THEME_ACCENT_AMBER : THEME_TEXT_SECONDARY, 1);
        }
    }

    // 2. Main Glass Display Card
    int disp_y = hist_y + hist_h + 8;
    int disp_h = 84;
    dui_glass_card(cx + pad, disp_y, card_w, disp_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 235, true);

    if (g_calc_pending_expr[0]) {
        int pw = dui_text_width(g_calc_pending_expr, 1);
        int px = cx + pad + card_w - 14 - pw;
        if (px < cx + pad + 12) px = cx + pad + 12;
        dui_text_ellipsis(px, disp_y + 12, card_w - 24, g_calc_pending_expr, THEME_ACCENT_AMBER, 1);
    }

    int dscale = (dui_text_width(g_calc_display, 3) <= card_w - 28) ? 3 : 2;
    int dw = dui_text_width(g_calc_display, dscale);
    int dx = cx + pad + card_w - 14 - dw;
    if (dx < cx + pad + 12) dx = cx + pad + 12;
    int dy = disp_y + (dscale == 3 ? 38 : 44);
    dui_text_ellipsis(dx, dy, card_w - 24, g_calc_display, g_calc_error ? THEME_ACCENT_ROSE : THEME_TEXT_PRIMARY, dscale);

    // 3. 5x4 Spatial Glass Keypad
    int grid_y = disp_y + disp_h + 10;
    int avail_h = (cy + ch - pad) - grid_y;
    int gap = 7;
    int btn_w = (card_w - gap * 3) / 4;
    int btn_h = (avail_h - gap * 4) / 5;
    if (btn_h > 46) btn_h = 46;
    if (btn_h < 30) btn_h = 30;

    const char* rows[5][4] = {
        { "C",   "+/-", "%", "/" },
        { "7",   "8",   "9", "*" },
        { "4",   "5",   "6", "-" },
        { "1",   "2",   "3", "+" },
        { "0",   "",    ".", "=" }
    };

    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            const char* lbl = rows[r][c];
            if (!lbl[0]) continue;

            int span = (r == 4 && c == 0) ? 2 : 1;
            int bx = cx + pad + c * (btn_w + gap);
            int by = grid_y + r * (btn_h + gap);
            int bw = (span == 2) ? (btn_w * 2 + gap) : btn_w;

            bool is_op = (c == 3);
            bool is_fn = (r == 0 && c < 3);
            bool is_eq = (r == 4 && c == 3);
            bool op_active = (g_calc_op != 0 && lbl[0] == g_calc_op && lbl[1] == '\0' && g_calc_reset_next);
            bool flashing = (g_calc_flash_ticks > 0 && calc_streq(g_calc_flash_btn, lbl));
            bool hov = calc_pt_in(mx, my, bx, by, bw, btn_h);

            uint32_t bg = THEME_SURFACE_CARD;
            uint32_t fg = THEME_TEXT_PRIMARY;
            uint32_t border = THEME_BORDER_SUBTLE;

            if (is_eq) {
                bg = flashing ? THEME_TEXT_PRIMARY : (hov ? 0xFCD34D : THEME_ACCENT_AMBER);
                fg = THEME_TEXT_ON_ACCENT;
                border = THEME_ACCENT_AMBER;
            } else if (is_op) {
                bg = (op_active || flashing) ? THEME_ACCENT_AMBER : (hov ? THEME_SURFACE_HOVER : THEME_SURFACE_ELEVATED);
                fg = (op_active || flashing) ? THEME_TEXT_ON_ACCENT : THEME_ACCENT_AMBER;
                border = op_active ? THEME_ACCENT_AMBER : THEME_BORDER_SPECULAR;
            } else if (is_fn) {
                bg = flashing ? THEME_ACCENT_CYAN : (hov ? THEME_SURFACE_HOVER : THEME_SURFACE_ELEVATED);
                fg = flashing ? THEME_TEXT_ON_ACCENT : (c == 0 ? THEME_ACCENT_ROSE : THEME_TEXT_SECONDARY);
            } else {
                bg = flashing ? THEME_ACCENT_AMBER : (hov ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD);
                fg = flashing ? THEME_TEXT_ON_ACCENT : THEME_TEXT_PRIMARY;
            }

            dui_rect_rounded(bx, by, bw, btn_h, THEME_RADIUS_MD, bg, (is_eq || op_active) ? 245 : 225);
            dui_rect_rounded_outline(bx, by, bw, btn_h, THEME_RADIUS_MD, hov ? THEME_ACCENT_AMBER : border, hov ? 200 : 120);

            int tw = dui_text_width(lbl, 1);
            int tx = bx + (bw - tw) / 2;
            int ty = by + (btn_h - 16) / 2;
            dui_text(tx, ty, lbl, fg, 1);

            if (hov && click_avail) {
                click_avail = false;
                calc_flash(lbl);
                play_system_sound((const char*)"click");
                if (lbl[0] >= '0' && lbl[0] <= '9' && lbl[1] == '\0') {
                    calc_input_digit(lbl[0]);
                } else if (calc_streq(lbl, ".")) {
                    calc_input_dot();
                } else if (calc_streq(lbl, "C")) {
                    calc_clear_all();
                } else if (calc_streq(lbl, "+/-")) {
                    calc_toggle_sign();
                } else if (calc_streq(lbl, "%")) {
                    calc_percent();
                } else if (calc_streq(lbl, "=")) {
                    calc_equals();
                } else if (lbl[1] == '\0') {
                    calc_apply_op(lbl[0]);
                }
            }
        }
    }
}

extern "C" {

void calculator_register(void) {
    wm_register_app(APP_CALCULATOR, "Calculator", 350, 470, THEME_ACCENT_AMBER, calculator_render, NULL);
}

void launch_calculator(EFI_SYSTEM_TABLE* st) {
    (void)st;
    wm_open_app(APP_CALCULATOR);
}

} // extern "C"
