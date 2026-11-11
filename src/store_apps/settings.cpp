#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"
#include "store_kit.h"

extern "C" int main(OS_API* api) {
    SkInputCtx in;
    sk_init_ctx(api, &in);
    api->play_sound("open");

    int win_w = (in.sw >= 860) ? 780 : (in.sw - 36);
    int win_h = (in.sh >= 620) ? 540 : (in.sh - 36);
    int win_x = (in.sw - win_w) / 2;
    int win_y = (in.sh - win_h) / 2;

    int tab = 0; // 0=Network, 1=Bluetooth, 2=Audio & Sound, 3=Hardware
    bool wifi_on = true;
    int wifi_sel = 0;
    bool bt_on = true;
    bool bt_paired[4] = { true, false, false, false };
    int pair_anim = 0;
    bool pairing = false;
    bool done = false;

    const char* ssids[4] = { "Horizon_5G_Studio", "Starlink_Mesh", "BlankNet_Guest", "QEMU_Slirp" };
    const char* bt_names[4] = { "Lira Studio Headphones", "Horizon Trackpad", "Mechanical KeyDeck", "Display Monitor BT" };

    // Read real UEFI firmware vendor ASCII string
    char fw_vendor[48] = "UEFI Standard Firmware";
    if (api->SystemTable && api->SystemTable->FirmwareVendor) {
        int i = 0;
        const CHAR16* wv = api->SystemTable->FirmwareVendor;
        while (wv[i] && i < 46) {
            fw_vendor[i] = (wv[i] < 128) ? (char)wv[i] : '?';
            i++;
        }
        fw_vendor[i] = '\0';
    }

    while (!done) {
        sk_poll_mouse(&in);

        EFI_INPUT_KEY key;
        while (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) {
                api->play_sound("close");
                done = true;
            }
        }

        if (pairing) {
            pair_anim += 2;
            if (pair_anim >= 100) {
                pairing = false;
                pair_anim = 0;
                bt_paired[1] = true;
                api->play_sound("success");
            }
        }

        if (sk_begin_app_frame(api, &in, win_x, win_y, win_w, win_h, "Control Deck", "BLOE Deck", SK_TEAL)) {
            done = true;
        }

        int bx = win_x + 14;
        int by = win_y + 46;
        int bh = win_h - 60;

        // Left Navigation Rail
        int side_w = 186;
        sk_bento_card(api, bx, by, side_w, bh, 16, SK_SURFACE_CARD, false, SK_BORDER_SUB);
        const char* nav[4] = { "Wi-Fi & Net", "Bluetooth", "Sound Lab", "UEFI System" };
        for (int i = 0; i < 4; i++) {
            if (sk_btn(api, bx + 12, by + 16 + i * 48, side_w - 24, 38, nav[i], SK_TEAL, tab == i, &in)) {
                tab = i;
            }
        }
        sk_pill(api, bx + 12, by + bh - 38, side_w - 24, 24, "BLOE v3.1", SK_CYAN, SK_SURFACE_INP);

        // Right Content Bento Area
        int rx = bx + side_w + 12;
        int rw = win_x + win_w - 14 - rx;
        sk_bento_card(api, rx, by, rw, bh, 16, SK_SURFACE_BASE, true, SK_BORDER_SUB);
        sk_dot_grid(api, rx, by, rw, 56, 16, SK_TEAL);

        if (tab == 0) {
            api->draw_text(rx + 18, by + 16, "WI-FI & NETWORK ADAPTER", SK_TEXT_PRI, 2);
            if (sk_btn(api, rx + rw - 118, by + 12, 102, 30, wifi_on ? "Wi-Fi: ON" : "Wi-Fi: OFF", SK_GREEN, wifi_on, &in)) {
                wifi_on = !wifi_on;
            }
            for (int i = 0; i < 4; i++) {
                int y = by + 64 + i * 58;
                bool sel = (wifi_on && wifi_sel == i);
                sk_bento_card(api, rx + 16, y, rw - 32, 48, 12, SK_SURFACE_CARD, sel, SK_CYAN);
                api->draw_circle(rx + 34, y + 24, 6, sel ? SK_GREEN : SK_TEXT_MUT, 255);
                api->draw_text(rx + 52, y + 16, ssids[i], SK_TEXT_PRI, 1);
                if (wifi_on && sk_btn(api, rx + rw - 134, y + 8, 104, 32, sel ? "Connected" : "Connect", sel ? SK_GREEN : SK_CYAN, sel, &in)) {
                    wifi_sel = i;
                    api->play_sound("notify");
                }
            }
        } else if (tab == 1) {
            api->draw_text(rx + 18, by + 16, "BLUETOOTH PERIPHERALS", SK_TEXT_PRI, 2);
            if (sk_btn(api, rx + rw - 118, by + 12, 102, 30, bt_on ? "BT: ON" : "BT: OFF", SK_IRIS, bt_on, &in)) {
                bt_on = !bt_on;
            }
            for (int i = 0; i < 4; i++) {
                int y = by + 64 + i * 58;
                sk_bento_card(api, rx + 16, y, rw - 32, 48, 12, SK_SURFACE_CARD, bt_paired[i], SK_IRIS);
                api->draw_text(rx + 28, y + 16, bt_names[i], SK_TEXT_PRI, 1);
                if (bt_on && sk_btn(api, rx + rw - 134, y + 8, 104, 32, bt_paired[i] ? "Paired" : "Pair", SK_IRIS, bt_paired[i], &in)) {
                    if (!bt_paired[i] && i == 1) {
                        pairing = true;
                        pair_anim = 0;
                    } else {
                        bt_paired[i] = !bt_paired[i];
                    }
                }
            }
            if (pairing) {
                api->draw_text(rx + 18, by + 310, "Pairing Horizon Trackpad...", SK_CYAN, 1);
                sk_progress_bar(api, rx + 18, by + 334, rw - 36, 10, (float)pair_anim / 100.0f, SK_IRIS);
            }
        } else if (tab == 2) {
            api->draw_text(rx + 18, by + 16, "LIRA ACOUSTIC SOUND LAB", SK_TEXT_PRI, 2);
            api->draw_text(rx + 18, by + 54, "Trigger real-time synthesized OS audio cues:", SK_TEXT_SEC, 1);
            const char* cues[6] = { "startup", "open", "notify", "success", "error", "close" };
            const uint32_t ccols[6] = { SK_CYAN, SK_TEAL, SK_IRIS, SK_GREEN, SK_ROSE, SK_AMBER };
            for (int i = 0; i < 6; i++) {
                int col = i % 2;
                int row = i / 2;
                int cx = rx + 18 + col * ((rw - 48) / 2 + 12);
                int cy = by + 92 + row * 66;
                int cw = (rw - 48) / 2;
                sk_bento_card(api, cx, cy, cw, 54, 12, SK_SURFACE_CARD, false, ccols[i]);
                api->draw_text(cx + 14, cy + 19, cues[i], ccols[i], 1);
                if (sk_btn(api, cx + cw - 92, cy + 11, 78, 32, "Play", ccols[i], true, &in)) {
                    api->play_sound(cues[i]);
                }
            }
        } else {
            api->draw_text(rx + 18, by + 16, "UEFI HARDWARE INSPECTOR", SK_TEXT_PRI, 2);
            char res_str[32];
            char wb[12], hb[12];
            sk_int_to_str(in.sw, wb);
            sk_int_to_str(in.sh, hb);
            sk_scpy(res_str, wb, sizeof(res_str));
            sk_scat(res_str, " x ", sizeof(res_str));
            sk_scat(res_str, hb, sizeof(res_str));
            sk_scat(res_str, " (GOP 32bpp)", sizeof(res_str));

            const char* keys[4] = { "OS Release", "Firmware Vendor", "Display Framebuffer", "Executable Runtime" };
            const char* vals[4] = { "BlankOS Horizon v3.1", fw_vendor, res_str, "ELF64 Relocatable BLOE" };
            for (int i = 0; i < 4; i++) {
                int y = by + 64 + i * 64;
                sk_bento_card(api, rx + 16, y, rw - 32, 54, 12, SK_SURFACE_CARD, false, SK_BORDER_SUB);
                api->draw_text(rx + 30, y + 10, keys[i], SK_TEXT_MUT, 1);
                api->draw_text(rx + 30, y + 30, vals[i], SK_CYAN, 1);
            }
        }

        api->draw_text(rx + 18, by + bh - 26, "ESC or Red Close Dot: Return to BlankOS Desktop", SK_TEXT_MUT, 1);
        sk_end_app_frame(api, &in);
    }

    return 0;
}
