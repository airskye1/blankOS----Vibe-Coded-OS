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

static void int_to_str(int val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char temp[12];
    int i = 0;
    while (val > 0) {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = temp[--i];
    }
    buf[j] = '\0';
}

extern "C" int main(OS_API* api) {
    int win_w = 640;
    int win_h = 680;
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

    // Interactive Tweeting system
    bool compose_active = false;
    char tweet_input[128] = "";
    int tweet_len = 0;

    // Feed items
    const char* feed_handles[] = { "@airskye", "@developer", "@blankOS" };
    const char* feed_texts[] = {
        "Just finalized the monolithic UEFI graphics compositor! #OSDev",
        "Writing dyn ELF loaders using position-independent wrappers.",
        "Version 1.3.1 has landed. V-Sync locking is gorgeous."
    };
    int feed_likes[] = { 42, 18, 99 };
    bool feed_liked[] = { false, false, false };

    // Dynamic user tweets
    char user_tweets[3][128];
    int user_likes[3] = { 0, 0, 0 };
    bool user_liked[3] = { false, false, false };
    int user_tweet_count = 0;

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

            if (compose_active) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) { // Backspace
                    if (tweet_len > 0) {
                        tweet_len--;
                        tweet_input[tweet_len] = '\0';
                    }
                } else if (key.UnicodeChar == 13 || key.UnicodeChar == 10) { // Enter
                    if (tweet_len > 0 && user_tweet_count < 3) {
                        str_copy(user_tweets[user_tweet_count], tweet_input, 128);
                        user_likes[user_tweet_count] = 0;
                        user_liked[user_tweet_count] = false;
                        user_tweet_count++;
                        
                        // Clear input
                        tweet_input[0] = '\0';
                        tweet_len = 0;
                        compose_active = false;
                        api->play_sound("startup");
                    }
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126 && tweet_len < 127) {
                    tweet_input[tweet_len++] = (char)key.UnicodeChar;
                    tweet_input[tweet_len] = '\0';
                }
            }
        }

        // Draw system UI
        api->draw_wallpaper();
        api->draw_menubar();
        api->draw_dock();

        // Draw X window
        api->draw_window(win_x, win_y, win_w, win_h, "X");

        // Close window hit test
        if (mouse_click && mouse_x >= win_x + 10 && mouse_x <= win_x + 22 && mouse_y >= win_y + 10 && mouse_y <= win_y + 22) {
            done = true;
        }

        // Header and branding
        api->draw_rect(win_x + 1, win_y + 33, win_w - 2, 45, 0x15202B, 255);
        api->draw_line(win_x + 1, win_y + 78, win_x + win_w - 2, win_y + 78, 0x38444D, 1);
        api->draw_text(win_x + 20, win_y + 46, "Home / X Feed", 0xFFFFFF, 2);

        // Compose Trigger Button
        if (api->draw_button(win_x + win_w - 140, win_y + 40, 120, 28, "Post Tweet", mouse_x, mouse_y, mouse_click)) {
            compose_active = !compose_active;
            api->play_sound("click");
        }

        int feed_y = win_y + 90;

        if (compose_active) {
            // Typing box
            api->draw_rect_rounded(win_x + 20, feed_y, win_w - 40, 100, 8, 0x192734, 255);
            api->draw_text(win_x + 35, feed_y + 15, "What's happening? (Press Enter to share)", 0x8899A6, 1);
            api->draw_text(win_x + 35, feed_y + 45, tweet_input, 0xFFFFFF, 1);
            
            feed_y += 115;
        }

        // Render dynamic user-sent tweets
        for (int i = user_tweet_count - 1; i >= 0; i--) {
            api->draw_rect_rounded(win_x + 20, feed_y, win_w - 40, 85, 8, 0x192734, 255);
            api->draw_text(win_x + 35, feed_y + 12, "@admin", 0x1DA1F2, 1);
            api->draw_text(win_x + 35, feed_y + 32, user_tweets[i], 0xFFFFFF, 1);

            // Likes button
            char l_lbl[32] = "Likes: ";
            char l_num[16];
            int_to_str(user_likes[i], l_num);
            char* ptr = l_lbl + 7;
            const char* n_ptr = l_num;
            while (*n_ptr) *ptr++ = *n_ptr++;
            *ptr = '\0';

            if (api->draw_button(win_x + 35, feed_y + 55, 100, 20, user_liked[i] ? "Liked!" : l_lbl, mouse_x, mouse_y, mouse_click)) {
                user_liked[i] = !user_liked[i];
                user_likes[i] += user_liked[i] ? 1 : -1;
                api->play_sound("click");
            }
            
            feed_y += 95;
        }

        // Render standard mock feed
        for (int i = 0; i < 3; i++) {
            if (feed_y < win_y + win_h - 100) {
                api->draw_rect_rounded(win_x + 20, feed_y, win_w - 40, 85, 8, 0x192734, 255);
                api->draw_text(win_x + 35, feed_y + 12, feed_handles[i], 0x1DA1F2, 1);
                api->draw_text(win_x + 35, feed_y + 32, feed_texts[i], 0xFFFFFF, 1);

                char l_lbl[32] = "Likes: ";
                char l_num[16];
                int_to_str(feed_likes[i], l_num);
                char* ptr = l_lbl + 7;
                const char* n_ptr = l_num;
                while (*n_ptr) *ptr++ = *n_ptr++;
                *ptr = '\0';

                if (api->draw_button(win_x + 35, feed_y + 55, 100, 20, feed_liked[i] ? "Liked!" : l_lbl, mouse_x, mouse_y, mouse_click)) {
                    feed_liked[i] = !feed_liked[i];
                    feed_likes[i] += feed_liked[i] ? 1 : -1;
                    api->play_sound("click");
                }
                
                feed_y += 95;
            }
        }

        // Draw cursor
        api->draw_cursor(mouse_x, mouse_y);
        api->swap_buffers();
    }
    return 0;
}
