#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern void play_system_sound(char* sound_name);
    extern int screen_width;
    extern int screen_height;
    
    // --- Tiny Math helpers ---
    static inline float d_sin(float rad) {
        // Minimal Taylor series approximation for sin
        float x = rad;
        while (x > 3.14159f) x -= 6.28318f;
        while (x < -3.14159f) x += 6.28318f;
        float x3 = x * x * x;
        float x5 = x3 * x * x;
        return x - (x3 / 6.0f) + (x5 / 120.0f);
    }
    static inline float d_cos(float rad) {
        return d_sin(rad + 1.57079f);
    }

    // --- WAD Structure Parser ---
    typedef struct {
        char magic[4];       // IWAD or PWAD
        uint32_t numlumps;
        uint32_t infotableofs;
    } __attribute__((packed)) WadHeader;

    typedef struct {
        uint32_t filepos;
        uint32_t size;
        char name[8];
    } __attribute__((packed)) WadEntry;

    // --- Game Logic ---
    static float player_x = 3.5f;
    static float player_y = 3.5f;
    static float player_angle = 0.0f; // radians
    
    #define MAP_WIDTH 16
    #define MAP_HEIGHT 16
    static const uint8_t doom_map[MAP_WIDTH * MAP_HEIGHT] = {
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        1,0,0,0,0,0,1,0,0,0,0,0,0,0,0,1,
        1,0,1,1,0,0,1,0,2,2,2,2,0,0,0,1,
        1,0,1,0,0,0,0,0,2,0,0,2,0,0,0,1,
        1,0,1,0,0,0,1,0,2,0,0,2,0,3,3,1,
        1,0,0,0,0,0,1,0,0,0,0,0,0,3,0,1,
        1,1,1,0,1,1,1,1,1,0,1,1,1,3,0,1,
        1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,
        1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,
        1,0,4,4,4,4,0,1,1,1,0,0,0,0,0,1,
        1,0,4,0,0,4,0,1,0,1,0,0,0,0,0,1,
        1,0,4,0,0,0,0,1,0,1,1,1,1,0,0,1,
        1,0,4,4,4,4,0,0,0,0,0,0,1,0,0,1,
        1,0,0,0,0,0,0,1,0,0,0,0,1,0,0,1,
        1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
    };

    static bool wad_loaded = false;
    static char wad_info[64] = "WAD Status: No WAD Loaded (Procedural Fallback)";

    // Minimal WAD Loader
    static bool load_doom_wad(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        EFI_STATUS Status = SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
        if (EFI_ERROR(Status) || numHandles == 0) return false;

        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            EFI_FILE_HANDLE file = NULL;
            // Look for doom1.wad or DOOM1.WAD
            Status = root->Open(root, &file, (CHAR16*)L"assets\\doom1.wad", EFI_FILE_MODE_READ, 0);
            if (EFI_ERROR(Status)) {
                Status = root->Open(root, &file, (CHAR16*)L"assets\\DOOM1.WAD", EFI_FILE_MODE_READ, 0);
            }
            
            if (!EFI_ERROR(Status) && file != NULL) {
                WadHeader header;
                UINTN sz = sizeof(WadHeader);
                Status = file->Read(file, &sz, &header);
                if (!EFI_ERROR(Status) && (header.magic[0] == 'I' || header.magic[0] == 'P') &&
                    header.magic[1] == 'W' && header.magic[2] == 'A' && header.magic[3] == 'D') {
                    
                    wad_loaded = true;
                    // Format success info
                    const char* type = (header.magic[0] == 'I') ? "IWAD" : "PWAD";
                    // Custom formatting since no sprintf
                    char* dest = wad_info;
                    const char* prefix = "Doom WAD: ";
                    while (*prefix) *dest++ = *prefix++;
                    *dest++ = type[0]; *dest++ = type[1]; *dest++ = type[2]; *dest++ = type[3];
                    const char* suffix = " Loaded Successfully!";
                    while (*suffix) *dest++ = *suffix++;
                    *dest = '\0';
                }
                file->Close(file);
                root->Close(root);
                break;
            }
            root->Close(root);
        }
        if (handleBuffer) SystemTable->BootServices->FreePool(handleBuffer);
        return wad_loaded;
    }

    void launch_doom(EFI_SYSTEM_TABLE* SystemTable) {
        // Init Mouse
        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool done = false;

        // Try to load doom1.wad
        load_doom_wad(SystemTable);

        // Raycasting screen setup
        int play_w = 640;
        int play_h = 360;
        int play_x = (screen_width - play_w) / 2;
        int play_y = (screen_height - play_h) / 2 - 30;

        float fov = 1.0f; // Field of view (approx 60 deg)
        int weapon_frame = 0;
        bool weapon_firing = false;

        while (!done) {
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) { done = true; } // ESC
                
                // Movement
                float dx = d_cos(player_angle) * 0.2f;
                float dy = d_sin(player_angle) * 0.2f;
                if (Key.UnicodeChar == 'w' || Key.UnicodeChar == 'W') {
                    if (doom_map[(int)(player_y) * MAP_WIDTH + (int)(player_x + dx)] == 0) player_x += dx;
                    if (doom_map[(int)(player_y + dy) * MAP_WIDTH + (int)(player_x)] == 0) player_y += dy;
                }
                if (Key.UnicodeChar == 's' || Key.UnicodeChar == 'S') {
                    if (doom_map[(int)(player_y) * MAP_WIDTH + (int)(player_x - dx)] == 0) player_x -= dx;
                    if (doom_map[(int)(player_y - dy) * MAP_WIDTH + (int)(player_x)] == 0) player_y -= dy;
                }
                if (Key.UnicodeChar == 'a' || Key.UnicodeChar == 'A') {
                    player_angle -= 0.15f;
                }
                if (Key.UnicodeChar == 'd' || Key.UnicodeChar == 'D') {
                    player_angle += 0.15f;
                }
                if (Key.UnicodeChar == ' ') {
                    weapon_firing = true;
                    weapon_frame = 1;
                    play_system_sound((char*)"shoot");
                }
            }

            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx != 0 || dy != 0) {
                        cursor_x += dx; cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                        
                        // Turn player with mouse X delta
                        player_angle += dx * 0.005f;
                    }
                    if (State.LeftButton && !weapon_firing) {
                        weapon_firing = true;
                        weapon_frame = 1;
                        play_system_sound((char*)"shoot");
                    }
                }
            }

            // Draw Base Desktop
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();

            // Draw Doom Window Frame (macOS inspired)
            dui_shadow(play_x, play_y, play_w, play_h, 12, 12, 0x000000, 80);
            dui_rect_rounded(play_x, play_y, play_w, play_h, 12, 0x1E1E1E, 255); // Dark window body
            dui_rect(play_x, play_y, play_w, 24, 0x2C2C2E, 255); // Header bar
            
            // Header buttons
            dui_circle(play_x + 12, play_y + 12, 5, 0xFF5F56, 255); // Close
            dui_circle(play_x + 28, play_y + 12, 5, 0xFFBD2E, 255); // Minimize
            dui_circle(play_x + 44, play_y + 12, 5, 0x27C93F, 255); // Maximize
            dui_text(play_x + play_w/2 - 40, play_y + 4, "DOOM 3D Port", 0xCDD6F4, 1);

            // Raycaster 3D Render Screen (draw inside the window body, offset by header 24px)
            int view_y = play_y + 24;
            int view_h = play_h - 24;
            
            // Draw Ceiling and Floor
            dui_rect(play_x, view_y, play_w, view_h / 2, 0x383838, 255); // Ceiling (dark grey)
            dui_rect(play_x, view_y + view_h / 2, play_w, view_h / 2, 0x707070, 255); // Floor (lighter grey)

            // Raycasting wall drawer
            for (int x = 0; x < play_w; x += 2) {
                // Calculate ray position and direction
                float ray_angle = (player_angle - fov / 2.0f) + ((float)x / play_w) * fov;
                float distance = 0.0f;
                bool hit_wall = false;
                
                float eye_x = d_cos(ray_angle);
                float eye_y = d_sin(ray_angle);
                
                int wall_type = 0;

                while (!hit_wall && distance < 16.0f) {
                    distance += 0.08f;
                    int test_x = (int)(player_x + eye_x * distance);
                    int test_y = (int)(player_y + eye_y * distance);
                    
                    // Out of bounds test
                    if (test_x < 0 || test_x >= MAP_WIDTH || test_y < 0 || test_y >= MAP_HEIGHT) {
                        hit_wall = true;
                        distance = 16.0f;
                    } else {
                        uint8_t cell = doom_map[test_y * MAP_WIDTH + test_x];
                        if (cell > 0) {
                            hit_wall = true;
                            wall_type = cell;
                        }
                    }
                }

                // Fish-eye correction
                float correct_distance = distance * d_cos(ray_angle - player_angle);
                if (correct_distance < 0.1f) correct_distance = 0.1f;

                // Calculate wall column height
                int wall_h = (int)(view_h / correct_distance);
                if (wall_h > view_h) wall_h = view_h;

                int wall_top = view_y + (view_h - wall_h) / 2;
                int wall_bottom = wall_top + wall_h;

                // Doom Wall Textures/Shading
                uint32_t wall_color = 0x880000; // default red brick
                if (wall_type == 1) wall_color = 0x555555; // Grey stone
                else if (wall_type == 2) wall_color = 0xAA6600; // Brown wood
                else if (wall_type == 3) wall_color = 0x008888; // Blue metal
                else if (wall_type == 4) wall_color = 0x770077; // Toxic waste brick

                // Shadow walls based on distance
                float shadow = 1.0f - (distance / 16.0f);
                if (shadow < 0.15f) shadow = 0.15f;
                uint8_t r = (uint8_t)(((wall_color >> 16) & 0xFF) * shadow);
                uint8_t g = (uint8_t)(((wall_color >> 8) & 0xFF) * shadow);
                uint8_t b = (uint8_t)((wall_color & 0xFF) * shadow);
                uint32_t final_color = (r << 16) | (g << 8) | b;

                // Draw vertical wall slice (2px wide for speed)
                dui_rect(play_x + x, wall_top, 2, wall_h, final_color, 255);
            }

            // Draw Weapon HUD (Doom Shotgun Overlay)
            int weapon_x = play_x + play_w / 2 - 64;
            int weapon_y = view_y + view_h - 128;
            
            if (weapon_firing) {
                // Shoot Frame (Draw muzzle flash & offset gun)
                dui_rect_rounded(weapon_x + 16, weapon_y - 16, 96, 96, 8, 0xFFE066, 220); // Muzzle flash
                dui_rect(weapon_x + 32, weapon_y + 20, 64, 108, 0x1A1A1A, 255); // Shotgun body
                dui_rect(weapon_x + 48, weapon_y + 10, 32, 10, 0x888888, 255); // Steel barrel
                
                weapon_frame++;
                if (weapon_frame > 5) {
                    weapon_firing = false;
                    weapon_frame = 0;
                }
            } else {
                // Idle Frame
                dui_rect(weapon_x + 32, weapon_y + 10, 64, 118, 0x1A1A1A, 255); // Shotgun body
                dui_rect(weapon_x + 48, weapon_y + 0, 32, 10, 0x888888, 255); // Steel barrel
            }

            // Draw Crosshair
            dui_rect(play_x + play_w/2 - 4, view_y + view_h/2 - 1, 8, 2, 0xFF3B30, 255);
            dui_rect(play_x + play_w/2 - 1, view_y + view_h/2 - 4, 2, 8, 0xFF3B30, 255);

            // Print WAD info overlay
            dui_text(play_x + 16, view_y + 16, wad_info, 0xFFCC00, 1);
            dui_text(play_x + 16, view_y + 36, "Controls: WASD to Move | MOUSE to Look | SPACE or CLICK to Fire", 0xFFFFFF, 1);
            
            // Draw desktop cursor & swap buffers
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();

            SystemTable->BootServices->Stall(16000); // 60 FPS lock
        }
    }
}
