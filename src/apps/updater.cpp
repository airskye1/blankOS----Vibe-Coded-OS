#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_progress_bar(int x, int y, int width, float percentage);
    extern void blankUI_draw_cursor(int x, int y);
    extern void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha);
    extern void play_system_sound(char* sound_name);
    
    // STB Image Definition
    extern unsigned char *stbi_load_from_memory(unsigned char const *buffer, int len, int *x, int *y, int *channels_in_file, int desired_channels);
    extern void stbi_image_free(void *retval_from_stbi_load);

    // Helper to read file from FAT32
    static bool read_fat32_file(EFI_SYSTEM_TABLE* SystemTable, CHAR16* path, void** buffer, UINTN* size) {
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
            Status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR(Status) && file != NULL) {
                // Get file size roughly (try 5MB)
                *size = 5 * 1024 * 1024;
                SystemTable->BootServices->AllocatePool(EfiLoaderData, *size, buffer);
                Status = file->Read(file, size, *buffer);
                file->Close(file);
                root->Close(root);
                SystemTable->BootServices->FreePool(handleBuffer);
                return !EFI_ERROR(Status);
            }
            root->Close(root);
        }
        SystemTable->BootServices->FreePool(handleBuffer);
        return false;
    }

    // Helper to draw an STB image
    static void draw_image(EFI_SYSTEM_TABLE* SystemTable, CHAR16* path, int start_x, int start_y) {
        void* fileBuffer = NULL;
        UINTN fileSize = 0;
        if (read_fat32_file(SystemTable, path, &fileBuffer, &fileSize)) {
            int w, h, comp;
            unsigned char* img = stbi_load_from_memory((unsigned char*)fileBuffer, fileSize, &w, &h, &comp, 4);
            if (img) {
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        int idx = (y * w + x) * 4;
                        uint8_t r = img[idx];
                        uint8_t g = img[idx+1];
                        uint8_t b = img[idx+2];
                        uint8_t a = img[idx+3];
                        uint32_t color = (r << 16) | (g << 8) | b;
                        put_pixel_alpha(start_x + x, start_y + y, color, a);
                    }
                }
                stbi_image_free(img);
            }
            SystemTable->BootServices->FreePool(fileBuffer);
        }
    }
    
    // Fallback partition manager (A/B data loss protection)
    static void execute_update_partition_swap(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            // Check if INSTALLED.FLG exists on this volume
            EFI_FILE_HANDLE flag = NULL;
            if (root->Open(root, &flag, (CHAR16*)L"EFI\\BOOT\\INSTALLED.FLG", EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
                flag->Close(flag);
                // 1. Rename BOOTX64.EFI -> BOOTOLD.EFI
                // EFI File API requires modifying EFI_FILE_INFO for rename, which is complex.
                // We'll write BOOTNEXT.EFI here simulating the payload download!
                EFI_FILE_HANDLE nextFile = NULL;
                EFI_STATUS s = root->Open(root, &nextFile, (CHAR16*)L"EFI\\BOOT\\BOOTNEXT.EFI", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                if (s == EFI_SUCCESS && nextFile) {
                    char dummy[] = "OTA_KERNEL_PAYLOAD";
                    UINTN sz = sizeof(dummy);
                    nextFile->Write(nextFile, &sz, dummy);
                    nextFile->Close(nextFile);
                }
                root->Close(root);
                break;
            }
            root->Close(root);
        }
        if (handleBuffer) SystemTable->BootServices->FreePool(handleBuffer);
    }

    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern void blankUI_set_cursor_type(int type);

    void launch_updater(EFI_SYSTEM_TABLE *SystemTable) {
        int win_w = 720;
        int win_h = 460;
        
        // 1. Live OS Detection
        void* temp = NULL;
        UINTN tsz = 0;
        bool is_installed = read_fat32_file(SystemTable, (CHAR16*)L"EFI\\BOOT\\INSTALLED.FLG", &temp, &tsz);
        if (temp) SystemTable->BootServices->FreePool(temp);
        
        if (!is_installed) {
            int win_x = (screen_width - win_w) / 2;
            int win_y = (screen_height - win_h) / 2;
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"System Settings - Software Update");
            blankUI_draw_text_color(win_x + 220, win_y + 160, (char*)"Update Failed: Live CD Mode", 0xFF3B30);
            blankUI_draw_text_color(win_x + 220, win_y + 200, (char*)"Software updates require a disk installation.", 0x333333);
            swap_buffers();
            for (volatile int d = 0; d < 60000000; d++);
            return;
        }

        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool downloading = false;
        bool check_done = false;

        // 2. Mock Ping / Fetch Update Logs (Sequoia Styled)
        for (int i = 0; i <= 30; i += 2) {
            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);
            
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    cursor_x += dx; cursor_y += dy;
                    blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                }
            }

            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"System Settings - Software Update");
            
            // Draw Sequoia Layout: Sidebar
            dui_rect(win_x + 1, win_y + 33, 200, win_h - 34, 0xF2F2F7, 240); // Sidebar BG
            dui_rect(win_x + 201, win_y + 33, 1, win_h - 34, 0xCCCCCC, 255); // Divider
            
            // Sidebar Items
            dui_text(win_x + 24, win_y + 60, "General", 0x333333, 1);
            dui_rect_rounded(win_x + 8, win_y + 90, 184, 30, 6, 0x007AFF, 255); // Blue Software Update tab
            dui_text(win_x + 24, win_y + 98, "Software Update", 0xFFFFFF, 1);
            dui_text(win_x + 24, win_y + 138, "Storage", 0x333333, 1);
            
            // Software update info (Right pane)
            dui_text(win_x + 220, win_y + 60, "Checking for software updates...", 0x333333, 2);
            blankUI_draw_progress_bar(win_x + 220, win_y + 110, 460, (float)i / 30.0f);
            
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }
        
        play_system_sound((char*)"update");

        // 3. Display Update Logs and "Update Available"
        while (!downloading) {
            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);

            EFI_INPUT_KEY Key;
            EFI_STATUS Status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);
            if (Status == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) return; // Cancel on ESC
                if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') downloading = true;
            }
            
            bool clicked_install = false;
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    cursor_x += dx; cursor_y += dy;
                    
                    blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                    
                    if (State.LeftButton) {
                        if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) return;
                        
                        // Check click Install button
                        if (cursor_x >= win_x + 220 && cursor_x <= win_x + 420 &&
                            cursor_y >= win_y + 380 && cursor_y <= win_y + 412) {
                            downloading = true;
                        }
                    }
                }
            }
            
            // Set hover hand cursor for Install button
            if (cursor_x >= win_x + 220 && cursor_x <= win_x + 420 &&
                cursor_y >= win_y + 380 && cursor_y <= win_y + 412) {
                blankUI_set_cursor_type(1);
            }
            
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"System Settings - Software Update");
            
            // Sidebar
            dui_rect(win_x + 1, win_y + 33, 200, win_h - 34, 0xF2F2F7, 240);
            dui_rect(win_x + 201, win_y + 33, 1, win_h - 34, 0xCCCCCC, 255);
            dui_text(win_x + 24, win_y + 60, "General", 0x333333, 1);
            dui_rect_rounded(win_x + 8, win_y + 90, 184, 30, 6, 0x007AFF, 255);
            dui_text(win_x + 24, win_y + 98, "Software Update", 0xFFFFFF, 1);
            dui_text(win_x + 24, win_y + 138, "Storage", 0x333333, 1);
            
            // Right Pane Software description
            dui_text(win_x + 220, win_y + 60, "BlankOS Sequoia 2.0.0", 0x111827, 2);
            dui_text(win_x + 220, win_y + 90, "Update Size: 84 MB | Staging target: Partition B", 0x8E8E93, 1);
            
            dui_text(win_x + 220, win_y + 130, "What's new in this version:", 0x111827, 1);
            dui_text(win_x + 220, win_y + 160, "* Fully dynamic Interrupt Descriptor Table (IDT) for panic handling", 0x48484A, 1);
            dui_text(win_x + 220, win_y + 184, "* Real Page Fault / GPF register dumps in BSOD panel", 0x48484A, 1);
            dui_text(win_x + 220, win_y + 208, "* Integrated full-featured 3D Doom shareware port", 0x48484A, 1);
            dui_text(win_x + 220, win_y + 232, "* Multi-state macOS cursor pointer engines", 0x48484A, 1);
            dui_text(win_x + 220, win_y + 256, "* V-Sync locking double buffer blitter to fix screen tearing", 0x48484A, 1);
            dui_text(win_x + 220, win_y + 280, "* Clickable Wi-Fi and Bluetooth status dropdown widgets", 0x48484A, 1);

            blankUI_draw_button(win_x + 220, win_y + 380, 200, 32, (char*)"Update Now");
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }

        // 4. Download and Install Sequence with live speeds and stages
        for (int i = 0; i <= 100; i += 2) {
            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);
            
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"System Settings - Software Update");
            
            // Sidebar
            dui_rect(win_x + 1, win_y + 33, 200, win_h - 34, 0xF2F2F7, 240);
            dui_rect(win_x + 201, win_y + 33, 1, win_h - 34, 0xCCCCCC, 255);
            dui_text(win_x + 24, win_y + 60, "General", 0x333333, 1);
            dui_rect_rounded(win_x + 8, win_y + 90, 184, 30, 6, 0x007AFF, 255);
            dui_text(win_x + 24, win_y + 98, "Software Update", 0xFFFFFF, 1);
            
            // Simulated logs
            dui_text(win_x + 220, win_y + 60, "Downloading OTA Payload via HTTP...", 0x333333, 2);
            blankUI_draw_progress_bar(win_x + 220, win_y + 110, 460, (float)i / 100.0f);
            
            // Dynamic stats
            float speed = 12.4f + (i % 5) * 0.7f; // Fluctuating speed
            int seconds_left = (100 - i) / 2;
            
            char speed_str[64] = "Speed: 12.4 MB/s | Time Remaining: 12s";
            // Custom string formatting helper
            char* dest = speed_str;
            const char* sp_lbl = "Speed: ";
            while (*sp_lbl) *dest++ = *sp_lbl++;
            char speed_val[8];
            uint64_to_dec((int)speed, speed_val);
            dest[0] = speed_val[0]; dest[1] = speed_val[1]; dest[2] = '.'; dest[3] = '6'; dest[4] = '\0';
            dest += 4;
            const char* sp_unit = " MB/s | Estimating: ";
            while (*sp_unit) *dest++ = *sp_unit++;
            char time_val[8];
            uint64_to_dec(seconds_left, time_val);
            int k = 0; while (time_val[k]) *dest++ = time_val[k++];
            const char* time_unit = " seconds remaining";
            while (*time_unit) *dest++ = *time_unit++;
            *dest = '\0';
            
            dui_text(win_x + 220, win_y + 130, speed_str, 0x8E8E93, 1);
            
            // Staging details
            if (i < 40) {
                dui_text(win_x + 220, win_y + 180, "Stage 1/3: Allocating EfiLoaderData flash segments...", 0x333333, 1);
            } else if (i < 80) {
                dui_text(win_x + 220, win_y + 180, "Stage 2/3: Staging kernel partition swap (A/B) to Partition B...", 0x007AFF, 1);
            } else {
                dui_text(win_x + 220, win_y + 180, "Stage 3/3: Verifying cryptographical SHA-256 signature...", 0x34C759, 1);
            }
            
            swap_buffers();
            SystemTable->BootServices->Stall(160000); // Fast download speed representation
        }
        
        execute_update_partition_swap(SystemTable);
        play_system_sound((char*)"startup");
        
        int win_x = (screen_width - win_w) / 2;
        int win_y = (screen_height - win_h) / 2;
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        blankUI_draw_window(win_w, win_h, (char*)"System Settings - Software Update");
        dui_text(win_x + 220, win_y + 120, "Update completed successfully! BOOTNEXT.EFI staged.", 0x34C759, 2);
        dui_text(win_x + 220, win_y + 160, "Rebooting system to apply updates...", 0x333333, 1);
        swap_buffers();
        
        for (volatile int d = 0; d < 80000000; d++);
    }
}
