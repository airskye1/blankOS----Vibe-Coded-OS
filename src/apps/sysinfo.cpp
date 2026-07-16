#include <stdbool.h>

extern void blankUI_draw_window(int width, int height, char* title);
extern void blankUI_draw_text(int x, int y, char* text);
extern void blankUI_draw_progress_bar(int x, int y, int width, float percentage);

// File I/O stub
extern char* read_file_from_disk(char* path);

extern "C" void launch_sysinfo(void) {
    blankUI_draw_window(600, 600, "System Information");
    
    // Parse version.json dynamically
    // char* version_json_data = read_file_from_disk("/version.json");
    // (Stub: In a real system, we'd parse the JSON here)
    
    blankUI_draw_text(50, 80, "OS: BlankOS (Vibe Coded Edition)");
    blankUI_draw_text(50, 110, "Version: 1.0.0 (Parsed from version.json)");
    blankUI_draw_text(50, 140, "Kernel: Custom Monolithic");
    blankUI_draw_text(50, 170, "UI Framework: blankUI");
    
    // CPU Info
    blankUI_draw_text(50, 220, "Processor: Custom x86_64 Architecture");
    blankUI_draw_text(50, 250, "CPU Usage:");
    blankUI_draw_progress_bar(150, 255, 300, 0.15f); // 15% usage
    
    // RAM Info
    blankUI_draw_text(50, 290, "Memory Usage:");
    blankUI_draw_progress_bar(150, 295, 300, 0.45f); // 45% usage
    
    // Graphics Info
    blankUI_draw_text(50, 340, "Graphics: BlankOS Direct Rendering Manager (BDRM)");
    
    // Update Logs
    blankUI_draw_text(50, 390, "Recent Update Logs (from version.json):");
    blankUI_draw_text(60, 420, "- 1.0.0: Initial release featuring BDRM, blankUI...");
    blankUI_draw_text(60, 450, "- 1.0.1 (Upcoming): Added blankC, App Store, Sysinfo.");
}
