#include <stdbool.h>
extern void blankUI_draw_topbar(char* app_title);
extern void blankUI_draw_tabs(int x, int y, char** tab_names, int tab_count, int active_index);
extern void blankUI_draw_toggle_switch(int x, int y, bool is_on, char* label);
extern void blankUI_draw_slider(int x, int y, int width, float value, char* label);

// Kernel stubs
extern void set_master_volume(float volume);
extern float get_master_volume(void);
// BDRM stub for brightness (adjusts screen backlight via ACPI/GPU)
extern void bdrm_set_brightness(float brightness);

void launch_settings(void) {
    blankUI_draw_topbar("Settings");
    
    char* tabs[] = {"Network & Bluetooth", "Sound & Display", "Appearance", "Accounts", "Notifications"};
    blankUI_draw_tabs(50, 50, tabs, 5, 1); // Rendering Sound & Display tab
    
    // Display Settings
    blankUI_draw_slider(100, 150, 400, 0.80f, "Screen Brightness");
    // If slider moves -> bdrm_set_brightness(new_value);
    
    // Sound Settings
    blankUI_draw_slider(100, 220, 400, get_master_volume(), "Master Volume");
    // If slider moves -> set_master_volume(new_value);
    
    // Network Settings (Mocking Tab 0)
    // blankUI_draw_toggle_switch(100, 150, true, "Enable Wi-Fi");
    // blankUI_draw_toggle_switch(100, 200, true, "Enable Bluetooth (BLE 5.0)");
}
