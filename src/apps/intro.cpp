#include <stdbool.h>
#include <stddef.h>

extern void blankUI_draw_modal(int width, int height, char* title, char* content);
extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
extern void blankUI_animate_slide_up(void* component);

static int current_step = 0;

extern "C" void launch_introduction_walkthrough(void) {
    blankUI_animate_slide_up(NULL);
    
    switch(current_step) {
        case 0:
            blankUI_draw_modal(800, 600, 
                "Welcome to BlankOS", 
                "Let's take a quick tour of your new operating system!\n\nBlankOS features a fully custom kernel and the beautiful blankUI framework."
            );
            // Draw "Next" button -> advances current_step to 1
            break;
        case 1:
            blankUI_draw_modal(800, 600, 
                "Global Search", 
                "You can press the Super Key (Windows key) at any time to open the Global Search Bar to find files, settings, or .bloe applications."
            );
            break;
        case 2:
            blankUI_draw_modal(800, 600, 
                "Power Management", 
                "Click the battery icon in the topbar to see your laptop battery percentage, or to quickly Sleep, Hibernate, Restart, or Shutdown."
            );
            break;
        case 3:
            blankUI_draw_modal(800, 600, 
                "You're Ready!", 
                "Enjoy the vibe of BlankOS, overseen by airskye."
            );
            // Draw "Finish" button to close modal
            break;
    }
}
