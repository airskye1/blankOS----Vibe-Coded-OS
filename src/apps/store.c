#include <stdbool.h>

extern void blankUI_draw_window(int width, int height, char* title);
extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
extern void os_send_notification(char* title, char* message);

// Network stub
extern char* http_get(char* url);

void launch_app_store(void) {
    blankUI_draw_window(800, 600, "BlankOS App Store");
    
    // The Store queries a curated GitHub repository containing a catalog of .bloe apps
    char* store_url = "https://github.com/airskye1/blankOS-App-Store/catalog.json";
    
    // char* catalog = http_get(store_url);
    // Parse JSON catalog to generate app cards
    
    // Render UI (Stub)
    // Draw App Card 1: "Discord (blankUI Port)"
    // Draw App Card 2: "VS Code (blankC Port)"
    
    // Install Button Handler
    /*
    void install_app() {
        os_send_notification("Installing", "Downloading .bloe binary from GitHub...");
        // Download binary, save to /Apps/ directory in VFS
        os_send_notification("Success", "App installed successfully!");
    }
    */
}
