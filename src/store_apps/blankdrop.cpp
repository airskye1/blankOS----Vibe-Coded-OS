#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"
#include "store_kit.h"

extern "C" int main(OS_API* api) {
    SkInputCtx in;
    sk_init_ctx(api, &in);
    api->play_sound("open");

    int win_w = (in.sw >= 720) ? 620 : (in.sw - 36);
    int win_h = (in.sh >= 560) ? 470 : (in.sh - 36);
    int win_x = (in.sw - win_w) / 2;
    int win_y = (in.sh - win_h) / 2;

    int state = 0; // 0=Scanning, 1=Ready, 2=Sending, 3=Sent
    int timer = 0;
    float progress = 0.0f;
    int selected_dev = 0;
    int payload_idx = 0;
    bool receipt_saved = false;
    bool done = false;

    const char* devs[3] = { "Horizon Workstation", "Studio ProBook", "Mobile Node X" };
    const char* files[3] = { "system_profile.json", "horizon_build_v3.1.log", "store_manifest.bloe" };

    while (!done) {
        sk_poll_mouse(&in);

        EFI_INPUT_KEY key;
        while (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) {
                api->play_sound("close");
                done = true;
            }
        }

        timer++;
        if (state == 0 && timer > 35) {
            state = 1;
            timer = 0;
            api->play_sound("notify");
        } else if (state == 2) {
            progress += 0.025f;
            if (progress >= 1.0f) {
                progress = 1.0f;
                state = 3;
                const char log[] = "BlankDrop Transfer Complete - Horizon v3.1\r\n";
                receipt_saved = api->write_file("DOWNLOADS\\blankdrop_log.txt", (void*)log, sizeof(log) - 1);
                api->play_sound("success");
            }
        }

        if (sk_begin_app_frame(api, &in, win_x, win_y, win_w, win_h, "BlankDrop AirShare", "BLOE Share", SK_CYAN)) {
            done = true;
        }

        int px = win_x + 16;
        int py = win_y + 48;
        int pw = win_w - 32;

        // Radar Hero Card
        sk_bento_card(api, px, py, pw, 126, 16, SK_SURFACE_CARD, true, SK_CYAN);
        sk_dot_grid(api, px + 8, py + 8, pw - 16, 110, 18, SK_CYAN);

        int rcx = px + 66;
        int rcy = py + 63;
        int ring_r = 16 + ((timer / 2) % 30);
        api->draw_circle(rcx, rcy, 40, SK_SURFACE_INP, 230);
        api->draw_circle(rcx, rcy, ring_r, SK_CYAN, 75);
        api->draw_circle(rcx, rcy, 12, SK_CYAN, 255);

        api->draw_text(px + 132, py + 24,
                       (state == 0) ? "Scanning for nearby BlankOS nodes..." :
                       (state == 1) ? "3 Nearby AirShare Devices Ready" :
                       (state == 2) ? "Encrypting & beaming payload..." :
                                      "Payload Delivered Successfully!",
                       (state == 3) ? SK_GREEN : SK_TEXT_PRI, 2);

        api->draw_text(px + 132, py + 56, "Active Payload:", SK_TEXT_MUT, 1);
        if (sk_btn(api, px + 260, py + 50, 210, 26, files[payload_idx], SK_TEAL, false, &in)) {
            payload_idx = (payload_idx + 1) % 3;
        }
        if (state == 2 || state == 3) {
            sk_progress_bar(api, px + 132, py + 90, pw - 156, 10, progress, (state == 3) ? SK_GREEN : SK_CYAN);
        } else {
            api->draw_text(px + 132, py + 88, receipt_saved ? "Saved \\DOWNLOADS\\blankdrop_log.txt" : "Click a device below to transfer", SK_CYAN, 1);
        }

        // Device Grid
        int dy = py + 140;
        for (int i = 0; i < 3; i++) {
            int y = dy + i * 64;
            bool sel = (selected_dev == i);
            sk_bento_card(api, px, y, pw, 54, 14, sel ? SK_SURFACE_ELEV : SK_SURFACE_CARD, sel, SK_CYAN);
            api->draw_circle(px + 28, y + 27, 14, sel ? SK_CYAN : SK_IRIS, 235);
            api->draw_text(px + 54, y + 18, devs[i], SK_TEXT_PRI, 1);
            if (state >= 1 && sk_btn(api, px + pw - 116, y + 11, 100, 32,
                                     (state == 3 && sel) ? "Resend" : "Send File", SK_CYAN, sel, &in)) {
                selected_dev = i;
                progress = 0.0f;
                state = 2;
                api->play_sound("send");
            }
        }

        if (state == 3 && receipt_saved) {
            sk_pill(api, px, dy + 206, 290, 24, "Receipt saved to \\DOWNLOADS\\blankdrop_log.txt", SK_GREEN, SK_SURFACE_CARD);
        }

        sk_end_app_frame(api, &in);
    }

    return 0;
}
