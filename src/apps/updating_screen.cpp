#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>

#include "../ui/blankDUI.h"
#include "../ui/theme.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"

extern "C" {
    extern void os_restart(void);
    extern EFI_SYSTEM_TABLE* gSystemTable;
}

static void upd_format_pct(int pct, char* out) {
    char tmp[8];
    int p = 0;
    if (pct <= 0) {
        tmp[p++] = '0';
    } else {
        while (pct > 0 && p < 6) {
            tmp[p++] = (char)('0' + (pct % 10));
            pct /= 10;
        }
    }
    int n = 0;
    while (p > 0) out[n++] = tmp[--p];
    out[n++] = '%';
    out[n] = '\0';
}

extern "C" void launch_updating_screen(float progress) {
    float p = horizon_clampf(progress, 0.0f, 1.0f);
    int sw = (int)(screen_width > 0 ? screen_width : 1024);
    int sh = (int)(screen_height > 0 ? screen_height : 768);

    BlankInputState in;
    input_poll(&in);
    audio_tick();

    dui_rect(0, 0, sw, sh, THEME_BG_VOID, 255);

    int cx = sw / 2;
    int cy = sh / 2;
    dui_circle(cx, cy - 20, 190, THEME_ACCENT_IRIS, 14);
    dui_circle(cx, cy - 20, 120, THEME_ACCENT_CYAN, 18);

    int card_w = 500;
    int card_h = 280;
    int card_x = cx - card_w / 2;
    int card_y = cy - card_h / 2;

    dui_shadow(card_x, card_y, card_w, card_h, THEME_RADIUS_XL, 12);
    dui_glass_card(card_x, card_y, card_w, card_h, THEME_RADIUS_XL, THEME_SURFACE_BASE, 224, true);

    int ring_y = card_y + 78;
    dui_progress_ring(cx, ring_y, 38, 5, p, THEME_SURFACE_ELEVATED, THEME_ACCENT_CYAN);

    int pct = (int)(p * 100.0f + 0.5f);
    if (pct > 100) pct = 100;
    char pct_str[8];
    upd_format_pct(pct, pct_str);
    int pw = dui_text_width(pct_str, 1);
    dui_text(cx - pw / 2, ring_y - 8, pct_str, THEME_TEXT_PRIMARY, 1);

    const char* title = "Installing BlankOS Horizon Updates";
    int tw = dui_text_width(title, 2);
    dui_text(cx - tw / 2, card_y + 136, title, THEME_TEXT_PRIMARY, 2);

    const char* sub = (p >= 1.0f)
        ? "Update complete. Restarting your system..."
        : "Please keep your computer powered on.";
    int sub_w = dui_text_width(sub, 1);
    dui_text(cx - sub_w / 2, card_y + 172, sub, THEME_TEXT_SECONDARY, 1);

    int bar_w = card_w - 96;
    int bar_x = card_x + 48;
    int bar_y = card_y + 214;
    dui_rect_rounded(bar_x, bar_y, bar_w, 8, 4, THEME_SURFACE_INPUT, 255);
    int fill_w = (int)((float)bar_w * p);
    if (fill_w >= 8) {
        dui_gradient_rounded(bar_x, bar_y, fill_w, 8, 4, THEME_ACCENT_CYAN, THEME_ACCENT_TEAL, 255);
    }

    blankUI_draw_cursor(in.mx, in.my, 0);
    swap_buffers();

    if (p >= 1.0f) {
        if (gSystemTable && gSystemTable->BootServices) {
            gSystemTable->BootServices->Stall(600000);
        }
        os_restart();
    }
}
