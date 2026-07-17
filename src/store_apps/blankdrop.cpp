#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"

extern "C" int main(OS_API* api) {
    int win_w = 540;
    int win_h = 420;
    int win_x = (1024 - win_w) / 2;
    int win_y = (768 - win_h) / 2;

    EFI_SIMPLE_POINTER_PROTOCOL* mouse = NULL;
    EFI_GUID mouse_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    api->SystemTable->BootServices->LocateProtocol(&mouse_guid, NULL, (void**)&mouse);
    if (mouse) mouse->Reset(mouse, TRUE);

    int mouse_x = 512;
    int mouse_y = 384;
    bool mouse_click = false;
    bool done = false;

    // BlankDrop state: 0=Scanning, 1=Device Found, 2=Sending, 3=Sent
    int state = 0;
    int timer = 0;
    float send_progress = 0.0f;
    int selected_device = -1;

    const char* devices[] = { "airskye's iPhone", "MacBook Pro", "iPad Pro" };

    while (!done) {
        mouse_click = false;
        if (mouse) {
            EFI_SIMPLE_POINTER_STATE mstate;
            if (mouse->GetState(mouse, &mstate) == EFI_SUCCESS) {
                int dx = mstate.RelativeMovementX / 1000;
                int dy = mstate.RelativeMovementY / 1000;
                mouse_x += dx;
                mouse_y += dy;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_x > 1023) mouse_x = 1023;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_y > 767) mouse_y = 767;
                if (mstate.LeftButton) {
                    mouse_click = true;
                }
            }
        }

        EFI_INPUT_KEY key;
        if (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) { // Escape
                done = true;
            }
        }

        // State Machine
        timer++;
        if (state == 0) { // Scanning
            if (timer > 40) {
                state = 1; // Discover devices
                timer = 0;
            }
        }
        else if (state == 2) { // Sending
            send_progress += 0.02f;
            if (send_progress >= 1.0f) {
                state = 3; // Sent
                api->play_sound("startup"); // Done chime
            }
        }

        // Draw system UI
        api->draw_wallpaper();
        api->draw_menubar();
        api->draw_dock();

        // Draw BlankDrop window
        api->draw_window(win_x, win_y, win_w, win_h, "BlankDrop");

        // Close window hit test
        if (mouse_click && mouse_x >= win_x + 10 && mouse_x <= win_x + 22 && mouse_y >= win_y + 10 && mouse_y <= win_y + 22) {
            done = true;
        }

        int center_x = win_x + win_w / 2;
        int center_y = win_y + 160;

        if (state == 0) { // Scanning Animation
            api->draw_text(win_x + 50, win_y + 60, "BlankDrop: Receiving or Sharing files...", 0x1C1C1E, 1);
            api->draw_text(win_x + 50, win_y + 85, "Ensure your local Wi-Fi / Bluetooth drivers are active.", 0x8E8E93, 1);

            // Draw radar circles expanding
            int r0 = (timer * 2) % 80;
            int r1 = ((timer * 2) + 40) % 80;
            api->draw_circle(center_x, center_y, r0, 0x007AFF, (uint8_t)(255 - r0 * 3));
            api->draw_circle(center_x, center_y, r1, 0x007AFF, (uint8_t)(255 - r1 * 3));
            api->draw_circle(center_x, center_y, 10, 0x007AFF, 255);

            api->draw_text(center_x - 55, center_y + 110, "Searching for nearby devices...", 0x8E8E93, 1);
        }
        else if (state == 1) { // Device selection list
            api->draw_text(win_x + 50, win_y + 60, "Select a device below to send 'BlankOS_SDK.pdf':", 0x1C1C1E, 1);
            
            for (int i = 0; i < 3; i++) {
                int item_y = win_y + 100 + i * 65;
                api->draw_rect_rounded(win_x + 40, item_y, win_w - 80, 50, 8, 0xF2F2F7, 255);
                
                // Icon circle
                api->draw_circle(win_x + 75, item_y + 25, 16, 0x007AFF, 255);
                api->draw_text(win_x + 70, item_y + 17, "D", 0xFFFFFF, 1);

                api->draw_text(win_x + 110, item_y + 17, devices[i], 0x1C1C1E, 1);
                api->draw_text(win_x + 110, item_y + 35, "Tap to send", 0x8E8E93, 1);

                if (mouse_click && mouse_x >= win_x + 40 && mouse_x <= win_x + win_w - 40 && mouse_y >= item_y && mouse_y <= item_y + 50) {
                    selected_device = i;
                    state = 2; // Transition to sending
                    send_progress = 0.0f;
                    api->play_sound("click");
                }
            }
        }
        else if (state == 2) { // Sending files animation
            api->draw_text(win_x + 50, win_y + 60, "Sending file to device:", 0x1C1C1E, 1);
            api->draw_text(win_x + 50, win_y + 85, devices[selected_device], 0x007AFF, 2);

            // Progress bar
            int progress_y = win_y + 180;
            api->draw_rect_rounded(win_x + 50, progress_y, win_w - 100, 14, 7, 0xE5E5EA, 255);
            api->draw_rect_rounded(win_x + 50, progress_y, (int)((win_w - 100) * send_progress), 14, 7, 0x34C759, 255);

            api->draw_text(win_x + 50, progress_y + 30, "File: BlankOS_SDK.pdf (4.2 MB)", 0x8E8E93, 1);
        }
        else if (state == 3) { // Send Complete
            api->draw_circle(center_x, center_y, 40, 0x34C759, 255);
            api->draw_text(center_x - 10, center_y - 10, "OK", 0xFFFFFF, 2);

            api->draw_text(win_x + 50, center_y + 70, "File sent successfully!", 0x34C759, 2);
            api->draw_text(win_x + 50, center_y + 105, devices[selected_device], 0x1C1C1E, 1);
            
            if (api->draw_button(win_x + (win_w - 160)/2, win_y + win_h - 70, 160, 36, "Send Another", mouse_x, mouse_y, mouse_click)) {
                state = 1;
                api->play_sound("click");
            }
        }

        // Draw cursor
        api->draw_cursor(mouse_x, mouse_y);
        api->swap_buffers();
    }
    return 0;
}
