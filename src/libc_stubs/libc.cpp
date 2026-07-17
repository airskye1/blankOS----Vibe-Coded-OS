#include <efi.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "math.h"
#include "sys/types.h"
#include "sys/stat.h"
#include "sys/time.h"

extern "C" {
    // Exported global System Table
    EFI_SYSTEM_TABLE* global_ST = NULL;
    
    // Ext declarations
    void* uefi_malloc(UINTN size);
    void uefi_free(void* ptr);
    void* uefi_realloc(void* ptr, UINTN size);
    double uefi_floor(double x);
    double uefi_ceil(double x);
    double uefi_sqrt(double x);
    double uefi_pow(double x, double y);
    double uefi_fmod(double x, double y);
    double uefi_cos(double x);
    double uefi_acos(double x);
    
    // FILE structure implementation
    struct FILE {
        EFI_FILE_HANDLE handle;
        uint64_t size;
        uint64_t position;
        bool is_eof;
    };

    FILE* stdin = NULL;
    FILE* stdout = NULL;
    FILE* stderr = NULL;

    // ---------------- MEMORY ----------------
    void* malloc(size_t size) {
        return uefi_malloc(size);
    }
    
    void free(void* ptr) {
        uefi_free(ptr);
    }
    
    void* realloc(void* ptr, size_t size) {
        return uefi_realloc(ptr, size);
    }
    
    void* calloc(size_t nmemb, size_t size) {
        size_t total = nmemb * size;
        void* p = uefi_malloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    
    void exit(int status) {
        // Safe lock or exception
        while (1) {
            __asm__ volatile("hlt");
        }
    }
    
    char* getenv(const char* name) {
        return NULL; // Mock empty env
    }
    
    int atoi(const char* nptr) {
        int res = 0;
        int sign = 1;
        int i = 0;
        if (nptr[0] == '-') {
            sign = -1;
            i++;
        }
        for (; nptr[i] != '\0'; ++i) {
            if (nptr[i] >= '0' && nptr[i] <= '9') {
                res = res * 10 + nptr[i] - '0';
            } else {
                break;
            }
        }
        return sign * res;
    }

    double atof(const char* nptr) {
        return (double)atoi(nptr);
    }

    long strtol(const char* nptr, char** endptr, int base) {
        return (long)atoi(nptr);
    }
    
    int abs(int j) {
        return j < 0 ? -j : j;
    }
    
    void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
        // Basic bubble sort mock for small arrays (DOOM uses it for sorting draws)
        char* arr = (char*)base;
        for (size_t i = 0; i < nmemb; i++) {
            for (size_t j = i + 1; j < nmemb; j++) {
                if (compar(arr + i * size, arr + j * size) > 0) {
                    // Swap
                    for (size_t k = 0; k < size; k++) {
                        char tmp = arr[i * size + k];
                        arr[i * size + k] = arr[j * size + k];
                        arr[j * size + k] = tmp;
                    }
                }
            }
        }
    }

    // ---------------- STRINGS ----------------
    void* memcpy(void* dest, const void* src, size_t n) {
        char* d = (char*)dest;
        const char* s = (const char*)src;
        for (size_t i = 0; i < n; i++) d[i] = s[i];
        return dest;
    }
    
    void* memset(void* s, int c, size_t n) {
        char* p = (char*)s;
        for (size_t i = 0; i < n; i++) p[i] = (char)c;
        return s;
    }
    
    int memcmp(const void* s1, const void* s2, size_t n) {
        const unsigned char* p1 = (const unsigned char*)s1;
        const unsigned char* p2 = (const unsigned char*)s2;
        for (size_t i = 0; i < n; i++) {
            if (p1[i] != p2[i]) return p1[i] - p2[i];
        }
        return 0;
    }
    
    void* memmove(void* dest, const void* src, size_t n) {
        char* d = (char*)dest;
        const char* s = (const char*)src;
        if (d < s) {
            for (size_t i = 0; i < n; i++) d[i] = s[i];
        } else {
            for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
        }
        return dest;
    }
    
    char* strcpy(char* dest, const char* src) {
        size_t i = 0;
        while (src[i] != '\0') {
            dest[i] = src[i];
            i++;
        }
        dest[i] = '\0';
        return dest;
    }
    
    char* strncpy(char* dest, const char* src, size_t n) {
        size_t i = 0;
        for (; i < n && src[i] != '\0'; i++) dest[i] = src[i];
        for (; i < n; i++) dest[i] = '\0';
        return dest;
    }
    
    char* strcat(char* dest, const char* src) {
        size_t d_len = strlen(dest);
        size_t i = 0;
        while (src[i] != '\0') {
            dest[d_len + i] = src[i];
            i++;
        }
        dest[d_len + i] = '\0';
        return dest;
    }
    
    int strcmp(const char* s1, const char* s2) {
        while (*s1 && (*s1 == *s2)) {
            s1++;
            s2++;
        }
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
    }
    
    int strncmp(const char* s1, const char* s2, size_t n) {
        if (n == 0) return 0;
        while (n-- > 0 && *s1 == *s2) {
            if (n == 0 || *s1 == '\0') return 0;
            s1++;
            s2++;
        }
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
    }
    
    size_t strlen(const char* s) {
        size_t len = 0;
        while (s[len] != '\0') len++;
        return len;
    }
    
    char* strchr(const char* s, int c) {
        while (*s != (char)c) {
            if (!*s) return NULL;
            s++;
        }
        return (char*)s;
    }
    
    char* strrchr(const char* s, int c) {
        char* last = NULL;
        do {
            if (*s == (char)c) last = (char*)s;
        } while (*s++);
        return last;
    }
    
    char* strstr(const char* haystack, const char* needle) {
        if (!*needle) return (char*)haystack;
        for (; *haystack; haystack++) {
            if (*haystack == *needle) {
                const char* h = haystack;
                const char* n = needle;
                while (*h && *n && *h == *n) {
                    h++;
                    n++;
                }
                if (!*n) return (char*)haystack;
            }
        }
        return NULL;
    }
    
    char* strerror(int errnum) {
        return (char*)"Unknown error";
    }
    
    int strcasecmp(const char* s1, const char* s2) {
        while (*s1 && *s2) {
            char c1 = *s1;
            char c2 = *s2;
            if (c1 >= 'A' && c1 <= 'Z') c1 = c1 - 'A' + 'a';
            if (c2 >= 'A' && c2 <= 'Z') c2 = c2 - 'A' + 'a';
            if (c1 != c2) return c1 - c2;
            s1++;
            s2++;
        }
        return *s1 - *s2;
    }
    
    int strncasecmp(const char* s1, const char* s2, size_t n) {
        if (n == 0) return 0;
        while (n-- > 0) {
            char c1 = *s1;
            char c2 = *s2;
            if (c1 >= 'A' && c1 <= 'Z') c1 = c1 - 'A' + 'a';
            if (c2 >= 'A' && c2 <= 'Z') c2 = c2 - 'A' + 'a';
            if (c1 != c2) return c1 - c2;
            if (c1 == '\0') return 0;
            s1++;
            s2++;
        }
        return 0;
    }
    
    char* strdup(const char* s) {
        size_t len = strlen(s) + 1;
        char* p = (char*)malloc(len);
        if (p) memcpy(p, s, len);
        return p;
    }

    // ---------------- MATHS ----------------
    double sin(double x) {
        // Taylor series approximation for sin
        double r = x;
        double term = x;
        for (int i = 1; i < 6; i++) {
            term *= -x * x / (2 * i * (2 * i + 1));
            r += term;
        }
        return r;
    }
    double cos(double x) { return uefi_cos(x); }
    double sqrt(double x) { return uefi_sqrt(x); }
    double floor(double x) { return uefi_floor(x); }
    double ceil(double x) { return uefi_ceil(x); }
    double fabs(double x) { return x < 0 ? -x : x; }
    double pow(double x, double y) { return uefi_pow(x, y); }
    double fmod(double x, double y) { return uefi_fmod(x, y); }
    double acos(double x) { return uefi_acos(x); }

    // ---------------- TIME ----------------
    int gettimeofday(struct timeval* tv, struct timezone* tz) {
        if (tv) {
            // Fake timer using UEFI Tick (e.g. increments roughly)
            static uint64_t fake_sec = 1773787200; // Mock Jul 2026 Epoch
            static uint32_t fake_usec = 0;
            fake_usec += 16666; // Add ~60fps step
            if (fake_usec >= 1000000) {
                fake_sec++;
                fake_usec -= 1000000;
            }
            tv->tv_sec = fake_sec;
            tv->tv_usec = fake_usec;
        }
        return 0;
    }

    // ---------------- FILE SYSTEM wrappers around UEFI ----------------
    FILE* fopen(const char* filename, const char* mode) {
        if (!global_ST) return NULL;
        
        // Convert paths like "doom1.wad" or "doom.cfg" to UEFI CHAR16 format
        // and convert '/' or relative paths
        CHAR16 wpath[128];
        int j = 0;
        
        // Convert to absolute UEFI style if relative
        // We'll search in EFI\APPS\ directory or root directory
        const char* prefix = "EFI\\APPS\\";
        int k = 0;
        while (prefix[k]) {
            wpath[j++] = (CHAR16)prefix[k++];
        }
        
        int src_idx = 0;
        // Skip leading ./ or /
        if (filename[0] == '.' && filename[1] == '/') src_idx = 2;
        else if (filename[0] == '/') src_idx = 1;
        
        while (filename[src_idx] && j < 127) {
            char c = filename[src_idx++];
            if (c == '/') c = '\\'; // convert separators
            wpath[j++] = (CHAR16)c;
        }
        wpath[j] = 0;
        
        // Open volume
        EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        UINTN numHandles = 0;
        EFI_HANDLE *handleBuffer = NULL;
        EFI_STATUS Status = global_ST->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handleBuffer);
        if (EFI_ERROR(Status) || numHandles == 0) return NULL;

        for (UINTN i = 0; i < numHandles; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
            global_ST->BootServices->HandleProtocol(handleBuffer[i], &fsGuid, (void**)&fs);
            if (!fs) continue;
            
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            
            EFI_FILE_HANDLE file = NULL;
            // Mode flags
            UINT64 uefi_mode = EFI_FILE_MODE_READ;
            if (mode[0] == 'w') uefi_mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;
            
            Status = root->Open(root, &file, wpath, uefi_mode, 0);
            if (EFI_ERROR(Status)) {
                // Try opening from Root directory directly (without prefix)
                j = 0;
                src_idx = 0;
                if (filename[0] == '.' && filename[1] == '/') src_idx = 2;
                else if (filename[0] == '/') src_idx = 1;
                while (filename[src_idx] && j < 127) {
                    char c = filename[src_idx++];
                    if (c == '/') c = '\\';
                    wpath[j++] = (CHAR16)c;
                }
                wpath[j] = 0;
                Status = root->Open(root, &file, wpath, uefi_mode, 0);
            }

            if (!EFI_ERROR(Status) && file != NULL) {
                // Get file info size
                UINTN info_size = 512;
                alignas(8) uint8_t info_buf[512];
                EFI_GUID infoGuid = EFI_FILE_INFO_ID;
                file->GetInfo(file, &infoGuid, &info_size, info_buf);
                EFI_FILE_INFO* info = (EFI_FILE_INFO*)info_buf;
                
                FILE* stream = (FILE*)malloc(sizeof(FILE));
                stream->handle = file;
                stream->size = info->FileSize;
                stream->position = 0;
                stream->is_eof = false;
                
                root->Close(root);
                global_ST->BootServices->FreePool(handleBuffer);
                return stream;
            }
            root->Close(root);
        }
        global_ST->BootServices->FreePool(handleBuffer);
        return NULL;
    }
    
    int fclose(FILE* stream) {
        if (!stream) return -1;
        stream->handle->Close(stream->handle);
        free(stream);
        return 0;
    }
    
    size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
        if (!stream || !ptr) return 0;
        UINTN bytes_to_read = size * nmemb;
        if (stream->position + bytes_to_read > stream->size) {
            bytes_to_read = stream->size - stream->position;
            stream->is_eof = true;
        }
        
        EFI_STATUS Status = stream->handle->Read(stream->handle, &bytes_to_read, ptr);
        if (EFI_ERROR(Status)) return 0;
        
        stream->position += bytes_to_read;
        return bytes_to_read / size;
    }
    
    size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
        if (!stream || !ptr) return 0;
        UINTN bytes_to_write = size * nmemb;
        EFI_STATUS Status = stream->handle->Write(stream->handle, &bytes_to_write, (void*)ptr);
        if (EFI_ERROR(Status)) return 0;
        stream->position += bytes_to_write;
        if (stream->position > stream->size) stream->size = stream->position;
        return bytes_to_write / size;
    }
    
    int fseek(FILE* stream, long offset, int whence) {
        if (!stream) return -1;
        uint64_t target = stream->position;
        if (whence == SEEK_SET) target = offset;
        else if (whence == SEEK_CUR) target += offset;
        else if (whence == SEEK_END) target = stream->size + offset;
        
        if (target > stream->size) return -1;
        
        EFI_STATUS Status = stream->handle->SetPosition(stream->handle, target);
        if (EFI_ERROR(Status)) return -1;
        
        stream->position = target;
        stream->is_eof = (stream->position == stream->size);
        return 0;
    }
    
    long ftell(FILE* stream) {
        if (!stream) return -1;
        return (long)stream->position;
    }

    int fflush(FILE* stream) {
        if (stream && stream->handle) {
            stream->handle->Flush(stream->handle);
        }
        return 0;
    }

    // ---------------- PRINTING / FORMAT stubs ----------------
    static void reverse(char* str, int len) {
        int i = 0, j = len - 1;
        while (i < j) {
            char temp = str[i];
            str[i] = str[j];
            str[j] = temp;
            i++; j--;
        }
    }

    static int itoa_helper(int num, char* str, int base) {
        int i = 0;
        bool isNegative = false;
        if (num == 0) {
            str[i++] = '0';
            str[i] = '\0';
            return i;
        }
        if (num < 0 && base == 10) {
            isNegative = true;
            num = -num;
        }
        while (num != 0) {
            int rem = num % base;
            str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
            num = num / base;
        }
        if (isNegative) str[i++] = '-';
        str[i] = '\0';
        reverse(str, i);
        return i;
    }

    int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
        size_t written = 0;
        const char* f = format;
        while (*f && written < size - 1) {
            if (*f == '%') {
                f++;
                if (*f == '\0') break;
                if (*f == 'd' || *f == 'i') {
                    int val = va_arg(ap, int);
                    char buf[32];
                    int len = itoa_helper(val, buf, 10);
                    for (int j = 0; j < len && written < size - 1; j++) {
                        str[written++] = buf[j];
                    }
                } else if (*f == 'x' || *f == 'X') {
                    int val = va_arg(ap, int);
                    char buf[32];
                    int len = itoa_helper(val, buf, 16);
                    for (int j = 0; j < len && written < size - 1; j++) {
                        str[written++] = buf[j];
                    }
                } else if (*f == 's') {
                    const char* s = va_arg(ap, const char*);
                    if (!s) s = "(null)";
                    while (*s && written < size - 1) {
                        str[written++] = *s++;
                    }
                } else if (*f == 'c') {
                    char c = (char)va_arg(ap, int);
                    str[written++] = c;
                } else if (*f == '%') {
                    str[written++] = '%';
                }
            } else {
                str[written++] = *f;
            }
            f++;
        }
        str[written] = '\0';
        return (int)written;
    }

    int sprintf(char* str, const char* format, ...) {
        va_list ap;
        va_start(ap, format);
        int res = vsnprintf(str, 1024, format, ap);
        va_end(ap);
        return res;
    }

    int snprintf(char* str, size_t size, const char* format, ...) {
        va_list ap;
        va_start(ap, format);
        int res = vsnprintf(str, size, format, ap);
        va_end(ap);
        return res;
    }

    int printf(const char* format, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, format);
        int res = vsnprintf(buf, sizeof(buf), format, ap);
        va_end(ap);
        
        // Log to UEFI screen console via ConOut
        if (global_ST && global_ST->ConOut) {
            CHAR16 wbuf[512];
            int i = 0;
            while (buf[i] && i < 511) {
                wbuf[i] = (CHAR16)buf[i];
                i++;
            }
            wbuf[i] = 0;
            global_ST->ConOut->OutputString(global_ST->ConOut, wbuf);
        }
        return res;
    }

    int fprintf(FILE* stream, const char* format, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, format);
        int res = vsnprintf(buf, sizeof(buf), format, ap);
        va_end(ap);
        
        if (stream == stderr || stream == stdout || stream == NULL) {
            printf("%s", buf);
        } else {
            UINTN size = res;
            stream->handle->Write(stream->handle, &size, buf);
        }
        return res;
    }

    int sscanf(const char* str, const char* format, ...) {
        // Stub basic parameters
        return 0;
    }

    int rename(const char* oldname, const char* newname) {
        return -1;
    }
    
    int remove(const char* filename) {
        return -1;
    }
    
    void perror(const char* s) {
        printf("Error: %s\r\n", s);
    }

    // ---------------- POSIX IO ----------------
    int open(const char* pathname, int flags, ...) {
        FILE* f = fopen(pathname, "r");
        if (!f) return -1;
        // Mock FD as address pointer cast
        return (int)(intptr_t)f;
    }

    int read(int fd, void* buf, size_t count) {
        FILE* f = (FILE*)(intptr_t)fd;
        return (int)fread(buf, 1, count, f);
    }

    int write(int fd, const void* buf, size_t count) {
        FILE* f = (FILE*)(intptr_t)fd;
        return (int)fwrite(buf, 1, count, f);
    }

    int close(int fd) {
        FILE* f = (FILE*)(intptr_t)fd;
        return fclose(f);
    }

    long lseek(int fd, long offset, int whence) {
        FILE* f = (FILE*)(intptr_t)fd;
        fseek(f, offset, whence);
        return ftell(f);
    }

    int unlink(const char* pathname) {
        return -1;
    }

    int stat(const char* pathname, struct stat* statbuf) {
        FILE* f = fopen(pathname, "r");
        if (!f) return -1;
        statbuf->st_size = f->size;
        statbuf->st_mode = 0100000; // Regular file
        fclose(f);
        return 0;
    }

    int mkdir(const char* pathname, mode_t mode) {
        return -1;
    }

    int access(const char* pathname, int mode) {
        FILE* f = fopen(pathname, "r");
        if (!f) return -1;
        fclose(f);
        return 0;
    }
}
