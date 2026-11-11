#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
    extern int screen_width;
    extern int screen_height;

    extern void swap_buffers(void);
    extern void play_system_sound(char* sound_name);

    extern void blankReg_init(void);
    extern int blankReg_get_count(void);
    extern bool blankReg_get_entry(int index, char* key_out, int* type_out, int* int_out, char* str_out);
    extern void blankReg_set_int(char* key, int val);
    extern void blankReg_set_string(char* key, char* val);
    extern bool blankReg_delete_key(const char* key);
    extern bool blankReg_save(EFI_SYSTEM_TABLE *st);
    extern bool blankReg_load(EFI_SYSTEM_TABLE *st);

    extern void dui_draw_wallpaper(void);
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);

    extern void blankUI_draw_menubar(void);
    extern void blankUI_draw_dock(void);
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_draw_topbar(char* app_title);
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    extern void blankUI_set_cursor_type(int type);
}

static int br_str_len(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void br_str_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void br_str_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int dlen = br_str_len(dst);
    int i = 0;
    while (src[i] && (dlen + i) < max_len - 1) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static char br_to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static bool br_starts_with_nocase(const char* str, const char* prefix) {
    if (!str || !prefix) return false;
    int i = 0;
    while (prefix[i]) {
        if (!str[i]) return false;
        if (br_to_upper(str[i]) != br_to_upper(prefix[i])) return false;
        i++;
    }
    return true;
}

static void br_int_to_str(int val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    bool neg = false;
    if (val < 0) {
        neg = true;
        val = -val;
    }
    char tmp[24];
    int pos = 0;
    while (val > 0 && pos < 20) {
        tmp[pos++] = (char)('0' + (val % 10));
        val /= 10;
    }
    if (neg && pos < 22) tmp[pos++] = '-';
    int j = 0;
    while (pos > 0 && j < max_len - 1) {
        out[j++] = tmp[--pos];
    }
    out[j] = '\0';
}

static bool br_parse_int(const char* str, int* out_val) {
    if (!str || !str[0] || !out_val) return false;
    int i = 0;
    bool neg = false;
    if (str[i] == '-') {
        neg = true;
        i++;
    } else if (str[i] == '+') {
        i++;
    }
    if (!str[i]) return false;
    int acc = 0;
    while (str[i]) {
        if (str[i] < '0' || str[i] > '9') return false;
        acc = acc * 10 + (str[i] - '0');
        i++;
    }
    *out_val = neg ? -acc : acc;
    return true;
}

typedef struct {
    char key[64];
    int type; // 0 = DWORD (INT), 1 = STRING
    int int_val;
    char str_val[128];
} RegRowView;

extern "C" void launch_blankreg_edit(void) {
    EFI_SYSTEM_TABLE *SystemTable = gSystemTable;
    blankUI_draw_topbar((char*)"blankReg Studio");
    blankReg_init();

    if (!SystemTable) return;

    EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;

    const int win_w = 740;
    const int win_h = 460;

    int hive_filter = 0; // 0=All, 1=System/*, 2=UI/*, 3=Audio/*, 4=Hardware/*, 5=Network/*
    int selected_row = 0;
    int active_field = 1; // 0 = none, 1 = value input box, 2 = key name input box
    char edit_key_buf[64];
    char edit_val_buf[128];
    int edit_type = 0;
    char status_msg[64];
    br_str_copy(status_msg, "Live Hive Ready (REGISTRY.INI)", sizeof(status_msg));

    edit_key_buf[0] = '\0';
    edit_val_buf[0] = '\0';

    int custom_key_seq = 1;
    bool prev_left = false;
    bool done = false;
    bool need_sync_inspector = true;

    const char* hive_prefixes[6] = {
        "",
        "System/",
        "UI/",
        "Audio/",
        "Hardware/",
        "Network/"
    };

    const char* hive_labels[6] = {
        "All Keys",
        "System/*",
        "UI/*",
        "Audio/*",
        "Hardware/*",
        "Network/*"
    };

    while (!done) {
        int win_x = 0, win_y = 0;
        blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);

        // Snapshot matching registry entries
        RegRowView rows[64];
        int row_count = 0;
        int total_keys = blankReg_get_count();
        for (int i = 0; i < total_keys && row_count < 64; i++) {
            char kname[64];
            int ktype = 0;
            int ival = 0;
            char sval[128];
            if (blankReg_get_entry(i, kname, &ktype, &ival, sval)) {
                if (hive_filter == 0 || br_starts_with_nocase(kname, hive_prefixes[hive_filter])) {
                    br_str_copy(rows[row_count].key, kname, 64);
                    rows[row_count].type = ktype;
                    rows[row_count].int_val = ival;
                    br_str_copy(rows[row_count].str_val, sval, 128);
                    row_count++;
                }
            }
        }

        if (selected_row >= row_count) selected_row = row_count - 1;
        if (selected_row < 0 && row_count > 0) selected_row = 0;

        if (need_sync_inspector) {
            if (selected_row >= 0 && selected_row < row_count) {
                br_str_copy(edit_key_buf, rows[selected_row].key, sizeof(edit_key_buf));
                edit_type = rows[selected_row].type;
                if (edit_type == 0) {
                    br_int_to_str(rows[selected_row].int_val, edit_val_buf, sizeof(edit_val_buf));
                } else {
                    br_str_copy(edit_val_buf, rows[selected_row].str_val, sizeof(edit_val_buf));
                }
            } else {
                edit_key_buf[0] = '\0';
                edit_val_buf[0] = '\0';
            }
            need_sync_inspector = false;
        }

        // Keyboard input
        EFI_INPUT_KEY Key;
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
            if (Key.ScanCode == 0x17) {
                done = true;
            } else if (Key.ScanCode == 0x01) { // Up arrow selects previous row
                if (selected_row > 0) {
                    selected_row--;
                    need_sync_inspector = true;
                }
            } else if (Key.ScanCode == 0x02) { // Down arrow selects next row
                if (selected_row < row_count - 1) {
                    selected_row++;
                    need_sync_inspector = true;
                }
            } else if (Key.UnicodeChar == 0x0D) { // Enter commits value/key edit
                if (edit_key_buf[0]) {
                    int parsed_int = 0;
                    if (edit_type == 0 && br_parse_int(edit_val_buf, &parsed_int)) {
                        blankReg_set_int(edit_key_buf, parsed_int);
                    } else if (edit_type == 0) {
                        blankReg_set_int(edit_key_buf, 0);
                    } else {
                        blankReg_set_string(edit_key_buf, edit_val_buf);
                    }
                    br_str_copy(status_msg, "Updated Key in Hive", sizeof(status_msg));
                    play_system_sound((char*)"click");
                    need_sync_inspector = true;
                }
            } else if (Key.UnicodeChar == 0x08) { // Backspace
                if (active_field == 1) {
                    int len = br_str_len(edit_val_buf);
                    if (len > 0) edit_val_buf[len - 1] = '\0';
                } else if (active_field == 2) {
                    int len = br_str_len(edit_key_buf);
                    if (len > 0) edit_key_buf[len - 1] = '\0';
                }
            } else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                if (active_field == 1) {
                    int len = br_str_len(edit_val_buf);
                    if (len < (int)sizeof(edit_val_buf) - 1) {
                        edit_val_buf[len] = (char)Key.UnicodeChar;
                        edit_val_buf[len + 1] = '\0';
                    }
                } else if (active_field == 2) {
                    int len = br_str_len(edit_key_buf);
                    if (len < (int)sizeof(edit_key_buf) - 1) {
                        edit_key_buf[len] = (char)Key.UnicodeChar;
                        edit_key_buf[len + 1] = '\0';
                    }
                }
            }
        }

        // Mouse input
        bool mouse_clicked = false;
        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;
                cursor_x += dx;
                cursor_y += dy;
                if (cursor_x < 0) cursor_x = 0;
                if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                if (cursor_y < 0) cursor_y = 0;
                if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;

                blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                if (State.LeftButton && !prev_left) {
                    mouse_clicked = true;
                }
                prev_left = State.LeftButton;
            }
        }

        int sidebar_w = 175;
        int top_y = win_y + 36;
        int table_x = win_x + sidebar_w;
        int table_w = win_w - sidebar_w;
        int inspector_y = win_y + win_h - 110;
        int table_h = inspector_y - top_y;

        if (mouse_clicked) {
            if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                play_system_sound((char*)"click");
                done = true;
            }

            // Left Sidebar Hive Filter clicks
            if (cursor_x >= win_x + 8 && cursor_x <= win_x + sidebar_w - 8) {
                for (int h = 0; h < 6; h++) {
                    int hy = top_y + 34 + h * 30;
                    if (cursor_y >= hy && cursor_y <= hy + 26) {
                        hive_filter = h;
                        selected_row = 0;
                        need_sync_inspector = true;
                        play_system_sound((char*)"click");
                    }
                }
                // Reload from FAT32 disk button at bottom of sidebar
                int reload_y = win_y + win_h - 42;
                if (cursor_y >= reload_y && cursor_y <= reload_y + 28) {
                    bool loaded = blankReg_load(SystemTable);
                    need_sync_inspector = true;
                    br_str_copy(status_msg, loaded ? "Reloaded REGISTRY.INI" : "Using Active Hive", sizeof(status_msg));
                    play_system_sound((char*)"notification");
                }
            }

            // Right Table Row clicks
            int rows_start_y = top_y + 32;
            int max_vis_rows = 11;
            if (cursor_x >= table_x + 6 && cursor_x <= win_x + win_w - 6 &&
                cursor_y >= rows_start_y && cursor_y < inspector_y - 4) {
                int clicked_idx = (cursor_y - rows_start_y) / 24;
                if (clicked_idx >= 0 && clicked_idx < row_count && clicked_idx < max_vis_rows) {
                    selected_row = clicked_idx;
                    need_sync_inspector = true;
                    active_field = 1;
                    play_system_sound((char*)"click");
                }
            }

            // Bottom Inspector Bar interactions
            if (cursor_x >= table_x && cursor_y >= inspector_y) {
                // Row 1 of Inspector (y = inspector_y + 10 .. + 36):
                // Key Name box: table_x + 48 .. + 230
                // Type Toggle badge: table_x + 238 .. + 298
                // Value input box: table_x + 344 .. + 484
                // [ Apply ] button: table_x + 490 .. + 552
                int r1_y = inspector_y + 10;
                if (cursor_y >= r1_y && cursor_y <= r1_y + 26) {
                    if (cursor_x >= table_x + 48 && cursor_x <= table_x + 230) {
                        active_field = 2;
                        play_system_sound((char*)"click");
                    } else if (cursor_x >= table_x + 238 && cursor_x <= table_x + 298) {
                        edit_type = (edit_type == 0) ? 1 : 0;
                        if (edit_type == 0) br_str_copy(edit_val_buf, "1", sizeof(edit_val_buf));
                        play_system_sound((char*)"click");
                    } else if (cursor_x >= table_x + 344 && cursor_x <= table_x + 484) {
                        active_field = 1;
                        play_system_sound((char*)"click");
                    } else if (cursor_x >= table_x + 490 && cursor_x <= table_x + 554) {
                        if (edit_key_buf[0]) {
                            int parsed_int = 0;
                            if (edit_type == 0 && br_parse_int(edit_val_buf, &parsed_int)) {
                                blankReg_set_int(edit_key_buf, parsed_int);
                            } else if (edit_type == 0) {
                                blankReg_set_int(edit_key_buf, 0);
                            } else {
                                blankReg_set_string(edit_key_buf, edit_val_buf);
                            }
                            br_str_copy(status_msg, "Applied Key Change", sizeof(status_msg));
                            need_sync_inspector = true;
                            play_system_sound((char*)"click");
                        }
                    }
                }

                // Row 2 of Inspector (y = inspector_y + 46 .. + 74):
                // Quick INT mutators:
                // [ +1 ]         (table_x + 12 .. + 58)
                // [ -1 ]         (table_x + 64 .. + 110)
                // [ Toggle 0/1 ] (table_x + 116 .. + 206)
                // Action buttons:
                // [ + New Key ]     (table_x + 220 .. + 310)
                // [ Delete Key ]    (table_x + 316 .. + 408)
                // [ Flush to Disk ] (table_x + 414 .. + 554)
                int r2_y = inspector_y + 46;
                if (cursor_y >= r2_y && cursor_y <= r2_y + 28) {
                    if (cursor_x >= table_x + 12 && cursor_x <= table_x + 58) {
                        if (selected_row >= 0 && selected_row < row_count && rows[selected_row].type == 0) {
                            int nv = rows[selected_row].int_val + 1;
                            blankReg_set_int(rows[selected_row].key, nv);
                            need_sync_inspector = true;
                            br_str_copy(status_msg, "Incremented DWORD (+1)", sizeof(status_msg));
                            play_system_sound((char*)"click");
                        }
                    } else if (cursor_x >= table_x + 64 && cursor_x <= table_x + 110) {
                        if (selected_row >= 0 && selected_row < row_count && rows[selected_row].type == 0) {
                            int nv = rows[selected_row].int_val - 1;
                            blankReg_set_int(rows[selected_row].key, nv);
                            need_sync_inspector = true;
                            br_str_copy(status_msg, "Decremented DWORD (-1)", sizeof(status_msg));
                            play_system_sound((char*)"click");
                        }
                    } else if (cursor_x >= table_x + 116 && cursor_x <= table_x + 206) {
                        if (selected_row >= 0 && selected_row < row_count && rows[selected_row].type == 0) {
                            int nv = (rows[selected_row].int_val == 0) ? 1 : 0;
                            blankReg_set_int(rows[selected_row].key, nv);
                            need_sync_inspector = true;
                            br_str_copy(status_msg, "Toggled Boolean DWORD", sizeof(status_msg));
                            play_system_sound((char*)"click");
                        }
                    } else if (cursor_x >= table_x + 220 && cursor_x <= table_x + 310) {
                        char newKey[64];
                        const char* pref = (hive_filter > 0) ? hive_prefixes[hive_filter] : "System/";
                        br_str_copy(newKey, pref, sizeof(newKey));
                        br_str_cat(newKey, "CustomKey", sizeof(newKey));
                        char seqStr[12];
                        br_int_to_str(custom_key_seq++, seqStr, sizeof(seqStr));
                        br_str_cat(newKey, seqStr, sizeof(newKey));

                        blankReg_set_int(newKey, 1);
                        br_str_copy(edit_key_buf, newKey, sizeof(edit_key_buf));
                        br_str_copy(edit_val_buf, "1", sizeof(edit_val_buf));
                        edit_type = 0;
                        active_field = 2; // Focus key name so user can customize if desired
                        br_str_copy(status_msg, "Created New Registry Key", sizeof(status_msg));
                        play_system_sound((char*)"click");
                    } else if (cursor_x >= table_x + 316 && cursor_x <= table_x + 408) {
                        if (selected_row >= 0 && selected_row < row_count) {
                            blankReg_delete_key(rows[selected_row].key);
                            if (selected_row > 0) selected_row--;
                            need_sync_inspector = true;
                            br_str_copy(status_msg, "Deleted Key from Hive", sizeof(status_msg));
                            play_system_sound((char*)"click");
                        }
                    } else if (cursor_x >= table_x + 414 && cursor_x <= table_x + 554) {
                        bool ok = blankReg_save(SystemTable);
                        br_str_copy(status_msg, ok ? "Flushed to FAT32 REGISTRY.INI" : "Saved in Memory Hive", sizeof(status_msg));
                        play_system_sound((char*)"notification");
                    }
                }
            }
        }

        // ======================== RENDER FRAME ========================
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();

        blankUI_draw_window(win_w, win_h, (char*)"blankReg Studio - System Registry Editor");

        // Left Sidebar (Hive Filter)
        dui_rect(win_x + 1, top_y, sidebar_w - 1, win_h - 37, 0xF2F2F7, 245);
        dui_line(win_x + sidebar_w, top_y, win_x + sidebar_w, win_y + win_h, 0xD1D1D6, 1);

        dui_text(win_x + 14, top_y + 12, "REGISTRY HIVES", 0x8E8E93, 1);
        uint32_t hive_colors[6] = {
            0x0A84FF, 0x5E5CE6, 0xBF5AF2, 0xFF9F0A, 0x30D158, 0x64D2FF
        };

        for (int h = 0; h < 6; h++) {
            int hy = top_y + 34 + h * 30;
            bool is_sel = (hive_filter == h);
            if (is_sel) {
                dui_rect_rounded(win_x + 8, hy, sidebar_w - 16, 26, 6, 0x0A84FF, 255);
            }
            dui_circle(win_x + 20, hy + 13, 4, is_sel ? 0xFFFFFF : hive_colors[h], 255);
            dui_text(win_x + 32, hy + 7, hive_labels[h], is_sel ? 0xFFFFFF : 0x1C1C1E, 1);
        }

        // Hive summary stats card in sidebar
        dui_line(win_x + 12, top_y + 224, win_x + sidebar_w - 12, top_y + 224, 0xD1D1D6, 1);
        dui_rect_rounded(win_x + 10, top_y + 236, sidebar_w - 20, 74, 8, 0xE5E5EA, 255);
        dui_text(win_x + 18, top_y + 244, "HIVE STATUS", 0x636366, 1);

        char statLine[36];
        br_str_copy(statLine, "Total Keys: ", sizeof(statLine));
        char nbuf[12];
        br_int_to_str(total_keys, nbuf, sizeof(nbuf));
        br_str_cat(statLine, nbuf, sizeof(statLine));
        dui_text(win_x + 18, top_y + 264, statLine, 0x1C1C1E, 1);

        br_str_copy(statLine, "Visible: ", sizeof(statLine));
        br_int_to_str(row_count, nbuf, sizeof(nbuf));
        br_str_cat(statLine, nbuf, sizeof(statLine));
        dui_text(win_x + 18, top_y + 282, statLine, 0x0A84FF, 1);

        // Reload from Disk button at bottom of sidebar
        int reload_y = win_y + win_h - 42;
        dui_rect_rounded(win_x + 10, reload_y, sidebar_w - 20, 28, 6, 0xFFFFFF, 255);
        dui_rect_rounded_outline(win_x + 10, reload_y, sidebar_w - 20, 28, 6, 0xC7C7CC, 1);
        dui_text(win_x + 22, reload_y + 8, "Reload FAT32 Hive", 0x0A84FF, 1);

        // Right Table Area
        dui_rect(table_x + 1, top_y, table_w - 2, table_h, 0xFFFFFF, 255);

        // Table Header Bar
        dui_rect(table_x + 1, top_y, table_w - 2, 28, 0xEBEBF0, 255);
        dui_line(table_x, top_y + 28, win_x + win_w, top_y + 28, 0xD1D1D6, 1);
        dui_text(table_x + 16, top_y + 8, "KEY NAME", 0x636366, 1);
        dui_text(table_x + 248, top_y + 8, "TYPE", 0x636366, 1);
        dui_text(table_x + 334, top_y + 8, "CURRENT VALUE", 0x636366, 1);

        // Table Rows
        int rows_start_y = top_y + 32;
        int max_vis_rows = 11;
        for (int i = 0; i < row_count && i < max_vis_rows; i++) {
            int ry = rows_start_y + i * 24;
            bool is_sel = (i == selected_row);

            if (is_sel) {
                dui_rect_rounded(table_x + 6, ry, table_w - 12, 22, 5, 0xD8EAFE, 255);
                dui_rect_rounded_outline(table_x + 6, ry, table_w - 12, 22, 5, 0x0A84FF, 1);
            } else if (i % 2 == 1) {
                dui_rect(table_x + 6, ry, table_w - 12, 22, 0xF9F9FB, 255);
            }

            // Key Name
            dui_text(table_x + 16, ry + 5, rows[i].key, is_sel ? 0x0040DD : 0x1C1C1E, 1);

            // Type Badge (DWORD in blue or STRING in purple)
            if (rows[i].type == 0) {
                dui_rect_rounded(table_x + 244, ry + 3, 56, 16, 4, 0x0A84FF, 255);
                dui_text(table_x + 252, ry + 5, "DWORD", 0xFFFFFF, 1);
            } else {
                dui_rect_rounded(table_x + 244, ry + 3, 56, 16, 4, 0xAF52DE, 255);
                dui_text(table_x + 250, ry + 5, "STRING", 0xFFFFFF, 1);
            }

            // Value Display
            char valDisp[44];
            if (rows[i].type == 0) {
                br_int_to_str(rows[i].int_val, valDisp, sizeof(valDisp));
            } else {
                br_str_copy(valDisp, rows[i].str_val, 36);
            }
            dui_text(table_x + 334, ry + 5, valDisp, 0x2C2C2E, 1);
        }

        // Bottom Inspector Bar
        dui_rect(table_x + 1, inspector_y, table_w - 2, win_h - (inspector_y - win_y) - 1, 0xF4F4F8, 255);
        dui_line(table_x, inspector_y, win_x + win_w, inspector_y, 0xC7C7CC, 1);

        int r1_y = inspector_y + 10;
        dui_text(table_x + 12, r1_y + 7, "Key:", 0x3A3A3C, 1);
        dui_rect_rounded(table_x + 48, r1_y, 182, 26, 5, 0xFFFFFF, 255);
        dui_rect_rounded_outline(table_x + 48, r1_y, 182, 26, 5, (active_field == 2) ? 0x0A84FF : 0xC7C7CC, (active_field == 2) ? 2 : 1);
        char keyClip[24];
        br_str_copy(keyClip, edit_key_buf, 22);
        dui_text(table_x + 54, r1_y + 7, keyClip, 0x1C1C1E, 1);

        // Type toggle pill in inspector
        dui_rect_rounded(table_x + 238, r1_y, 60, 26, 5, (edit_type == 0) ? 0x0A84FF : 0xAF52DE, 255);
        dui_text(table_x + 247, r1_y + 7, (edit_type == 0) ? "DWORD" : "STRING", 0xFFFFFF, 1);

        dui_text(table_x + 306, r1_y + 7, "Val:", 0x3A3A3C, 1);
        dui_rect_rounded(table_x + 344, r1_y, 140, 26, 5, 0xFFFFFF, 255);
        dui_rect_rounded_outline(table_x + 344, r1_y, 140, 26, 5, (active_field == 1) ? 0x0A84FF : 0xC7C7CC, (active_field == 1) ? 2 : 1);
        char valClip[20];
        br_str_copy(valClip, edit_val_buf, 18);
        dui_text(table_x + 350, r1_y + 7, valClip, 0x1C1C1E, 1);

        dui_rect_rounded(table_x + 490, r1_y, 64, 26, 5, 0x0A84FF, 255);
        dui_text(table_x + 504, r1_y + 7, "Apply", 0xFFFFFF, 1);

        // Row 2 of Inspector: Quick INT controls + Key lifecycle + Flush to Disk
        int r2_y = inspector_y + 46;
        dui_rect_rounded(table_x + 12, r2_y, 46, 28, 6, 0xE5E5EA, 255);
        dui_text(table_x + 24, r2_y + 8, "+1", 0x1C1C1E, 1);

        dui_rect_rounded(table_x + 64, r2_y, 46, 28, 6, 0xE5E5EA, 255);
        dui_text(table_x + 76, r2_y + 8, "-1", 0x1C1C1E, 1);

        dui_rect_rounded(table_x + 116, r2_y, 90, 28, 6, 0xE5E5EA, 255);
        dui_text(table_x + 124, r2_y + 8, "Toggle 0/1", 0x1C1C1E, 1);

        dui_rect_rounded(table_x + 220, r2_y, 90, 28, 6, 0xFFFFFF, 255);
        dui_rect_rounded_outline(table_x + 220, r2_y, 90, 28, 6, 0x0A84FF, 1);
        dui_text(table_x + 230, r2_y + 8, "+ New Key", 0x0A84FF, 1);

        dui_rect_rounded(table_x + 316, r2_y, 92, 28, 6, 0xFF3B30, 255);
        dui_text(table_x + 326, r2_y + 8, "Delete Key", 0xFFFFFF, 1);

        dui_rect_rounded(table_x + 414, r2_y, 140, 28, 6, 0x30D158, 255);
        dui_text(table_x + 434, r2_y + 8, "Flush to Disk", 0xFFFFFF, 1);

        // Status footer line inside inspector
        dui_text(table_x + 14, inspector_y + 84, status_msg, 0x636366, 1);

        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();
        SystemTable->BootServices->Stall(16000);
    }
}
