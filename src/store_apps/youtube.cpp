#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"

static bool str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return (*a == *b);
}

static void str_copy(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

extern "C" int main(OS_API* api) {
    int win_w = 920;
    int win_h = 600;
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

    // Video states
    bool playing = true;
    float video_progress = 0.24f;
    int active_video_index = 0; // 0=OSDev, 1=Composition, 2=Setup

    const char* video_titles[] = {
        "Building a Custom OS from Scratch Natively",
        "VSync & Page Flipping: Fix Screen Tearing",
        "Formatting GPT and FAT32 Natively in C++"
    };

    const char* video_channels[] = {
        "airskye OSDev",
        "Compositor Devs",
        "Disk Formatting Pro"
    };

    const char* video_views[] = {
        "124K views - 2 days ago",
        "89K views - 5 days ago",
        "42K views - 1 week ago"
    };

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

        // Auto progress playing video
        if (playing) {
            video_progress += 0.002f;
            if (video_progress > 1.0f) {
                video_progress = 0.0f;
            }
        }

        // Draw system UI
        api->draw_wallpaper();
        api->draw_menubar();
        api->draw_dock();

        // Draw YouTube window
        api->draw_window(win_x, win_y, win_w, win_h, "YouTube");

        // Close window hit test
        if (mouse_click && mouse_x >= win_x + 10 && mouse_x <= win_x + 22 && mouse_y >= win_y + 10 && mouse_y <= win_y + 22) {
            done = true;
        }

        // Red topbar branding
        api->draw_rect(win_x + 1, win_y + 33, win_w - 2, 45, 0xFFFFFF, 255);
        api->draw_line(win_x + 1, win_y + 78, win_x + win_w - 2, win_y + 78, 0xE5E5EA, 1);
        
        api->draw_rect_rounded(win_x + 20, win_y + 44, 40, 22, 4, 0xFF0000, 255);
        api->draw_text(win_x + 24, win_y + 49, "Play", 0xFFFFFF, 1);
        api->draw_text(win_x + 70, win_y + 46, "YouTube", 0x1C1C1E, 2);

        // Left Main Video Column
        int main_col_x = win_x + 20;
        int main_col_y = win_y + 95;

        // Video Player mockup background (deep dark gray)
        api->draw_rect_rounded(main_col_x, main_col_y, 560, 315, 8, 0x1C1C1E, 255);
        
        // Circular play status indicator on screen
        if (!playing) {
            api->draw_circle(main_col_x + 280, main_col_y + 157, 30, 0x000000, 160);
            api->draw_text(main_col_x + 276, main_col_y + 149, "II", 0xFFFFFF, 2);
        } else {
            api->draw_text(main_col_x + 260, main_col_y + 150, "PLAYING", 0x444444, 1);
        }

        // Video Timeline Bar at bottom of screen
        int bar_y = main_col_y + 285;
        api->draw_rect_rounded(main_col_x + 15, bar_y, 530, 8, 4, 0x444444, 255);
        api->draw_rect_rounded(main_col_x + 15, bar_y, (int)(530 * video_progress), 8, 4, 0xFF0000, 255);

        // Control panel bar below player
        int control_y = main_col_y + 300;
        const char* play_lbl = playing ? "Pause" : "Play";
        if (api->draw_button(main_col_x + 15, control_y, 70, 20, play_lbl, mouse_x, mouse_y, mouse_click)) {
            playing = !playing;
            api->play_sound("click");
        }

        // Render Titles & Stats
        api->draw_text(main_col_x, main_col_y + 335, video_titles[active_video_index], 0x1C1C1E, 2);
        api->draw_text(main_col_x, main_col_y + 365, video_channels[active_video_index], 0x3A3A3C, 1);
        api->draw_text(main_col_x, main_col_y + 385, video_views[active_video_index], 0x8E8E93, 1);

        // Right recommendations column
        int rec_x = win_x + 600;
        int rec_y = win_y + 95;
        api->draw_text(rec_x, rec_y, "Recommended Up Next", 0x1C1C1E, 1);

        for (int i = 0; i < 3; i++) {
            int card_y = rec_y + 30 + i * 115;
            bool is_active = (active_video_index == i);

            api->draw_rect_rounded(rec_x, card_y, 300, 100, 8, is_active ? 0xE5E5EA : 0xF2F2F7, 255);
            
            // Red thumbnail placeholder
            api->draw_rect_rounded(rec_x + 10, card_y + 10, 100, 80, 4, 0xFF4D4D, 255);
            api->draw_text(rec_x + 40, card_y + 40, "YT", 0xFFFFFF, 1);

            api->draw_text(rec_x + 120, card_y + 15, (i == 0) ? "OSDev" : ((i == 1) ? "VSync" : "FAT32"), 0x1C1C1E, 1);
            api->draw_text(rec_x + 120, card_y + 40, video_channels[i], 0x8E8E93, 1);
            api->draw_text(rec_x + 120, card_y + 60, "Views: 100K", 0xAEAEB2, 1);

            if (mouse_click && mouse_x >= rec_x && mouse_x <= rec_x + 300 && mouse_y >= card_y && mouse_y <= card_y + 100) {
                active_video_index = i;
                video_progress = 0.0f;
                playing = true;
                api->play_sound("click");
            }
        }

        // Draw cursor
        api->draw_cursor(mouse_x, mouse_y);
        api->swap_buffers();
    }
    return 0;
}
