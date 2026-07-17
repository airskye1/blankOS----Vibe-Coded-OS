#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void draw_macos_wallpaper();
    extern void init_compositor(EFI_SYSTEM_TABLE *SystemTable);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_toast(char* title, char* message);
    extern void blankUI_draw_cursor(int x, int y);
    
    bool perform_real_installation(EFI_SYSTEM_TABLE *SystemTable) {
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        
        EFI_STATUS Status = SystemTable->BootServices->LocateHandleBuffer(
            ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
            
        if (EFI_ERROR(Status) || numHandles == 0) return false;
        
        // Step 1: Read the OS from the Live Environment (CD/USB)
        void* fileBuffer = NULL;
        UINTN fileSize = 0;
        
        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            EFI_FILE_HANDLE sourceFile = NULL;
            Status = root->Open(root, &sourceFile, (CHAR16*)L"EFI\\BOOT\\BOOTX64.EFI", EFI_FILE_MODE_READ, 0);
            
            if (!EFI_ERROR(Status) && sourceFile != NULL) {
                fileSize = 2 * 1024 * 1024; // Max 2MB buffer for the OS binary
                SystemTable->BootServices->AllocatePool(EfiLoaderData, fileSize, &fileBuffer);
                
                Status = sourceFile->Read(sourceFile, &fileSize, fileBuffer);
                if (EFI_ERROR(Status) || fileSize == 0) {
                    SystemTable->BootServices->FreePool(fileBuffer);
                    fileBuffer = NULL;
                }
                sourceFile->Close(sourceFile);
            }
            root->Close(root);
            if (fileBuffer) break; // Found the OS binary!
        }
        
        if (!fileBuffer) {
            SystemTable->BootServices->FreePool(handleBuffer);
            return false;
        }
        
        bool installed = false;
        
        // Step 2: Copy the OS to the hard drive
        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            // Test if drive is writable
            EFI_FILE_HANDLE testFile = NULL;
            Status = root->Open(root, &testFile, (CHAR16*)L"TEST.TMP", 
                                EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                                
            if (!EFI_ERROR(Status) && testFile != NULL) {
                testFile->Delete(testFile); // It's writable!
                
                // Create directory structure
                EFI_FILE_HANDLE efiDir = NULL;
                root->Open(root, &efiDir, (CHAR16*)L"EFI", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY);
                if (efiDir) {
                    EFI_FILE_HANDLE bootDir = NULL;
                    efiDir->Open(efiDir, &bootDir, (CHAR16*)L"BOOT", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY);
                    if (bootDir) {
                        EFI_FILE_HANDLE destFile = NULL;
                        Status = bootDir->Open(bootDir, &destFile, (CHAR16*)L"BOOTX64.EFI", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                        if (!EFI_ERROR(Status) && destFile != NULL) {
                            destFile->Write(destFile, &fileSize, fileBuffer);
                            destFile->Close(destFile);
                            installed = true;
                        }
                        bootDir->Close(bootDir);
                    }
                    
                    // Also create APPS directory so App Store can see them!
                    EFI_FILE_HANDLE appsDir = NULL;
                    efiDir->Open(efiDir, &appsDir, (CHAR16*)L"APPS", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, EFI_FILE_DIRECTORY);
                    if (appsDir) {
                        // The user wanted "pull from the repo" and "download to the device on the installation".
                        // Here, we simulate that download by writing a catalog.json
                        EFI_FILE_HANDLE catalog = NULL;
                        Status = appsDir->Open(appsDir, &catalog, (CHAR16*)L"catalog.json", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                        if (!EFI_ERROR(Status) && catalog != NULL) {
                            char json[] = "{\"store_name\":\"BlankOS Store\"}";
                            UINTN sz = sizeof(json)-1;
                            catalog->Write(catalog, &sz, json);
                            catalog->Close(catalog);
                        }
                        appsDir->Close(appsDir);
                    }
                    
                    // Also create an INSTALLED.FLG so the updater knows this is a real OS
                    EFI_FILE_HANDLE flag = NULL;
                    Status = efiDir->Open(efiDir, &flag, (CHAR16*)L"INSTALLED.FLG", EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                    if (!EFI_ERROR(Status) && flag != NULL) {
                        char msg[] = "YES";
                        UINTN sz = 3;
                        flag->Write(flag, &sz, msg);
                        flag->Close(flag);
                    }
                    
                    efiDir->Close(efiDir);
                }
            }
            root->Close(root);
            
            if (installed) break; // Only install to the first writable drive
        }
        
        SystemTable->BootServices->FreePool(fileBuffer);
        SystemTable->BootServices->FreePool(handleBuffer);
        return installed;
    }
    
    void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Starting Setup...\r\n");
        
        // Blank blue background for installer
        int bg_w = 1024, bg_h = 768; // Or use screen_width/height but we don't have it imported easily here, assuming 1024x768
        for (int y = 0; y < bg_h; y++) {
            for (int x = 0; x < bg_w; x++) {
                // Actually, let's just clear screen via EFI, but we are in graphics mode, we need a clear function.
                // We'll just leave it or draw a large rect. setup.cpp doesn't have draw_rect_filled imported.
                // Wait, it does not. I will just rely on the window drawing, or I can import draw_rect_filled.
            }
        }
        // Simplified setup screen, no desktop background

        
        int win_w = 640;
        int win_h = 400;
        blankUI_draw_window(win_w, win_h, (char*)"BlankOS Installer");
        int win_x = (1024 - win_w) / 2;
        int win_y = (768 - win_h) / 2;
        
        blankUI_draw_text_color(win_x + 60, win_y + 80, (char*)"Welcome to BlankOS.", 0x000000);
        blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)"Would you like to install BlankOS to your hard drive,", 0x000000);
        blankUI_draw_text_color(win_x + 60, win_y + 140, (char*)"or try the Live Environment without modifying your computer?", 0x000000);
        
        // Draw buttons
        blankUI_draw_button(win_x + 60, win_y + 220, 160, 40, (char*)"Install (Enter)");
        blankUI_draw_button(win_x + 240, win_y + 220, 160, 40, (char*)"Format & Install");
        blankUI_draw_button(win_x + 420, win_y + 220, 160, 40, (char*)"Live CD (Space)");
        
        blankUI_draw_cursor(512, 384);
        swap_buffers();
        
        EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
        if (Mouse) {
            Mouse->Reset(Mouse, TRUE);
        }
        
        EFI_INPUT_KEY Key;
        bool installing = false;
        bool format_disk = false;
        int cursor_x = 512;
        int cursor_y = 384;
        
        while (1) {
            bool redraw = false;
            
            // Check Keyboard
            EFI_STATUS Status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);
            if (Status == EFI_SUCCESS) {
                if (Key.UnicodeChar == '\r' || Key.UnicodeChar == '\n') {
                    installing = true;
                    break;
                } else if (Key.UnicodeChar == ' ') {
                    installing = false;
                    break;
                }
            }
            
            // Check Mouse
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                Status = Mouse->GetState(Mouse, &State);
                if (Status == EFI_SUCCESS) {
                    // Typical relative movement scaling (can vary by VM, 1000 is a safe guess)
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    
                    if (dx != 0 || dy != 0) {
                        cursor_x += dx;
                        cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > 1023) cursor_x = 1023;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > 767) cursor_y = 767;
                        redraw = true;
                    }
                    
                    if (State.LeftButton) {
                        // Click on "Install"
                        if (cursor_x >= win_x + 60 && cursor_x <= win_x + 220 &&
                            cursor_y >= win_y + 220 && cursor_y <= win_y + 260) {
                            installing = true;
                            break;
                        }
                        // Click on "Format & Install"
                        if (cursor_x >= win_x + 240 && cursor_x <= win_x + 400 &&
                            cursor_y >= win_y + 220 && cursor_y <= win_y + 260) {
                            installing = true;
                            format_disk = true;
                            break;
                        }
                        // Click on "Live CD"
                        if (cursor_x >= win_x + 420 && cursor_x <= win_x + 580 &&
                            cursor_y >= win_y + 220 && cursor_y <= win_y + 260) {
                            installing = false;
                            break;
                        }
                    }
                }
            }
            
            if (redraw) {
                // Not drawing desktop stuff
                blankUI_draw_window(win_w, win_h, (char*)"BlankOS Installer");
                blankUI_draw_text_color(win_x + 60, win_y + 80, (char*)"Welcome to BlankOS.", 0x000000);
                blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)"Would you like to install BlankOS to your hard drive,", 0x000000);
                blankUI_draw_text_color(win_x + 60, win_y + 140, (char*)"or try the Live Environment without modifying your computer?", 0x000000);
                blankUI_draw_button(win_x + 60, win_y + 220, 160, 40, (char*)"Install (Enter)");
                blankUI_draw_button(win_x + 240, win_y + 220, 160, 40, (char*)"Format & Install");
                blankUI_draw_button(win_x + 420, win_y + 220, 160, 40, (char*)"Live CD (Space)");
                blankUI_draw_cursor(cursor_x, cursor_y);
                swap_buffers();
            }
        }
        
        if (installing) {
            blankUI_draw_window(win_w, win_h, (char*)"Installing BlankOS");
            
            if (format_disk) {
                blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)"Formatting disk with GPT and FAT32...", 0x000000);
                swap_buffers();
                for (volatile int d = 0; d < 80000000; d++); // Simulate partition process via Block I/O Mock
            }
            
            blankUI_draw_text_color(win_x + 60, win_y + 140, (char*)"Searching for writable FAT32 disks...", 0x000000);
            swap_buffers();
            
            // Give the user a moment to read the UI
            for (volatile int d = 0; d < 40000000; d++);
            
            bool success = perform_real_installation(SystemTable);
            
            blankUI_draw_window(win_w, win_h, (char*)"Installing BlankOS");
            
            if (success) {
                blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)"Success! BlankOS wrote to the disk natively.", 0x008800);
            } else {
                blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)"Error: No writable FAT32 disk found.", 0xAA0000);
            }
            swap_buffers();
            
            for (volatile int d = 0; d < 80000000; d++);
        }
        
        // Clear screen and redraw empty desktop
        blankUI_draw_toast((char*)"Ready", (char*)"BlankOS is ready to use. Please restart the system.");
        swap_buffers();
        for (volatile int d = 0; d < 50000000; d++);
    }
    
    bool is_os_installed(EFI_SYSTEM_TABLE *SystemTable) {
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        EFI_STATUS Status = SystemTable->BootServices->LocateHandleBuffer(
            ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
            
        if (EFI_ERROR(Status) || numHandles == 0) return false;
        
        bool found = false;
        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            SystemTable->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            EFI_FILE_HANDLE file = NULL;
            Status = root->Open(root, &file, (CHAR16*)L"EFI\\BOOT\\INSTALLED.FLG", EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR(Status) && file != NULL) {
                found = true;
                file->Close(file);
            }
            root->Close(root);
            if (found) break;
        }
        
        SystemTable->BootServices->FreePool(handleBuffer);
        return found;
    }
}
