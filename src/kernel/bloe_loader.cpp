#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>
#include "os_api.h"
#include "bloe_loader.h"

#define BLOE_MAGIC_LE  0x424C4F45U // 'BLOE' as uint32_t on little-endian ('E','O','L','B')
#define BLOE_MAGIC_BE  0x454F4C42U // "BLOE" ASCII bytes
#define BLOE_MAX_BYTES (8ULL * 1024ULL * 1024ULL)

typedef struct {
    uint32_t magic;
    uint32_t entry_point;
    uint32_t text_segment_offset;
    uint32_t data_segment_offset;
    uint32_t bss_size;
} bloe_header;

typedef void (*BloeEntryFunc)(OS_API* api);

static EFI_GUID g_bloe_file_info_guid = EFI_FILE_INFO_ID;

static void bloe_zero(void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void bloe_copy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void bloe_scpy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void bloe_scat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int i = 0;
    while (dst[i] && i < max_len - 1) i++;
    int j = 0;
    while (src[j] && i < max_len - 1) dst[i++] = src[j++];
    dst[i] = '\0';
}

static int bloe_slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void bloe_wide_to_ascii(const CHAR16* w, char* out, int max_len) {
    if (!out || max_len <= 0) return;
    int i = 0;
    while (w && w[i] && i < max_len - 1) {
        CHAR16 c = w[i++];
        out[i - 1] = (c < 128) ? (char)c : '?';
    }
    out[i] = '\0';
}

static void bloe_ascii_to_wide(const char* a, CHAR16* out, int max_len) {
    if (!out || max_len <= 0) return;
    int i = 0;
    while (a && a[i] && i < max_len - 1) {
        char c = a[i++];
        if (c == '/') c = '\\';
        out[i - 1] = (CHAR16)(uint8_t)c;
    }
    out[i] = 0;
}

static bool bloe_starts_with(const uint8_t* buf, uint64_t len, const char* prefix) {
    int plen = bloe_slen(prefix);
    if (len < (uint64_t)plen) return false;
    for (int i = 0; i < plen; i++) {
        if ((char)buf[i] != prefix[i]) return false;
    }
    return true;
}

static bool bloe_is_text_stub(const uint8_t* buf, uint64_t len) {
    if (!buf || len == 0) return true;
    if (bloe_starts_with(buf, len, "BLOE_EXECUTABLE_STUB") ||
        bloe_starts_with(buf, len, "BLOE_PKG_")) {
        return true;
    }
    uint64_t sample = (len < 64) ? len : 64;
    bool all_ascii = true;
    for (uint64_t i = 0; i < sample; i++) {
        uint8_t c = buf[i];
        if (!((c >= 32 && c < 127) || c == '\r' || c == '\n' || c == '\t')) {
            all_ascii = false;
            break;
        }
    }
    return all_ascii;
}

static bool bloe_parse_exec_key(const uint8_t* buf, uint64_t len, char* out_path, int max_len) {
    uint64_t scan = (len < 2048) ? len : 2048;
    const char* p = (const char*)buf;
    uint64_t i = 0;
    while (i < scan) {
        while (i < scan && (p[i] == '\r' || p[i] == '\n' || p[i] == ' ' || p[i] == '\t')) i++;
        if (i + 5 < scan &&
            (p[i] == 'E' || p[i] == 'e') &&
            (p[i + 1] == 'X' || p[i + 1] == 'x') &&
            (p[i + 2] == 'E' || p[i + 2] == 'e') &&
            (p[i + 3] == 'C' || p[i + 3] == 'c') &&
            p[i + 4] == '=') {
            i += 5;
            int o = 0;
            while (i < scan && p[i] != '\r' && p[i] != '\n' && o < max_len - 1) {
                char c = p[i++];
                if (c == '/') c = '\\';
                out_path[o++] = c;
            }
            out_path[o] = '\0';
            return o > 0;
        }
        while (i < scan && p[i] != '\r' && p[i] != '\n') i++;
    }
    return false;
}

static bool bloe_read_file(EFI_SYSTEM_TABLE* SystemTable, const CHAR16* filename, uint8_t** out_buf, uint64_t* out_size) {
    if (!SystemTable || !SystemTable->BootServices || !filename || !out_buf || !out_size) return false;
    *out_buf = NULL;
    *out_size = 0;

    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN numHandles = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &numHandles, &handles)) || !handles) {
        return false;
    }

    bool ok = false;
    for (UINTN i = 0; i < numHandles; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(SystemTable->BootServices->HandleProtocol(handles[i], &fsGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE file = NULL;
        if (!EFI_ERROR(root->Open(root, &file, (CHAR16*)filename, EFI_FILE_MODE_READ, 0)) && file) {
            uint8_t info_buf[384];
            UINTN info_sz = sizeof(info_buf);
            UINT64 fsz = BLOE_MAX_BYTES;
            if (!EFI_ERROR(file->GetInfo(file, &g_bloe_file_info_guid, &info_sz, info_buf))) {
                UINT64 reported = ((EFI_FILE_INFO*)info_buf)->FileSize;
                if (reported > 0 && reported <= BLOE_MAX_BYTES) fsz = reported;
            }
            void* pool = NULL;
            if (!EFI_ERROR(SystemTable->BootServices->AllocatePool(EfiLoaderData, (UINTN)(fsz + 1), &pool)) && pool) {
                UINTN rsz = (UINTN)fsz;
                if (!EFI_ERROR(file->Read(file, &rsz, pool)) && rsz > 0) {
                    ((uint8_t*)pool)[rsz] = 0;
                    *out_buf = (uint8_t*)pool;
                    *out_size = (uint64_t)rsz;
                    ok = true;
                } else {
                    SystemTable->BootServices->FreePool(pool);
                }
            }
            file->Close(file);
        }
        root->Close(root);
        if (ok) break;
    }

    SystemTable->BootServices->FreePool(handles);
    return ok;
}

static bool bloe_try_run_flat_binary(EFI_SYSTEM_TABLE* SystemTable, const uint8_t* buf, uint64_t fsz, char* out_status, int status_max) {
    if (!buf || fsz < sizeof(bloe_header)) return false;
    const bloe_header* hdr = (const bloe_header*)buf;
    if (hdr->magic != BLOE_MAGIC_LE && hdr->magic != BLOE_MAGIC_BE) return false;
    if (hdr->bss_size > BLOE_MAX_BYTES) return false;
    if (hdr->text_segment_offset < sizeof(bloe_header) || hdr->text_segment_offset >= fsz) return false;
    if (hdr->data_segment_offset < hdr->text_segment_offset || hdr->data_segment_offset > fsz) return false;
    if (hdr->entry_point < hdr->text_segment_offset || hdr->entry_point >= fsz) return false;

    const uint8_t* entry_bytes = buf + hdr->entry_point;
    if (entry_bytes[0] == 0x00 && entry_bytes[1] == 0x00) {
        bloe_scpy(out_status, "BLOE entry bytes invalid (zeroed code)", status_max);
        return false;
    }

    uint64_t total_sz = fsz + hdr->bss_size;
    if (total_sz > BLOE_MAX_BYTES * 2ULL) return false;

    void* exec_mem = NULL;
    if (EFI_ERROR(SystemTable->BootServices->AllocatePool(EfiLoaderCode, (UINTN)total_sz, &exec_mem)) || !exec_mem) {
        bloe_scpy(out_status, "BLOE allocation failed", status_max);
        return false;
    }

    bloe_zero(exec_mem, total_sz);
    bloe_copy(exec_mem, buf, fsz);

    OS_API api;
    elf_init_os_api(SystemTable, &api);

    BloeEntryFunc entry = (BloeEntryFunc)((uint8_t*)exec_mem + hdr->entry_point);
    entry(&api);

    SystemTable->BootServices->FreePool(exec_mem);
    bloe_scpy(out_status, "BLOE executable exited cleanly", status_max);
    return true;
}

extern "C" {

extern EFI_SYSTEM_TABLE* gSystemTable;

void load_bloe_executable(uint8_t* file_buffer) {
    if (!file_buffer || !gSystemTable) return;
    const bloe_header* header = (const bloe_header*)file_buffer;
    if (header->magic != BLOE_MAGIC_LE && header->magic != BLOE_MAGIC_BE) return;
    if (header->data_segment_offset <= header->text_segment_offset) return;
    uint64_t est_size = (uint64_t)header->data_segment_offset;
    if (est_size > BLOE_MAX_BYTES || header->entry_point < header->text_segment_offset || header->entry_point >= est_size) {
        return;
    }
    char status[64];
    bloe_try_run_flat_binary(gSystemTable, file_buffer, est_size, status, sizeof(status));
}

bool bloe_launch_file(EFI_SYSTEM_TABLE* SystemTable, const CHAR16* filename, char* out_status, int status_max) {
    if (out_status && status_max > 0) out_status[0] = '\0';
    if (!SystemTable || !filename) {
        bloe_scpy(out_status, "Missing system table or filename", status_max);
        return false;
    }

    uint8_t* buf = NULL;
    uint64_t fsz = 0;
    if (!bloe_read_file(SystemTable, filename, &buf, &fsz)) {
        bloe_scpy(out_status, "Failed to open .bloe file on disk", status_max);
        return false;
    }

    // 1. ELF64 binary wrapped in .bloe container
    if (fsz >= 64 && buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        bool ok = elf_run_from_memory(SystemTable, buf, fsz);
        SystemTable->BootServices->FreePool(buf);
        bloe_scpy(out_status, ok ? "BLOE ELF64 app exited cleanly" : "Invalid ELF64 image inside .bloe", status_max);
        return ok;
    }

    // 2. Text stub / package manifest (never execute ASCII bytes!)
    if (bloe_is_text_stub(buf, fsz)) {
        char exec_rel[160] = {0};
        CHAR16 wexec[160];
        if (bloe_parse_exec_key(buf, fsz, exec_rel, sizeof(exec_rel))) {
            bloe_ascii_to_wide(exec_rel, wexec, 160);
            SystemTable->BootServices->FreePool(buf);
            bool ok = load_and_run_elf(SystemTable, wexec);
            bloe_scpy(out_status, ok ? "Paired ELF app exited cleanly" : "Paired EXEC target not found", status_max);
            return ok;
        }
        SystemTable->BootServices->FreePool(buf);

        // Try paired <stem>.elf in the same directory and in EFI\APPS\<stem>.elf
        char ascii_path[160];
        bloe_wide_to_ascii(filename, ascii_path, sizeof(ascii_path));
        int alen = bloe_slen(ascii_path);
        if (alen > 5 && ascii_path[alen - 5] == '.') {
            ascii_path[alen - 5] = '\0';
        }
        char cand1[180];
        bloe_scpy(cand1, ascii_path, sizeof(cand1));
        bloe_scat(cand1, ".elf", sizeof(cand1));
        bloe_ascii_to_wide(cand1, wexec, 180);
        if (load_and_run_elf(SystemTable, wexec)) {
            bloe_scpy(out_status, "BLOE paired ELF binary exited cleanly", status_max);
            return true;
        }

        // Extract bare filename stem for EFI\APPS\<stem>.elf fallback
        const char* stem = ascii_path;
        for (int i = 0; ascii_path[i]; i++) {
            if (ascii_path[i] == '\\' || ascii_path[i] == '/') stem = &ascii_path[i + 1];
        }
        char cand2[180];
        bloe_scpy(cand2, "EFI\\APPS\\", sizeof(cand2));
        bloe_scat(cand2, stem, sizeof(cand2));
        bloe_scat(cand2, ".elf", sizeof(cand2));
        bloe_ascii_to_wide(cand2, wexec, 180);
        if (load_and_run_elf(SystemTable, wexec)) {
            bloe_scpy(out_status, "BLOE launched EFI\\APPS binary cleanly", status_max);
            return true;
        }

        bloe_scpy(out_status, "BLOE manifest verified (no native .elf binary attached)", status_max);
        return false;
    }

    // 3. Legacy flat BLOE binary header (strictly bounds-checked before jump)
    bool flat_ok = bloe_try_run_flat_binary(SystemTable, buf, fsz, out_status, status_max);
    SystemTable->BootServices->FreePool(buf);
    if (!flat_ok && out_status && !out_status[0]) {
        bloe_scpy(out_status, "Unsupported or corrupted .bloe binary", status_max);
    }
    return flat_ok;
}

} // extern "C"
