#include <efi.h>
#include <efilib.h>

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;

    void* uefi_malloc(UINTN size) {
        if (!gSystemTable) return NULL;
        UINTN* ptr = NULL;
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
