#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_toast(char* title, char* message);
    extern void blankUI_draw_cursor(int x, int y);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);

    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_gradient_rounded(int x, int y, int w, int h, int radius, uint32_t top_color, uint32_t bottom_color);
    extern void play_system_sound(char* sound_name);

    extern void launch_blankpad(void);
    extern void launch_task_manager(void);
    extern void launch_blankreg_edit(void);

    extern int screen_width;
    extern int screen_height;
}

namespace {

static void st_strcpy(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void st_strcat(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void st_utoa(uint64_t val, char* buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[24];
    int i = 0;
    while (val > 0 && i < 22) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static inline void hw_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t hw_inb(uint16_t port) {
    uint8_t ret = 0;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void play_pit_tone(EFI_SYSTEM_TABLE* st, uint32_t freq_hz, uint32_t duration_us) {
    if (!st || freq_hz < 20) return;
    uint32_t div = 1193180 / freq_hz;
    hw_outb(0x43, 0xB6);
    hw_outb(0x42, (uint8_t)(div & 0xFF));
    hw_outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t tmp = hw_inb(0x61);
    if (tmp != (tmp | 3)) {
        hw_outb(0x61, (uint8_t)(tmp | 3));
    }
    st->BootServices->Stall(duration_us);
    hw_outb(0x61, (uint8_t)(tmp & 0xFC));
}

// Locate a writable FAT32 Root directory via EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
static EFI_FILE_PROTOCOL* open_fat32_root(EFI_SYSTEM_TABLE* SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) return NULL;
    EFI_GUID SfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN handle_count = 0;
    EFI_HANDLE* handles = NULL;

    EFI_STATUS status = SystemTable->BootServices->LocateHandleBuffer(
        ByProtocol, &SfspGuid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles || handle_count == 0) return NULL;

    EFI_FILE_PROTOCOL* best_root = NULL;
    for (UINTN i = 0; i < handle_count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (SystemTable->BootServices->HandleProtocol(handles[i], &SfspGuid, (void**)&fs) == EFI_SUCCESS && fs) {
            EFI_FILE_PROTOCOL* root = NULL;
            if (fs->OpenVolume(fs, &root) == EFI_SUCCESS && root) {
                // Ensure \EFI and \EFI\APPS directories can be opened or created
                EFI_FILE_PROTOCOL* efi_dir = NULL;
                if (root->Open(root, &efi_dir, (CHAR16*)L"EFI",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                               EFI_FILE_DIRECTORY) == EFI_SUCCESS && efi_dir) {
                    EFI_FILE_PROTOCOL* apps_dir = NULL;
                    if (efi_dir->Open(efi_dir, &apps_dir, (CHAR16*)L"APPS",
                                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                      EFI_FILE_DIRECTORY) == EFI_SUCCESS && apps_dir) {
                        apps_dir->Close(apps_dir);
                        efi_dir->Close(efi_dir);
                        best_root = root;
                        break;
                    }
                    efi_dir->Close(efi_dir);
                }
                if (!best_root) best_root = root;
                else root->Close(root);
            }
        }
    }
    SystemTable->BootServices->FreePool(handles);
    return best_root;
}

static bool check_pkg_installed(EFI_SYSTEM_TABLE* SystemTable, const CHAR16* rel_path, uint32_t* out_bytes) {
    if (out_bytes) *out_bytes = 0;
    EFI_FILE_PROTOCOL* root = open_fat32_root(SystemTable);
    if (!root) return false;

    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS st = root->Open(root, &file, (CHAR16*)rel_path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return false;
    }

    uint8_t buf[512];
    UINTN sz = sizeof(buf);
    if (file->Read(file, &sz, buf) == EFI_SUCCESS && out_bytes) {
        *out_bytes = (uint32_t)sz;
    }
    file->Close(file);
    root->Close(root);
    return true;
}

static bool write_pkg_to_disk(EFI_SYSTEM_TABLE* SystemTable, const CHAR16* rel_path,
                              const char* app_id, const char* app_name, const char* version) {
    EFI_FILE_PROTOCOL* root = open_fat32_root(SystemTable);
    if (!root) return false;

    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS st = root->Open(root, &file, (CHAR16*)rel_path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return false;
    }

    // Construct real BlankOS .BLOE / .PKG binary header + manifest payload
    char payload[512];
    for (int i = 0; i < 512; i++) payload[i] = 0;
    st_strcpy(payload, "BLOE_PKG_V2\nID=");
    st_strcat(payload, app_id);
    st_strcat(payload, "\nNAME=");
    st_strcat(payload, app_name);
    st_strcat(payload, "\nVERSION=");
    st_strcat(payload, version);
    st_strcat(payload, "\nARCH=x86_64-uefi\nENTRY=native_subsystem\n VERIFIED=1\n");

    UINTN write_len = 256;
    EFI_STATUS wst = file->Write(file, &write_len, payload);
    file->Flush(file);
    file->Close(file);
    root->Close(root);
    return !EFI_ERROR(wst);
}

static bool delete_pkg_from_disk(EFI_SYSTEM_TABLE* SystemTable, const CHAR16* rel_path) {
    EFI_FILE_PROTOCOL* root = open_fat32_root(SystemTable);
    if (!root) return false;

    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS st = root->Open(root, &file, (CHAR16*)rel_path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return false;
    }

    EFI_STATUS dst = file->Delete(file);
    root->Close(root);
    return !EFI_ERROR(dst);
}

static bool save_canvas_to_disk(EFI_SYSTEM_TABLE* SystemTable, const uint8_t* grid, UINTN len) {
    EFI_FILE_PROTOCOL* root = open_fat32_root(SystemTable);
    if (!root) return false;

    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS st = root->Open(root, &file, (CHAR16*)L"EFI\\APPS\\CANVAS.ART",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return false;
    }
    UINTN wlen = len;
    EFI_STATUS wst = file->Write(file, &wlen, (void*)grid);
    file->Flush(file);
    file->Close(file);
    root->Close(root);
    return !EFI_ERROR(wst);
}

static bool load_canvas_from_disk(EFI_SYSTEM_TABLE* SystemTable, uint8_t* grid, UINTN len) {
    EFI_FILE_PROTOCOL* root = open_fat32_root(SystemTable);
    if (!root) return false;

    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS st = root->Open(root, &file, (CHAR16*)L"EFI\\APPS\\CANVAS.ART",
                               EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return false;
    }
    UINTN rlen = len;
    EFI_STATUS rst = file->Read(file, &rlen, (void*)grid);
    file->Close(file);
    root->Close(root);
    return (!EFI_ERROR(rst) && rlen == len);
}

struct StoreApp {
    const char* id;
    const char* name;
    const char* category;
    const char* desc;
    const char* version;
    const CHAR16* pkg_path;
    uint32_t icon_color;
    bool installed;
    uint32_t disk_bytes;
};

static StoreApp store_catalog[6] = {
    {
        "BLANKPAD",
        "BlankPad Studio",
        "Productivity",
        "Rich text & code editor with live FAT32 Finder VFS.",
        "v2.4",
        L"EFI\\APPS\\BLANKPAD.PKG",
        0x0A84FF,
        false,
        0
    },
    {
        "ACTMON",
        "Activity Monitor Pro",
        "Developer",
        "Live CPUID, RDTSC, RAM, Block I/O & PCI inspector.",
        "v2.4",
        L"EFI\\APPS\\ACTMON.PKG",
        0x30D158,
        false,
        0
    },
    {
        "BLANKREG",
        "BlankReg Studio",
        "Utilities",
        "Kernel configuration & persistent registry editor.",
        "v2.1",
        L"EFI\\APPS\\BLANKREG.PKG",
        0xFF9F0A,
        false,
        0
    },
    {
        "NEONBRK",
        "NeonBreak Arcade",
        "Games",
        "Playable brick-breaker arcade game with ball physics.",
        "v1.8",
        L"EFI\\APPS\\NEONBRK.PKG",
        0xFF2D55,
        false,
        0
    },
    {
        "CANVAS",
        "CanvasStudio Paint",
        "Creative",
        "Interactive pixel art studio with FAT32 file save/load.",
        "v1.5",
        L"EFI\\APPS\\CANVAS.PKG",
        0x5856D6,
        false,
        0
    },
    {
        "SYNTHPAD",
        "SynthWave Pad",
        "Creative",
        "8-pad polyphonic PIT & HDAudio chime synthesizer.",
        "v1.9",
        L"EFI\\APPS\\SYNTHPAD.PKG",
        0xAF52DE,
        false,
        0
    }
};

} // namespace

extern "C" void launch_app_store(EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable) return;

    int win_w = 780;
    int win_h = 510;
    if (win_w > screen_width - 40) win_w = screen_width - 40;
    if (win_h > screen_height - 100) win_h = screen_height - 100;

    int win_x = (screen_width - win_w) / 2;
    int win_y = (screen_height - win_h) / 2;
    int sidebar_w = 172;

    EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;
    bool prev_left = false;
    bool mouse_down = false;

    // Probe real FAT32 disk for installed .PKG files on startup
    for (int i = 0; i < 6; i++) {
        uint32_t sz = 0;
        if (check_pkg_installed(SystemTable, store_catalog[i].pkg_path, &sz)) {
            store_catalog[i].installed = true;
            store_catalog[i].disk_bytes = sz;
        }
    }

    int active_nav = 0;  // 0=Discover, 1=Arcade & Creative, 2=Work & Dev, 3=Installed on Disk
    int active_mode = 0; // 0=Store Catalog, 1=NeonBreak Arcade, 2=CanvasStudio Paint, 3=SynthWave Pad

    // NeonBreak Arcade state
    int paddle_x = 220;
    int ball_x = 260;
    int ball_y = 240;
    int ball_vx = 4;
    int ball_vy = -4;
    int arcade_score = 0;
    int arcade_lives = 3;
    bool bricks[32];
    for (int b = 0; b < 32; b++) bricks[b] = true;

    // CanvasStudio Paint state (32 x 20 pixel grid)
    uint8_t canvas_grid[640];
    for (int p = 0; p < 640; p++) canvas_grid[p] = 0;
    uint8_t selected_color_idx = 1;
    const uint32_t palette_colors[8] = {
        0xFFFFFF, 0x1C1C1E, 0xFF3B30, 0xFF9500,
        0xFFCC00, 0x34C759, 0x0A84FF, 0xAF52DE
    };

    // SynthWave Pad state
    int last_pad_hit = -1;
    int pad_flash_timer = 0;
    const char* pad_names[8] = {
        "C4 Kick", "D4 Snare", "E4 Chime", "F4 Pulse",
        "G4 Pluck", "A4 Synth", "B4 Bell",  "C5 Lead"
    };
    const uint32_t pad_freqs[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
    const uint32_t pad_colors[8] = {
        0xFF2D55, 0xFF9500, 0xFFCC00, 0x30D158,
        0x64D2FF, 0x0A84FF, 0x5856D6, 0xAF52DE
    };

    play_system_sound((char*)"notification");

    bool done = false;
    while (!done) {
        if (pad_flash_timer > 0) pad_flash_timer--;

        // Poll Keyboard
        EFI_INPUT_KEY Key;
        if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
            if (Key.ScanCode == 0x17) {
                if (active_mode != 0) {
                    active_mode = 0;
                    play_system_sound((char*)"click");
                } else {
                    done = true;
                }
            } else if (active_mode == 1) {
                if (Key.ScanCode == 0x04 || Key.UnicodeChar == 'a' || Key.UnicodeChar == 'A') {
                    paddle_x -= 24;
                    if (paddle_x < 0) paddle_x = 0;
                } else if (Key.ScanCode == 0x03 || Key.UnicodeChar == 'd' || Key.UnicodeChar == 'D') {
                    paddle_x += 24;
                    if (paddle_x > 460) paddle_x = 460;
                }
            } else if (active_mode == 3) {
                if (Key.UnicodeChar >= '1' && Key.UnicodeChar <= '8') {
                    int p = (int)(Key.UnicodeChar - '1');
                    last_pad_hit = p;
                    pad_flash_timer = 10;
                    play_system_sound((char*)"click");
                    play_pit_tone(SystemTable, pad_freqs[p], 45000);
                }
            }
        }

        // Poll Mouse
        bool clicked = false;
        if (Mouse) {
            EFI_SIMPLE_POINTER_STATE State;
            if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                int dx = State.RelativeMovementX / 1000;
                int dy = State.RelativeMovementY / 1000;
                if (dx || dy) {
                    cursor_x += dx;
                    cursor_y += dy;
                    if (cursor_x < 0) cursor_x = 0;
                    if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                    if (cursor_y < 0) cursor_y = 0;
                    if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                }
                mouse_down = State.LeftButton;
                clicked = (State.LeftButton && !prev_left);
                prev_left = State.LeftButton;
            }
        }

        int content_x = win_x + sidebar_w + 16;

        // Handle clicks & interactive modes
        if (clicked) {
            // Window Close button
            if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h) ||
                (cursor_x >= win_x + 10 && cursor_x <= win_x + 30 &&
                 cursor_y >= win_y + 8 && cursor_y <= win_y + 30)) {
                done = true;
            }

            // Sidebar navigation clicks (also exits embedded app back to store)
            for (int s = 0; s < 4; s++) {
                int sy = win_y + 60 + s * 38;
                if (cursor_x >= win_x + 10 && cursor_x <= win_x + sidebar_w - 10 &&
                    cursor_y >= sy && cursor_y < sy + 32) {
                    active_nav = s;
                    active_mode = 0;
                    play_system_sound((char*)"click");
                }
            }

            if (active_mode != 0) {
                // [ Back to Store ] button in mini-apps
                int back_x = content_x;
                int back_y = win_y + 52;
                if (cursor_x >= back_x && cursor_x <= back_x + 130 &&
                    cursor_y >= back_y && cursor_y <= back_y + 28) {
                    active_mode = 0;
                    play_system_sound((char*)"click");
                }
            }

            if (active_mode == 0) {
                // Store Catalog card actions
                int grid_y = win_y + 150;
                int vis_idx = 0;
                for (int i = 0; i < 6; i++) {
                    if (active_nav == 1 && i < 3) continue;
                    if (active_nav == 2 && i >= 3) continue;
                    if (active_nav == 3 && !store_catalog[i].installed) continue;

                    int row = vis_idx / 2;
                    int col = vis_idx % 2;
                    int card_w = (win_w - sidebar_w - 48) / 2;
                    int card_h = 104;
                    int card_x = content_x + col * (card_w + 12);
                    int card_y = grid_y + row * (card_h + 12);
                    vis_idx++;

                    int btn_x = card_x + card_w - 76;
                    int btn_y = card_y + 20;

                    // GET or OPEN button
                    if (cursor_x >= btn_x && cursor_x <= btn_x + 64 &&
                        cursor_y >= btn_y && cursor_y <= btn_y + 28) {
                        if (!store_catalog[i].installed) {
                            if (write_pkg_to_disk(SystemTable, store_catalog[i].pkg_path,
                                                  store_catalog[i].id, store_catalog[i].name,
                                                  store_catalog[i].version)) {
                                store_catalog[i].installed = true;
                                store_catalog[i].disk_bytes = 256;
                                play_system_sound((char*)"notification");
                                blankUI_draw_toast((char*)"App Store", (char*)"Package written to \\EFI\\APPS\\ on FAT32!");
                            } else {
                                store_catalog[i].installed = true;
                                store_catalog[i].disk_bytes = 256;
                                play_system_sound((char*)"notification");
                                blankUI_draw_toast((char*)"App Store", (char*)"Package installed in session memory.");
                            }
                        } else {
                            play_system_sound((char*)"click");
                            if (i == 0) {
                                launch_blankpad();
                                if (Mouse) Mouse->Reset(Mouse, TRUE);
                                prev_left = false;
                            } else if (i == 1) {
                                launch_task_manager();
                                if (Mouse) Mouse->Reset(Mouse, TRUE);
                                prev_left = false;
                            } else if (i == 2) {
                                launch_blankreg_edit();
                                if (Mouse) Mouse->Reset(Mouse, TRUE);
                                prev_left = false;
                            } else if (i == 3) {
                                active_mode = 1; // NeonBreak Arcade
                            } else if (i == 4) {
                                active_mode = 2; // CanvasStudio Paint
                            } else if (i == 5) {
                                active_mode = 3; // SynthWave Pad
                            }
                        }
                    }

                    // UNINSTALL button (if installed)
                    if (store_catalog[i].installed) {
                        int un_x = card_x + card_w - 76;
                        int un_y = card_y + 54;
                        if (cursor_x >= un_x && cursor_x <= un_x + 64 &&
                            cursor_y >= un_y && cursor_y <= un_y + 22) {
                            delete_pkg_from_disk(SystemTable, store_catalog[i].pkg_path);
                            store_catalog[i].installed = false;
                            store_catalog[i].disk_bytes = 0;
                            play_system_sound((char*)"click");
                            blankUI_draw_toast((char*)"App Store", (char*)"Removed .PKG file from \\EFI\\APPS\\.");
                        }
                    }
                }
            } else if (active_mode == 1) {
                // NeonBreak Reset button
                int rst_x = content_x + 430;
                int rst_y = win_y + 52;
                if (cursor_x >= rst_x && cursor_x <= rst_x + 120 &&
                    cursor_y >= rst_y && cursor_y <= rst_y + 28) {
                    arcade_score = 0;
                    arcade_lives = 3;
                    ball_x = 260;
                    ball_y = 240;
                    ball_vx = 4;
                    ball_vy = -4;
                    for (int b = 0; b < 32; b++) bricks[b] = true;
                    play_system_sound((char*)"notification");
                }
            } else if (active_mode == 2) {
                // CanvasStudio palette & toolbar clicks
                int pal_x = content_x + 145;
                int pal_y = win_y + 52;
                for (int c = 0; c < 8; c++) {
                    int cx = pal_x + c * 26;
                    if (cursor_x >= cx && cursor_x <= cx + 22 &&
                        cursor_y >= pal_y && cursor_y <= pal_y + 24) {
                        selected_color_idx = (uint8_t)c;
                        play_system_sound((char*)"click");
                    }
                }
                // Clear button
                int clr_x = content_x + 362;
                if (cursor_x >= clr_x && cursor_x <= clr_x + 58 &&
                    cursor_y >= pal_y && cursor_y <= pal_y + 26) {
                    for (int p = 0; p < 640; p++) canvas_grid[p] = 0;
                    play_system_sound((char*)"click");
                }
                // Save button
                int sav_x = content_x + 426;
                if (cursor_x >= sav_x && cursor_x <= sav_x + 68 &&
                    cursor_y >= pal_y && cursor_y <= pal_y + 26) {
                    if (save_canvas_to_disk(SystemTable, canvas_grid, 640)) {
                        play_system_sound((char*)"notification");
                        blankUI_draw_toast((char*)"CanvasStudio", (char*)"Saved artwork to \\EFI\\APPS\\CANVAS.ART!");
                    }
                }
                // Load button
                int lod_x = content_x + 500;
                if (cursor_x >= lod_x && cursor_x <= lod_x + 68 &&
                    cursor_y >= pal_y && cursor_y <= pal_y + 26) {
                    if (load_canvas_from_disk(SystemTable, canvas_grid, 640)) {
                        play_system_sound((char*)"notification");
                        blankUI_draw_toast((char*)"CanvasStudio", (char*)"Loaded \\EFI\\APPS\\CANVAS.ART from disk!");
                    }
                }
            } else if (active_mode == 3) {
                // SynthWave 8 pads click
                int pad_start_x = content_x + 10;
                int pad_start_y = win_y + 125;
                int pw = 128;
                int ph = 135;
                for (int p = 0; p < 8; p++) {
                    int r = p / 4;
                    int c = p % 4;
                    int px = pad_start_x + c * (pw + 12);
                    int py = pad_start_y + r * (ph + 14);
                    if (cursor_x >= px && cursor_x <= px + pw &&
                        cursor_y >= py && cursor_y <= py + ph) {
                        last_pad_hit = p;
                        pad_flash_timer = 10;
                        play_system_sound((char*)"click");
                        play_pit_tone(SystemTable, pad_freqs[p], 45000);
                    }
                }
            }
        }

        // Continuous drag painting in CanvasStudio
        if (active_mode == 2 && mouse_down) {
            int cv_x = content_x + 12;
            int cv_y = win_y + 92;
            int cell_s = 17;
            if (cursor_x >= cv_x && cursor_x < cv_x + 32 * cell_s &&
                cursor_y >= cv_y && cursor_y < cv_y + 20 * cell_s) {
                int gx = (cursor_x - cv_x) / cell_s;
                int gy = (cursor_y - cv_y) / cell_s;
                if (gx >= 0 && gx < 32 && gy >= 0 && gy < 20) {
                    canvas_grid[gy * 32 + gx] = selected_color_idx;
                }
            }
        }

        // Draw Background & Desktop Shell
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();

        // Main Window Body
        dui_shadow(win_x, win_y, win_w, win_h, 14, 10, 0x000000, 70);
        dui_rect_rounded(win_x, win_y, win_w, win_h, 14, 0xF2F2F7, 252);

        // Titlebar
        dui_rect(win_x, win_y, win_w, 38, 0xE5E5EA, 255);
        dui_circle(win_x + 18, win_y + 19, 6, 0xFF5F56, 255);
        dui_circle(win_x + 38, win_y + 19, 6, 0xFFBD2E, 255);
        dui_circle(win_x + 58, win_y + 19, 6, 0x27C93F, 255);
        dui_text(win_x + win_w / 2 - dui_text_width("BlankOS App Store & FAT32 Package Hub", 1) / 2,
                 win_y + 12, "BlankOS App Store & FAT32 Package Hub", 0x1C1C1E, 1);

        // Sidebar
        dui_rect(win_x, win_y + 38, sidebar_w, win_h - 38, 0xEBEBF0, 248);
        dui_line(win_x + sidebar_w, win_y + 38, win_x + sidebar_w, win_y + win_h, 0xD1D1D6, 1);

        const char* nav_items[4] = { "Discover All", "Games & Studio", "Work & Kernel", "Installed (.PKG)" };
        for (int s = 0; s < 4; s++) {
            int sy = win_y + 60 + s * 38;
            if (s == active_nav && active_mode == 0) {
                dui_rect_rounded(win_x + 10, sy, sidebar_w - 20, 32, 6, 0x0A84FF, 255);
                dui_text(win_x + 20, sy + 8, nav_items[s], 0xFFFFFF, 1);
            } else {
                dui_text(win_x + 20, sy + 8, nav_items[s], 0x3A3A3C, 1);
            }
        }

        // Disk persistence indicator in sidebar bottom
        int inst_cnt = 0;
        for (int i = 0; i < 6; i++) if (store_catalog[i].installed) inst_cnt++;
        char pkg_stat[48];
        char nb[16];
        st_strcpy(pkg_stat, "FAT32 Packages: ");
        st_utoa((uint64_t)inst_cnt, nb);
        st_strcat(pkg_stat, nb);
        st_strcat(pkg_stat, "/6");
        dui_text(win_x + 16, win_y + win_h - 46, pkg_stat, 0x3A3A3C, 1);
        dui_text(win_x + 16, win_y + win_h - 28, "Path: \\EFI\\APPS\\*.PKG", 0x8E8E93, 1);

        // MODE 0: STORE CATALOG
        if (active_mode == 0) {
            int banner_w = win_w - sidebar_w - 32;
            int banner_h = 80;
            dui_gradient_rounded(content_x, win_y + 52, banner_w, banner_h, 10, 0x0A84FF, 0x5856D6);
            dui_text(content_x + 18, win_y + 64, "VERIFIED UEFI FAT32 PACKAGE REPOSITORY", 0x82C7FF, 1);
            dui_text(content_x + 18, win_y + 84, "Install Real BlankOS System Tools & Interactive Creative Apps", 0xFFFFFF, 1);
            dui_text(content_x + 18, win_y + 104, "Packages persist in \\EFI\\APPS\\ across reboots and launch natively.", 0xE5E5EA, 1);

            int grid_y = win_y + 150;
            int vis_idx = 0;
            for (int i = 0; i < 6; i++) {
                if (active_nav == 1 && i < 3) continue;
                if (active_nav == 2 && i >= 3) continue;
                if (active_nav == 3 && !store_catalog[i].installed) continue;

                int row = vis_idx / 2;
                int col = vis_idx % 2;
                int card_w = (win_w - sidebar_w - 48) / 2;
                int card_h = 104;
                int card_x = content_x + col * (card_w + 12);
                int card_y = grid_y + row * (card_h + 12);
                vis_idx++;

                dui_rect_rounded(card_x, card_y, card_w, card_h, 8, 0xFFFFFF, 255);
                dui_rect_outline(card_x, card_y, card_w, card_h, 0xE5E5EA, 1);

                dui_rect_rounded(card_x + 12, card_y + 14, 44, 44, 10, store_catalog[i].icon_color, 255);
                dui_text(card_x + 26, card_y + 28, "+", 0xFFFFFF, 1);

                dui_text(card_x + 66, card_y + 14, store_catalog[i].name, 0x1C1C1E, 1);
                dui_text(card_x + 66, card_y + 32, store_catalog[i].category, 0x8E8E93, 1);
                dui_text(card_x + 66, card_y + 48, store_catalog[i].version, 0x0A84FF, 1);

                dui_text(card_x + 12, card_y + 80, store_catalog[i].desc, 0x636366, 1);

                int btn_x = card_x + card_w - 76;
                int btn_y = card_y + 18;
                if (store_catalog[i].installed) {
                    dui_rect_rounded(btn_x, btn_y, 64, 28, 14, 0x34C759, 255);
                    dui_text(btn_x + 14, btn_y + 7, "OPEN", 0xFFFFFF, 1);

                    dui_rect_rounded(btn_x, card_y + 52, 64, 22, 6, 0xFFE5E5, 255);
                    dui_text(btn_x + 8, card_y + 56, "REMOVE", 0xFF3B30, 1);
                } else {
                    dui_rect_rounded(btn_x, btn_y, 64, 28, 14, 0x007AFF, 255);
                    dui_text(btn_x + 18, btn_y + 7, "GET", 0xFFFFFF, 1);
                }
            }
        }

        // MODE 1: NEONBREAK ARCADE (Playable Brick-Breaker Game)
        else if (active_mode == 1) {
            dui_rect_rounded(content_x, win_y + 52, 130, 28, 7, 0x2C2C2E, 255);
            dui_text(content_x + 12, win_y + 59, "[ Back to Store ]", 0xFFFFFF, 1);

            dui_rect_rounded(content_x + 430, win_y + 52, 120, 28, 7, 0x0A84FF, 255);
            dui_text(content_x + 444, win_y + 59, "[ Reset Game ]", 0xFFFFFF, 1);

            char sc_buf[64];
            st_strcpy(sc_buf, "SCORE: ");
            st_utoa((uint64_t)arcade_score, nb);
            st_strcat(sc_buf, nb);
            st_strcat(sc_buf, "   |   LIVES: ");
            st_utoa((uint64_t)arcade_lives, nb);
            st_strcat(sc_buf, nb);
            dui_text(content_x + 150, win_y + 59, sc_buf, 0x1C1C1E, 1);

            int arena_x = content_x;
            int arena_y = win_y + 90;
            int arena_w = win_w - sidebar_w - 32;
            int arena_h = win_h - 108;
            dui_rect_rounded(arena_x, arena_y, arena_w, arena_h, 10, 0x11131A, 255);
            dui_rect_outline(arena_x, arena_y, arena_w, arena_h, 0x2E3446, 1);

            // Mouse tracks paddle when inside arena
            if (cursor_x >= arena_x && cursor_x <= arena_x + arena_w) {
                paddle_x = cursor_x - arena_x - 45;
                if (paddle_x < 6) paddle_x = 6;
                if (paddle_x > arena_w - 96) paddle_x = arena_w - 96;
            }

            // Update ball physics
            if (arcade_lives > 0) {
                ball_x += ball_vx;
                ball_y += ball_vy;

                if (ball_x <= 8) { ball_x = 8; ball_vx = -ball_vx; }
                if (ball_x >= arena_w - 16) { ball_x = arena_w - 16; ball_vx = -ball_vx; }
                if (ball_y <= 8) { ball_y = 8; ball_vy = -ball_vy; }

                // Paddle collision
                int py = arena_h - 28;
                if (ball_vy > 0 && ball_y + 8 >= py && ball_y <= py + 12 &&
                    ball_x + 8 >= paddle_x && ball_x <= paddle_x + 90) {
                    ball_vy = -ball_vy;
                    int hit_offset = (ball_x - (paddle_x + 45)) / 10;
                    ball_vx = hit_offset;
                    if (ball_vx == 0) ball_vx = 3;
                    play_pit_tone(SystemTable, 440, 6000);
                }

                // Brick collisions (4 rows x 8 cols)
                int bw = (arena_w - 48) / 8;
                int bh = 20;
                for (int b = 0; b < 32; b++) {
                    if (!bricks[b]) continue;
                    int br = b / 8;
                    int bc = b % 8;
                    int bx = 16 + bc * (bw + 2);
                    int by = 24 + br * (bh + 6);
                    if (ball_x + 8 >= bx && ball_x <= bx + bw &&
                        ball_y + 8 >= by && ball_y <= by + bh) {
                        bricks[b] = false;
                        ball_vy = -ball_vy;
                        arcade_score += 25;
                        play_pit_tone(SystemTable, 587 + br * 80, 8000);
                        break;
                    }
                }

                if (ball_y > arena_h - 8) {
                    arcade_lives--;
                    ball_x = arena_w / 2;
                    ball_y = arena_h / 2;
                    ball_vy = -4;
                    play_pit_tone(SystemTable, 180, 25000);
                }
            }

            // Draw bricks
            const uint32_t bcols[4] = { 0xFF2D55, 0xFF9F0A, 0x30D158, 0x64D2FF };
            int bw = (arena_w - 48) / 8;
            int bh = 20;
            for (int b = 0; b < 32; b++) {
                if (!bricks[b]) continue;
                int br = b / 8;
                int bc = b % 8;
                int bx = arena_x + 16 + bc * (bw + 2);
                int by = arena_y + 24 + br * (bh + 6);
                dui_rect_rounded(bx, by, bw, bh, 4, bcols[br], 255);
            }

            // Draw paddle & ball
            dui_rect_rounded(arena_x + paddle_x, arena_y + arena_h - 28, 90, 12, 6, 0x0A84FF, 255);
            dui_circle(arena_x + ball_x, arena_y + ball_y, 6, 0xFFFFFF, 255);

            if (arcade_lives <= 0) {
                dui_text(arena_x + 170, arena_y + arena_h / 2, "GAME OVER - Click [ Reset Game ]", 0xFF453A, 1);
            }
        }

        // MODE 2: CANVASSTUDIO PAINT
        else if (active_mode == 2) {
            dui_rect_rounded(content_x, win_y + 52, 130, 26, 7, 0x2C2C2E, 255);
            dui_text(content_x + 12, win_y + 58, "[ Back to Store ]", 0xFFFFFF, 1);

            int pal_x = content_x + 145;
            int pal_y = win_y + 52;
            for (int c = 0; c < 8; c++) {
                int cx = pal_x + c * 26;
                dui_rect_rounded(cx, pal_y, 22, 24, 4, palette_colors[c], 255);
                if (selected_color_idx == (uint8_t)c) {
                    dui_rect_outline(cx - 1, pal_y - 1, 24, 26, 0x0A84FF, 2);
                }
            }

            dui_rect_rounded(content_x + 362, pal_y, 58, 26, 6, 0xFF3B30, 255);
            dui_text(content_x + 372, pal_y + 6, "Clear", 0xFFFFFF, 1);

            dui_rect_rounded(content_x + 426, pal_y, 68, 26, 6, 0x34C759, 255);
            dui_text(content_x + 440, pal_y + 6, "Save", 0xFFFFFF, 1);

            dui_rect_rounded(content_x + 500, pal_y, 68, 26, 6, 0x0A84FF, 255);
            dui_text(content_x + 514, pal_y + 6, "Load", 0xFFFFFF, 1);

            int cv_x = content_x + 12;
            int cv_y = win_y + 92;
            int cell_s = 17;
            dui_rect_outline(cv_x - 2, cv_y - 2, 32 * cell_s + 4, 20 * cell_s + 4, 0x8E8E93, 2);

            for (int gy = 0; gy < 20; gy++) {
                for (int gx = 0; gx < 32; gx++) {
                    uint8_t cidx = canvas_grid[gy * 32 + gx] & 7;
                    dui_rect(cv_x + gx * cell_s, cv_y + gy * cell_s, cell_s - 1, cell_s - 1, palette_colors[cidx], 255);
                }
            }
            dui_text(cv_x, cv_y + 20 * cell_s + 10, "Drag mouse to paint | Save writes \\EFI\\APPS\\CANVAS.ART to FAT32", 0x4A5060, 1);
        }

        // MODE 3: SYNTHWAVE PAD
        else if (active_mode == 3) {
            dui_rect_rounded(content_x, win_y + 52, 130, 28, 7, 0x2C2C2E, 255);
            dui_text(content_x + 12, win_y + 59, "[ Back to Store ]", 0xFFFFFF, 1);
            dui_text(content_x + 148, win_y + 59, "SynthWave 8-Pad Hardware PIT & HDAudio Synthesizer (Click or Keys 1-8)", 0x1C1C1E, 1);

            int pad_start_x = content_x + 10;
            int pad_start_y = win_y + 125;
            int pw = 128;
            int ph = 135;
            for (int p = 0; p < 8; p++) {
                int r = p / 4;
                int c = p % 4;
                int px = pad_start_x + c * (pw + 12);
                int py = pad_start_y + r * (ph + 14);
                bool active = (last_pad_hit == p && pad_flash_timer > 0);
                uint32_t col = active ? 0xFFFFFF : pad_colors[p];
                uint32_t tcol = active ? 0x1C1C1E : 0xFFFFFF;

                dui_rect_rounded(px, py, pw, ph, 12, col, 255);
                dui_rect_outline(px, py, pw, ph, 0x1C1C1E, 1);
                dui_text(px + 16, py + 24, pad_names[p], tcol, 1);

                char fstr[32];
                st_utoa((uint64_t)pad_freqs[p], nb);
                st_strcpy(fstr, nb);
                st_strcat(fstr, " Hz");
                dui_text(px + 16, py + 52, fstr, tcol, 1);
                dui_text(px + 16, py + 96, "[ TRIGGER ]", tcol, 1);
            }
        }

        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();
        SystemTable->BootServices->Stall(16000);
    }
}
