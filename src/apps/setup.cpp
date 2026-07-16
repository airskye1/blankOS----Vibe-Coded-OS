#include <stdint.h>
#include <stdbool.h>
#include <efi.h>
#include <efilib.h>

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
    
    void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Starting Setup...\r\n");
        
        draw_macos_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        
        int win_w = 640;
        int win_h = 400;
        blankUI_draw_window(win_w, win_h, (char*)"BlankOS Installer");
        int win_x = (1024 - win_w) / 2;
        int win_y = (768 - win_h) / 2;
        
        blankUI_draw_text_color(win_x + 60, win_y + 80, (char*)"Welcome to BlankOS.", 0x000000);
        blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)"Would you like to install BlankOS to your hard drive,", 0x000000);
        blankUI_draw_text_color(win_x + 60, win_y + 140, (char*)"or try the Live Environment without modifying your computer?", 0x000000);
        
        // Draw buttons
        blankUI_draw_button(win_x + 80, win_y + 220, 200, 40, (char*)"Install BlankOS (Enter)");
        blankUI_draw_button(win_x + 360, win_y + 220, 200, 40, (char*)"Try Live CD (Space)");
        
        blankUI_draw_cursor(512, 384);
        swap_buffers();
        
        EFI_INPUT_KEY Key;
        bool installing = false;
        while (1) {
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
        }
        
        if (installing) {
            const char* stages[] = {
                "Formatting /dev/nvme0n1...",
                "Creating EFI System Partition...",
                "Copying kernel.elf...",
                "Writing Boot Sector...",
                "Installation Complete!"
            };
            for (int step = 0; step < 5; step++) {
                draw_macos_wallpaper();
                blankUI_draw_menubar();
                blankUI_draw_dock();
                blankUI_draw_window(win_w, win_h, (char*)"Installing BlankOS");
                
                blankUI_draw_text_color(win_x + 60, win_y + 120, (char*)stages[step], 0x000000);
                swap_buffers();
                // fake progress
                for (volatile int d = 0; d < 80000000; d++);
            }
        }
        
        // Clear screen and redraw empty desktop
        draw_macos_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();
        blankUI_draw_toast((char*)"Ready", (char*)"BlankOS is ready to use.");
        swap_buffers();
    }
}
