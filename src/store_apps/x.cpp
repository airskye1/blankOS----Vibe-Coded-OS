#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"
#include "store_kit.h"

extern "C" int main(OS_API* api) {
    SkInputCtx in;
    sk_init_ctx(api, &in);
    api->play_sound("open");

    int win_w = (in.sw >= 820) ? 740 : (in.sw - 36);
    int win_h = (in.sh >= 680) ? 600 : (in.sh - 36);
    int win_x = (in.sw - win_w) / 2;
    int win_y = (in.sh - win_h) / 2;

    int tab = 0; // 0=For You, 1=Following, 2=Profile
    bool compose_focus = true;
    char compose_buf[72] = "";
    int compose_len = 0;

    const char* authors[3] = { "@blankos_dev", "@horizon_ui", "@lira_audio" };
    const char* posts[3] = {
        "Shipped TLS 1.3 X25519 + ChaCha20-Poly1305 natively in UEFI!",
        "Tactile Obsidian Bento cards + zero neon glow = pure clarity.",
        "48kHz stereo Intel HDA FM synthesis feels alive on every click."
    };
    int likes[3] = { 128, 84, 67 };
    bool liked[3] = { false, false, false };
    int reposts[3] = { 34, 19, 15 };
    bool reposted[3] = { false, false, false };

    char user_posts[3][72];
    int user_cnt = 0;
    bool done = false;

    while (!done) {
        sk_poll_mouse(&in);

        EFI_INPUT_KEY key;
        while (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) {
                api->play_sound("close");
                done = true;
            } else if (compose_focus) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) {
                    if (compose_len > 0) compose_buf[--compose_len] = '\0';
                } else if (key.UnicodeChar == 13 || key.UnicodeChar == 10) {
                    if (compose_len > 0) {
                        int slot = (user_cnt < 3) ? user_cnt++ : 2;
                        if (user_cnt == 3 && slot == 2) {
                            sk_scpy(user_posts[0], user_posts[1], 72);
                            sk_scpy(user_posts[1], user_posts[2], 72);
                        }
                        sk_scpy(user_posts[slot], compose_buf, 72);
                        compose_buf[0] = '\0';
                        compose_len = 0;
                        api->play_sound("send");
                    }
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && compose_len < 68) {
                    compose_buf[compose_len++] = (char)key.UnicodeChar;
                    compose_buf[compose_len] = '\0';
                }
            }
        }

        if (sk_begin_app_frame(api, &in, win_x, win_y, win_w, win_h, "X Horizon", "BLOE Feed", SK_CYAN)) {
            done = true;
        }

        int px = win_x + 14;
        int py = win_y + 46;
        int pw = win_w - 28;

        // Top Navigation Pills
        const char* tab_lbl[3] = { "For You", "Following", "My Posts" };
        for (int t = 0; t < 3; t++) {
            if (sk_btn(api, px + t * 124, py, 114, 32, tab_lbl[t], SK_CYAN, tab == t, &in)) {
                tab = t;
            }
        }
        sk_pill(api, px + pw - 110, py + 4, 110, 24, "Live Stream", SK_GREEN, SK_SURFACE_CARD);

        // Composer Card
        int cy = py + 42;
        sk_bento_card(api, px, cy, pw, 84, 16, SK_SURFACE_CARD, compose_focus, SK_CYAN);
        if (in.left_pressed && sk_pt_in(in.mx, in.my, px + 14, cy + 12, pw - 130, 34)) {
            compose_focus = true;
        }
        sk_bento_card(api, px + 14, cy + 12, pw - 134, 34, 10, SK_SURFACE_INP, compose_focus, SK_BORDER_SPEC);
        api->draw_text(px + 26, cy + 21,
                       compose_len > 0 ? compose_buf : "What's building on bare metal?",
                       compose_len > 0 ? SK_TEXT_PRI : SK_TEXT_MUT, 1);

        if (sk_btn(api, px + pw - 108, cy + 12, 94, 34, "Post", SK_CYAN, true, &in) && compose_len > 0) {
            int slot = (user_cnt < 3) ? user_cnt++ : 2;
            sk_scpy(user_posts[slot], compose_buf, 72);
            compose_buf[0] = '\0';
            compose_len = 0;
            api->play_sound("send");
        }
        api->draw_text(px + 18, cy + 56, "Horizon BLOE Social  -  Connected via BlankNet", SK_TEXT_MUT, 1);

        // Feed Cards
        int fy = cy + 96;
        if (tab < 2) {
            for (int i = 0; i < 3; i++) {
                int y = fy + i * 118;
                sk_bento_card(api, px, y, pw, 108, 16, SK_SURFACE_CARD, false, SK_BORDER_SUB);
                api->draw_circle(px + 30, y + 32, 16, (i == 0) ? SK_CYAN : ((i == 1) ? SK_IRIS : SK_TEAL), 240);
                api->draw_text(px + 58, y + 14, authors[i], SK_CYAN, 1);
                api->draw_text(px + 58, y + 38, posts[i], SK_TEXT_PRI, 1);

                char lbuf[24] = "Like ";
                char nb[12];
                sk_int_to_str(likes[i], nb);
                sk_scat(lbuf, nb, sizeof(lbuf));
                if (sk_btn(api, px + 58, y + 66, 98, 28, lbuf, liked[i] ? SK_ROSE : SK_SURFACE_ELEV, liked[i], &in)) {
                    liked[i] = !liked[i];
                    likes[i] += liked[i] ? 1 : -1;
                    api->play_sound(liked[i] ? "success" : "click");
                }

                char rbuf[24] = "Repost ";
                sk_int_to_str(reposts[i], nb);
                sk_scat(rbuf, nb, sizeof(rbuf));
                if (sk_btn(api, px + 166, y + 66, 110, 28, rbuf, reposted[i] ? SK_GREEN : SK_SURFACE_ELEV, reposted[i], &in)) {
                    reposted[i] = !reposted[i];
                    reposts[i] += reposted[i] ? 1 : -1;
                    api->play_sound("notify");
                }
            }
        } else {
            if (user_cnt == 0) {
                sk_bento_card(api, px, fy, pw, 120, 16, SK_SURFACE_CARD, false, SK_BORDER_SUB);
                api->draw_text(px + 24, fy + 48, "No posts yet. Write in the composer above and click Post!", SK_TEXT_SEC, 1);
            } else {
                for (int i = 0; i < user_cnt; i++) {
                    int y = fy + i * 96;
                    sk_bento_card(api, px, y, pw, 86, 16, SK_SURFACE_ELEV, true, SK_TEAL);
                    api->draw_text(px + 20, y + 14, "@horizon_user  -  Just now", SK_TEAL, 1);
                    api->draw_text(px + 20, y + 40, user_posts[i], SK_TEXT_PRI, 1);
                }
            }
        }

        sk_end_app_frame(api, &in);
    }

    return 0;
}
