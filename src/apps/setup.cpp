#include <efi.h>
#include <efilib.h>

extern "C" {
    extern void init_compositor(void);
    extern void blankUI_draw_topbar(char* app_title);
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text(int x, int y, char* text);
    extern void blankUI_draw_progress_bar(int x, int y, int width, float percentage);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_toast(char* title, char* message);
    
    void launch_setup_screen(EFI_SYSTEM_TABLE *SystemTable) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"\r\n--- BlankOS Out Of Box Experience (OOBE) ---\r\n\r\n");
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ OOBE ] Probing System Hardware...\r\n");
        
        // Render step-by-step graphical progress bar animation
        const char* stages[] = {
            "Probing Hardware...",
            "Formatting NVMe Disk...",
            "Creating Partition Table (GPT)...",
            "Copying src/kernel/kernel.elf...",
            "Extracting blankUI Component Library...",
            "Writing System Registry (blankReg)...",
            "Installing App: blankBrowser.bloe...",
            "Installing App: store.bloe...",
            "Writing Boot Sector...",
            "Installation Complete!"
        };

        for (int step = 0; step < 10; step++) {
            // Re-render frame
            init_compositor();
            
            // Draw desktop shell
            blankUI_draw_topbar((char*)"BlankOS Out Of Box Experience (OOBE)");
            
            // Draw center window
            int win_w = 640;
            int win_h = 400;
            blankUI_draw_window(win_w, win_h, (char*)"BlankOS Setup Installer");
            
            // Window content coordinates relative to screen center
            int win_x = (1024 - win_w) / 2; // Assumed screen size fallback
            int win_y = (768 - win_h) / 2;
            
            blankUI_draw_text(win_x + 40, win_y + 80, (char*)"Welcome to the Vibe-Coded Future.");
            blankUI_draw_text(win_x + 40, win_y + 120, (char*)"Target: /dev/nvme0n1 (25 GB NVMe)");
            
            // Draw current step text
            blankUI_draw_text(win_x + 40, win_y + 200, (char*)stages[step]);
            
            // Draw progress bar
            float pct = (float)(step + 1) / 10.0f;
            blankUI_draw_progress_bar(win_x + 40, win_y + 230, 560, pct);
            
            if (step == 9) {
                blankUI_draw_button(win_x + win_w - 140, win_y + win_h - 60, 100, 35, (char*)"Restart");
                blankUI_draw_toast((char*)"Setup Complete", (char*)"Operating System successfully installed.");
            } else {
                blankUI_draw_button(win_x + win_w - 140, win_y + win_h - 60, 100, 35, (char*)"Next >");
            }
            
            // Simulate delay for file extraction
            for (volatile int d = 0; d < 70000000; d++);
        }
        
        SystemTable->ConOut->OutputString(SystemTable->ConOut, L"[ INSTALL ] BlankOS Installation Complete!\r\n");
    }
}
