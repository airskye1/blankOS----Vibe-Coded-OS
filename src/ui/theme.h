#ifndef BLANKOS_THEME_H
#define BLANKOS_THEME_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// BlankOS Horizon x Lira Architectural Design Language Tokens
// Obsidian + Aurora palette with tactile two-tone bevels, 1px specular
// hairlines, generous squircle/pill radii, and damped Lira spring motion
// ============================================================================

// Depth Surfaces (0xAARRGGBB or 0x00RRGGBB where alpha is passed explicitly)
#define THEME_BG_VOID          0x06080F
#define THEME_BG_DEEP          0x0B0F1A
#define THEME_SURFACE_BASE     0x111726
#define THEME_SURFACE_CARD     0x182134
#define THEME_SURFACE_ELEVATED 0x202C44
#define THEME_SURFACE_INPUT    0x0D1320
#define THEME_SURFACE_HOVER    0x273552

// Specular Glass Borders & Dividers
#define THEME_BORDER_SUBTLE    0x26324A
#define THEME_BORDER_SPECULAR  0x465B82
#define THEME_BORDER_FOCUS     0x38BDF8

// Typography Colors
#define THEME_TEXT_PRIMARY     0xF8FAFC
#define THEME_TEXT_SECONDARY   0x94A3B8
#define THEME_TEXT_MUTED       0x64748B
#define THEME_TEXT_ON_ACCENT   0x071019

// Aurora Accents & Semantic Tokens
#define THEME_ACCENT_CYAN      0x38BDF8
#define THEME_ACCENT_IRIS      0x818CF8
#define THEME_ACCENT_TEAL      0x2DD4BF
#define THEME_ACCENT_GREEN     0x34D399
#define THEME_ACCENT_AMBER     0xFBBF24
#define THEME_ACCENT_ROSE      0xFB7185
#define THEME_ACCENT_RED       0xEF4444

// Radii & Spacing (Lira Generous Squircle & Pill Geometry)
#define THEME_RADIUS_SM        8
#define THEME_RADIUS_MD        12
#define THEME_RADIUS_LG        16
#define THEME_RADIUS_XL        20
#define THEME_RADIUS_CARD      24
#define THEME_RADIUS_PILL      999

#define THEME_TITLEBAR_H       38
#define THEME_TOPBAR_H         40
#define THEME_DOCK_H           66

// Motion & Easing Helpers (safe integer/float freestanding math)
static inline float horizon_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int horizon_clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float horizon_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float horizon_ease_out_cubic(float t) {
    t = horizon_clampf(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

// Fast-settle Lira quintic-out curve (crisp ~240ms feel, zero bounce)
static inline float horizon_ease_lira(float t) {
    t = horizon_clampf(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    float inv2 = inv * inv;
    return 1.0f - inv2 * inv2;
}

// Emil-design-eng rule: never animate from scale(0); start at 0.94..1.0
static inline float horizon_enter_scale(float anim) {
    float e = horizon_ease_lira(anim);
    return 0.94f + 0.06f * e;
}

static inline int horizon_enter_offset_y(float anim, int travel_px) {
    float e = horizon_ease_lira(anim);
    return (int)((1.0f - e) * (float)travel_px);
}

static inline float horizon_spring_step(float cur, float target, float stiffness) {
    float diff = target - cur;
    if (diff > -0.004f && diff < 0.004f) return target;
    return cur + diff * stiffness;
}

static inline uint32_t horizon_lerp_color(uint32_t c1, uint32_t c2, float t) {
    t = horizon_clampf(t, 0.0f, 1.0f);
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    int r = r1 + (int)((r2 - r1) * t);
    int g = g1 + (int)((g2 - g1) * t);
    int b = b1 + (int)((b2 - b1) * t);
    return ((uint32_t)(r & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}

#endif // BLANKOS_THEME_H
