#ifndef STORE_KIT_H
#define STORE_KIT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>
#include "../../src/kernel/os_api.h"

// Horizon v3.1 Lira Obsidian Palette
#define SK_BG_DEEP      0x0B0F1A
#define SK_SURFACE_BASE 0x101624
#define SK_SURFACE_CARD 0x161E31
#define SK_SURFACE_ELEV 0x1D2842
#define SK_SURFACE_INP  0x0C111E
#define SK_BORDER_SUB   0x25314B
#define SK_BORDER_SPEC  0x36476B

#define SK_TEXT_PRI     0xF8FAFC
#define SK_TEXT_SEC     0x94A3B8
#define SK_TEXT_MUT     0x64748B
#define SK_TEXT_DARK    0x07101C

#define SK_CYAN         0x22D3EE
#define SK_TEAL         0x2DD4BF
#define SK_GREEN        0x34D399
#define SK_IRIS         0x818CF8
#define SK_AMBER        0xFBBF24
#define SK_ROSE         0xFB7185

// Standalone EFI Absolute Pointer definition (avoids header mismatch)
typedef struct {
    uint64_t AbsoluteMinX, AbsoluteMinY, AbsoluteMinZ;
    uint64_t AbsoluteMaxX, AbsoluteMaxY, AbsoluteMaxZ;
    uint32_t Attributes;
} SkAbsMode;

typedef struct {
    uint64_t CurrentX, CurrentY, CurrentZ;
    uint32_t ActiveButtons;
} SkAbsState;

typedef struct _SkAbsPtr {
    EFI_STATUS (EFIAPI *Reset)(struct _SkAbsPtr* This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *GetState)(struct _SkAbsPtr* This, SkAbsState* State);
    EFI_EVENT WaitForInput;
    SkAbsMode* Mode;
} SkAbsPtr;

typedef struct {
    int sw, sh;
    int mx, my;
    bool left_down;
    bool prev_down;
    bool left_pressed;
    EFI_SIMPLE_POINTER_PROTOCOL* rel_mouse;
    SkAbsPtr* abs_mouse;
} SkInputCtx;

static inline bool sk_str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static inline int sk_slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static inline void sk_scpy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline void sk_scat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int i = sk_slen(dst);
    int j = 0;
    while (src[j] && i < max_len - 1) dst[i++] = src[j++];
    dst[i] = '\0';
}

static inline void sk_int_to_str(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int p = 0;
    if (val < 0) { val = -val; buf[0] = '-'; buf++; }
    while (val > 0 && p < 14) { tmp[p++] = (char)('0' + (val % 10)); val /= 10; }
    int o = 0;
    while (p > 0) buf[o++] = tmp[--p];
    buf[o] = '\0';
}

static inline bool sk_pt_in(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static inline void sk_init_ctx(OS_API* api, SkInputCtx* ctx) {
    ctx->sw = 1024;
    ctx->sh = 768;
    if (api && api->SystemTable && api->SystemTable->BootServices) {
        EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a} };
        EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
        if (!EFI_ERROR(api->SystemTable->BootServices->LocateProtocol(&gop_guid, NULL, (void**)&gop)) &&
            gop && gop->Mode && gop->Mode->Info) {
            if (gop->Mode->Info->HorizontalResolution >= 640) ctx->sw = (int)gop->Mode->Info->HorizontalResolution;
            if (gop->Mode->Info->VerticalResolution >= 480)   ctx->sh = (int)gop->Mode->Info->VerticalResolution;
        }

        EFI_GUID abs_guid = { 0x8D59D32B, 0xC655, 0x4AE9, {0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43} };
        ctx->abs_mouse = NULL;
        api->SystemTable->BootServices->LocateProtocol(&abs_guid, NULL, (void**)&ctx->abs_mouse);
        if (ctx->abs_mouse && ctx->abs_mouse->Reset) ctx->abs_mouse->Reset(ctx->abs_mouse, FALSE);

        EFI_GUID rel_guid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        ctx->rel_mouse = NULL;
        api->SystemTable->BootServices->LocateProtocol(&rel_guid, NULL, (void**)&ctx->rel_mouse);
        if (ctx->rel_mouse && ctx->rel_mouse->Reset) ctx->rel_mouse->Reset(ctx->rel_mouse, FALSE);
    }
    ctx->mx = ctx->sw / 2;
    ctx->my = ctx->sh / 2;
    ctx->left_down = false;
    ctx->prev_down = false;
    ctx->left_pressed = false;
}

static inline void sk_poll_mouse(SkInputCtx* ctx) {
    ctx->prev_down = ctx->left_down;
    bool btn_now = ctx->left_down;
    bool got_event = false;

    if (ctx->abs_mouse && ctx->abs_mouse->Mode) {
        SkAbsState ast;
        if (!EFI_ERROR(ctx->abs_mouse->GetState(ctx->abs_mouse, &ast))) {
            uint64_t rx = ctx->abs_mouse->Mode->AbsoluteMaxX - ctx->abs_mouse->Mode->AbsoluteMinX;
            uint64_t ry = ctx->abs_mouse->Mode->AbsoluteMaxY - ctx->abs_mouse->Mode->AbsoluteMinY;
            if (rx > 0 && ry > 0) {
                ctx->mx = (int)(((ast.CurrentX - ctx->abs_mouse->Mode->AbsoluteMinX) * (uint64_t)(ctx->sw - 1)) / rx);
                ctx->my = (int)(((ast.CurrentY - ctx->abs_mouse->Mode->AbsoluteMinY) * (uint64_t)(ctx->sh - 1)) / ry);
            }
            btn_now = (ast.ActiveButtons & 1U) != 0;
            got_event = true;
        }
    }

    if (ctx->rel_mouse) {
        EFI_SIMPLE_POINTER_STATE mst;
        if (!EFI_ERROR(ctx->rel_mouse->GetState(ctx->rel_mouse, &mst))) {
            int64_t res_x = (ctx->rel_mouse->Mode && ctx->rel_mouse->Mode->ResolutionX > 0) ? (int64_t)ctx->rel_mouse->Mode->ResolutionX : 1;
            int64_t res_y = (ctx->rel_mouse->Mode && ctx->rel_mouse->Mode->ResolutionY > 0) ? (int64_t)ctx->rel_mouse->Mode->ResolutionY : 1;
            int dx = (res_x > 16) ? (int)((mst.RelativeMovementX * 48) / res_x) : (mst.RelativeMovementX * 2);
            int dy = (res_y > 16) ? (int)((mst.RelativeMovementY * 48) / res_y) : (mst.RelativeMovementY * 2);
            if (!got_event) {
                ctx->mx += dx;
                ctx->my += dy;
                btn_now = (mst.LeftButton != 0);
            } else if (mst.LeftButton) {
                btn_now = true;
            }
        }
    }

    if (ctx->mx < 0) ctx->mx = 0;
    if (ctx->mx >= ctx->sw) ctx->mx = ctx->sw - 1;
    if (ctx->my < 0) ctx->my = 0;
    if (ctx->my >= ctx->sh) ctx->my = ctx->sh - 1;

    ctx->left_down = btn_now;
    ctx->left_pressed = (ctx->left_down && !ctx->prev_down);
}

static inline void sk_bento_card(OS_API* api, int x, int y, int w, int h, int rad, uint32_t bg, bool active, uint32_t accent) {
    api->draw_rect_rounded(x, y, w, h, rad, bg, 238);
    api->draw_rect_outline(x, y, w, h, active ? accent : SK_BORDER_SUB, 1);
}

static inline void sk_pill(OS_API* api, int x, int y, int w, int h, const char* txt, uint32_t fg, uint32_t bg) {
    api->draw_rect_rounded(x, y, w, h, h / 2, bg, 235);
    int tw = api->get_text_width(txt, 1);
    int th = api->get_text_height(1);
    api->draw_text(x + (w - tw) / 2, y + (h - th) / 2, txt, fg, 1);
}

static inline bool sk_btn(OS_API* api, int x, int y, int w, int h, const char* txt,
                          uint32_t accent, bool primary, const SkInputCtx* in) {
    bool hov = sk_pt_in(in->mx, in->my, x, y, w, h);
    int y_off = (hov && in->left_down) ? 1 : 0;
    uint32_t bg = primary ? accent : (hov ? SK_SURFACE_ELEV : SK_SURFACE_CARD);
    uint32_t fg = primary ? SK_TEXT_DARK : SK_TEXT_PRI;
    api->draw_rect_rounded(x, y + y_off, w, h, 10, bg, 245);
    api->draw_rect_outline(x, y + y_off, w, h, hov ? accent : SK_BORDER_SPEC, 1);
    int tw = api->get_text_width(txt, 1);
    int th = api->get_text_height(1);
    api->draw_text(x + (w - tw) / 2, y + y_off + (h - th) / 2, txt, fg, 1);
    if (hov && in->left_pressed) {
        api->play_sound("click");
        return true;
    }
    return false;
}

static inline void sk_progress_bar(OS_API* api, int x, int y, int w, int h, float frac, uint32_t fill) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    api->draw_rect_rounded(x, y, w, h, h / 2, SK_SURFACE_INP, 245);
    int fw = (int)((float)w * frac);
    if (fw >= h) {
        api->draw_rect_rounded(x, y, fw, h, h / 2, fill, 255);
    }
}

static inline void sk_dot_grid(OS_API* api, int x, int y, int w, int h, int step, uint32_t col) {
    for (int gy = y + 8; gy < y + h - 8; gy += step) {
        for (int gx = x + 8; gx < x + w - 8; gx += step) {
            api->draw_rect(gx, gy, 2, 2, col, 32);
        }
    }
}

static inline bool sk_begin_app_frame(OS_API* api, SkInputCtx* ctx, int win_x, int win_y, int win_w, int win_h,
                                      const char* title, const char* badge, uint32_t accent) {
    api->draw_wallpaper();
    api->draw_menubar();
    api->draw_dock();

    api->draw_window(win_x, win_y, win_w, win_h, title);
    if (badge && badge[0]) {
        sk_pill(api, win_x + win_w - 134, win_y + 6, 118, 22, badge, accent, SK_SURFACE_CARD);
    }

    // Close button on top-left red traffic dot
    bool close_hov = sk_pt_in(ctx->mx, ctx->my, win_x + 10, win_y + 10, 16, 16);
    if (close_hov) {
        api->draw_circle(win_x + 18, win_y + 18, 7, SK_ROSE, 255);
        api->draw_text(win_x + 15, win_y + 11, "x", SK_TEXT_DARK, 1);
        if (ctx->left_pressed) {
            api->play_sound("close");
            return true;
        }
    }
    return false;
}

static inline void sk_end_app_frame(OS_API* api, const SkInputCtx* ctx) {
    api->draw_cursor(ctx->mx, ctx->my);
    api->swap_buffers();
    if (api && api->SystemTable && api->SystemTable->BootServices) {
        api->SystemTable->BootServices->Stall(16000);
    }
}

#endif // STORE_KIT_H
