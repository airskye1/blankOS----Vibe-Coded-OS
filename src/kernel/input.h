#ifndef BLANKOS_INPUT_H
#define BLANKOS_INPUT_H

#include <efi.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Unified Hardware Input State
typedef struct {
    int mx;                  // Current smoothed/clamped cursor X
    int my;                  // Current smoothed/clamped cursor Y
    int prev_mx;             // Previous frame cursor X
    int prev_my;             // Previous frame cursor Y
    int dx;                  // Frame delta X
    int dy;                  // Frame delta Y

    bool left_down;          // Currently held
    bool left_pressed;       // Edge: just pressed this frame
    bool left_released;      // Edge: just released this frame
    bool left_double_click;  // Double-click detected

    bool right_down;         // Right button held
    bool right_pressed;      // Edge: right button just pressed
    bool right_released;     // Edge: right button just released

    int scroll_y;            // Vertical wheel steps this frame (+up, -down)

    bool has_key;            // True if key_char or scan_code is valid
    char key_char;           // Printable ASCII / '\b' / '\n' / '\t'
    uint16_t scan_code;      // UEFI ScanCode (0x01 Up, 0x02 Down, 0x03 Right, 0x04 Left, 0x17 Esc, etc.)
    bool ctrl_held;          // True when Ctrl shift state is active
    bool alt_held;           // True when Alt shift state is active
    bool shift_held;         // True when Shift state is active

    bool has_abs_pointer;    // True if USB tablet / VMware Absolute Pointer active
    bool has_rel_pointer;    // True if PS/2 or USB relative mouse active
    uint64_t frame_ms;       // Monotonic millisecond counter
} BlankInputState;

// Common UEFI ScanCodes for convenience
#define BLANK_SCAN_NULL      0x00
#define BLANK_SCAN_UP        0x01
#define BLANK_SCAN_DOWN      0x02
#define BLANK_SCAN_RIGHT     0x03
#define BLANK_SCAN_LEFT      0x04
#define BLANK_SCAN_HOME      0x05
#define BLANK_SCAN_END       0x06
#define BLANK_SCAN_INSERT    0x07
#define BLANK_SCAN_DELETE    0x08
#define BLANK_SCAN_PGUP      0x09
#define BLANK_SCAN_PGDN      0x0A
#define BLANK_SCAN_F1        0x0B
#define BLANK_SCAN_F2        0x0C
#define BLANK_SCAN_F3        0x0D
#define BLANK_SCAN_F4        0x0E
#define BLANK_SCAN_F5        0x0F
#define BLANK_SCAN_F11       0x15
#define BLANK_SCAN_ESC       0x17

// Initialize pointer protocols (Absolute Pointer, Simple Pointer) and Keyboard Input
void input_init(EFI_SYSTEM_TABLE* st);

// Re-probe pointers & keyboard (call after GOP resolution changes or driver reconnect)
void input_refresh_bounds(int width, int height);

// Poll all active input devices non-blockingly and produce unified BlankInputState
void input_poll(BlankInputState* out);

// Programmatically set cursor position
void input_set_cursor_pos(int x, int y);

// Consume current mouse click/key so lower layers do not also react to it
void input_consume_mouse(BlankInputState* st);
void input_consume_key(BlankInputState* st);

// Diagnostic info strings
const char* input_get_pointer_driver_name(void);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_INPUT_H
