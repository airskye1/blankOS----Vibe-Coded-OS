#include <stdint.h>
#include <stdbool.h>
#include "input.h"
#include <efilib.h>
#include <stddef.h>

extern "C" {
    extern uint32_t screen_width;
    extern uint32_t screen_height;
}

// Local definition of EFI_ABSOLUTE_POINTER_PROTOCOL (UEFI Spec 12.7)
// Prefixed with Blank* to avoid colliding with varying gnu-efi versions
#define BLANK_ABS_PTR_GUID \
    { 0x8D59D32B, 0xC655, 0x4AE9, { 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43 } }

typedef struct {
    UINT64 AbsoluteMinX;
    UINT64 AbsoluteMinY;
    UINT64 AbsoluteMinZ;
    UINT64 AbsoluteMaxX;
    UINT64 AbsoluteMaxY;
    UINT64 AbsoluteMaxZ;
    UINT32 Attributes;
} BlankEfiAbsPtrMode;

typedef struct {
    UINT64 CurrentX;
    UINT64 CurrentY;
    UINT64 CurrentZ;
    UINT32 ActiveButtons;
} BlankEfiAbsPtrState;

struct _BlankEfiAbsPtr;
typedef EFI_STATUS (EFIAPI *BlankAbsPtrReset)(struct _BlankEfiAbsPtr* This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *BlankAbsPtrGetState)(struct _BlankEfiAbsPtr* This, BlankEfiAbsPtrState* State);

typedef struct _BlankEfiAbsPtr {
    BlankAbsPtrReset Reset;
    BlankAbsPtrGetState GetState;
    EFI_EVENT WaitForInput;
    BlankEfiAbsPtrMode* Mode;
} BlankEfiAbsPtr;

// Internal state
static EFI_SYSTEM_TABLE* g_input_st = NULL;
static EFI_SIMPLE_POINTER_PROTOCOL* g_rel_ptrs[8];
static int g_rel_count = 0;
static BlankEfiAbsPtr* g_abs_ptrs[8];
static int g_abs_count = 0;

static int g_bounds_w = 1024;
static int g_bounds_h = 768;

// Sub-pixel accumulators for relative mice so micro-movements never round to 0
static int64_t g_acc_x = 0;
static int64_t g_acc_y = 0;
static int64_t g_acc_z = 0;

static int g_cur_x = 512;
static int g_cur_y = 384;
static uint64_t g_last_abs_x = 0;
static uint64_t g_last_abs_y = 0;
static bool g_abs_has_prev = false;

static bool g_prev_left = false;
static bool g_prev_right = false;
static uint64_t g_frame_ms = 0;
static uint64_t g_last_click_ms = 0;
static int g_last_click_x = -100;
static int g_last_click_y = -100;

static char g_driver_name[64] = "PS/2 & UEFI Pointer";

static void copy_str(char* dst, const char* src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void probe_pointers(void) {
    g_rel_count = 0;
    g_abs_count = 0;
    if (!g_input_st || !g_input_st->BootServices) return;

    // 1. Probe EFI_ABSOLUTE_POINTER_PROTOCOL (VMware/QEMU USB Tablet)
    EFI_GUID abs_guid = BLANK_ABS_PTR_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count = 0;
    if (g_input_st->BootServices->LocateHandleBuffer(ByProtocol, &abs_guid, NULL, &count, &handles) == EFI_SUCCESS && handles) {
        for (UINTN i = 0; i < count && g_abs_count < 8; i++) {
            BlankEfiAbsPtr* ptr = NULL;
            if (g_input_st->BootServices->HandleProtocol(handles[i], &abs_guid, (VOID**)&ptr) == EFI_SUCCESS && ptr && ptr->Mode) {
                if (ptr->Mode->AbsoluteMaxX > ptr->Mode->AbsoluteMinX &&
                    ptr->Mode->AbsoluteMaxY > ptr->Mode->AbsoluteMinY) {
                    ptr->Reset(ptr, FALSE);
                    g_abs_ptrs[g_abs_count++] = ptr;
                }
            }
        }
        g_input_st->BootServices->FreePool(handles);
    }

    // 2. Probe all EFI_SIMPLE_POINTER_PROTOCOL handles (USB/PS2 Relative Mouse)
    EFI_GUID rel_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    handles = NULL;
    count = 0;
    if (g_input_st->BootServices->LocateHandleBuffer(ByProtocol, &rel_guid, NULL, &count, &handles) == EFI_SUCCESS && handles) {
        for (UINTN i = 0; i < count && g_rel_count < 8; i++) {
            EFI_SIMPLE_POINTER_PROTOCOL* sptr = NULL;
            if (g_input_st->BootServices->HandleProtocol(handles[i], &rel_guid, (VOID**)&sptr) == EFI_SUCCESS && sptr) {
                sptr->Reset(sptr, FALSE);
                g_rel_ptrs[g_rel_count++] = sptr;
            }
        }
        g_input_st->BootServices->FreePool(handles);
    }

    if (g_abs_count > 0 && g_rel_count > 0) {
        copy_str(g_driver_name, "USB Tablet + Relative HID", 64);
    } else if (g_abs_count > 0) {
        copy_str(g_driver_name, "UEFI Absolute Tablet", 64);
    } else if (g_rel_count > 0) {
        copy_str(g_driver_name, "UEFI Simple Pointer", 64);
    } else {
        copy_str(g_driver_name, "Keyboard Fallback Cursor", 64);
    }
}

extern "C" {

void input_init(EFI_SYSTEM_TABLE* st) {
    g_input_st = st;
    g_bounds_w = (screen_width > 0) ? (int)screen_width : 1024;
    g_bounds_h = (screen_height > 0) ? (int)screen_height : 768;
    g_cur_x = g_bounds_w / 2;
    g_cur_y = g_bounds_h / 2;
    g_prev_left = false;
    g_prev_right = false;
    g_abs_has_prev = false;
    g_acc_x = 0;
    g_acc_y = 0;
    g_acc_z = 0;
    probe_pointers();
}

void input_refresh_bounds(int width, int height) {
    if (width > 0) g_bounds_w = width;
    if (height > 0) g_bounds_h = height;
    if (g_cur_x >= g_bounds_w) g_cur_x = g_bounds_w - 1;
    if (g_cur_y >= g_bounds_h) g_cur_y = g_bounds_h - 1;
    if (g_cur_x < 0) g_cur_x = 0;
    if (g_cur_y < 0) g_cur_y = 0;
}

void input_set_cursor_pos(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_bounds_w) x = g_bounds_w - 1;
    if (y >= g_bounds_h) y = g_bounds_h - 1;
    g_cur_x = x;
    g_cur_y = y;
}

void input_poll(BlankInputState* out) {
    if (!out) return;

    // Refresh bounds lazily from compositor globals if resolution changed
    if (screen_width > 0 && (int)screen_width != g_bounds_w) {
        input_refresh_bounds((int)screen_width, (int)screen_height);
    }

    g_frame_ms += 16; // ~60Hz cadence approximation when EFI timer isn't sampled
    if (g_input_st && g_input_st->RuntimeServices) {
        // Keep monotonic increment deterministic and jitter-free for double-click checks
    }

    int prev_x = g_cur_x;
    int prev_y = g_cur_y;
    bool left_now = g_prev_left;
    bool right_now = g_prev_right;
    int scroll_y = 0;

    // 1. Poll Absolute Pointers (USB Tablet)
    bool abs_updated = false;
    for (int i = 0; i < g_abs_count; i++) {
        BlankEfiAbsPtrState st_abs;
        if (g_abs_ptrs[i] && g_abs_ptrs[i]->GetState(g_abs_ptrs[i], &st_abs) == EFI_SUCCESS) {
            UINT64 span_x = g_abs_ptrs[i]->Mode->AbsoluteMaxX - g_abs_ptrs[i]->Mode->AbsoluteMinX;
            UINT64 span_y = g_abs_ptrs[i]->Mode->AbsoluteMaxY - g_abs_ptrs[i]->Mode->AbsoluteMinY;
            if (span_x > 0 && span_y > 0) {
                UINT64 rel_x = (st_abs.CurrentX >= g_abs_ptrs[i]->Mode->AbsoluteMinX)
                    ? (st_abs.CurrentX - g_abs_ptrs[i]->Mode->AbsoluteMinX) : 0;
                UINT64 rel_y = (st_abs.CurrentY >= g_abs_ptrs[i]->Mode->AbsoluteMinY)
                    ? (st_abs.CurrentY - g_abs_ptrs[i]->Mode->AbsoluteMinY) : 0;
                int nx = (int)((rel_x * (UINT64)(g_bounds_w - 1)) / span_x);
                int ny = (int)((rel_y * (UINT64)(g_bounds_h - 1)) / span_y);
                if (!g_abs_has_prev || st_abs.CurrentX != g_last_abs_x || st_abs.CurrentY != g_last_abs_y) {
                    g_cur_x = nx;
                    g_cur_y = ny;
                    g_last_abs_x = st_abs.CurrentX;
                    g_last_abs_y = st_abs.CurrentY;
                    g_abs_has_prev = true;
                }
                left_now = (st_abs.ActiveButtons & 0x01) != 0;
                right_now = (st_abs.ActiveButtons & 0x02) != 0;
                abs_updated = true;
            }
        }
    }

    // 2. Poll Relative Pointers with ResolutionX/Y Sub-pixel Accumulator
    for (int i = 0; i < g_rel_count; i++) {
        EFI_SIMPLE_POINTER_STATE st_rel;
        EFI_SIMPLE_POINTER_PROTOCOL* ptr = g_rel_ptrs[i];
        if (!ptr) continue;
        // Drain up to 4 queued events so fast swipes stay responsive
        for (int tries = 0; tries < 4; tries++) {
            if (ptr->GetState(ptr, &st_rel) != EFI_SUCCESS) break;

            int64_t rx = (ptr->Mode && ptr->Mode->ResolutionX > 0) ? (int64_t)ptr->Mode->ResolutionX : 1;
            int64_t ry = (ptr->Mode && ptr->Mode->ResolutionY > 0) ? (int64_t)ptr->Mode->ResolutionY : 1;
            int64_t rz = (ptr->Mode && ptr->Mode->ResolutionZ > 0) ? (int64_t)ptr->Mode->ResolutionZ : 1;

            // Convert counts to smooth pixels (~22 px per Resolution unit)
            g_acc_x += (int64_t)st_rel.RelativeMovementX * 22;
            g_acc_y += (int64_t)st_rel.RelativeMovementY * 22;
            g_acc_z += (int64_t)st_rel.RelativeMovementZ;

            int dx_px = (int)(g_acc_x / rx);
            int dy_px = (int)(g_acc_y / ry);
            g_acc_x -= (int64_t)dx_px * rx;
            g_acc_y -= (int64_t)dy_px * ry;

            // Fallback scaling if firmware reports tiny raw units with huge Resolution
            if (dx_px == 0 && st_rel.RelativeMovementX != 0 && rx > 256) {
                dx_px = (st_rel.RelativeMovementX > 0) ? 1 : -1;
            }
            if (dy_px == 0 && st_rel.RelativeMovementY != 0 && ry > 256) {
                dy_px = (st_rel.RelativeMovementY > 0) ? 1 : -1;
            }

            if (dx_px != 0 || dy_px != 0) {
                g_cur_x += dx_px;
                g_cur_y += dy_px;
            }

            int dz = (int)(g_acc_z / rz);
            if (dz != 0) {
                scroll_y += dz;
                g_acc_z -= (int64_t)dz * rz;
            } else if (st_rel.RelativeMovementZ != 0) {
                scroll_y += (st_rel.RelativeMovementZ > 0) ? 1 : -1;
            }

            if (!abs_updated) {
                left_now = (st_rel.LeftButton != 0);
                right_now = (st_rel.RightButton != 0);
            } else {
                if (st_rel.LeftButton) left_now = true;
                if (st_rel.RightButton) right_now = true;
            }
        }
    }

    // Clamp cursor inside screen bounds
    if (g_cur_x < 0) g_cur_x = 0;
    if (g_cur_y < 0) g_cur_y = 0;
    if (g_cur_x >= g_bounds_w) g_cur_x = g_bounds_w - 1;
    if (g_cur_y >= g_bounds_h) g_cur_y = g_bounds_h - 1;

    // 3. Poll Keyboard Input
    bool has_key = false;
    char key_char = 0;
    uint16_t scan_code = 0;
    bool ctrl_held = false;
    bool alt_held = false;
    bool shift_held = false;

    if (g_input_st && g_input_st->ConIn) {
        EFI_INPUT_KEY k;
        if (g_input_st->ConIn->ReadKeyStroke(g_input_st->ConIn, &k) == EFI_SUCCESS) {
            has_key = true;
            scan_code = k.ScanCode;
            if (k.UnicodeChar == '\r') key_char = '\n';
            else if (k.UnicodeChar >= 1 && k.UnicodeChar <= 26 && k.UnicodeChar != '\b' && k.UnicodeChar != '\t' && k.UnicodeChar != '\n') {
                // Control+letter chord (e.g. Ctrl+K = 11, Ctrl+L = 12, Ctrl+W = 23)
                ctrl_held = true;
                key_char = (char)('a' + (k.UnicodeChar - 1));
            } else if (k.UnicodeChar > 0 && k.UnicodeChar < 128) {
                key_char = (char)k.UnicodeChar;
                if (key_char >= 'A' && key_char <= 'Z') shift_held = true;
            }
        }
    }

    bool left_pressed = left_now && !g_prev_left;
    bool left_released = !left_now && g_prev_left;
    bool right_pressed = right_now && !g_prev_right;
    bool right_released = !right_now && g_prev_right;

    bool double_click = false;
    if (left_pressed) {
        int dcx = g_cur_x - g_last_click_x;
        int dcy = g_cur_y - g_last_click_y;
        if (dcx < 0) dcx = -dcx;
        if (dcy < 0) dcy = -dcy;
        if ((g_frame_ms - g_last_click_ms) <= 420 && dcx <= 8 && dcy <= 8) {
            double_click = true;
            g_last_click_ms = 0;
        } else {
            g_last_click_ms = g_frame_ms;
            g_last_click_x = g_cur_x;
            g_last_click_y = g_cur_y;
        }
    }

    g_prev_left = left_now;
    g_prev_right = right_now;

    out->mx = g_cur_x;
    out->my = g_cur_y;
    out->prev_mx = prev_x;
    out->prev_my = prev_y;
    out->dx = g_cur_x - prev_x;
    out->dy = g_cur_y - prev_y;
    out->left_down = left_now;
    out->left_pressed = left_pressed;
    out->left_released = left_released;
    out->left_double_click = double_click;
    out->right_down = right_now;
    out->right_pressed = right_pressed;
    out->right_released = right_released;
    out->scroll_y = scroll_y;
    out->has_key = has_key;
    out->key_char = key_char;
    out->scan_code = scan_code;
    out->ctrl_held = ctrl_held;
    out->alt_held = alt_held;
    out->shift_held = shift_held;
    out->has_abs_pointer = (g_abs_count > 0);
    out->has_rel_pointer = (g_rel_count > 0);
    out->frame_ms = g_frame_ms;
}

void input_consume_mouse(BlankInputState* st) {
    if (!st) return;
    st->left_pressed = false;
    st->left_double_click = false;
    st->right_pressed = false;
    st->scroll_y = 0;
}

void input_consume_key(BlankInputState* st) {
    if (!st) return;
    st->has_key = false;
    st->key_char = 0;
    st->scan_code = 0;
}

const char* input_get_pointer_driver_name(void) {
    return g_driver_name;
}

} // extern "C"
