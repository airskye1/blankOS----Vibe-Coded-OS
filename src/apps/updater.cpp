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

    void launch_updater(EFI_SYSTEM_TABLE *SystemTable) {
        int win_w = 640;
        int win_h = 420;
        int win_x = (1024 - win_w) / 2;
        int win_y = (768 - win_h) / 2;
        
        // 1. Live OS Detection
        void* temp = NULL;
        UINTN tsz = 0;
        bool is_installed = read_fat32_file(SystemTable, (CHAR16*)L"EFI\\BOOT\\INSTALLED.FLG", &temp, &tsz);
        if (temp) SystemTable->BootServices->FreePool(temp);
        
        if (!is_installed) {
            draw_macos_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"BlankOS Software Update");
            blankUI_draw_text_color(win_x + 40, win_y + 80, (char*)"Update Failed: Live OS Detected.", 0xAA0000);
            blankUI_draw_text_color(win_x + 40, win_y + 110, (char*)"The updater only works on a fully installed OS.", 0x000000);
            swap_buffers();
            for (volatile int d = 0; d < 50000000; d++);
            return;
        }

        // 2. Mock Ping / Fetch Update Logs
        for (int i = 0; i <= 20; i += 2) {
            draw_macos_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"BlankOS Software Update");
            blankUI_draw_text_color(win_x + 40, win_y + 80, (char*)"Checking for updates...", 0x000000);
            blankUI_draw_progress_bar(win_x + 40, win_y + 120, 560, (float)i / 20.0f);
            swap_buffers();
            for (volatile int d = 0; d < 1000000; d++);
        }
        
        play_system_sound((char*)"update");

        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        EFI_INPUT_KEY Key;
        int cursor_x = 512;
        int cursor_y = 384;
        bool downloading = false;
        
        // 3. Display Update Logs and "Update Available"
        while (!downloading) {
            bool redraw = false;
            
            EFI_STATUS Status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);
            if (Status == EFI_SUCCESS) {
                if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') downloading = true;
            }
            
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx != 0 || dy != 0) {
                        cursor_x += dx; cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0; if (cursor_x > 1023) cursor_x = 1023;
                        if (cursor_y < 0) cursor_y = 0; if (cursor_y > 767) cursor_y = 767;
                        redraw = true;
                    }
                    if (State.LeftButton) {
                        // Click Install
                        if (cursor_x >= win_x + 220 && cursor_x <= win_x + 420 &&
                            cursor_y >= win_y + 360 && cursor_y <= win_y + 392) {
                            downloading = true;
                        }
                    }
                }
            }
            
            if (redraw) {
                draw_macos_wallpaper();
                blankUI_draw_menubar();
                blankUI_draw_dock();
                blankUI_draw_window(win_w, win_h, (char*)"BlankOS Software Update");
                
                // Draw icons
                draw_image(SystemTable, (CHAR16*)L"assets\\logo.jpg", win_x + 40, win_y + 60);
                draw_image(SystemTable, (CHAR16*)L"assets\\nano_banana.jpg", win_x + 500, win_y + 60);
                
                blankUI_draw_text_color(win_x + 180, win_y + 80, (char*)"Update Available: Version 1.4.0", 0x0000FF);
                blankUI_draw_text_color(win_x + 180, win_y + 110, (char*)"A new version of BlankOS is ready to install.", 0x666666);
                
                blankUI_draw_text_color(win_x + 40, win_y + 180, (char*)"Update Logs:", 0x000000);
                blankUI_draw_text_color(win_x + 40, win_y + 210, (char*)"* Added Nano Banana and System Icons", 0x333333);
                blankUI_draw_text_color(win_x + 40, win_y + 240, (char*)"* Added PC Speaker Audio Drivers (Startup Chimes)", 0x333333);
                blankUI_draw_text_color(win_x + 40, win_y + 270, (char*)"* Implemented A/B Data Loss Protection (OTA Rollback)", 0x333333);
                blankUI_draw_text_color(win_x + 40, win_y + 300, (char*)"* Added Live OS restrictions to prevent corruptions", 0x333333);

                blankUI_draw_button(win_x + 220, win_y + 360, 200, 32, (char*)"Download & Install");
                blankUI_draw_cursor(cursor_x, cursor_y);
                swap_buffers();
            }
        }
        
        // 4. Download and Install Sequence
        for (int i = 0; i <= 100; i += 1) {
            draw_macos_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();
            blankUI_draw_window(win_w, win_h, (char*)"BlankOS Software Update");
            blankUI_draw_text_color(win_x + 40, win_y + 80, (char*)"Downloading kernel OTA payload...", 0x000000);
            blankUI_draw_progress_bar(win_x + 40, win_y + 120, 560, (float)i / 100.0f);
            swap_buffers();
            for (volatile int d = 0; d < 1000000; d++);
        }
        
        execute_update_partition_swap(SystemTable);
        play_system_sound((char*)"startup");
        
        draw_macos_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        blankUI_draw_window(win_w, win_h, (char*)"BlankOS Software Update");
        blankUI_draw_text_color(win_x + 40, win_y + 120, (char*)"Update completed. BOOTNEXT.EFI staged.", 0x008800);
        blankUI_draw_text_color(win_x + 40, win_y + 160, (char*)"Rebooting system to apply updates...", 0x000000);
        swap_buffers();
        
        for (volatile int d = 0; d < 80000000; d++);
    }
}
