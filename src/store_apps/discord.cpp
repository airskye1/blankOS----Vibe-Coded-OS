#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/kernel/os_api.h"

// Simple string operations
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
    int win_w = 800;
    int win_h = 580;
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

    // App state: 0=Authentication Screen, 1=Chat Screen
    int state = 0; 
    char username_input[32] = "admin";
    int username_len = 5;
    bool username_active = false;

    // Chat room state
    int active_channel = 0; // 0=#general, 1=#lounge, 2=#bot-spam
    
    // User custom messages
    char user_messages[5][64];
    int user_message_count = 0;
    for (int i=0; i<5; i++) user_messages[i][0] = '\0';

    char current_input[64] = "";
    int current_input_len = 0;
    bool input_active = false;

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

        // Keyboard handler for typing
        EFI_INPUT_KEY key;
        if (api->SystemTable->ConIn->ReadKeyStroke(api->SystemTable->ConIn, &key) == EFI_SUCCESS) {
            if (key.ScanCode == 0x0017) { // Escape
                done = true;
            }
            
            // Handle typing based on active input field
            if (username_active && state == 0) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) { // Backspace
                    if (username_len > 0) {
                        username_len--;
                        username_input[username_len] = '\0';
                    }
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126 && username_len < 31) {
                    username_input[username_len++] = (char)key.UnicodeChar;
                    username_input[username_len] = '\0';
                }
            }
            else if (input_active && state == 1) {
                if (key.UnicodeChar == 8 || key.UnicodeChar == 127) { // Backspace
                    if (current_input_len > 0) {
                        current_input_len--;
                        current_input[current_input_len] = '\0';
                    }
                } else if (key.UnicodeChar == 13 || key.UnicodeChar == 10) { // Enter
                    if (current_input_len > 0 && user_message_count < 5) {
                        str_copy(user_messages[user_message_count], current_input, 64);
                        user_message_count++;
                        current_input[0] = '\0';
                        current_input_len = 0;
                        api->play_sound("startup"); // Play discord notification tone
                    }
                } else if (key.UnicodeChar >= 32 && key.UnicodeChar <= 126 && current_input_len < 63) {
                    current_input[current_input_len++] = (char)key.UnicodeChar;
                    current_input[current_input_len] = '\0';
                }
            }
        }

        // Render wallpaper and system components
        api->draw_wallpaper();
        api->draw_menubar();
        api->draw_dock();

        // Draw Discord main window
        api->draw_window(win_x, win_y, win_w, win_h, "Discord");

        // Close window hit test
        if (mouse_click && mouse_x >= win_x + 10 && mouse_x <= win_x + 22 && mouse_y >= win_y + 10 && mouse_y <= win_y + 22) {
            done = true;
        }

        if (state == 0) { // Auth Screen
            // Centered Discord Login Form
            api->draw_rect_rounded(win_x + 200, win_y + 100, 400, 380, 12, 0x2F3136, 255);
            api->draw_text(win_x + 300, win_y + 140, "Welcome back!", 0xFFFFFF, 2);
            api->draw_text(win_x + 260, win_y + 180, "We're so excited to see you again!", 0xB9BBBE, 1);

            // Input: USERNAME
            api->draw_text(win_x + 230, win_y + 230, "USERNAME", 0xB9BBBE, 1);
            api->draw_rect_rounded(win_x + 230, win_y + 250, 340, 40, 6, username_active ? 0x007AFF : 0x202225, 255);
            api->draw_text(win_x + 245, win_y + 262, username_input, 0xFFFFFF, 1);

            // Toggle active input field on click
            if (mouse_click) {
                username_active = (mouse_x >= win_x + 230 && mouse_x <= win_x + 570 && mouse_y >= win_y + 250 && mouse_y <= win_y + 290);
            }

            // Fake label
            api->draw_text(win_x + 230, win_y + 310, "PASSWORD / TOKEN (AUTOLOGIN)", 0xB9BBBE, 1);
            api->draw_rect_rounded(win_x + 230, win_y + 330, 340, 40, 6, 0x202225, 255);
            api->draw_text(win_x + 245, win_y + 342, "â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢", 0x8E9297, 1);

            // Login Button
            if (api->draw_button(win_x + 230, win_y + 400, 340, 45, "Log In", mouse_x, mouse_y, mouse_click)) {
                state = 1; // Transition to main chat app
                api->play_sound("startup"); // Play join sound
            }
        } 
        else if (state == 1) { // Chat Screen
            // Guilds bar on far left
            api->draw_rect(win_x + 1, win_y + 33, 70, win_h - 34, 0x202225, 255);
            api->draw_circle(win_x + 35, win_y + 68, 22, 0x5865F2, 255); // Discord Icon
            api->draw_text(win_x + 23, win_y + 60, "OS", 0xFFFFFF, 2);

            api->draw_circle(win_x + 35, win_y + 128, 22, 0x35393E, 255);
            api->draw_text(win_x + 23, win_y + 120, "AC", 0x8E9297, 2);

            // Channel Sidebar
            int side_x = win_x + 71;
            api->draw_rect(side_x, win_y + 33, 170, win_h - 34, 0x2F3136, 255);
            api->draw_text(side_x + 15, win_y + 55, "TEXT CHANNELS", 0x8E9297, 1);

            // Draw Channels List
            for (int ch = 0; ch < 3; ch++) {
                int ch_y = win_y + 80 + ch * 40;
                bool is_active = (active_channel == ch);
                if (is_active) {
                    api->draw_rect_rounded(side_x + 8, ch_y, 154, 30, 4, 0x393C43, 255);
                } else {
                    if (mouse_click && mouse_x >= side_x + 8 && mouse_x <= side_x + 162 && mouse_y >= ch_y && mouse_y <= ch_y + 30) {
                        active_channel = ch;
                        api->play_sound("click");
                    }
                }
                const char* ch_name = (ch == 0) ? "# general" : ((ch == 1) ? "# lounge" : "# bot-spam");
                uint32_t text_color = is_active ? 0xFFFFFF : 0x8E9297;
                api->draw_text(side_x + 18, ch_y + 8, ch_name, text_color, 1);
            }

            // Main chat area
            int chat_x = win_x + 241;
            api->draw_rect(chat_x, win_y + 33, win_w - 242, win_h - 34, 0x36393F, 255);
            
            // Header bar
            api->draw_rect(chat_x, win_y + 33, win_w - 242, 48, 0x2F3136, 255);
            api->draw_line(chat_x, win_y + 81, win_x + win_w - 1, win_y + 81, 0x202225, 1);
            const char* header_title = (active_channel == 0) ? "# general" : ((active_channel == 1) ? "# lounge" : "# bot-spam");
            api->draw_text(chat_x + 20, win_y + 48, header_title, 0xFFFFFF, 1);

            // Render Message Feed (Static/Mocked + Dynamic User messages)
            int msg_y = win_y + 100;
            if (active_channel == 0) {
                api->draw_text(chat_x + 20, msg_y, "@airskye", 0x5865F2, 1);
                api->draw_text(chat_x + 20, msg_y + 20, "Ethernet networking is now active! Test ELFs can connect to the catalog.", 0xDCDDDE, 1);
                
                api->draw_text(chat_x + 20, msg_y + 55, "@developer", 0x34C759, 1);
                api->draw_text(chat_x + 20, msg_y + 75, "The new monolithic UEFI syscall api works perfectly for custom binaries.", 0xDCDDDE, 1);
            } else if (active_channel == 1) {
                api->draw_text(chat_x + 20, msg_y, "@admin", 0xE5E5EA, 1);
                api->draw_text(chat_x + 20, msg_y + 20, "Just chilled out. This OS has such a clean and premium layout.", 0xDCDDDE, 1);
            } else {
                api->draw_text(chat_x + 20, msg_y, "@bot", 0xFFCC00, 1);
                api->draw_text(chat_x + 20, msg_y + 20, "uptime: 42 minutes | active sessions: 1 | RAM usage: 384 KB", 0xDCDDDE, 1);
            }

            // Draw dynamic user-sent messages
            for (int m = 0; m < user_message_count; m++) {
                int item_y = msg_y + 120 + m * 55;
                if (item_y < win_y + win_h - 130) {
                    char name_hdr[64] = "@";
                    char* n_ptr = name_hdr + 1;
                    const char* u_ptr = username_input;
                    while (*u_ptr) *n_ptr++ = *u_ptr++;
                    *n_ptr = '\0';
                    
                    api->draw_text(chat_x + 20, item_y, name_hdr, 0x34C759, 1);
                    api->draw_text(chat_x + 20, item_y + 20, user_messages[m], 0xDCDDDE, 1);
                }
            }

            // Message Input field at bottom
            int input_y = win_y + win_h - 75;
            api->draw_rect_rounded(chat_x + 20, input_y, win_w - 242 - 40, 42, 8, 0x40444B, 255);
            
            if (current_input_len == 0 && !input_active) {
                api->draw_text(chat_x + 35, input_y + 14, "Message active channel (Click to type...)", 0x72767D, 1);
            } else {
                api->draw_text(chat_x + 35, input_y + 14, current_input, 0xFFFFFF, 1);
            }

            if (mouse_click) {
                input_active = (mouse_x >= chat_x + 20 && mouse_x <= win_x + win_w - 20 && mouse_y >= input_y && mouse_y <= input_y + 42);
                if (input_active) username_active = false;
            }
        }

        // Draw cursor
        api->draw_cursor(mouse_x, mouse_y);
        api->swap_buffers();
    }
    return 0;
}
