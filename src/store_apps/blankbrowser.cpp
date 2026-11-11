#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"
#include "store_kit.h"

extern "C" int main(OS_API* api) {
    SkInputCtx in;
    sk_init_ctx(api, &in);
    api->play_sound("open");

    int win_w = (in.sw >= 900) ? 840 : (in.sw - 36);
    int win_h = (in.sh >= 660) ? 570 : (in.sh - 36);
    int win_x = (in.sw - win_w) / 2;
    int win_y = (in.sh - win_h) / 2;

    char url[96] = "blank://home";
    int url_len = 12;
    bool url_focus = false;
    int page = 0; // 0=home, 1=sdk, 2=tls, 3=notes
    int load_anim = 100;
    bool done = false;

    const char* urls[4] = { "blank://home", "blank://bloe-sdk", "blank://tls13", "blank://changelog" };

    auto go_page = [&](int p) {
        page = p;
        sk_scpy(url, urls[p], sizeof(url));
        url_len = sk_slen(url);
        load_anim = 15;
        api->play_sound("tab");
    };

    while (!done) {
        sk_poll_mouse(&in);

        EFI_INPUT_KEY key;
        while (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) {
                api->play_sound("close");
                done = true;
            } else if (url_focus) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) {
                    if (url_len > 0) url[--url_len] = '\0';
                } else if (key.UnicodeChar == 13 || key.UnicodeChar == 10) {
                    if (sk_str_eq(url, urls[1])) go_page(1);
                    else if (sk_str_eq(url, urls[2])) go_page(2);
                    else if (sk_str_eq(url, urls[3])) go_page(3);
                    else go_page(0);
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar < 127 && url_len < 90) {
                    url[url_len++] = (char)key.UnicodeChar;
                    url[url_len] = '\0';
                }
            }
        }

        if (load_anim < 100) load_anim += 12;
        if (load_anim > 100) load_anim = 100;

        if (sk_begin_app_frame(api, &in, win_x, win_y, win_w, win_h, "MiniSurf Web", "BLOE Browser", SK_CYAN)) {
            done = true;
        }

        int px = win_x + 14;
        int py = win_y + 46;
        int pw = win_w - 28;

        // Top URL & Bookmark Bar
        sk_bento_card(api, px, py, pw, 78, 14, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        if (sk_btn(api, px + 12, py + 10, 68, 30, "Home", SK_SURFACE_ELEV, false, &in)) go_page(0);

        int ux = px + 88;
        int uw = pw - 190;
        if (in.left_pressed) url_focus = sk_pt_in(in.mx, in.my, ux, py + 10, uw, 30);
        sk_bento_card(api, ux, py + 10, uw, 30, 8, SK_SURFACE_INP, url_focus, SK_CYAN);
        api->draw_text(ux + 10, py + 18, url_len > 0 ? url : "blank://...", SK_TEXT_PRI, 1);

        if (sk_btn(api, ux + uw + 8, py + 10, 78, 30, "Reload", SK_CYAN, true, &in)) go_page(page);

        const char* bm[4] = { "Start Page", "BLOE SDK", "TLS 1.3 Net", "v3.1 Notes" };
        for (int i = 0; i < 4; i++) {
            if (sk_btn(api, px + 12 + i * 116, py + 46, 108, 24, bm[i], SK_TEAL, page == i, &in)) {
                go_page(i);
            }
        }

        if (load_anim < 100) {
            sk_progress_bar(api, px, py + 82, pw, 4, (float)load_anim / 100.0f, SK_CYAN);
        }

        // Page Viewport
        int vy = py + 92;
        int vh = win_y + win_h - 14 - vy;
        sk_bento_card(api, px, vy, pw, vh, 16, SK_SURFACE_BASE, true, SK_BORDER_SUB);
        sk_dot_grid(api, px + 8, vy + 8, pw - 16, 110, 18, SK_CYAN);

        if (page == 0) {
            api->draw_text(px + 28, vy + 28, "MINISURF - BLANKOS HORIZON", SK_CYAN, 2);
            api->draw_text(px + 28, vy + 60, "Lightweight standalone web & docs reader running inside BLOE ELF64.", SK_TEXT_SEC, 1);

            int cw = (pw - 72) / 3;
            const char* c_tit[3] = { "BLOE App Kit", "TLS 1.3 Stack", "v3.1 Changelog" };
            const char* c_sub[3] = { "Relocated ELF64 SDK", "GitHub X25519 HTTPS", "Lira UI & Sound" };
            const uint32_t c_col[3] = { SK_IRIS, SK_GREEN, SK_TEAL };
            for (int i = 0; i < 3; i++) {
                int cx = px + 24 + i * (cw + 12);
                sk_bento_card(api, cx, vy + 108, cw, 150, 14, SK_SURFACE_CARD, false, c_col[i]);
                api->draw_text(cx + 16, vy + 128, c_tit[i], c_col[i], 2);
                api->draw_text(cx + 16, vy + 162, c_sub[i], SK_TEXT_PRI, 1);
                if (sk_btn(api, cx + 16, vy + 206, cw - 32, 34, "Open Doc", c_col[i], true, &in)) {
                    go_page(i + 1);
                }
            }
        } else if (page == 1) {
            api->draw_text(px + 28, vy + 28, "BLOE ELF64 RUNTIME SPECIFICATION", SK_IRIS, 2);
            api->draw_text(px + 28, vy + 66, "- Maps PT_LOAD program headers + zeroes .bss safely in EfiLoaderCode", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 94, "- Resolves R_X86_64_RELATIVE GOT relocations for PIC binaries", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 122, "- Resolves named entry symbol 'main' before transferring control", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 150, "- Exposes OS_API graphics, FAT32 file I/O, RTC, and Lira audio", SK_TEXT_SEC, 1);
        } else if (page == 2) {
            api->draw_text(px + 28, vy + 28, "FREESTANDING TLS 1.3 NETWORKING", SK_GREEN, 2);
            api->draw_text(px + 28, vy + 66, "- Ephemeral X25519 key exchange (RFC 7748) + HKDF-SHA256", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 94, "- ChaCha20-Poly1305 & AES-128-GCM AEAD record protection", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 122, "- Powers GitHub OTA updates & Horizon Web HTTPS navigation", SK_TEXT_PRI, 1);
        } else {
            api->draw_text(px + 28, vy + 28, "BLANKOS HORIZON v3.1 RELEASE NOTES", SK_TEAL, 2);
            api->draw_text(px + 28, vy + 66, "1. GitHub TLS 1.3 System Updater with ISO release download", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 94, "2. Adaptive GPT + FAT32 disk installer (>= 40 MB disks supported)", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 122, "3. Intel HDA 48kHz stereo PCM + acoustic micro-detent clicks", SK_TEXT_PRI, 1);
            api->draw_text(px + 28, vy + 150, "4. Complete Lira Obsidian redesign of all 6 BLOE App Store apps", SK_TEXT_PRI, 1);
        }

        api->draw_text(px + 24, vy + vh - 28, "ESC or Red Close Dot: Return to Desktop", SK_TEXT_MUT, 1);
        sk_end_app_frame(api, &in);
    }

    return 0;
}
