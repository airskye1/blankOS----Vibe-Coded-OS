#include <efi.h>
#include <efilib.h>

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;

    UINTN uefi_strlen(const char* s) {
        if (!s) return 0;
        UINTN len = 0;
        while (s[len]) len++;
        return len;
    }

    void* uefi_memcpy(void* dst, const void* src, UINTN n) {
        if (!dst || !src) return dst;
        uint8_t* d = (uint8_t*)dst;
        const uint8_t* s = (const uint8_t*)src;
        for (UINTN i = 0; i < n; i++) d[i] = s[i];
        return dst;
    }

    void* uefi_memset(void* dst, int val, UINTN n) {
        if (!dst) return dst;
        uint8_t* d = (uint8_t*)dst;
        uint8_t v = (uint8_t)val;
        for (UINTN i = 0; i < n; i++) d[i] = v;
        return dst;
    }

    UINTN __attribute__((weak)) strlen(const char* s) {
        return uefi_strlen(s);
    }

    void* __attribute__((weak)) memcpy(void* dst, const void* src, UINTN n) {
        return uefi_memcpy(dst, src, n);
    }

    void* __attribute__((weak)) memset(void* dst, int val, UINTN n) {
        return uefi_memset(dst, val, n);
    }

    void* uefi_malloc(UINTN size) {
        if (!gSystemTable) return NULL;
        UINTN* ptr = (UINTN*)NULL;
        gSystemTable->BootServices->AllocatePool(EfiLoaderData, size + sizeof(UINTN), (void**)&ptr);
        if (ptr) {
            *ptr = size;
            return (void*)(ptr + 1);
        }
        return NULL;
    }

    void uefi_free(void* ptr) {
        if (!gSystemTable || !ptr) return;
        UINTN* real_ptr = ((UINTN*)ptr) - 1;
        gSystemTable->BootServices->FreePool((void*)real_ptr);
    }

    void* uefi_realloc(void* ptr, UINTN new_size) {
        if (!ptr) return uefi_malloc(new_size);
        if (new_size == 0) { uefi_free(ptr); return NULL; }
        
        UINTN* real_ptr = ((UINTN*)ptr) - 1;
        UINTN old_size = *real_ptr;
        
        void* new_ptr = uefi_malloc(new_size);
        if (new_ptr) {
            UINTN copy_size = (old_size < new_size) ? old_size : new_size;
            __builtin_memcpy(new_ptr, ptr, copy_size);
            uefi_free(ptr);
        }
        return new_ptr;
    }
    
    // Math functions
    double uefi_floor(double x) {
        if (x >= 0.0) return (double)((long long)x);
        long long i = (long long)x;
        return (x == (double)i) ? (double)i : (double)(i - 1);
    }

    double uefi_ceil(double x) {
        if (x < 0.0) return (double)((long long)x);
        long long i = (long long)x;
        return (x == (double)i) ? (double)i : (double)(i + 1);
    }

    double uefi_fmod(double x, double y) {
        if (y == 0.0) return 0.0;
        return x - (int)(x / y) * y;
    }

    double uefi_sqrt(double x) {
        if (x <= 0) return 0;
        double res = x;
        for (int i = 0; i < 15; i++) {
            res = (res + x / res) / 2.0;
        }
        return res;
    }
    
    double uefi_pow(double base, double exp) {
        double res = 1.0;
        for (int i = 0; i < (int)exp; i++) res *= base;
        return res;
    }
    
    double uefi_cos(double x) {
        double x2 = x * x;
        return 1.0 - (x2)/2.0 + (x2*x2)/24.0 - (x2*x2*x2)/720.0;
    }
    
    double uefi_acos(double x) {
        return 1.570796 - x; // basic linear approx
    }
}
