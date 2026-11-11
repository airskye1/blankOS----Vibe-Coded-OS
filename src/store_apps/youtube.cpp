#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"
#include "store_kit.h"

extern "C" int main(OS_API* api) {
    SkInputCtx in;
    sk_init_ctx(api, &in);
    api->play_sound("open");

    int win_w = (in.sw >= 940) ? 880 : (in.sw - 36);
    int win_h = (in.sh >= 660) ? 580 : (in.sh - 36);
    int win_x = (in.sw - win_w) / 2;
    int win_y = (in.sh - win_h) / 2;

    bool playing = true;
    float progress = 0.22f;
    int active_idx = 0;
    int likes[4] = { 1420, 892, 645, 518 };
    bool liked[4] = { false, false, false, false };
    bool subscribed = false;
    int frame = 0;
    bool done = false;

    const char* titles[4] = {
        "BlankOS Horizon v3.1: Lira Design Architecture",
        "TLS 1.3 + X25519 From Scratch in Freestanding C++",
        "Zero-Crash ELF64 Relocator & BLOE Container Spec",
        "Intel HDA 48kHz Polyphonic Audio Synthesis in UEFI"
    };
    const char* authors[4] = { "Horizon Labs", "Kernel Systems", "BLOE SDK Team", "Lira Acoustics" };
    const char* stats[4]   = { "128K views  -  Today", "64K views  -  2 days ago", "41K views  -  4 days ago", "29K views  -  1 week ago" };

    while (!done) {
        frame++;
        sk_poll_mouse(&in);

        EFI_INPUT_KEY key;
        while (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) {
                api->play_sound("close");
                done = true;
            } else if (key.UnicodeChar == ' ') {
                playing = !playing;
                api->play_sound("click");
            }
        }

        if (playing) {
            progress += 0.0014f;
            if (progress > 1.0f) progress = 0.0f;
        }

        if (sk_begin_app_frame(api, &in, win_x, win_y, win_w, win_h, "YouTube Studio", "BLOE Video", SK_ROSE)) {
            done = true;
        }

        int px = win_x + 14;
        int py = win_y + 46;
        int player_w = (win_w * 60) / 100;
        int video_h = (player_w * 9) / 16;
        if (video_h > win_h - 230) video_h = win_h - 230;

        // 1. Video Viewport Card
        sk_bento_card(api, px, py, player_w, video_h, 16, SK_SURFACE_INP, true, SK_ROSE);
        sk_dot_grid(api, px + 8, py + 8, player_w - 16, video_h - 40, 18, SK_CYAN);

        // Live animated audio spectrum inside video viewport
        int eq_x = px + 36;
        int eq_base_y = py + video_h - 52;
        int eq_bars = (player_w - 72) / 14;
        if (eq_bars > 28) eq_bars = 28;
        for (int b = 0; b < eq_bars; b++) {
            int amp = playing ? (18 + ((frame * 3 + b * 17 + active_idx * 9) % 68)) : 10;
            uint32_t col = (b % 3 == 0) ? SK_ROSE : ((b % 3 == 1) ? SK_CYAN : SK_TEAL);
            api->draw_rect_rounded(eq_x + b * 14, eq_base_y - amp, 8, amp, 4, col, 215);
        }

        sk_pill(api, px + 14, py + 14, 118, 22, playing ? "HD 60FPS LIVE" : "PAUSED",
                playing ? SK_ROSE : SK_AMBER, SK_SURFACE_CARD);

        // Seek scrub bar
        int bar_x = px + 14;
        int bar_y = py + video_h - 20;
        int bar_w = player_w - 28;
        if (in.left_pressed && sk_pt_in(in.mx, in.my, bar_x, bar_y - 6, bar_w, 20)) {
            progress = (float)(in.mx - bar_x) / (float)bar_w;
            api->play_sound("tab");
        }
        sk_progress_bar(api, bar_x, bar_y, bar_w, 8, progress, SK_ROSE);

        // 2. Controls + Info Bento Card
        int info_y = py + video_h + 12;
        int info_h = win_y + win_h - 14 - info_y;
        sk_bento_card(api, px, info_y, player_w, info_h, 16, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        api->draw_text(px + 16, info_y + 14, titles[active_idx], SK_TEXT_PRI, 1);
        api->draw_text(px + 16, info_y + 38, authors[active_idx], SK_CYAN, 1);
        api->draw_text(px + 150, info_y + 38, stats[active_idx], SK_TEXT_MUT, 1);

        int btn_y = info_y + info_h - 48;
        if (sk_btn(api, px + 16, btn_y, 88, 34, playing ? "Pause" : "Play", SK_ROSE, true, &in)) {
            playing = !playing;
        }
        if (sk_btn(api, px + 112, btn_y, 62, 34, "-10s", SK_SURFACE_ELEV, false, &in)) {
            progress -= 0.08f;
            if (progress < 0.0f) progress = 0.0f;
        }
        if (sk_btn(api, px + 180, btn_y, 62, 34, "+10s", SK_SURFACE_ELEV, false, &in)) {
            progress += 0.08f;
            if (progress > 1.0f) progress = 1.0f;
        }

        char ltxt[24] = "Like ";
        char lnum[12];
        sk_int_to_str(likes[active_idx], lnum);
        sk_scat(ltxt, lnum, sizeof(ltxt));
        if (sk_btn(api, px + player_w - 224, btn_y, 98, 34, ltxt, liked[active_idx] ? SK_CYAN : SK_SURFACE_ELEV, liked[active_idx], &in)) {
            liked[active_idx] = !liked[active_idx];
            likes[active_idx] += liked[active_idx] ? 1 : -1;
            api->play_sound(liked[active_idx] ? "success" : "click");
        }
        if (sk_btn(api, px + player_w - 118, btn_y, 102, 34, subscribed ? "Subscribed" : "Subscribe", SK_TEXT_PRI, !subscribed, &in)) {
            subscribed = !subscribed;
            api->play_sound(subscribed ? "reward" : "close");
        }

        // 3. Up Next Queue Sidebar
        int qx = px + player_w + 14;
        int qw = win_x + win_w - 14 - qx;
        int qh = win_y + win_h - 14 - py;
        sk_bento_card(api, qx, py, qw, qh, 16, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        api->draw_text(qx + 16, py + 14, "UP NEXT QUEUE", SK_TEXT_MUT, 1);

        for (int i = 0; i < 4; i++) {
            int cy = py + 44 + i * 96;
            bool sel = (i == active_idx);
            bool hov = sk_pt_in(in.mx, in.my, qx + 12, cy, qw - 24, 84);
            sk_bento_card(api, qx + 12, cy, qw - 24, 84, 12,
                          sel ? SK_SURFACE_ELEV : (hov ? SK_SURFACE_BASE : SK_SURFACE_INP),
                          sel || hov, sel ? SK_ROSE : SK_BORDER_SPEC);
            api->draw_rect_rounded(qx + 22, cy + 12, 84, 60, 8, (i == 0) ? SK_IRIS : (i == 1 ? SK_CYAN : (i == 2 ? SK_TEAL : SK_AMBER)), 180);
            api->draw_text(qx + 48, cy + 34, "PLAY", SK_TEXT_DARK, 1);
            api->draw_text(qx + 116, cy + 16, authors[i], SK_CYAN, 1);
            api->draw_text(qx + 116, cy + 38, (i == 0) ? "Horizon UI" : (i == 1 ? "TLS 1.3 Stack" : (i == 2 ? "BLOE ELF64" : "HDA Synth")), SK_TEXT_PRI, 1);
            api->draw_text(qx + 116, cy + 58, stats[i], SK_TEXT_MUT, 1);

            if (hov && in.left_pressed) {
                active_idx = i;
                progress = 0.0f;
                playing = true;
                api->play_sound("tab");
            }
        }

        api->draw_text(qx + 16, py + qh - 24, "Space: Play/Pause  |  ESC: Exit", SK_TEXT_MUT, 1);
        sk_end_app_frame(api, &in);
    }

    return 0;
}
