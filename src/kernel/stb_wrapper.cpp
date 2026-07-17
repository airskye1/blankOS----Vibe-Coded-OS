#include <stdint.h>
#include <stddef.h>
#include <efi.h>

extern "C" {
    void* uefi_malloc(UINTN size);
    void uefi_free(void* ptr);
    void* uefi_realloc(void* ptr, UINTN new_size);
    double uefi_floor(double x);
    double uefi_ceil(double x);
    double uefi_sqrt(double x);
    double uefi_pow(double x, double y);
    double uefi_fmod(double x, double y);
    double uefi_cos(double x);
    double uefi_acos(double x);
}

// ---------------- STB IMAGE ----------------
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD // Prevents including emmintrin.h and stdlib.h for SSE2
#define STBI_MALLOC uefi_malloc
#define STBI_REALLOC uefi_realloc
#define STBI_FREE uefi_free
#define STBI_MEMCPY __builtin_memcpy
#define STBI_MEMSET __builtin_memset
#define STBI_ABS(x) ((x) < 0 ? -(x) : (x))
#define STBI_ASSERT(x)

extern "C" {
    static inline int abs(int x) { return x < 0 ? -x : x; }
}

#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"

// ---------------- STB TRUETYPE ----------------
#define STBTT_ifloor(x)   ((int) uefi_floor(x))
#define STBTT_iceil(x)    ((int) uefi_ceil(x))
#define STBTT_sqrt(x)     uefi_sqrt(x)
#define STBTT_pow(x,y)    uefi_pow(x,y)
#define STBTT_fmod(x,y)   uefi_fmod(x,y)
#define STBTT_cos(x)      uefi_cos(x)
#define STBTT_acos(x)     uefi_acos(x)
#define STBTT_fabs(x)     STBI_ABS(x)
#define STBTT_malloc(x,u) uefi_malloc(x)
#define STBTT_free(x,u)   uefi_free(x)
#define STBTT_assert(x)

#define STB_TRUETYPE_IMPLEMENTATION
#include "../stb_truetype.h"
