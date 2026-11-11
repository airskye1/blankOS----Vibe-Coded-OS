#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "blankDUI.h"
#include "theme.h"
#include "../stb_truetype.h"

extern "C" {

extern EFI_SYSTEM_TABLE* gSystemTable;
extern void* uefi_malloc(UINTN size);
extern void uefi_free(void* ptr);
extern void* uefi_realloc(void* ptr, UINTN new_size);
extern UINTN uefi_strlen(const char* s);
extern void* uefi_memcpy(void* dst, const void* src, UINTN n);
extern void* uefi_memset(void* dst, int val, UINTN n);

extern unsigned char* stbi_load_from_memory(unsigned char const* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
extern void stbi_image_free(void* retval_from_stbi_load);

extern void compositor_fill_span(int x0, int x1, int y, uint32_t color, uint8_t alpha);
extern const uint8_t font8x8_basic[95][8];

int g_wallpaper_theme = 0;

// ============================================================================
// Internal Math & Color Helpers (Freestanding, No Libc)
// ============================================================================

static inline int imin(int a, int b) { return (a < b) ? a : b; }
static inline int imax(int a, int b) { return (a > b) ? a : b; }
static inline float fmin_val(float a, float b) { return (a < b) ? a : b; }
static inline float fmax_val(float a, float b) { return (a > b) ? a : b; }
static inline float fabs_val(float x) { return (x < 0.0f) ? -x : x; }

static inline float dui_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u = 0x1FBD1DF5U + (v.u >> 1);
    float y = v.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

// Clockwise angle in [0.0, 1.0) starting at 12 o'clock (0, -1)
static inline float dui_atan2_cw01(float dx, float dy) {
    float vx = dx;
    float vy = -dy;
    float ax = fabs_val(vx);
    float ay = fabs_val(vy);
    float mx = fmax_val(ax, ay);
    if (mx <= 1e-6f) return 0.0f;
    float mn = fmin_val(ax, ay);
    float a = mn / mx;
    float rad = 0.78539816f * a - a * (a - 1.0f) * (0.2447f + 0.0663f * a);
    float ang = (ax < ay) ? rad : (1.57079633f - rad);
    if (vx >= 0.0f && vy >= 0.0f) {
        // Quadrant 1
    } else if (vx >= 0.0f && vy < 0.0f) {
        ang = 3.14159265f - ang;
    } else if (vx < 0.0f && vy < 0.0f) {
        ang = 3.14159265f + ang;
    } else {
        ang = 6.28318531f - ang;
    }
    float turns = ang * 0.159154943f;
    if (turns < 0.0f) turns = 0.0f;
    if (turns >= 1.0f) turns -= 1.0f;
    return turns;
}

static inline uint32_t blend_pixel(uint32_t bg, uint32_t fg, uint8_t alpha) {
    uint32_t inv = 255U - (uint32_t)alpha;
    uint32_t rb = (fg & 0x00FF00FFU) * (uint32_t)alpha + (bg & 0x00FF00FFU) * inv + 0x00800080U;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFU)) >> 8) & 0x00FF00FFU;

    uint32_t g = (fg & 0x0000FF00U) * (uint32_t)alpha + (bg & 0x0000FF00U) * inv + 0x00008000U;
    g = ((g + ((g >> 8) & 0x0000FF00U)) >> 8) & 0x0000FF00U;

    return rb | g;
}

static inline uint8_t mul_alpha(uint8_t a, float cov) {
    if (cov <= 0.0f) return 0;
    if (cov >= 1.0f) return a;
    int v = (int)((float)a * cov + 0.5f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

// ============================================================================
// Memory & Color Utility Functions (Legacy + Internal)
// ============================================================================

void dui_memset32(uint32_t* dst, uint32_t val, int count) {
    if (!dst || count <= 0) return;
    for (int i = 0; i < count; i++) dst[i] = val;
}

void dui_memcpy32(uint32_t* dst, const uint32_t* src, int count) {
    if (!dst || !src || count <= 0) return;
    for (int i = 0; i < count; i++) dst[i] = src[i];
}

uint32_t dui_color_lerp(uint32_t a, uint32_t b, float t) {
    return horizon_lerp_color(a, b, t);
}

uint32_t dui_color_darken(uint32_t color, float amount) {
    return horizon_lerp_color(color, 0x000000, amount);
}

uint32_t dui_color_lighten(uint32_t color, float amount) {
    return horizon_lerp_color(color, 0xFFFFFF, amount);
}

uint32_t dui_color_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) return fg & 0x00FFFFFFU;
    if (alpha == 0) return bg & 0x00FFFFFFU;
    return blend_pixel(bg & 0x00FFFFFFU, fg & 0x00FFFFFFU, alpha);
}

// ============================================================================
// UEFI File Reader Helper (LoadedImage DeviceHandle -> All FS Handles)
// ============================================================================

static EFI_SYSTEM_TABLE* g_dui_st = NULL;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE DeviceHandle;
} DuiLoadedImagePrefix;

static uint8_t* read_file_from_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs, const CHAR16* wpath, UINTN* out_size) {
    if (!fs || !wpath) return NULL;
    EFI_FILE_HANDLE root = NULL;
    if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) return NULL;

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS st = root->Open(root, &file, (CHAR16*)wpath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !file) {
        if (wpath[0] == L'\\') {
            st = root->Open(root, &file, (CHAR16*)(wpath + 1), EFI_FILE_MODE_READ, 0);
        }
    }
    if (EFI_ERROR(st) || !file) {
        root->Close(root);
        return NULL;
    }

    UINTN cap = 1024 * 1024;
    uint8_t* buf = (uint8_t*)uefi_malloc(cap);
    UINTN total = 0;

    while (buf) {
        if (total == cap) {
            UINTN new_cap = cap * 2;
            if (new_cap > 16 * 1024 * 1024) break;
            uint8_t* nb = (uint8_t*)uefi_realloc(buf, new_cap);
            if (!nb) break;
            buf = nb;
            cap = new_cap;
        }
        UINTN chunk = cap - total;
        EFI_STATUS r_st = file->Read(file, &chunk, buf + total);
        if (EFI_ERROR(r_st) || chunk == 0) break;
        total += chunk;
    }

    file->Close(file);
    root->Close(root);

    if (!buf || total == 0) {
        if (buf) uefi_free(buf);
        return NULL;
    }
    if (out_size) *out_size = total;
    return buf;
}

static uint8_t* read_uefi_file(EFI_SYSTEM_TABLE* st, const CHAR16* wpath, UINTN* out_size) {
    if (!st) st = gSystemTable;
    if (!st || !st->BootServices || !wpath) return NULL;

    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID liGuid = { 0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B} };

    DuiLoadedImagePrefix* li = NULL;
    if (!EFI_ERROR(st->BootServices->LocateProtocol(&liGuid, NULL, (void**)&li)) && li && li->DeviceHandle) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* dev_fs = NULL;
        if (!EFI_ERROR(st->BootServices->HandleProtocol(li->DeviceHandle, &fsGuid, (void**)&dev_fs)) && dev_fs) {
            uint8_t* data = read_file_from_volume(dev_fs, wpath, out_size);
            if (data) return data;
        }
    }

    UINTN numHandles = 0;
    EFI_HANDLE* handleBuffer = NULL;
    if (EFI_ERROR(st->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer))) {
        return NULL;
    }

    uint8_t* result = NULL;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(st->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs)) || !fs) continue;
        result = read_file_from_volume(fs, wpath, out_size);
        if (result) break;
    }

    if (handleBuffer) st->BootServices->FreePool(handleBuffer);
    return result;
}

// ============================================================================
// Pre-Rasterized TrueType Font Glyph Atlas
// ============================================================================

#define GLYPH_FIRST 32
#define GLYPH_LAST  126
#define GLYPH_COUNT (GLYPH_LAST - GLYPH_FIRST + 1)
#define FONT_SCALES 3

typedef struct {
    int16_t xoff;
    int16_t yoff;
    int16_t advance;
    uint16_t gw;
    uint16_t gh;
    uint32_t offset;
} CachedGlyph;

typedef struct {
    int px_size;
    int ascent;
    int descent;
    CachedGlyph glyphs[GLYPH_COUNT];
    uint8_t* bitmap_pool;
    uint32_t pool_size;
} FontAtlasScale;

static stbtt_fontinfo font_info;
static uint8_t* font_buffer = NULL;
static bool font_loaded = false;
static FontAtlasScale font_atlas[FONT_SCALES];

static void bake_font_atlas(void) {
    static const int sizes[FONT_SCALES] = { 14, 20, 32 };
    int asc_raw = 0, desc_raw = 0, gap_raw = 0;
    stbtt_GetFontVMetrics(&font_info, &asc_raw, &desc_raw, &gap_raw);

    for (int s = 0; s < FONT_SCALES; s++) {
        FontAtlasScale* sc = &font_atlas[s];
        sc->px_size = sizes[s];
        float sf = stbtt_ScaleForPixelHeight(&font_info, (float)sizes[s]);
        sc->ascent = (int)((float)asc_raw * sf + 0.5f);
        sc->descent = (int)((float)desc_raw * sf);

        if (sc->bitmap_pool) {
            uefi_free(sc->bitmap_pool);
            sc->bitmap_pool = NULL;
        }
        sc->pool_size = 0;

        uint32_t total = 0;
        for (int i = 0; i < GLYPH_COUNT; i++) {
            int cp = GLYPH_FIRST + i;
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&font_info, cp, &adv, &lsb);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(&font_info, cp, sf, sf, &x0, &y0, &x1, &y1);

            int gw = (x1 > x0) ? (x1 - x0) : 0;
            int gh = (y1 > y0) ? (y1 - y0) : 0;

            CachedGlyph* g = &sc->glyphs[i];
            g->xoff = (int16_t)x0;
            g->yoff = (int16_t)y0;
            int adv_px = (int)((float)adv * sf + 0.5f);
            if (adv > 0 && adv_px < 1) adv_px = 1;
            g->advance = (int16_t)adv_px;
            g->gw = (uint16_t)gw;
            g->gh = (uint16_t)gh;
            g->offset = total;
            total += (uint32_t)(gw * gh);
        }

        sc->pool_size = (total > 0) ? total : 1;
        sc->bitmap_pool = (uint8_t*)uefi_malloc(sc->pool_size);
        if (!sc->bitmap_pool) {
            font_loaded = false;
            return;
        }
        uefi_memset(sc->bitmap_pool, 0, sc->pool_size);

        for (int i = 0; i < GLYPH_COUNT; i++) {
            CachedGlyph* g = &sc->glyphs[i];
            if (g->gw > 0 && g->gh > 0) {
                stbtt_MakeCodepointBitmap(
                    &font_info,
                    sc->bitmap_pool + g->offset,
                    (int)g->gw,
                    (int)g->gh,
                    (int)g->gw,
                    sf,
                    sf,
                    GLYPH_FIRST + i
                );
            }
        }
    }
}

void dui_init(EFI_SYSTEM_TABLE* ST) {
    g_dui_st = ST ? ST : gSystemTable;
    if (font_loaded) return;

    UINTN font_sz = 0;
    uint8_t* data = read_uefi_file(g_dui_st, (const CHAR16*)L"\\assets\\font.ttf", &font_sz);
    if (!data) {
        data = read_uefi_file(g_dui_st, (const CHAR16*)L"assets\\font.ttf", &font_sz);
    }

    if (data && font_sz > 0) {
        int offset = stbtt_GetFontOffsetForIndex(data, 0);
        if (offset >= 0 && stbtt_InitFont(&font_info, data, offset)) {
            if (font_buffer) uefi_free(font_buffer);
            font_buffer = data;
            font_loaded = true;
            bake_font_atlas();
        } else {
            uefi_free(data);
        }
    }
}

// ============================================================================
// Cached High-Craft Aurora Wallpaper Engine
// ============================================================================

static uint32_t* wallpaper_cache = NULL;
static int cached_theme_id = -1;
static uint32_t cached_w = 0;
static uint32_t cached_h = 0;
static int g_wallpaper_id = 0;

typedef struct {
    float bx, by;
    float rx, ry;
    uint32_t color;
    float intensity;
} AuroraBlob;

typedef struct {
    uint32_t top_color;
    uint32_t bot_color;
    AuroraBlob blobs[3];
    uint32_t ribbon_color;
    float ribbon_y;
    float ribbon_amp;
} WallpaperThemeSpec;

static const WallpaperThemeSpec k_wallpapers[5] = {
    // 0: Horizon Midnight
    {
        0x050812, 0x0B1328,
        {
            { 0.22f, 0.28f, 0.42f, 0.36f, 0x38BDF8, 0.46f },
            { 0.76f, 0.34f, 0.45f, 0.40f, 0x6366F1, 0.44f },
            { 0.52f, 0.78f, 0.55f, 0.32f, 0x14B8A6, 0.34f }
        },
        0x38BDF8, 0.54f, 0.22f
    },
    // 1: Aurora Borealis
    {
        0x03090E, 0x071821,
        {
            { 0.28f, 0.32f, 0.48f, 0.34f, 0x10B981, 0.48f },
            { 0.68f, 0.26f, 0.42f, 0.36f, 0x2DD4BF, 0.42f },
            { 0.72f, 0.74f, 0.50f, 0.38f, 0x4F46E5, 0.38f }
        },
        0x2DD4BF, 0.46f, 0.26f
    },
    // 2: Cobalt Nebulae
    {
        0x060714, 0x101330,
        {
            { 0.26f, 0.35f, 0.44f, 0.38f, 0x2563EB, 0.50f },
            { 0.74f, 0.30f, 0.42f, 0.38f, 0x818CF8, 0.44f },
            { 0.54f, 0.72f, 0.48f, 0.34f, 0xC026D3, 0.36f }
        },
        0x818CF8, 0.52f, 0.22f
    },
    // 3: Obsidian Ember
    {
        0x0B070A, 0x190E15,
        {
            { 0.24f, 0.68f, 0.46f, 0.36f, 0xD97706, 0.42f },
            { 0.72f, 0.36f, 0.44f, 0.38f, 0xE11D48, 0.44f },
            { 0.48f, 0.22f, 0.48f, 0.34f, 0x7C3AED, 0.32f }
        },
        0xFB7185, 0.56f, 0.20f
    },
    // 4: Alpine Mist
    {
        0x080E16, 0x152233,
        {
            { 0.32f, 0.26f, 0.50f, 0.38f, 0x38BDF8, 0.38f },
            { 0.74f, 0.46f, 0.48f, 0.40f, 0x64748B, 0.42f },
            { 0.40f, 0.76f, 0.52f, 0.34f, 0x34D399, 0.30f }
        },
        0x94A3B8, 0.50f, 0.18f
    }
};

static inline int normalize_theme_id(int id) {
    int t = id % 5;
    if (t < 0) t += 5;
    return t;
}

static void generate_wallpaper_cache(int tid) {
    if (screen_width == 0 || screen_height == 0) return;

    if (!wallpaper_cache || cached_w != screen_width || cached_h != screen_height) {
        if (wallpaper_cache) {
            uefi_free(wallpaper_cache);
            wallpaper_cache = NULL;
        }
        wallpaper_cache = (uint32_t*)uefi_malloc((UINTN)screen_width * (UINTN)screen_height * 4U);
        if (!wallpaper_cache) return;
    }

    const WallpaperThemeSpec* spec = &k_wallpapers[tid];
    float tr = (float)((spec->top_color >> 16) & 0xFF);
    float tg = (float)((spec->top_color >> 8) & 0xFF);
    float tb = (float)(spec->top_color & 0xFF);

    float br = (float)((spec->bot_color >> 16) & 0xFF);
    float bg = (float)((spec->bot_color >> 8) & 0xFF);
    float bb = (float)(spec->bot_color & 0xFF);

    float rr = (float)((spec->ribbon_color >> 16) & 0xFF);
    float rg = (float)((spec->ribbon_color >> 8) & 0xFF);
    float rb = (float)(spec->ribbon_color & 0xFF);

    float blob_r[3], blob_g[3], blob_b[3];
    float inv_rx[3], inv_ry[3];
    for (int k = 0; k < 3; k++) {
        blob_r[k] = (float)((spec->blobs[k].color >> 16) & 0xFF) * spec->blobs[k].intensity;
        blob_g[k] = (float)((spec->blobs[k].color >> 8) & 0xFF) * spec->blobs[k].intensity;
        blob_b[k] = (float)(spec->blobs[k].color & 0xFF) * spec->blobs[k].intensity;
        inv_rx[k] = 1.0f / spec->blobs[k].rx;
        inv_ry[k] = 1.0f / spec->blobs[k].ry;
    }

    float inv_w = 1.0f / (float)screen_width;
    float inv_h = 1.0f / (float)screen_height;

    for (uint32_t y = 0; y < screen_height; y++) {
        float ny = (float)y * inv_h;
        float base_r = tr + (br - tr) * ny;
        float base_g = tg + (bg - tg) * ny;
        float base_b = tb + (bb - tb) * ny;

        uint32_t* dst = wallpaper_cache + (uint64_t)y * screen_width;
        uint32_t y_hash = y * 668265263U;

        for (uint32_t x = 0; x < screen_width; x++) {
            float nx = (float)x * inv_w;
            float r = base_r;
            float g = base_g;
            float b = base_b;

            // Multi-stop radial aurora glow blobs
            for (int k = 0; k < 3; k++) {
                float dx = (nx - spec->blobs[k].bx) * inv_rx[k];
                float dy = (ny - spec->blobs[k].by) * inv_ry[k];
                float d2 = dx * dx + dy * dy;
                if (d2 < 4.5f) {
                    float w = 1.0f / (1.0f + d2 * (1.25f + 0.40f * d2));
                    r += blob_r[k] * w;
                    g += blob_g[k] * w;
                    b += blob_b[k] * w;
                }
            }

            // Diagonal aurora ribbon sweep
            float wave = spec->ribbon_y + 0.14f * (nx - 0.5f) - 0.18f * (nx - 0.5f) * (nx - 0.5f);
            float dist_band = (ny - wave) * 6.5f;
            float b2 = dist_band * dist_band;
            if (b2 < 4.0f) {
                float bw = spec->ribbon_amp / (1.0f + b2 * (1.6f + 0.5f * b2));
                r += rr * bw;
                g += rg * bw;
                b += rb * bw;
            }

            // Subtle film-grain hash dither (-1, 0, +1) + Lira architectural 28px dot grid
            uint32_t h = (x * 374761393U + y_hash) ^ 0x5BF03635U;
            h = (h ^ (h >> 13)) * 1274126177U;
            int dither = (int)((h >> 24) & 3U) - 1;
            int dot = ((x % 28U) == 0U && (y % 28U) == 0U) ? 7 : 0;

            int ir = (int)r + dither + dot;
            int ig = (int)g + dither + dot;
            int ib = (int)b + dither + dot;
            if (ir < 0) ir = 0; else if (ir > 255) ir = 255;
            if (ig < 0) ig = 0; else if (ig > 255) ig = 255;
            if (ib < 0) ib = 0; else if (ib > 255) ib = 255;

            dst[x] = ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
        }
    }

    cached_theme_id = tid;
    cached_w = screen_width;
    cached_h = screen_height;
}

void dui_on_resolution_change(void) {
    cached_theme_id = -1;
    cached_w = 0;
    cached_h = 0;
    if (wallpaper_cache) {
        uefi_free(wallpaper_cache);
        wallpaper_cache = NULL;
    }
}

int dui_get_wallpaper_id(void) {
    return g_wallpaper_id;
}

void dui_set_wallpaper_id(int id) {
    g_wallpaper_id = normalize_theme_id(id);
    g_wallpaper_theme = g_wallpaper_id;
}

void dui_draw_wallpaper(int theme_id) {
    if (!backbuffer || screen_width == 0 || screen_height == 0) return;

    int tid = (theme_id < 0) ? g_wallpaper_id : normalize_theme_id(theme_id);
    if (!wallpaper_cache || cached_theme_id != tid || cached_w != screen_width || cached_h != screen_height) {
        generate_wallpaper_cache(tid);
    }

    if (wallpaper_cache) {
        uint64_t total_pixels = (uint64_t)screen_width * (uint64_t)screen_height;
        uint64_t qwords = total_pixels >> 1;
        uint64_t* dst64 = (uint64_t*)backbuffer;
        const uint64_t* src64 = (const uint64_t*)wallpaper_cache;
        for (uint64_t i = 0; i < qwords; i++) {
            dst64[i] = src64[i];
        }
        if (total_pixels & 1ULL) {
            backbuffer[total_pixels - 1] = wallpaper_cache[total_pixels - 1];
        }
    } else {
        compositor_clear(THEME_BG_VOID);
    }
}

// ============================================================================
// Scissor Clip Stack
// ============================================================================

#define CLIP_STACK_MAX 32
typedef struct { int x, y, w, h; } DuiClipRect;
static DuiClipRect g_clip_stack[CLIP_STACK_MAX];
static int g_clip_sp = 0;
static int g_clip_overflow = 0;

void dui_push_clip(int x, int y, int w, int h) {
    DuiClipRect cur;
    if (g_clip_sp > 0) {
        cur = g_clip_stack[g_clip_sp - 1];
    } else {
        cur.x = 0;
        cur.y = 0;
        cur.w = (int)screen_width;
        cur.h = (int)screen_height;
    }

    int ix0 = imax(cur.x, x);
    int iy0 = imax(cur.y, y);
    int ix1 = imin(cur.x + cur.w, x + w);
    int iy1 = imin(cur.y + cur.h, y + h);
    int iw = (ix1 > ix0) ? (ix1 - ix0) : 0;
    int ih = (iy1 > iy0) ? (iy1 - iy0) : 0;

    if (g_clip_sp < CLIP_STACK_MAX) {
        g_clip_stack[g_clip_sp].x = ix0;
        g_clip_stack[g_clip_sp].y = iy0;
        g_clip_stack[g_clip_sp].w = iw;
        g_clip_stack[g_clip_sp].h = ih;
        g_clip_sp++;
        compositor_set_clip(ix0, iy0, iw, ih);
    } else {
        g_clip_overflow++;
    }
}

void dui_pop_clip(void) {
    if (g_clip_overflow > 0) {
        g_clip_overflow--;
        return;
    }
    if (g_clip_sp > 0) {
        g_clip_sp--;
    }
    if (g_clip_sp > 0) {
        DuiClipRect top = g_clip_stack[g_clip_sp - 1];
        compositor_set_clip(top.x, top.y, top.w, top.h);
    } else {
        compositor_set_clip(0, 0, (int)screen_width, (int)screen_height);
    }
}

void dui_reset_clip(void) {
    g_clip_sp = 0;
    g_clip_overflow = 0;
    compositor_set_clip(0, 0, (int)screen_width, (int)screen_height);
}

// Legacy clip wrappers
void dui_clip_push(int x, int y, int w, int h) { dui_push_clip(x, y, w, h); }
void dui_clip_pop(void) { dui_pop_clip(); }
bool dui_clip_test(int x, int y) {
    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    return (x >= cx && x < cx + cw && y >= cy && y < cy + ch);
}

// ============================================================================
// Core Anti-Aliased & Alpha Primitives
// ============================================================================

void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0 || alpha == 0) return;
    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    int x0 = imax(x, cx);
    int y0 = imax(y, cy);
    int x1 = imin(x + w, cx + cw);
    int y1 = imin(y + h, cy + ch);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t fg = color & 0x00FFFFFFU;
    if (alpha == 255) {
        for (int py = y0; py < y1; py++) {
            uint32_t* row = backbuffer + (uint32_t)py * screen_width;
            for (int px = x0; px < x1; px++) row[px] = fg;
        }
    } else {
        for (int py = y0; py < y1; py++) {
            uint32_t* row = backbuffer + (uint32_t)py * screen_width;
            for (int px = x0; px < x1; px++) row[px] = blend_pixel(row[px], fg, alpha);
        }
    }
}

void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    if (thickness * 2 > w) thickness = w / 2;
    if (thickness * 2 > h) thickness = h / 2;
    if (thickness <= 0) thickness = 1;
    dui_rect(x, y, w, thickness, color, 255);
    dui_rect(x, y + h - thickness, w, thickness, color, 255);
    dui_rect(x, y + thickness, thickness, h - 2 * thickness, color, 255);
    dui_rect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color, 255);
}

void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0 || alpha == 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius == 0) {
        dui_rect(x, y, w, h, color, alpha);
        return;
    }

    // Middle band without corner clipping
    if (h > 2 * radius) {
        dui_rect(x, y + radius, w, h - 2 * radius, color, alpha);
    }

    float fr = (float)radius;
    // Top and bottom corner rows with AA corner edges + fast center span
    for (int dy = 0; dy < radius; dy++) {
        float ry = fr - 0.5f - (float)dy;
        int top_y = y + dy;
        int bot_y = y + h - 1 - dy;
        bool same_row = (top_y == bot_y);

        compositor_fill_span(x + radius, x + w - radius, top_y, color, alpha);
        if (!same_row) {
            compositor_fill_span(x + radius, x + w - radius, bot_y, color, alpha);
        }

        for (int dx = 0; dx < radius; dx++) {
            float rx = fr - 0.5f - (float)dx;
            float dist = dui_sqrtf(rx * rx + ry * ry);
            float cov = fr + 0.5f - dist;
            if (cov <= 0.0f) continue;
            uint8_t a = (cov >= 1.0f) ? alpha : mul_alpha(alpha, cov);
            if (a == 0) continue;

            put_pixel_alpha(x + dx, top_y, color, a);
            put_pixel_alpha(x + w - 1 - dx, top_y, color, a);
            if (!same_row) {
                put_pixel_alpha(x + dx, bot_y, color, a);
                put_pixel_alpha(x + w - 1 - dx, bot_y, color, a);
            }
        }
    }
}

void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0 || alpha == 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    if (radius == 0) {
        compositor_fill_span(x, x + w, y, color, alpha);
        if (h > 1) compositor_fill_span(x, x + w, y + h - 1, color, alpha);
        for (int py = y + 1; py < y + h - 1; py++) {
            put_pixel_alpha(x, py, color, alpha);
            if (w > 1) put_pixel_alpha(x + w - 1, py, color, alpha);
        }
        return;
    }

    // Straight top & bottom edges between corners
    if (w > 2 * radius) {
        compositor_fill_span(x + radius, x + w - radius, y, color, alpha);
        if (h > 1) {
            compositor_fill_span(x + radius, x + w - radius, y + h - 1, color, alpha);
        }
    }

    // Straight left & right edges between corners
    for (int py = y + radius; py < y + h - radius; py++) {
        put_pixel_alpha(x, py, color, alpha);
        if (w > 1) put_pixel_alpha(x + w - 1, py, color, alpha);
    }

    // 4 Anti-aliased quarter-circle corner arcs (1px stroke)
    float ring_r = (float)radius - 0.5f;
    float fr = (float)radius;
    for (int dy = 0; dy < radius; dy++) {
        float ry = fr - 0.5f - (float)dy;
        int top_y = y + dy;
        int bot_y = y + h - 1 - dy;
        for (int dx = 0; dx < radius; dx++) {
            float rx = fr - 0.5f - (float)dx;
            float dist = dui_sqrtf(rx * rx + ry * ry);
            float d_stroke = fabs_val(dist - ring_r);
            if (d_stroke >= 1.0f) continue;

            uint8_t a = mul_alpha(alpha, 1.0f - d_stroke);
            if (a == 0) continue;

            int lx = x + dx;
            int rx_pos = x + w - 1 - dx;
            put_pixel_alpha(lx, top_y, color, a);
            if (rx_pos != lx) put_pixel_alpha(rx_pos, top_y, color, a);
            if (bot_y != top_y) {
                put_pixel_alpha(lx, bot_y, color, a);
                if (rx_pos != lx) put_pixel_alpha(rx_pos, bot_y, color, a);
            }
        }
    }
}

void dui_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0 || alpha == 0) return;
    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    int x0 = imax(x, cx);
    int y0 = imax(y, cy);
    int x1 = imin(x + w, cx + cw);
    int y1 = imin(y + h, cy + ch);
    if (x0 >= x1 || y0 >= y1) return;

    float denom = (h > 1) ? (float)(h - 1) : 1.0f;
    for (int py = y0; py < y1; py++) {
        float t = (float)(py - y) / denom;
        uint32_t row_col = horizon_lerp_color(c1, c2, t) & 0x00FFFFFFU;
        uint32_t* row = backbuffer + (uint32_t)py * screen_width;
        if (alpha == 255) {
            for (int px = x0; px < x1; px++) row[px] = row_col;
        } else {
            for (int px = x0; px < x1; px++) row[px] = blend_pixel(row[px], row_col, alpha);
        }
    }
}

void dui_gradient_h(int x, int y, int w, int h, uint32_t c1, uint32_t c2, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0 || alpha == 0) return;
    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    int x0 = imax(x, cx);
    int y0 = imax(y, cy);
    int x1 = imin(x + w, cx + cw);
    int y1 = imin(y + h, cy + ch);
    if (x0 >= x1 || y0 >= y1) return;

    float denom = (w > 1) ? (float)(w - 1) : 1.0f;
    for (int px = x0; px < x1; px++) {
        float t = (float)(px - x) / denom;
        uint32_t col = horizon_lerp_color(c1, c2, t) & 0x00FFFFFFU;
        if (alpha == 255) {
            for (int py = y0; py < y1; py++) {
                backbuffer[(uint32_t)py * screen_width + (uint32_t)px] = col;
            }
        } else {
            for (int py = y0; py < y1; py++) {
                uint32_t idx = (uint32_t)py * screen_width + (uint32_t)px;
                backbuffer[idx] = blend_pixel(backbuffer[idx], col, alpha);
            }
        }
    }
}

void dui_gradient_rounded(int x, int y, int w, int h, int r, uint32_t c1, uint32_t c2, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0 || alpha == 0) return;
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r == 0) {
        dui_gradient_v(x, y, w, h, c1, c2, alpha);
        return;
    }

    float denom = (h > 1) ? (float)(h - 1) : 1.0f;
    if (h > 2 * r) {
        for (int dy = r; dy < h - r; dy++) {
            uint32_t col = horizon_lerp_color(c1, c2, (float)dy / denom);
            compositor_fill_span(x, x + w, y + dy, col, alpha);
        }
    }

    float fr = (float)r;
    for (int dy = 0; dy < r; dy++) {
        float ry = fr - 0.5f - (float)dy;
        int top_y = y + dy;
        int bot_y = y + h - 1 - dy;
        bool same_row = (top_y == bot_y);

        uint32_t top_col = horizon_lerp_color(c1, c2, (float)dy / denom);
        uint32_t bot_col = horizon_lerp_color(c1, c2, (float)(h - 1 - dy) / denom);

        compositor_fill_span(x + r, x + w - r, top_y, top_col, alpha);
        if (!same_row) {
            compositor_fill_span(x + r, x + w - r, bot_y, bot_col, alpha);
        }

        for (int dx = 0; dx < r; dx++) {
            float rx = fr - 0.5f - (float)dx;
            float dist = dui_sqrtf(rx * rx + ry * ry);
            float cov = fr + 0.5f - dist;
            if (cov <= 0.0f) continue;
            uint8_t a = (cov >= 1.0f) ? alpha : mul_alpha(alpha, cov);
            if (a == 0) continue;

            put_pixel_alpha(x + dx, top_y, top_col, a);
            put_pixel_alpha(x + w - 1 - dx, top_y, top_col, a);
            if (!same_row) {
                put_pixel_alpha(x + dx, bot_y, bot_col, a);
                put_pixel_alpha(x + w - 1 - dx, bot_y, bot_col, a);
            }
        }
    }
}

void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha) {
    if (!backbuffer || r <= 0 || alpha == 0) return;
    float fr = (float)r;
    int r_in_sq = (r > 2) ? (r - 2) * (r - 2) : 0;
    int r_out_sq = (r + 1) * (r + 1);

    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        int dy2 = dy * dy;
        int inner_hw = 0;
        if (dy2 < r_in_sq) {
            inner_hw = (int)dui_sqrtf((float)(r_in_sq - dy2));
            compositor_fill_span(cx - inner_hw, cx + inner_hw + 1, py, color, alpha);
        }
        for (int dx = -r; dx <= r; dx++) {
            if (dy2 < r_in_sq && dx >= -inner_hw && dx <= inner_hw) continue;
            int d2 = dx * dx + dy2;
            if (d2 > r_out_sq) continue;
            float dist = dui_sqrtf((float)d2);
            float cov = fr + 0.5f - dist;
            if (cov <= 0.0f) continue;
            uint8_t a = (cov >= 1.0f) ? alpha : mul_alpha(alpha, cov);
            if (a > 0) put_pixel_alpha(cx + dx, py, color, a);
        }
    }
}

void dui_circle_outline(int cx, int cy, int r, uint32_t color, uint8_t alpha) {
    if (!backbuffer || r <= 0 || alpha == 0) return;
    float fr = (float)r;
    int r_min_sq = (r > 2) ? (r - 2) * (r - 2) : 0;
    int r_max_sq = (r + 2) * (r + 2);

    for (int dy = -r - 1; dy <= r + 1; dy++) {
        int dy2 = dy * dy;
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            int d2 = dx * dx + dy2;
            if (d2 < r_min_sq || d2 > r_max_sq) continue;
            float dist = dui_sqrtf((float)d2);
            float diff = fabs_val(dist - fr);
            if (diff >= 1.0f) continue;
            uint8_t a = mul_alpha(alpha, 1.0f - diff);
            if (a > 0) put_pixel_alpha(cx + dx, cy + dy, color, a);
        }
    }
}

void dui_line(int x0, int y0, int x1, int y1, uint32_t color, uint8_t alpha) {
    if (!backbuffer || alpha == 0) return;

    // Fast exact axis-aligned lines
    if (y0 == y1) {
        int lx = imin(x0, x1);
        int rx = imax(x0, x1);
        compositor_fill_span(lx, rx + 1, y0, color, alpha);
        return;
    }
    if (x0 == x1) {
        int ty = imin(y0, y1);
        int by = imax(y0, y1);
        for (int py = ty; py <= by; py++) {
            put_pixel_alpha(x0, py, color, alpha);
        }
        return;
    }

    // Xiaolin Wu anti-aliased diagonal line
    bool steep = ( (y1 > y0 ? y1 - y0 : y0 - y1) > (x1 > x0 ? x1 - x0 : x0 - x1) );
    if (steep) {
        int tmp = x0; x0 = y0; y0 = tmp;
        tmp = x1; x1 = y1; y1 = tmp;
    }
    if (x0 > x1) {
        int tmp = x0; x0 = x1; x1 = tmp;
        tmp = y0; y0 = y1; y1 = tmp;
    }

    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);
    float grad = (dx != 0.0f) ? (dy / dx) : 1.0f;

    float intery = (float)y0 + grad;
    if (steep) {
        put_pixel_alpha(y0, x0, color, alpha);
        put_pixel_alpha(y1, x1, color, alpha);
    } else {
        put_pixel_alpha(x0, y0, color, alpha);
        put_pixel_alpha(x1, y1, color, alpha);
    }

    for (int px = x0 + 1; px < x1; px++) {
        int ip = (int)intery;
        if (intery < 0.0f && (float)ip > intery) ip--;
        float frac = intery - (float)ip;
        uint8_t a0 = mul_alpha(alpha, 1.0f - frac);
        uint8_t a1 = mul_alpha(alpha, frac);
        if (steep) {
            if (a0 > 0) put_pixel_alpha(ip, px, color, a0);
            if (a1 > 0) put_pixel_alpha(ip + 1, px, color, a1);
        } else {
            if (a0 > 0) put_pixel_alpha(px, ip, color, a0);
            if (a1 > 0) put_pixel_alpha(px, ip + 1, color, a1);
        }
        intery += grad;
    }
}

void dui_progress_ring(int cx, int cy, int r, int thickness, float progress, uint32_t bg_color, uint32_t fg_color) {
    if (!backbuffer || r <= 1 || thickness <= 0) return;
    progress = horizon_clampf(progress, 0.0f, 1.0f);
    if (thickness > r) thickness = r;

    float r_out = (float)r;
    float r_in = (float)(r - thickness);
    int r_in_sq = (r - thickness > 2) ? (r - thickness - 2) * (r - thickness - 2) : 0;
    int r_out_sq = (r + 2) * (r + 2);

    for (int dy = -r - 1; dy <= r + 1; dy++) {
        int dy2 = dy * dy;
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            int d2 = dx * dx + dy2;
            if (d2 < r_in_sq || d2 > r_out_sq) continue;

            float dist = dui_sqrtf((float)d2);
            float cov_out = r_out + 0.5f - dist;
            float cov_in = dist - (r_in - 0.5f);
            float cov = fmin_val(cov_out, cov_in);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;

            uint8_t a = (uint8_t)(cov * 255.0f + 0.5f);
            if (a == 0) continue;

            if (progress <= 0.001f) {
                put_pixel_alpha(cx + dx, cy + dy, bg_color, a);
            } else if (progress >= 0.999f) {
                put_pixel_alpha(cx + dx, cy + dy, fg_color, a);
            } else {
                float t = dui_atan2_cw01((float)dx, (float)dy);
                if (t <= progress) {
                    put_pixel_alpha(cx + dx, cy + dy, fg_color, a);
                } else {
                    put_pixel_alpha(cx + dx, cy + dy, bg_color, a);
                }
            }
        }
    }
}

void dui_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, uint8_t alpha) {
    if (!backbuffer || alpha == 0) return;
    int minX = imin(x0, imin(x1, x2));
    int minY = imin(y0, imin(y1, y2));
    int maxX = imax(x0, imax(x1, x2));
    int maxY = imax(y0, imax(y1, y2));

    int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0) return;
    int sign = (area > 0) ? 1 : -1;

    for (int y = minY; y <= maxY; y++) {
        for (int x = minX; x <= maxX; x++) {
            int e0 = ((x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)) * sign;
            int e1 = ((x2 - x1) * (y - y1) - (y2 - y1) * (x - x1)) * sign;
            int e2 = ((x0 - x2) * (y - y2) - (y0 - y2) * (x - x2)) * sign;
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                put_pixel_alpha(x, y, color, alpha);
            }
        }
    }
}

// ============================================================================
// Horizon Spatial Glass Surfaces & Elevation
// ============================================================================

static uint32_t* glass_buf_a = NULL;
static uint32_t* glass_buf_b = NULL;
static uint32_t glass_buf_cap = 0;

static bool ensure_glass_buffers(uint32_t needed) {
    if (needed <= glass_buf_cap && glass_buf_a && glass_buf_b) return true;
    uint32_t* na = (uint32_t*)uefi_malloc((UINTN)needed * 4U);
    uint32_t* nb = (uint32_t*)uefi_malloc((UINTN)needed * 4U);
    if (!na || !nb) {
        if (na) uefi_free(na);
        if (nb) uefi_free(nb);
        return false;
    }
    if (glass_buf_a) uefi_free(glass_buf_a);
    if (glass_buf_b) uefi_free(glass_buf_b);
    glass_buf_a = na;
    glass_buf_b = nb;
    glass_buf_cap = needed;
    return true;
}

void dui_frosted_glass(int x, int y, int w, int h, int r, uint32_t tint, uint8_t alpha) {
    if (!backbuffer || w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    int x0 = imax(x, cx);
    int y0 = imax(y, cy);
    int x1 = imin(x + w, cx + cw);
    int y1 = imin(y + h, cy + ch);
    if (x0 >= x1 || y0 >= y1) return;

    int bw = x1 - x0;
    int bh = y1 - y0;
    int dw = (bw + 1) >> 1;
    int dh = (bh + 1) >> 1;
    if (dw <= 0 || dh <= 0) return;

    if (!ensure_glass_buffers((uint32_t)dw * (uint32_t)dh)) {
        dui_rect_rounded(x, y, w, h, r, tint, alpha);
        return;
    }

    // 1. 2x2 downsample backbuffer into glass_buf_a
    for (int gy = 0; gy < dh; gy++) {
        int sy0 = y0 + (gy << 1);
        int sy1 = (sy0 + 1 < y1) ? (sy0 + 1) : sy0;
        const uint32_t* r0 = backbuffer + (uint32_t)sy0 * screen_width;
        const uint32_t* r1 = backbuffer + (uint32_t)sy1 * screen_width;
        uint32_t* dst = glass_buf_a + (uint32_t)gy * (uint32_t)dw;

        for (int gx = 0; gx < dw; gx++) {
            int sx0 = x0 + (gx << 1);
            int sx1 = (sx0 + 1 < x1) ? (sx0 + 1) : sx0;
            uint32_t p00 = r0[sx0];
            uint32_t p01 = r0[sx1];
            uint32_t p10 = r1[sx0];
            uint32_t p11 = r1[sx1];

            uint32_t rr = (((p00 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF) + ((p10 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF)) >> 2;
            uint32_t gg = (((p00 >> 8) & 0xFF) + ((p01 >> 8) & 0xFF) + ((p10 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF)) >> 2;
            uint32_t bb = ((p00 & 0xFF) + (p01 & 0xFF) + (p10 & 0xFF) + (p11 & 0xFF)) >> 2;
            dst[gx] = (rr << 16) | (gg << 8) | bb;
        }
    }

    // 2. Horizontal sliding-window box blur (radius 3 in 2x space -> ~6px blur)
    for (int gy = 0; gy < dh; gy++) {
        const uint32_t* src = glass_buf_a + (uint32_t)gy * (uint32_t)dw;
        uint32_t* dst = glass_buf_b + (uint32_t)gy * (uint32_t)dw;
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -3; k <= 3; k++) {
            int idx = horizon_clampi(k, 0, dw - 1);
            uint32_t p = src[idx];
            sr += (p >> 16) & 0xFF;
            sg += (p >> 8) & 0xFF;
            sb += p & 0xFF;
        }
        for (int gx = 0; gx < dw; gx++) {
            uint32_t or_c = (sr * 9363U) >> 16;
            uint32_t og_c = (sg * 9363U) >> 16;
            uint32_t ob_c = (sb * 9363U) >> 16;
            dst[gx] = (or_c << 16) | (og_c << 8) | ob_c;

            int rem_idx = horizon_clampi(gx - 3, 0, dw - 1);
            int add_idx = horizon_clampi(gx + 4, 0, dw - 1);
            uint32_t pr = src[rem_idx];
            uint32_t pa = src[add_idx];
            sr = sr - ((pr >> 16) & 0xFF) + ((pa >> 16) & 0xFF);
            sg = sg - ((pr >> 8) & 0xFF) + ((pa >> 8) & 0xFF);
            sb = sb - (pr & 0xFF) + (pa & 0xFF);
        }
    }

    // 3. Vertical sliding-window box blur (radius 3 in 2x space) back into glass_buf_a
    for (int gx = 0; gx < dw; gx++) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -3; k <= 3; k++) {
            int idx = horizon_clampi(k, 0, dh - 1);
            uint32_t p = glass_buf_b[(uint32_t)idx * (uint32_t)dw + (uint32_t)gx];
            sr += (p >> 16) & 0xFF;
            sg += (p >> 8) & 0xFF;
            sb += p & 0xFF;
        }
        for (int gy = 0; gy < dh; gy++) {
            uint32_t or_c = (sr * 9363U) >> 16;
            uint32_t og_c = (sg * 9363U) >> 16;
            uint32_t ob_c = (sb * 9363U) >> 16;
            glass_buf_a[(uint32_t)gy * (uint32_t)dw + (uint32_t)gx] = (or_c << 16) | (og_c << 8) | ob_c;

            int rem_idx = horizon_clampi(gy - 3, 0, dh - 1);
            int add_idx = horizon_clampi(gy + 4, 0, dh - 1);
            uint32_t pr = glass_buf_b[(uint32_t)rem_idx * (uint32_t)dw + (uint32_t)gx];
            uint32_t pa = glass_buf_b[(uint32_t)add_idx * (uint32_t)dw + (uint32_t)gx];
            sr = sr - ((pr >> 16) & 0xFF) + ((pa >> 16) & 0xFF);
            sg = sg - ((pr >> 8) & 0xFF) + ((pa >> 8) & 0xFF);
            sb = sb - (pr & 0xFF) + (pa & 0xFF);
        }
    }

    // 4. Composite blurred buffer + tint inside rounded rect mask
    uint32_t fg_tint = tint & 0x00FFFFFFU;
    float fr = (float)r;

    for (int py = y0; py < y1; py++) {
        int dy = py - y;
        int gy = (py - y0) >> 1;
        const uint32_t* blur_row = glass_buf_a + (uint32_t)gy * (uint32_t)dw;
        uint32_t* dst_row = backbuffer + (uint32_t)py * screen_width;

        bool in_corner_y = (r > 0) && (dy < r || dy >= h - r);
        if (!in_corner_y) {
            for (int px = x0; px < x1; px++) {
                uint32_t bg = blur_row[(px - x0) >> 1];
                dst_row[px] = blend_pixel(bg, fg_tint, alpha);
            }
        } else {
            int local_dy = (dy < r) ? dy : (h - 1 - dy);
            float ry = fr - 0.5f - (float)local_dy;
            for (int px = x0; px < x1; px++) {
                int dx = px - x;
                uint32_t glass_px = blend_pixel(blur_row[(px - x0) >> 1], fg_tint, alpha);
                if (dx >= r && dx < w - r) {
                    dst_row[px] = glass_px;
                } else {
                    int local_dx = (dx < r) ? dx : (w - 1 - dx);
                    float rx = fr - 0.5f - (float)local_dx;
                    float dist = dui_sqrtf(rx * rx + ry * ry);
                    float cov = fr + 0.5f - dist;
                    if (cov >= 1.0f) {
                        dst_row[px] = glass_px;
                    } else if (cov > 0.0f) {
                        uint8_t ca = (uint8_t)(cov * 255.0f + 0.5f);
                        dst_row[px] = blend_pixel(dst_row[px], glass_px, ca);
                    }
                }
            }
        }
    }

    // 5. Crisp 1px top specular glass rim
    if (h > 2) {
        int inset = imax(r, 3);
        if (w > 2 * inset) {
            compositor_fill_span(x + inset, x + w - inset, y + 1, 0x6680A8, 90);
        }
    }
}

void dui_shadow(int x, int y, int w, int h, int radius, int elevation) {
    if (!backbuffer || w <= 0 || h <= 0 || elevation <= 0) return;
    int spread = horizon_clampi(elevation, 1, 36);
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int sy_off = spread / 4;
    int max_alpha = horizon_clampi(40 + spread * 4, 48, 120);

    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);

    int wx0 = x;
    int wy0 = y;
    int wx1 = x + w;
    int wy1 = y + h;

    int sx0 = imax(x - spread, cx);
    int sy0 = imax(y - spread, cy);
    int sx1 = imin(x + w + spread, cx + cw);
    int sy1 = imin(y + h + spread + sy_off, cy + ch);
    if (sx0 >= sx1 || sy0 >= sy1) return;

    float inv_spread = 1.0f / (float)spread;
    float fr = (float)radius;
    float fsy_off = (float)sy_off;

    // 1. Top exterior strip: [sy0, min(sy1, wy0))
    int top_end = imin(sy1, wy0);
    for (int py = sy0; py < top_end; py++) {
        float eff_d = (float)(wy0 - py) + fsy_off;
        int mid_x0 = imax(sx0, wx0 + radius);
        int mid_x1 = imin(sx1, wx1 - radius);

        if (eff_d < (float)spread) {
            float t = 1.0f - eff_d * inv_spread;
            uint8_t a = (uint8_t)((float)max_alpha * t * t);
            if (a > 0 && mid_x0 < mid_x1) {
                compositor_fill_span(mid_x0, mid_x1, py, 0x000000, a);
            }
        }
        int lc_end = imin(sx1, wx0 + radius);
        float c_ly = (float)(wy0 + radius) - 0.5f;
        float c_lx = (float)(wx0 + radius) - 0.5f;
        for (int px = sx0; px < lc_end; px++) {
            float dx = c_lx - (float)px;
            float dy = c_ly - (float)py;
            float dist = (dui_sqrtf(dx * dx + dy * dy) - fr) + fsy_off;
            if (dist > 0.0f && dist < (float)spread) {
                float t = 1.0f - dist * inv_spread;
                uint8_t a = (uint8_t)((float)max_alpha * t * t);
                if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
            }
        }
        int rc_start = imax(sx0, wx1 - radius);
        float c_rx = (float)(wx1 - radius) - 0.5f;
        for (int px = rc_start; px < sx1; px++) {
            float dx = (float)px - c_rx;
            float dy = c_ly - (float)py;
            float dist = (dui_sqrtf(dx * dx + dy * dy) - fr) + fsy_off;
            if (dist > 0.0f && dist < (float)spread) {
                float t = 1.0f - dist * inv_spread;
                uint8_t a = (uint8_t)((float)max_alpha * t * t);
                if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
            }
        }
    }

    // 2. Bottom exterior strip: [max(sy0, wy1), sy1)
    int bot_start = imax(sy0, wy1);
    for (int py = bot_start; py < sy1; py++) {
        float raw_d = (float)(py - (wy1 - 1));
        float eff_d = fmax_val(0.0f, raw_d - fsy_off);
        int mid_x0 = imax(sx0, wx0 + radius);
        int mid_x1 = imin(sx1, wx1 - radius);

        if (eff_d < (float)spread) {
            float t = 1.0f - eff_d * inv_spread;
            uint8_t a = (uint8_t)((float)max_alpha * t * t);
            if (a > 0 && mid_x0 < mid_x1) {
                compositor_fill_span(mid_x0, mid_x1, py, 0x000000, a);
            }
        }
        int lc_end = imin(sx1, wx0 + radius);
        float c_by = (float)(wy1 - radius) - 0.5f;
        float c_lx = (float)(wx0 + radius) - 0.5f;
        for (int px = sx0; px < lc_end; px++) {
            float dx = c_lx - (float)px;
            float dy = (float)py - c_by;
            float dist = fmax_val(0.0f, (dui_sqrtf(dx * dx + dy * dy) - fr) - fsy_off);
            if (dist < (float)spread) {
                float t = 1.0f - dist * inv_spread;
                uint8_t a = (uint8_t)((float)max_alpha * t * t);
                if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
            }
        }
        int rc_start = imax(sx0, wx1 - radius);
        float c_rx = (float)(wx1 - radius) - 0.5f;
        for (int px = rc_start; px < sx1; px++) {
            float dx = (float)px - c_rx;
            float dy = (float)py - c_by;
            float dist = fmax_val(0.0f, (dui_sqrtf(dx * dx + dy * dy) - fr) - fsy_off);
            if (dist < (float)spread) {
                float t = 1.0f - dist * inv_spread;
                uint8_t a = (uint8_t)((float)max_alpha * t * t);
                if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
            }
        }
    }

    // 3. Left & Right side strips + rounded corner cutouts: [max(sy0, wy0), min(sy1, wy1))
    int mid_y0 = imax(sy0, wy0);
    int mid_y1 = imin(sy1, wy1);
    int left_end = imin(sx1, wx0);
    int right_start = imax(sx0, wx1);

    for (int py = mid_y0; py < mid_y1; py++) {
        bool in_top_r = (py < wy0 + radius);
        bool in_bot_r = (py >= wy1 - radius);

        if (!in_top_r && !in_bot_r) {
            for (int px = sx0; px < left_end; px++) {
                int d_x = wx0 - px;
                if (d_x < spread) {
                    float t = 1.0f - (float)d_x * inv_spread;
                    uint8_t a = (uint8_t)((float)max_alpha * t * t);
                    if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
                }
            }
            for (int px = right_start; px < sx1; px++) {
                int d_x = px - (wx1 - 1);
                if (d_x < spread) {
                    float t = 1.0f - (float)d_x * inv_spread;
                    uint8_t a = (uint8_t)((float)max_alpha * t * t);
                    if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
                }
            }
        } else {
            float cy_c = in_top_r ? ((float)(wy0 + radius) - 0.5f) : ((float)(wy1 - radius) - 0.5f);
            float dy = in_top_r ? (cy_c - (float)py) : ((float)py - cy_c);
            float bias = in_top_r ? fsy_off : -fsy_off;

            int lc_limit = imin(sx1, wx0 + radius);
            float c_lx = (float)(wx0 + radius) - 0.5f;
            for (int px = sx0; px < lc_limit; px++) {
                float dx = c_lx - (float)px;
                float raw_out = dui_sqrtf(dx * dx + dy * dy) - fr;
                if (raw_out <= 0.0f) continue;
                float eff = fmax_val(0.0f, raw_out + bias);
                if (eff < (float)spread) {
                    float t = 1.0f - eff * inv_spread;
                    uint8_t a = (uint8_t)((float)max_alpha * t * t);
                    if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
                }
            }

            int rc_limit = imax(sx0, wx1 - radius);
            float c_rx = (float)(wx1 - radius) - 0.5f;
            for (int px = rc_limit; px < sx1; px++) {
                float dx = (float)px - c_rx;
                float raw_out = dui_sqrtf(dx * dx + dy * dy) - fr;
                if (raw_out <= 0.0f) continue;
                float eff = fmax_val(0.0f, raw_out + bias);
                if (eff < (float)spread) {
                    float t = 1.0f - eff * inv_spread;
                    uint8_t a = (uint8_t)((float)max_alpha * t * t);
                    if (a > 0) put_pixel_alpha(px, py, 0x000000, a);
                }
            }
        }
    }
}

void dui_glass_card(int x, int y, int w, int h, int radius, uint32_t tint, uint8_t alpha, bool focused) {
    if (w <= 0 || h <= 0) return;
    dui_frosted_glass(x, y, w, h, radius, tint, alpha);
    int r_clamped = imin(radius, imin(w, h) / 2);
    if (w > 2 * r_clamped + 4) {
        // Directional top specular catchlight + bottom grounding shade (zero glow)
        compositor_fill_span(x + r_clamped + 1, x + w - r_clamped - 1, y + 1, 0xFFFFFF, focused ? 42 : 24);
        compositor_fill_span(x + r_clamped + 1, x + w - r_clamped - 1, y + h - 2, 0x000000, 48);
    }
    uint32_t border_col = focused ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE;
    uint8_t border_alpha = focused ? 215 : 165;
    dui_rect_rounded_outline(x, y, w, h, radius, border_col, border_alpha);
}

void dui_bento_card(int x, int y, int w, int h, int radius, uint32_t tint, uint8_t alpha, bool active) {
    if (w <= 0 || h <= 0) return;
    int r = (radius <= 0) ? THEME_RADIUS_LG : radius;
    dui_shadow(x, y + 2, w, h, r, active ? 2 : 1);
    dui_rect_rounded(x, y, w, h, r, tint, alpha);
    int rc = imin(r, imin(w, h) / 2);
    if (w > 2 * rc + 4) {
        compositor_fill_span(x + rc + 1, x + w - rc - 1, y + 1, 0xFFFFFF, active ? 46 : 26);
        compositor_fill_span(x + rc + 1, x + w - rc - 1, y + h - 2, 0x000000, 56);
    }
    uint32_t border = active ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE;
    dui_rect_rounded_outline(x, y, w, h, r, border, active ? 210 : 155);
}

void dui_dot_grid_overlay(int x, int y, int w, int h, int spacing, uint32_t color, uint8_t alpha) {
    if (w <= 0 || h <= 0 || spacing < 6) return;
    for (int py = y + spacing / 2; py < y + h - 2; py += spacing) {
        for (int px = x + spacing / 2; px < x + w - 2; px += spacing) {
            put_pixel_alpha(px, py, color, alpha);
        }
    }
}

// ============================================================================
// Cached TrueType Typography
// ============================================================================

static int get_atlas_index(int scale) {
    if (scale <= 1) return 0;
    if (scale == 2) return 1;
    return 2;
}

void dui_text(int x, int y, const char* text, uint32_t color, int scale) {
    if (!backbuffer || !text) return;

    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    int clip_x1 = cx + cw;
    int clip_y1 = cy + ch;

    if (!font_loaded) {
        int s = horizon_clampi(scale, 1, 4);
        int cur_x = x;
        while (*text) {
            char c = *text++;
            if (c >= GLYPH_FIRST && c <= GLYPH_LAST) {
                int idx = c - GLYPH_FIRST;
                for (int row = 0; row < 8; row++) {
                    uint8_t byte = font8x8_basic[idx][row];
                    for (int col = 0; col < 8; col++) {
                        if (byte & (0x80 >> col)) {
                            dui_rect(cur_x + col * s, y + row * s, s, s, color, 255);
                        }
                    }
                }
            }
            cur_x += 8 * s;
        }
        return;
    }

    int s_idx = get_atlas_index(scale);
    const FontAtlasScale* atlas = &font_atlas[s_idx];
    uint32_t fg = color & 0x00FFFFFFU;
    int baseline_y = y + atlas->ascent;
    int cur_x = x;

    while (*text) {
        unsigned char uc = (unsigned char)(*text++);
        if (uc < GLYPH_FIRST || uc > GLYPH_LAST) continue;
        const CachedGlyph* g = &atlas->glyphs[uc - GLYPH_FIRST];

        if (g->gw > 0 && g->gh > 0 && atlas->bitmap_pool) {
            int gx0 = cur_x + (int)g->xoff;
            int gy0 = baseline_y + (int)g->yoff;
            int gx1 = gx0 + (int)g->gw;
            int gy1 = gy0 + (int)g->gh;

            int dx0 = imax(gx0, cx);
            int dy0 = imax(gy0, cy);
            int dx1 = imin(gx1, clip_x1);
            int dy1 = imin(gy1, clip_y1);

            const uint8_t* src_base = atlas->bitmap_pool + g->offset;
            for (int py = dy0; py < dy1; py++) {
                int row_off = (py - gy0) * (int)g->gw;
                uint32_t* dst_row = backbuffer + (uint32_t)py * screen_width;
                for (int px = dx0; px < dx1; px++) {
                    uint8_t a = src_base[row_off + (px - gx0)];
                    if (a == 255) {
                        dst_row[px] = fg;
                    } else if (a > 0) {
                        dst_row[px] = blend_pixel(dst_row[px], fg, a);
                    }
                }
            }
        }
        cur_x += (int)g->advance;
    }
}

void dui_text_bold(int x, int y, const char* text, uint32_t color, int scale) {
    dui_text(x, y, text, color, scale);
    dui_text(x + 1, y, text, color, scale);
}

int dui_text_width(const char* text, int scale) {
    if (!text) return 0;
    if (!font_loaded) {
        int s = horizon_clampi(scale, 1, 4);
        int len = 0;
        while (*text++) len++;
        return len * 8 * s;
    }

    int s_idx = get_atlas_index(scale);
    const FontAtlasScale* atlas = &font_atlas[s_idx];
    int w = 0;
    while (*text) {
        unsigned char uc = (unsigned char)(*text++);
        if (uc >= GLYPH_FIRST && uc <= GLYPH_LAST) {
            w += (int)atlas->glyphs[uc - GLYPH_FIRST].advance;
        }
    }
    return w;
}

int dui_text_height(int scale) {
    if (!font_loaded) {
        return 8 * horizon_clampi(scale, 1, 4);
    }
    return font_atlas[get_atlas_index(scale)].px_size;
}

void dui_text_ellipsis(int x, int y, int max_w, const char* text, uint32_t color, int scale) {
    if (!text || max_w <= 0) return;
    if (dui_text_width(text, scale) <= max_w) {
        dui_text(x, y, text, color, scale);
        return;
    }

    int ell_w = dui_text_width("...", scale);
    if (ell_w > max_w) return;

    char buf[256];
    int out_len = 0;
    int cur_w = 0;
    int s_idx = get_atlas_index(scale);
    int fallback_adv = 8 * horizon_clampi(scale, 1, 4);

    while (text[out_len] && out_len < 250) {
        unsigned char uc = (unsigned char)text[out_len];
        int cw = 0;
        if (uc >= GLYPH_FIRST && uc <= GLYPH_LAST) {
            cw = font_loaded ? (int)font_atlas[s_idx].glyphs[uc - GLYPH_FIRST].advance : fallback_adv;
        }
        if (cur_w + cw + ell_w > max_w) break;
        buf[out_len] = text[out_len];
        cur_w += cw;
        out_len++;
    }
    buf[out_len++] = '.';
    buf[out_len++] = '.';
    buf[out_len++] = '.';
    buf[out_len] = '\0';
    dui_text(x, y, buf, color, scale);
}

// ============================================================================
// System & App Icon Families
// ============================================================================

static void draw_thick_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    if (thickness <= 1) {
        dui_line(x0, y0, x1, y1, color, 255);
        return;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int half = thickness / 2;
    for (int t = -half; t < thickness - half; t++) {
        if (dx >= dy) {
            dui_line(x0, y0 + t, x1, y1 + t, color, 255);
        } else {
            dui_line(x0 + t, y0, x1 + t, y1, color, 255);
        }
    }
}

void dui_draw_sys_icon(int x, int y, int size, int sys_icon_id, uint32_t color) {
    if (size < 8) return;
    int cx = x + size / 2;
    int cy = y + size / 2;
    int t = (size >= 24) ? 2 : 1;

    switch (sys_icon_id) {
        case 0: { // search
            int r = imax(3, size * 24 / 100);
            int gx = x + size * 40 / 100;
            int gy = y + size * 40 / 100;
            dui_circle_outline(gx, gy, r, color, 255);
            if (t > 1) dui_circle_outline(gx, gy, r - 1, color, 180);
            int sx = gx + (r * 7) / 10;
            int sy = gy + (r * 7) / 10;
            draw_thick_line(sx, sy, x + size * 82 / 100, y + size * 82 / 100, color, t);
            break;
        }
        case 1: { // wifi
            int wy = y + size * 76 / 100;
            dui_circle(cx, wy, imax(1, size / 12), color, 255);
            for (int arc = 1; arc <= 3; arc++) {
                int r = (size * arc * 22) / 100;
                for (int dy = -r; dy < 0; dy++) {
                    for (int dx = -r; dx <= r; dx++) {
                        int d2 = dx * dx + dy * dy;
                        int adx = (dx < 0) ? -dx : dx;
                        if (adx <= -dy + 1 && d2 >= (r - t) * (r - t) && d2 <= r * r) {
                            put_pixel_alpha(cx + dx, wy + dy, color, 255);
                        }
                    }
                }
            }
            break;
        }
        case 2: { // volume
            int bw = imax(2, size * 18 / 100);
            int bh = imax(4, size * 30 / 100);
            dui_rect(x + size * 16 / 100, cy - bh / 2, bw, bh, color, 255);
            dui_triangle(
                x + size * 30 / 100, cy,
                x + size * 54 / 100, y + size * 20 / 100,
                x + size * 54 / 100, y + size * 80 / 100,
                color, 255
            );
            int w1 = x + size * 66 / 100;
            int w2 = x + size * 80 / 100;
            draw_thick_line(w1, cy - size / 6, w1 + 2, cy, color, 1);
            draw_thick_line(w1 + 2, cy, w1, cy + size / 6, color, 1);
            draw_thick_line(w2, cy - size / 4, w2 + 2, cy, color, 1);
            draw_thick_line(w2 + 2, cy, w2, cy + size / 4, color, 1);
            break;
        }
        case 3: { // battery
            int bx = x + size * 12 / 100;
            int by = y + size * 28 / 100;
            int bw = size * 68 / 100;
            int bh = size * 44 / 100;
            dui_rect_rounded_outline(bx, by, bw, bh, 2, color, 255);
            dui_rect(bx + bw, cy - imax(2, bh / 4), imax(2, size / 12), imax(3, bh / 2), color, 255);
            dui_rect(bx + 2, by + 2, imax(2, bw - 6), imax(2, bh - 4), color, 220);
            break;
        }
        case 4: { // gear
            int r = imax(3, size * 26 / 100);
            dui_circle_outline(cx, cy, r, color, 255);
            dui_circle_outline(cx, cy, imax(1, r / 2), color, 220);
            int tooth = imax(2, size / 8);
            int d_diag = (r * 75) / 100;
            dui_rect(cx - 1, cy - r - tooth, 2, tooth + 1, color, 255);
            dui_rect(cx - 1, cy + r, 2, tooth + 1, color, 255);
            dui_rect(cx - r - tooth, cy - 1, tooth + 1, 2, color, 255);
            dui_rect(cx + r, cy - 1, tooth + 1, 2, color, 255);
            dui_rect(cx - d_diag - 2, cy - d_diag - 2, 2, 2, color, 255);
            dui_rect(cx + d_diag + 1, cy - d_diag - 2, 2, 2, color, 255);
            dui_rect(cx - d_diag - 2, cy + d_diag + 1, 2, 2, color, 255);
            dui_rect(cx + d_diag + 1, cy + d_diag + 1, 2, 2, color, 255);
            break;
        }
        case 5: { // folder
            int fx = x + size * 12 / 100;
            int fy = y + size * 22 / 100;
            int fw = size * 76 / 100;
            int fh = size * 58 / 100;
            dui_rect_rounded(fx, fy - size / 12, fw * 42 / 100, size / 5, 2, color, 230);
            dui_rect_rounded_outline(fx, fy, fw, fh, 2, color, 255);
            dui_rect(fx + 1, fy + 1, fw - 2, imax(2, fh / 4), color, 140);
            break;
        }
        case 6: { // globe
            int r = imax(4, size * 36 / 100);
            dui_circle_outline(cx, cy, r, color, 255);
            dui_line(cx - r, cy, cx + r, cy, color, 220);
            dui_line(cx, cy - r, cx, cy + r, color, 220);
            dui_line(cx - r / 2, cy - r * 3 / 4, cx - r / 2, cy + r * 3 / 4, color, 150);
            dui_line(cx + r / 2, cy - r * 3 / 4, cx + r / 2, cy + r * 3 / 4, color, 150);
            break;
        }
        case 7: { // terminal
            dui_rect_rounded_outline(x + size * 10 / 100, y + size * 16 / 100, size * 80 / 100, size * 68 / 100, 3, color, 220);
            draw_thick_line(x + size * 26 / 100, y + size * 34 / 100, x + size * 44 / 100, cy, color, t);
            draw_thick_line(x + size * 44 / 100, cy, x + size * 26 / 100, y + size * 66 / 100, color, t);
            draw_thick_line(x + size * 52 / 100, y + size * 66 / 100, x + size * 74 / 100, y + size * 66 / 100, color, t);
            break;
        }
        case 8: { // download
            draw_thick_line(cx, y + size * 18 / 100, cx, y + size * 64 / 100, color, t);
            draw_thick_line(cx - size * 18 / 100, y + size * 46 / 100, cx, y + size * 64 / 100, color, t);
            draw_thick_line(cx + size * 18 / 100, y + size * 46 / 100, cx, y + size * 64 / 100, color, t);
            draw_thick_line(x + size * 20 / 100, y + size * 78 / 100, x + size * 80 / 100, y + size * 78 / 100, color, t);
            draw_thick_line(x + size * 20 / 100, y + size * 66 / 100, x + size * 20 / 100, y + size * 78 / 100, color, t);
            draw_thick_line(x + size * 80 / 100, y + size * 66 / 100, x + size * 80 / 100, y + size * 78 / 100, color, t);
            break;
        }
        case 9: { // lock
            int bw = size * 56 / 100;
            int bh = size * 40 / 100;
            int bx = cx - bw / 2;
            int by = y + size * 46 / 100;
            int sr = imax(2, bw * 34 / 100);
            for (int dy = -sr; dy <= 0; dy++) {
                for (int dx = -sr; dx <= sr; dx++) {
                    int d2 = dx * dx + dy * dy;
                    if (d2 >= (sr - t) * (sr - t) && d2 <= sr * sr) {
                        put_pixel_alpha(cx + dx, by + dy, color, 255);
                    }
                }
            }
            dui_rect_rounded(bx, by, bw, bh, 2, color, 235);
            dui_circle(cx, by + bh / 2, imax(1, size / 14), THEME_BG_VOID, 255);
            break;
        }
        case 10: { // power
            int r = imax(4, size * 32 / 100);
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (dy < -r / 3 && dx > -r / 2 && dx < r / 2) continue;
                    int d2 = dx * dx + dy * dy;
                    if (d2 >= (r - t) * (r - t) && d2 <= r * r) {
                        put_pixel_alpha(cx + dx, cy + dy, color, 255);
                    }
                }
            }
            draw_thick_line(cx, y + size * 14 / 100, cx, cy, color, t);
            break;
        }
        case 11: { // check
            draw_thick_line(x + size * 20 / 100, cy + size / 16, x + size * 42 / 100, y + size * 72 / 100, color, t);
            draw_thick_line(x + size * 42 / 100, y + size * 72 / 100, x + size * 80 / 100, y + size * 28 / 100, color, t);
            break;
        }
        case 12: { // close
            draw_thick_line(x + size * 26 / 100, y + size * 26 / 100, x + size * 74 / 100, y + size * 74 / 100, color, t);
            draw_thick_line(x + size * 74 / 100, y + size * 26 / 100, x + size * 26 / 100, y + size * 74 / 100, color, t);
            break;
        }
        case 13: { // plus
            draw_thick_line(x + size * 22 / 100, cy, x + size * 78 / 100, cy, color, t);
            draw_thick_line(cx, y + size * 22 / 100, cx, y + size * 78 / 100, color, t);
            break;
        }
        case 14: { // back
            draw_thick_line(x + size * 64 / 100, y + size * 22 / 100, x + size * 32 / 100, cy, color, t);
            draw_thick_line(x + size * 32 / 100, cy, x + size * 64 / 100, y + size * 78 / 100, color, t);
            break;
        }
        case 15: { // forward
            draw_thick_line(x + size * 36 / 100, y + size * 22 / 100, x + size * 68 / 100, cy, color, t);
            draw_thick_line(x + size * 68 / 100, cy, x + size * 36 / 100, y + size * 78 / 100, color, t);
            break;
        }
        case 16: { // refresh
            int r = imax(4, size * 32 / 100);
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (dx > 0 && dy > 0 && dx > dy) continue; // open gap bottom-right
                    int d2 = dx * dx + dy * dy;
                    if (d2 >= (r - t) * (r - t) && d2 <= r * r) {
                        put_pixel_alpha(cx + dx, cy + dy, color, 255);
                    }
                }
            }
            int ax = cx + r;
            int ay = cy - r / 3;
            draw_thick_line(ax - size / 6, ay, ax, ay, color, t);
            draw_thick_line(ax, ay - size / 6, ax, ay, color, t);
            break;
        }
        case 17: { // star
            int r_out = imax(4, size * 38 / 100);
            static const int sx[10] = { 0, 22, 95, 36, 59, 0, -59, -36, -95, -22 };
            static const int sy[10] = { -100, -31, -31, 12, 81, 38, 81, 12, -31, -31 };
            int px[10], py[10];
            for (int i = 0; i < 10; i++) {
                px[i] = cx + (sx[i] * r_out) / 100;
                py[i] = cy + (sy[i] * r_out) / 100;
            }
            for (int i = 0; i < 10; i++) {
                int n = (i + 1) % 10;
                draw_thick_line(px[i], py[i], px[n], py[n], color, 1);
            }
            break;
        }
        case 18: { // bolt
            int x_top = cx + size * 10 / 100;
            int x_mid_l = cx - size * 20 / 100;
            int x_mid_r = cx + size * 20 / 100;
            int x_bot = cx - size * 10 / 100;
            draw_thick_line(x_top, y + size * 14 / 100, x_mid_l, cy + 1, color, t);
            draw_thick_line(x_mid_l, cy + 1, x_mid_r, cy - 1, color, t);
            draw_thick_line(x_mid_r, cy - 1, x_bot, y + size * 86 / 100, color, t);
            break;
        }
        case 19:   // shield
        default: {
            int sw = size * 64 / 100;
            int lx = cx - sw / 2;
            int rx = cx + sw / 2;
            int ty = y + size * 18 / 100;
            int my = cy + size / 12;
            int by = y + size * 84 / 100;
            draw_thick_line(lx, ty, cx, ty - size / 14, color, t);
            draw_thick_line(cx, ty - size / 14, rx, ty, color, t);
            draw_thick_line(lx, ty, lx, my, color, t);
            draw_thick_line(rx, ty, rx, my, color, t);
            draw_thick_line(lx, my, cx, by, color, t);
            draw_thick_line(rx, my, cx, by, color, t);
            break;
        }
    }
}

void dui_draw_squircle_icon(int x, int y, int size, int app_id, bool hovered) {
    if (size < 12) return;
    int r = imax(4, (size * 24) / 100);

    uint32_t top_col = 0x38BDF8;
    uint32_t bot_col = 0x0284C7;
    switch (app_id) {
        case 0: top_col = 0x38BDF8; bot_col = 0x0284C7; break; // Files
        case 1: top_col = 0x60A5FA; bot_col = 0x1D4ED8; break; // Browser
        case 2: top_col = 0x64748B; bot_col = 0x334155; break; // Settings
        case 3: top_col = 0x38BDF8; bot_col = 0x4F46E5; break; // Store
        case 4: top_col = 0x1E293B; bot_col = 0x090D16; break; // Terminal
        case 5: top_col = 0x10B981; bot_col = 0x047857; break; // Monitor
        case 6: top_col = 0xFB923C; bot_col = 0xEA580C; break; // Calculator
        case 7: top_col = 0x38BDF8; bot_col = 0x0369A1; break; // Weather
        case 8: top_col = 0xA78BFA; bot_col = 0x6D28D9; break; // RegEdit
        case 9: top_col = 0x34D399; bot_col = 0x059669; break; // Updater
        default: top_col = 0x818CF8; bot_col = 0x0D9488; break; // Generic App
    }

    if (hovered) {
        top_col = horizon_lerp_color(top_col, 0xFFFFFF, 0.14f);
        bot_col = horizon_lerp_color(bot_col, 0xFFFFFF, 0.08f);
    }

    dui_gradient_rounded(x, y, size, size, r, top_col, bot_col, 255);

    // Tactile top inner specular catchlight + crisp hairline (zero outer neon glow)
    if (size >= 24) {
        dui_rect_rounded(x + 2, y + 2, size - 4, imax(2, size / 6), imax(2, r - 2), 0xFFFFFF, hovered ? 42 : 28);
    }
    dui_rect_rounded_outline(x, y, size, size, r, 0xFFFFFF, hovered ? 145 : 70);

    int cx = x + size / 2;
    int cy = y + size / 2;
    int lt = imax(2, size / 14);

    switch (app_id) {
        case 0: { // Files
            int fw = size * 60 / 100;
            int fh = size * 44 / 100;
            int fx = cx - fw / 2;
            int fy = cy - fh / 2 + size / 20;
            dui_rect_rounded(fx, fy - size / 10, fw * 42 / 100, size / 6, 2, 0xE0F2FE, 255);
            dui_rect_rounded(fx, fy, fw, fh, imax(2, size / 12), 0xFFFFFF, 255);
            dui_rect(fx + 3, fy + fh / 3, fw - 6, imax(2, size / 18), 0x38BDF8, 190);
            break;
        }
        case 1: { // Browser compass
            int cr = imax(4, size * 32 / 100);
            dui_circle_outline(cx, cy, cr, 0xFFFFFF, 230);
            dui_circle(cx, cy, imax(2, cr - 2), 0x0F172A, 150);
            draw_thick_line(cx, cy, cx + cr * 55 / 100, cy - cr * 55 / 100, 0xFB7185, lt);
            draw_thick_line(cx, cy, cx - cr * 55 / 100, cy + cr * 55 / 100, 0xF8FAFC, lt);
            dui_circle(cx, cy, imax(1, size / 16), 0xFFFFFF, 255);
            break;
        }
        case 2: { // Settings gear
            int gr = imax(4, size * 24 / 100);
            for (int a = 0; a < 8; a++) {
                static const int ox[8] = { 0, 71, 100, 71, 0, -71, -100, -71 };
                static const int oy[8] = { -100, -71, 0, 71, 100, 71, 0, -71 };
                int tx = cx + (ox[a] * (gr + size / 14)) / 100;
                int ty = cy + (oy[a] * (gr + size / 14)) / 100;
                dui_circle(tx, ty, imax(1, size / 14), 0xE2E8F0, 255);
            }
            dui_circle(cx, cy, gr, 0xF8FAFC, 255);
            dui_circle(cx, cy, imax(2, gr / 2), 0x334155, 255);
            break;
        }
        case 3: { // App Store stylized 'A'
            draw_thick_line(cx - size * 20 / 100, cy + size * 22 / 100, cx, cy - size * 24 / 100, 0xFFFFFF, lt);
            draw_thick_line(cx + size * 20 / 100, cy + size * 22 / 100, cx, cy - size * 24 / 100, 0xFFFFFF, lt);
            draw_thick_line(cx - size * 22 / 100, cy + size * 8 / 100, cx + size * 22 / 100, cy + size * 8 / 100, 0xFFFFFF, lt);
            break;
        }
        case 4: { // Terminal >_
            int pad = size * 16 / 100;
            dui_rect_rounded(x + pad, y + pad, size - 2 * pad, size - 2 * pad, imax(2, size / 12), 0x05080F, 220);
            draw_thick_line(cx - size * 18 / 100, cy - size * 12 / 100, cx - size * 4 / 100, cy, 0x34D399, lt);
            draw_thick_line(cx - size * 4 / 100, cy, cx - size * 18 / 100, cy + size * 12 / 100, 0x34D399, lt);
            draw_thick_line(cx + size * 2 / 100, cy + size * 12 / 100, cx + size * 20 / 100, cy + size * 12 / 100, 0x34D399, lt);
            break;
        }
        case 5: { // Monitor pulse waveform
            int pad = size * 16 / 100;
            dui_rect_rounded(x + pad, y + pad, size - 2 * pad, size - 2 * pad, imax(2, size / 12), 0x064E3B, 200);
            int p0 = x + size * 22 / 100;
            int p1 = x + size * 36 / 100;
            int p2 = x + size * 48 / 100;
            int p3 = x + size * 60 / 100;
            int p4 = x + size * 78 / 100;
            draw_thick_line(p0, cy, p1, cy, 0xA7F3D0, lt);
            draw_thick_line(p1, cy, p2, cy - size * 22 / 100, 0xFFFFFF, lt);
            draw_thick_line(p2, cy - size * 22 / 100, p3, cy + size * 22 / 100, 0xFFFFFF, lt);
            draw_thick_line(p3, cy + size * 22 / 100, p4, cy, 0xA7F3D0, lt);
            break;
        }
        case 6: { // Calculator
            int pad = size * 20 / 100;
            dui_rect_rounded(x + pad, y + pad, size - 2 * pad, imax(3, size * 16 / 100), 2, 0x1E293B, 230);
            int bs = imax(3, size * 18 / 100);
            int gx0 = cx - bs - size / 16;
            int gx1 = cx + size / 16;
            int gy0 = cy - size / 20;
            int gy1 = cy + bs + size / 16;
            dui_rect_rounded(gx0, gy0, bs, bs, 2, 0xFFFFFF, 240);
            dui_rect_rounded(gx1, gy0, bs, bs, 2, 0xFFFFFF, 240);
            dui_rect_rounded(gx0, gy1, bs, bs, 2, 0xFFFFFF, 240);
            dui_rect_rounded(gx1, gy1, bs, bs, 2, 0xFEF08A, 255);
            break;
        }
        case 7: { // Weather sun + cloud
            dui_circle(cx + size * 12 / 100, cy - size * 12 / 100, imax(3, size * 18 / 100), 0xFBBF24, 255);
            int cr = imax(2, size * 14 / 100);
            dui_circle(cx - size * 12 / 100, cy + size * 10 / 100, cr, 0xFFFFFF, 255);
            dui_circle(cx + size * 4 / 100, cy + size * 4 / 100, imax(3, size * 17 / 100), 0xFFFFFF, 255);
            dui_circle(cx + size * 18 / 100, cy + size * 12 / 100, imax(2, size * 12 / 100), 0xFFFFFF, 255);
            dui_rect_rounded(cx - size * 18 / 100, cy + size * 8 / 100, size * 40 / 100, imax(3, size * 18 / 100), 3, 0xFFFFFF, 255);
            break;
        }
        case 8: { // RegEdit hierarchy/node tree
            int nr = imax(2, size / 10);
            int top_y = y + size * 28 / 100;
            int bot_y = y + size * 68 / 100;
            int lx = cx - size * 20 / 100;
            int rx = cx + size * 20 / 100;
            draw_thick_line(cx, top_y, lx, bot_y, 0xE9D5FF, lt);
            draw_thick_line(cx, top_y, rx, bot_y, 0xE9D5FF, lt);
            dui_circle(cx, top_y, nr, 0x38BDF8, 255);
            dui_circle(lx, bot_y, nr, 0xFFFFFF, 255);
            dui_circle(rx, bot_y, nr, 0xFFFFFF, 255);
            break;
        }
        case 9: { // Updater
            dui_circle_outline(cx, cy, imax(4, size * 28 / 100), 0xFFFFFF, 220);
            draw_thick_line(cx, cy - size * 16 / 100, cx, cy + size * 16 / 100, 0xFFFFFF, lt);
            draw_thick_line(cx - size * 14 / 100, cy + size * 2 / 100, cx, cy + size * 16 / 100, 0xFFFFFF, lt);
            draw_thick_line(cx + size * 14 / 100, cy + size * 2 / 100, cx, cy + size * 16 / 100, 0xFFFFFF, lt);
            break;
        }
        default: { // Generic App Diamond
            int d = imax(4, size * 26 / 100);
            draw_thick_line(cx, cy - d, cx + d, cy, 0xFFFFFF, lt);
            draw_thick_line(cx + d, cy, cx, cy + d, 0xFFFFFF, lt);
            draw_thick_line(cx, cy + d, cx - d, cy, 0xFFFFFF, lt);
            draw_thick_line(cx - d, cy, cx, cy - d, 0xFFFFFF, lt);
            dui_circle(cx, cy, imax(2, size / 12), 0xFFFFFF, 230);
            break;
        }
    }
}

void dui_draw_icon(int x, int y, int size, int icon_id) {
    dui_draw_squircle_icon(x, y, size, icon_id, false);
}

// ============================================================================
// STB Image Loader & Cached Blitter
// ============================================================================

#define IMAGE_CACHE_SLOTS 4
typedef struct {
    char path[128];
    uint8_t* rgba;
    int w;
    int h;
    uint32_t stamp;
} CachedImage;

static CachedImage g_img_cache[IMAGE_CACHE_SLOTS];
static uint32_t g_img_stamp = 1;

static void ascii_to_wpath(const char* src, CHAR16* dst, int max_chars) {
    int i = 0;
    while (src[i] && i < max_chars - 1) {
        char c = src[i];
        if (c == '/') c = '\\';
        dst[i] = (CHAR16)(unsigned char)c;
        i++;
    }
    dst[i] = L'\0';
}

static bool str_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static void str_copy_lim(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

bool dui_draw_image_path(const char* path, int x, int y, int w, int h) {
    if (!backbuffer || !path || !path[0] || w <= 0 || h <= 0) return false;

    const CachedImage* found = NULL;
    for (int i = 0; i < IMAGE_CACHE_SLOTS; i++) {
        if (g_img_cache[i].rgba && str_eq(g_img_cache[i].path, path)) {
            g_img_cache[i].stamp = ++g_img_stamp;
            found = &g_img_cache[i];
            break;
        }
    }

    if (!found) {
        CHAR16 wpath[128];
        ascii_to_wpath(path, wpath, 128);

        UINTN raw_sz = 0;
        uint8_t* raw = read_uefi_file(g_dui_st, wpath, &raw_sz);
        if (!raw || raw_sz == 0) {
            if (raw) uefi_free(raw);
            return false;
        }

        int iw = 0, ih = 0, comp = 0;
        uint8_t* rgba = stbi_load_from_memory(raw, (int)raw_sz, &iw, &ih, &comp, 4);
        uefi_free(raw);

        if (!rgba || iw <= 0 || ih <= 0) {
            if (rgba) stbi_image_free(rgba);
            return false;
        }

        // Evict LRU slot
        int slot = 0;
        uint32_t oldest = 0xFFFFFFFFU;
        for (int i = 0; i < IMAGE_CACHE_SLOTS; i++) {
            if (!g_img_cache[i].rgba) {
                slot = i;
                break;
            }
            if (g_img_cache[i].stamp < oldest) {
                oldest = g_img_cache[i].stamp;
                slot = i;
            }
        }
        if (g_img_cache[slot].rgba) {
            stbi_image_free(g_img_cache[slot].rgba);
        }
        str_copy_lim(g_img_cache[slot].path, path, 128);
        g_img_cache[slot].rgba = rgba;
        g_img_cache[slot].w = iw;
        g_img_cache[slot].h = ih;
        g_img_cache[slot].stamp = ++g_img_stamp;
        found = &g_img_cache[slot];
    }

    int cx, cy, cw, ch;
    compositor_get_clip(&cx, &cy, &cw, &ch);
    int x0 = imax(x, cx);
    int y0 = imax(y, cy);
    int x1 = imin(x + w, cx + cw);
    int y1 = imin(y + h, cy + ch);
    if (x0 >= x1 || y0 >= y1) return true;

    const uint8_t* src = found->rgba;
    int iw = found->w;
    int ih = found->h;

    // 16.8 fixed-point bilinear scaled blit
    for (int py = y0; py < y1; py++) {
        int sy_fp = ((py - y) * ih * 256) / h;
        int sy0 = sy_fp >> 8;
        int fy = sy_fp & 0xFF;
        if (sy0 >= ih) sy0 = ih - 1;
        int sy1 = (sy0 + 1 < ih) ? (sy0 + 1) : sy0;

        const uint8_t* row0 = src + (sy0 * iw) * 4;
        const uint8_t* row1 = src + (sy1 * iw) * 4;
        uint32_t* dst_row = backbuffer + (uint32_t)py * screen_width;

        for (int px = x0; px < x1; px++) {
            int sx_fp = ((px - x) * iw * 256) / w;
            int sx0 = sx_fp >> 8;
            int fx = sx_fp & 0xFF;
            if (sx0 >= iw) sx0 = iw - 1;
            int sx1 = (sx0 + 1 < iw) ? (sx0 + 1) : sx0;

            const uint8_t* p00 = row0 + sx0 * 4;
            const uint8_t* p01 = row0 + sx1 * 4;
            const uint8_t* p10 = row1 + sx0 * 4;
            const uint8_t* p11 = row1 + sx1 * 4;

            int ifx = 256 - fx;
            int ify = 256 - fy;
            int w00 = ifx * ify;
            int w01 = fx * ify;
            int w10 = ifx * fy;
            int w11 = fx * fy;

            uint32_t rr = (uint32_t)((p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 16);
            uint32_t gg = (uint32_t)((p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 16);
            uint32_t bb = (uint32_t)((p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 16);
            uint8_t aa = (uint8_t)((p00[3] * w00 + p01[3] * w01 + p10[3] * w10 + p11[3] * w11) >> 16);

            uint32_t col = ((rr & 0xFFU) << 16) | ((gg & 0xFFU) << 8) | (bb & 0xFFU);
            if (aa == 255) {
                dst_row[px] = col;
            } else if (aa > 0) {
                dst_row[px] = blend_pixel(dst_row[px], col, aa);
            }
        }
    }
    return true;
}

// ============================================================================
// Cohesive Horizon Interactive Controls
// ============================================================================

static bool g_dui_pointer_down = false;

void dui_set_pointer_down(bool down) {
    g_dui_pointer_down = down;
}

bool dui_button_tactile(int x, int y, int w, int h, const char* label, uint32_t accent, bool primary, int mx, int my, bool down, bool clicked) {
    if (w <= 0 || h <= 0) return false;
    bool hovered = (mx >= x && mx < x + w && my >= y && my < y + h);
    bool pressed = hovered && down;
    int r = (h <= 28) ? THEME_RADIUS_SM : ((h >= 34) ? THEME_RADIUS_PILL : THEME_RADIUS_MD);
    int rc = imin(r, imin(w, h) / 2);
    int by = pressed ? (y + 1) : y;

    uint32_t text_col = THEME_TEXT_PRIMARY;
    if (primary) {
        uint32_t base = accent;
        if (pressed) base = horizon_lerp_color(base, 0x000000, 0.16f);
        else if (hovered) base = horizon_lerp_color(base, 0xFFFFFF, 0.12f);

        uint32_t c_top = horizon_lerp_color(base, 0xFFFFFF, pressed ? 0.04f : 0.16f);
        uint32_t c_bot = horizon_lerp_color(base, 0x000000, pressed ? 0.28f : 0.20f);
        dui_gradient_rounded(x, by, w, h, rc, c_top, c_bot, 255);
        if (w > 2 * rc + 2) {
            compositor_fill_span(x + rc, x + w - rc, by + 1, 0xFFFFFF, pressed ? 32 : 85);
            compositor_fill_span(x + rc, x + w - rc, by + h - 2, 0x000000, pressed ? 24 : 55);
        }
        dui_rect_rounded_outline(x, by, w, h, rc, horizon_lerp_color(base, 0xFFFFFF, 0.34f), hovered ? 215 : 155);

        int ar = (accent >> 16) & 0xFF;
        int ag = (accent >> 8) & 0xFF;
        int ab = accent & 0xFF;
        int lum = (ar * 299 + ag * 587 + ab * 114) / 1000;
        text_col = (lum > 140) ? THEME_TEXT_ON_ACCENT : THEME_TEXT_PRIMARY;
    } else {
        uint32_t top_col = pressed ? THEME_SURFACE_BASE : (hovered ? horizon_lerp_color(THEME_SURFACE_HOVER, 0xFFFFFF, 0.06f) : THEME_SURFACE_ELEVATED);
        uint32_t bot_col = pressed ? THEME_SURFACE_INPUT : (hovered ? THEME_SURFACE_CARD : horizon_lerp_color(THEME_SURFACE_CARD, 0x000000, 0.10f));
        dui_gradient_rounded(x, by, w, h, rc, top_col, bot_col, 240);
        if (w > 2 * rc + 2) {
            compositor_fill_span(x + rc, x + w - rc, by + 1, 0xFFFFFF, pressed ? 16 : (hovered ? 48 : 28));
            compositor_fill_span(x + rc, x + w - rc, by + h - 2, 0x000000, 50);
        }
        dui_rect_rounded_outline(x, by, w, h, rc, hovered ? THEME_BORDER_SPECULAR : THEME_BORDER_SUBTLE, hovered ? 215 : 175);
    }

    if (label && label[0]) {
        int tw = dui_text_width(label, 1);
        int th = dui_text_height(1);
        int tx = x + (w - tw) / 2;
        int ty = by + (h - th) / 2;
        if (tw > w - 12) {
            dui_text_ellipsis(x + 6, ty, w - 12, label, text_col, 1);
        } else {
            dui_text(tx, ty, label, text_col, 1);
        }
    }
    return hovered && clicked;
}

bool dui_button(int x, int y, int w, int h, const char* label, uint32_t accent, bool primary, int mx, int my, bool clicked) {
    return dui_button_tactile(x, y, w, h, label, accent, primary, mx, my, g_dui_pointer_down, clicked);
}

bool dui_icon_pill(int x, int y, int w, int h, const char* label, int sys_icon, bool active, int mx, int my, bool clicked) {
    if (w <= 0 || h <= 0) return false;
    bool hovered = (mx >= x && mx < x + w && my >= y && my < y + h);
    bool pressed = hovered && g_dui_pointer_down;
    int r = h / 2;
    int by = pressed ? (y + 1) : y;

    uint32_t top_col = THEME_SURFACE_CARD;
    uint32_t bot_col = THEME_SURFACE_BASE;
    uint8_t bg_a = 200;
    uint32_t border = THEME_BORDER_SUBTLE;
    uint8_t border_a = 150;
    uint32_t fg = THEME_TEXT_SECONDARY;

    if (active) {
        top_col = horizon_lerp_color(THEME_SURFACE_ELEVATED, THEME_ACCENT_CYAN, 0.24f);
        bot_col = horizon_lerp_color(THEME_SURFACE_CARD, THEME_ACCENT_CYAN, 0.14f);
        bg_a = 240;
        border = THEME_BORDER_SPECULAR;
        border_a = 220;
        fg = THEME_TEXT_PRIMARY;
    } else if (hovered) {
        top_col = pressed ? THEME_SURFACE_BASE : THEME_SURFACE_HOVER;
        bot_col = pressed ? THEME_SURFACE_INPUT : THEME_SURFACE_ELEVATED;
        bg_a = 235;
        border = THEME_BORDER_SPECULAR;
        border_a = 195;
        fg = THEME_TEXT_PRIMARY;
    }

    dui_gradient_rounded(x, by, w, h, r, top_col, bot_col, bg_a);
    if (w > 2 * r + 2) {
        compositor_fill_span(x + r, x + w - r, by + 1, 0xFFFFFF, active ? 46 : (hovered ? 34 : 20));
    }
    dui_rect_rounded_outline(x, by, w, h, r, border, border_a);

    int pad_x = imax(8, h / 3);
    int tx = x + pad_x;
    if (sys_icon >= 0) {
        int icon_sz = (h >= 24) ? 14 : 12;
        dui_draw_sys_icon(tx, by + (h - icon_sz) / 2, icon_sz, sys_icon, active ? THEME_ACCENT_CYAN : fg);
        tx += icon_sz + 6;
    }

    if (label && label[0]) {
        int ty = by + (h - dui_text_height(1)) / 2;
        int avail = (x + w - pad_x) - tx;
        if (avail > 8) {
            dui_text_ellipsis(tx, ty, avail, label, fg, 1);
        }
    }

    return hovered && clicked;
}

void dui_status_pill(int x, int y, int w, int h, const char* label, uint32_t dot_color, uint32_t bg, bool active) {
    if (w <= 0 || h <= 0) return;
    int r = h / 2;
    dui_rect_rounded(x, y, w, h, r, bg, active ? 235 : 195);
    if (w > 2 * r + 2) {
        compositor_fill_span(x + r, x + w - r, y + 1, 0xFFFFFF, 26);
    }
    dui_rect_rounded_outline(x, y, w, h, r, active ? dot_color : THEME_BORDER_SUBTLE, active ? 180 : 150);

    int pad = imax(8, h / 3);
    dui_circle(x + pad + 3, y + h / 2, 3, dot_color, 255);
    if (label && label[0]) {
        int ty = y + (h - dui_text_height(1)) / 2;
        dui_text_ellipsis(x + pad + 11, ty, w - pad * 2 - 12, label, THEME_TEXT_PRIMARY, 1);
    }
}

void dui_eq_bars(int x, int y, int w, int h, const int* bands, int count, uint32_t accent, bool active) {
    if (w <= 0 || h <= 0 || !bands || count <= 0) return;
    int gap = 2;
    int bw = (w - (count - 1) * gap) / count;
    if (bw < 2) bw = 2;
    int total_w = count * bw + (count - 1) * gap;
    int sx = x + (w - total_w) / 2;
    for (int i = 0; i < count; i++) {
        int lvl = bands[i];
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        int min_bh = imax(3, h / 5);
        int bh = active ? (min_bh + ((h - min_bh) * lvl) / 100) : min_bh;
        int bx = sx + i * (bw + gap);
        int by = y + (h - bh) / 2;
        uint32_t col = active ? accent : THEME_TEXT_MUTED;
        dui_rect_rounded(bx, by, bw, bh, 1, col, active ? 245 : 140);
    }
}

void dui_toggle_switch(int x, int y, bool on, uint32_t accent) {
    const int w = 40;
    const int h = 22;
    const int r = 11;

    if (on) {
        uint32_t c_top = horizon_lerp_color(accent, 0xFFFFFF, 0.14f);
        uint32_t c_bot = horizon_lerp_color(accent, 0x000000, 0.18f);
        dui_gradient_rounded(x, y, w, h, r, c_top, c_bot, 255);
        dui_rect_rounded_outline(x, y, w, h, r, horizon_lerp_color(accent, 0xFFFFFF, 0.34f), 175);
    } else {
        dui_gradient_rounded(x, y, w, h, r, THEME_SURFACE_ELEVATED, THEME_SURFACE_BASE, 235);
        dui_rect_rounded_outline(x, y, w, h, r, THEME_BORDER_SUBTLE, 175);
    }

    int kx = on ? (x + w - 11) : (x + 11);
    int ky = y + h / 2;
    dui_circle(kx, ky + 1, 7, 0x000000, 70);
    dui_circle(kx, ky, 7, 0xF8FAFC, 255);
}

} // extern "C"
