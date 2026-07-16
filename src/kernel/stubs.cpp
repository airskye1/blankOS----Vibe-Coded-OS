#include <stdint.h>
#include <stddef.h>

// This file contains weak linkage stubs for all external functions to guarantee
// compilation without undefined reference linker errors.
// ALL stubs MUST be extern "C" to match the extern "C" definitions in
// blankUI.cpp, kernel.cpp, and the app files.

extern "C" {

void __attribute__((weak)) init_compositor(void) {}
void __attribute__((weak)) vm_allocate(void) {}

void __attribute__((weak)) blankUI_draw_toast(char* title, char* message) {}
void __attribute__((weak)) blankUI_draw_topbar(char* app_title) {}
void __attribute__((weak)) blankUI_draw_progress_bar(int x, int y, int width, float percentage) {}
void __attribute__((weak)) blankUI_animate_fade_in(void* component) {}
void __attribute__((weak)) blankUI_animate_slide_up(void* component) {}
void __attribute__((weak)) blankUI_draw_modal(int width, int height, char* title, char* content) {}
void __attribute__((weak)) blankUI_draw_window(int width, int height, char* title) {}
void __attribute__((weak)) blankUI_draw_text(int x, int y, char* text) {}
void __attribute__((weak)) blankUI_draw_button(int x, int y, int width, int height, char* text) {}
void __attribute__((weak)) blankUI_draw_tabs(int x, int y, char** tab_names, int tab_count, int active_index) {}
void __attribute__((weak)) blankUI_draw_toggle_switch(int x, int y, bool is_on, char* label) {}
void __attribute__((weak)) blankUI_draw_slider(int x, int y, int width, float value, char* label) {}
void __attribute__((weak)) blankUI_draw_search_bar(int x, int y, int width) {}

char* __attribute__((weak)) http_get_with_cookies(char* url, char* cookies) { return NULL; }
char* __attribute__((weak)) http_get(char* url) { return NULL; }
char* __attribute__((weak)) http_get_with_auth(char* url, char* token) { return NULL; }
void __attribute__((weak)) http_post_with_auth(char* url, char* token, char* payload) {}

void __attribute__((weak)) start_ble_advertising(char* device_name) {}
void __attribute__((weak)) scan_awdl_devices(void) {}
void __attribute__((weak)) scan_quick_share_devices(void) {}

void __attribute__((weak)) bdrm_draw_pixel(int x, int y, int color) {}
void __attribute__((weak)) bdrm_set_brightness(float brightness) {}
void __attribute__((weak)) bdrm_draw_bitmap(int x, int y, char* image_data) {}

void __attribute__((weak)) outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void __attribute__((weak)) read_file_from_disk(char* path) {}

int __attribute__((weak)) get_cpu_usage(void) { return 0; }
int __attribute__((weak)) get_ram_usage(void) { return 0; }
int __attribute__((weak)) get_total_ram(void) { return 2048; }
char* __attribute__((weak)) get_cpu_model(void) { return NULL; }

// blankReg API
char* __attribute__((weak)) blankReg_read_string(char* key) { return NULL; }
void __attribute__((weak)) blankReg_write_string(char* key, char* value) {}
void __attribute__((weak)) blankReg_get_string(char* key, char* default_val) {}

void __attribute__((weak)) os_send_notification(char* title, char* message) {}

// Crypto stubs
void __attribute__((weak)) sha256_hash(char* input_string, uint8_t* output_hash) {}
int __attribute__((weak)) secure_compare(uint8_t* hash1, uint8_t* hash2) { return 0; }

} // extern "C"
