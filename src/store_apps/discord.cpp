#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"
#include "store_kit.h"

extern "C" int main(OS_API* api) {
    SkInputCtx in;
    sk_init_ctx(api, &in);
    api->play_sound("open");

    int win_w = (in.sw >= 900) ? 840 : (in.sw - 40);
    int win_h = (in.sh >= 660) ? 570 : (in.sh - 40);
    int win_x = (in.sw - win_w) / 2;
    int win_y = (in.sh - win_h) / 2;

    int active_srv = 0; // 0=BlankOS, 1=Kernel, 2=Audio
    int active_ch = 0;  // 0=#general, 1=#horizon-ui, 2=#kernel-dev, 3=#releases
    bool voice_joined = false;
    bool input_focus = true;
    int frame = 0;

    char user_msgs[4][8][72];
    int user_msg_cnt[4] = {0, 0, 0, 0};
    for (int c = 0; c < 4; c++) {
        for (int m = 0; m < 8; m++) user_msgs[c][m][0] = '\0';
    }

    char draft[72] = "";
    int draft_len = 0;
    bool done = false;

    const char* srv_tags[3] = { "BL", "KN", "FX" };
    const uint32_t srv_cols[3] = { SK_IRIS, SK_CYAN, SK_TEAL };
    const char* ch_names[4] = { "# general", "# horizon-ui", "# kernel-dev", "# releases" };

    while (!done) {
        frame++;
        sk_poll_mouse(&in);

        EFI_INPUT_KEY key;
        while (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) {
                api->play_sound("close");
                done = true;
            } else if (input_focus) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) {
                    if (draft_len > 0) draft[--draft_len] = '\0';
                } else if (key.UnicodeChar == 13 || key.UnicodeChar == 10) {
                    if (draft_len > 0) {
                        int idx = user_msg_cnt[active_ch];
                        if (idx >= 8) {
                            for (int k = 0; k < 7; k++) sk_scpy(user_msgs[active_ch][k], user_msgs[active_ch][k + 1], 72);
                            idx = 7;
                        } else {
                            user_msg_cnt[active_ch]++;
                        }
                        sk_scpy(user_msgs[active_ch][idx], draft, 72);
                        draft[0] = '\0';
                        draft_len = 0;
                        api->play_sound("send");
                    }
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && draft_len < 68) {
                    draft[draft_len++] = (char)key.UnicodeChar;
                    draft[draft_len] = '\0';
                }
            }
        }

        if (sk_begin_app_frame(api, &in, win_x, win_y, win_w, win_h, "Discord Horizon", "BLOE Chat", SK_IRIS)) {
            done = true;
        }

        int bx = win_x + 12;
        int by = win_y + 44;
        int bh = win_h - 56;

        // 1. Server Rail
        int rail_w = 56;
        sk_bento_card(api, bx, by, rail_w, bh, 14, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        for (int s = 0; s < 3; s++) {
            int sy = by + 14 + s * 56;
            bool sel = (s == active_srv);
            if (sel) api->draw_rect_rounded(bx + 2, sy + 10, 4, 24, 2, srv_cols[s], 255);
            if (sk_btn(api, bx + 10, sy + 2, 36, 36, srv_tags[s], srv_cols[s], sel, &in)) {
                active_srv = s;
            }
        }
        api->draw_circle(bx + 28, by + bh - 26, 6, SK_GREEN, 255);

        // 2. Channels Sidebar
        int ch_x = bx + rail_w + 10;
        int ch_w = 178;
        sk_bento_card(api, ch_x, by, ch_w, bh, 14, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        api->draw_text(ch_x + 14, by + 14, "HORIZON STUDIO", SK_TEXT_PRI, 1);
        api->draw_text(ch_x + 14, by + 34, "CHANNELS", SK_TEXT_MUT, 1);

        for (int c = 0; c < 4; c++) {
            int cy = by + 56 + c * 38;
            bool sel = (c == active_ch);
            if (sk_btn(api, ch_x + 10, cy, ch_w - 20, 32, ch_names[c], SK_IRIS, sel, &in)) {
                active_ch = c;
            }
        }

        // Voice Lounge Card at bottom of sidebar
        int vc_y = by + bh - 92;
        sk_bento_card(api, ch_x + 10, vc_y, ch_w - 20, 80, 12, SK_SURFACE_INP, voice_joined, SK_GREEN);
        api->draw_text(ch_x + 20, vc_y + 10, voice_joined ? "Voice: Connected" : "Voice Lounge", voice_joined ? SK_GREEN : SK_TEXT_SEC, 1);
        if (voice_joined) {
            for (int b = 0; b < 5; b++) {
                int bh_bar = 6 + ((frame / 4 + b * 5) % 14);
                api->draw_rect_rounded(ch_x + 20 + b * 8, vc_y + 42 - bh_bar, 4, bh_bar, 2, SK_GREEN, 235);
            }
        }
        if (sk_btn(api, ch_x + 20, vc_y + 46, ch_w - 40, 26, voice_joined ? "Disconnect" : "Join Audio", SK_GREEN, voice_joined, &in)) {
            voice_joined = !voice_joined;
            api->play_sound(voice_joined ? "notify" : "close");
        }

        // 3. Main Chat Bento Panel
        int chat_x = ch_x + ch_w + 10;
        int chat_w = win_x + win_w - 12 - chat_x - 160;
        sk_bento_card(api, chat_x, by, chat_w, bh, 14, SK_SURFACE_BASE, true, SK_BORDER_SUB);
        sk_dot_grid(api, chat_x, by, chat_w, 50, 16, SK_IRIS);

        api->draw_text(chat_x + 16, by + 14, ch_names[active_ch], SK_TEXT_PRI, 2);
        sk_pill(api, chat_x + chat_w - 104, by + 14, 90, 22, "TLS Online", SK_GREEN, SK_SURFACE_CARD);
        api->draw_line(chat_x + 10, by + 48, chat_x + chat_w - 10, by + 48, SK_BORDER_SUB, 1);

        // Seeded channel messages + live user messages
        const char* seed_author[4][2] = {
            { "Studio Bot", "Architect" },
            { "UI Lead",    "Lira Kit"  },
            { "Kernel",     "NetStack"  },
            { "ReleaseBot", "Updater"   }
        };
        const char* seed_body[4][2] = {
            { "Welcome to Horizon v3.1! Press Enter to send.", "BLOE apps now launch on relocated ELF64 segments." },
            { "Zero-halo Obsidian bento cards look ultra clean.", "Tactile spring detents feel crisp at 60 FPS." },
            { "TLS 1.3 X25519 + ChaCha20 connected to GitHub.", "GPT FAT32 adaptive cluster sizing is active." },
            { "BlankOS Horizon v3.1 BLOE Suite ready for ISO.", "OTA manifest verified on github.com/airskye1." }
        };

        int msg_y = by + 62;
        for (int i = 0; i < 2; i++) {
            sk_bento_card(api, chat_x + 14, msg_y, chat_w - 28, 52, 12, SK_SURFACE_CARD, false, SK_BORDER_SUB);
            api->draw_circle(chat_x + 34, msg_y + 26, 12, (i == 0) ? SK_IRIS : SK_CYAN, 235);
            api->draw_text(chat_x + 56, msg_y + 8, seed_author[active_ch][i], (i == 0) ? SK_IRIS : SK_CYAN, 1);
            api->draw_text(chat_x + 56, msg_y + 28, seed_body[active_ch][i], SK_TEXT_PRI, 1);
            msg_y += 60;
        }

        int max_show = (bh - 246) / 48;
        if (max_show < 1) max_show = 1;
        int start_i = (user_msg_cnt[active_ch] > max_show) ? (user_msg_cnt[active_ch] - max_show) : 0;
        for (int i = start_i; i < user_msg_cnt[active_ch]; i++) {
            sk_bento_card(api, chat_x + 14, msg_y, chat_w - 28, 42, 10, SK_SURFACE_ELEV, true, SK_TEAL);
            api->draw_text(chat_x + 26, msg_y + 6, "You  -  Just now", SK_TEAL, 1);
            api->draw_text(chat_x + 26, msg_y + 22, user_msgs[active_ch][i], SK_TEXT_PRI, 1);
            msg_y += 48;
        }

        // Message Composer Bar
        int comp_y = by + bh - 50;
        int comp_w = chat_w - 108;
        if (in.left_pressed) {
            input_focus = sk_pt_in(in.mx, in.my, chat_x + 14, comp_y, comp_w, 36);
        }
        sk_bento_card(api, chat_x + 14, comp_y, comp_w, 36, 10, SK_SURFACE_INP, input_focus, SK_IRIS);
        api->draw_text(chat_x + 26, comp_y + 10,
                       draft_len > 0 ? draft : "Type a message and press Enter...",
                       draft_len > 0 ? SK_TEXT_PRI : SK_TEXT_MUT, 1);

        if (sk_btn(api, chat_x + 14 + comp_w + 8, comp_y, 72, 36, "Send", SK_IRIS, true, &in) && draft_len > 0) {
            int idx = user_msg_cnt[active_ch];
            if (idx >= 8) {
                for (int k = 0; k < 7; k++) sk_scpy(user_msgs[active_ch][k], user_msgs[active_ch][k + 1], 72);
                idx = 7;
            } else {
                user_msg_cnt[active_ch]++;
            }
            sk_scpy(user_msgs[active_ch][idx], draft, 72);
            draft[0] = '\0';
            draft_len = 0;
            api->play_sound("send");
        }

        // 4. Online Members Drawer
        int mem_x = chat_x + chat_w + 10;
        int mem_w = win_x + win_w - 12 - mem_x;
        sk_bento_card(api, mem_x, by, mem_w, bh, 14, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        api->draw_text(mem_x + 12, by + 14, "ONLINE - 4", SK_TEXT_MUT, 1);
        const char* members[4] = { "HorizonBot", "KernelDev", "LiraSynth", "You" };
        const uint32_t mcols[4] = { SK_IRIS, SK_CYAN, SK_TEAL, SK_GREEN };
        for (int m = 0; m < 4; m++) {
            int my = by + 44 + m * 42;
            api->draw_rect_rounded(mem_x + 8, my, mem_w - 16, 34, 8, SK_SURFACE_INP, 220);
            api->draw_circle(mem_x + 22, my + 17, 5, mcols[m], 255);
            api->draw_text(mem_x + 36, my + 9, members[m], SK_TEXT_PRI, 1);
        }

        api->draw_text(mem_x + 12, by + bh - 26, "ESC: Exit", SK_TEXT_MUT, 1);
        sk_end_app_frame(api, &in);
    }

    return 0;
}
