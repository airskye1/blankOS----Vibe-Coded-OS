extern void blankUI_draw_topbar(char* app_title);

extern "C" void launch_blankpad(void) {
    blankUI_draw_topbar((char*)"blankPad");
    
    // 1. Render a large multi-line text input component using blankUI
    // 2. Support basic keyboard shortcuts (Ctrl+C, Ctrl+V, Ctrl+S)
    // 3. Render a clean, distraction-free writing surface
}
