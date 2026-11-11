#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <efi.h>

#include "../ui/blankDUI.h"
#include "../ui/theme.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"

extern "C" {
    extern EFI_SYSTEM_TABLE* gSystemTable;
}

static int intro_step = 0;

extern "C" void play_intro_animation(void) {
    int sw = (int)(screen_width > 0 ? screen_width : 1024);
    int sh = (int)(screen_height > 0 ? screen_height : 768);
    BlankInputState in;

    const int total_frames = 55;
    for (int frame = 0; frame <= total_frames; frame++) {
        input_poll(&in);
        audio_tick();

        if (in.left_pressed ||
            (in.has_key && (in.key_char == '\r' || in.key_char == '\n' ||
                            in.key_char == ' ' || in.scan_code == BLANK_SCAN_ESC))) {
            break;
        }

        float t = (float)frame / (float)total_frames;
        float e = horizon_ease_out_cubic(t);

        dui_draw_wallpaper(dui_get_wallpaper_id());
        uint8_t scrim_alpha = (uint8_t)(210 - (int)(130.0f * e));
        dui_rect(0, 0, sw, sh, THEME_BG_VOID, scrim_alpha);

        int cx = sw / 2;
        int cy = sh / 2;

        int ring_r = 36 + (int)(26.0f * e);
        dui_circle(cx, cy - 36, ring_r + 36, THEME_ACCENT_IRIS, (uint8_t)(24.0f * e));
        dui_progress_ring(cx, cy - 36, ring_r, 4, e, THEME_SURFACE_ELEVATED, THEME_ACCENT_CYAN);
        dui_circle(cx, cy - 36, 16, THEME_ACCENT_CYAN, (uint8_t)(160.0f * e));

        const char* title = "Welcome to BlankOS Horizon";
        int tw = dui_text_width(title, 2);
        dui_text(cx - tw / 2, cy + 40, title, THEME_TEXT_PRIMARY, 2);

        const char* sub = "Spatial Glass Desktop Environment";
        int sub_w = dui_text_width(sub, 1);
        dui_text(cx - sub_w / 2, cy + 74, sub, THEME_TEXT_SECONDARY, 1);

        blankUI_draw_cursor(in.mx, in.my, 0);
        swap_buffers();

        if (gSystemTable && gSystemTable->BootServices) {
            gSystemTable->BootServices->Stall(20000);
        }
    }
}

extern "C" void launch_introduction_walkthrough(void) {
    int sw = (int)(screen_width > 0 ? screen_width : 1024);
    int sh = (int)(screen_height > 0 ? screen_height : 768);
    BlankInputState in;
    bool open = true;

    const char* titles[4] = {
        "Welcome to BlankOS Horizon",
        "Global Spotlight Search",
        "Control Center & Power",
        "You Are Ready"
    };
    const char* bodies[4] = {
        "A custom 64-bit UEFI kernel paired with the Horizon Spatial Glass compositor.",
        "Open Spotlight from the top bar or press Super/F1 to search apps and system tools.",
        "Use the top-right status pill for Audio, Network, Display Resolution, and Power.",
        "Everything is configured and ready. Enjoy exploring BlankOS Horizon."
    };

    while (open) {
        input_poll(&in);
        audio_tick();

        if (in.has_key && in.scan_code == BLANK_SCAN_ESC) {
            open = false;
            break;
        }
        if (in.has_key && (in.key_char == '\r' || in.key_char == '\n')) {
            if (intro_step < 3) intro_step++;
            else open = false;
        }

        dui_draw_wallpaper(dui_get_wallpaper_id());
        dui_rect(0, 0, sw, sh, THEME_BG_VOID, 90);

        int card_w = 560;
        int card_h = 300;
        int card_x = (sw - card_w) / 2;
        int card_y = (sh - card_h) / 2;

        dui_shadow(card_x, card_y, card_w, card_h, THEME_RADIUS_XL, 12);
        dui_glass_card(card_x, card_y, card_w, card_h, THEME_RADIUS_XL, THEME_SURFACE_BASE, 224, true);

        // Step dots
        int dot_start_x = card_x + card_w / 2 - 30;
        for (int i = 0; i < 4; i++) {
            dui_circle(dot_start_x + i * 20, card_y + 30, (i == intro_step) ? 5 : 3,
                       (i == intro_step) ? THEME_ACCENT_CYAN : THEME_BORDER_SPECULAR, 255);
        }

        const char* t = titles[intro_step];
        int tw = dui_text_width(t, 2);
        dui_text(card_x + (card_w - tw) / 2, card_y + 68, t, THEME_TEXT_PRIMARY, 2);

        const char* b = bodies[intro_step];
        int bw = dui_text_width(b, 1);
        int bx = (bw < card_w - 64) ? (card_x + (card_w - bw) / 2) : (card_x + 32);
        dui_text_ellipsis(bx, card_y + 128, card_w - 64, b, THEME_TEXT_SECONDARY, 1);

        int btn_y = card_y + card_h - 62;
        if (intro_step > 0) {
            if (dui_button(card_x + 32, btn_y, 100, 36, "Back",
                           THEME_SURFACE_ELEVATED, false, in.mx, in.my, in.left_pressed)) {
                play_system_sound((char*)"click");
                intro_step--;
            }
        }

        const char* next_lbl = (intro_step == 3) ? "Finish" : "Next";
        if (dui_button(card_x + card_w - 152, btn_y, 120, 36, next_lbl,
                       THEME_ACCENT_CYAN, true, in.mx, in.my, in.left_pressed)) {
            play_system_sound((char*)"click");
            if (intro_step < 3) intro_step++;
            else open = false;
        }

        blankUI_draw_cursor(in.mx, in.my, 0);
        swap_buffers();

        if (gSystemTable && gSystemTable->BootServices) {
            gSystemTable->BootServices->Stall(16000);
        }
    }
}
