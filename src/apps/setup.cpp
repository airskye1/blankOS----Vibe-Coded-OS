#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/blankDUI.h"
#include "../ui/theme.h"
#include "../kernel/input.h"
#include "../kernel/audio.h"

extern "C" {
    extern bool g_is_live_cd;
    extern bool format_disk_gpt_fat32(EFI_SYSTEM_TABLE* SystemTable, EFI_BLOCK_IO_PROTOCOL* BlockIo);
    extern bool blankReg_save(EFI_SYSTEM_TABLE* st);
    extern void os_restart(void);
}

#define SETUP_MAX_DISKS 10
#define SETUP_CHUNK_SIZE (256 * 1024)

typedef struct {
    EFI_HANDLE handle;
    EFI_BLOCK_IO_PROTOCOL* blk;
    uint64_t size_mb;
    uint32_t block_size;
    bool removable;
    bool gpt_compatible;
} SetupDiskInfo;

typedef struct {
    uint8_t type;
    uint8_t sub_type;
    uint8_t length[2];
} SetupDevPathNode;

typedef struct {
    int phase_idx;
    int total_phases;
    float progress;
    char status_line[96];
    char current_file[64];
    uint64_t bytes_copied;
    int files_copied;
} SetupInstallProgress;

typedef enum {
    STEP_WELCOME = 0,
    STEP_DISK_SELECT = 1,
    STEP_INSTALLING = 2,
    STEP_COMPLETE = 3
} SetupStep;

static void stp_str_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int stp_str_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void stp_str_append(char* dst, const char* src, int max_len) {
    int cur = stp_str_len(dst);
    if (cur >= max_len - 1) return;
    stp_str_copy(dst + cur, src, max_len - cur);
}

static int stp_u64_to_str(uint64_t v, char* out) {
    char tmp[32];
    int p = 0;
    if (v == 0) {
        tmp[p++] = '0';
    } else {
        while (v > 0 && p < 30) {
            tmp[p++] = (char)('0' + (v % 10ULL));
            v /= 10ULL;
        }
    }
    int n = 0;
    while (p > 0) out[n++] = tmp[--p];
    out[n] = '\0';
    return n;
}

static void stp_format_bytes(uint64_t bytes, char* out, int max_len) {
    char num[32];
    if (bytes >= 1024ULL * 1024ULL) {
        uint64_t mb = bytes / (1024ULL * 1024ULL);
        uint64_t dec = ((bytes % (1024ULL * 1024ULL)) * 100ULL) / (1024ULL * 1024ULL);
        stp_u64_to_str(mb, num);
        stp_str_copy(out, num, max_len);
        stp_str_append(out, ".", max_len);
        if (dec < 10) stp_str_append(out, "0", max_len);
        stp_u64_to_str(dec, num);
        stp_str_append(out, num, max_len);
        stp_str_append(out, " MB", max_len);
    } else {
        uint64_t kb = (bytes + 1023ULL) / 1024ULL;
        stp_u64_to_str(kb, num);
        stp_str_copy(out, num, max_len);
        stp_str_append(out, " KB", max_len);
    }
}

static bool stp_is_cdrom_device_path(EFI_SYSTEM_TABLE* st, EFI_HANDLE handle) {
    if (!st || !st->BootServices || !handle) return false;
    EFI_GUID dp_guid = { 0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };
    const uint8_t* dp = NULL;
    EFI_STATUS status = st->BootServices->HandleProtocol(handle, &dp_guid, (void**)&dp);
    if (EFI_ERROR(status) || !dp) return false;

    uint32_t offset = 0;
    while (offset < 4096) {
        const SetupDevPathNode* node = (const SetupDevPathNode*)(dp + offset);
        if (node->type == 0x7F) break; // End of Device Path
        if (node->type == 0x04 && node->sub_type == 0x02) {
            return true; // MEDIA_DEVICE_PATH / MEDIA_CDROM_DP
        }
        uint16_t node_len = (uint16_t)node->length[0] | ((uint16_t)node->length[1] << 8);
        if (node_len < 4) break;
        offset += node_len;
    }
    return false;
}

static uint32_t stp_dp_prefix_len(const uint8_t* dp) {
    if (!dp) return 0;
    uint32_t offset = 0;
    while (offset < 4096) {
        const SetupDevPathNode* node = (const SetupDevPathNode*)(dp + offset);
        if (node->type == 0x7F) break;
        uint16_t node_len = (uint16_t)node->length[0] | ((uint16_t)node->length[1] << 8);
        if (node_len < 4) return 0;
        offset += node_len;
    }
    return offset;
}

static bool stp_dp_is_child(EFI_SYSTEM_TABLE* st, EFI_HANDLE parent, EFI_HANDLE child) {
    if (!st || !st->BootServices || !parent || !child) return false;
    EFI_GUID dp_guid = { 0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };
    const uint8_t* p_dp = NULL;
    const uint8_t* c_dp = NULL;
    if (EFI_ERROR(st->BootServices->HandleProtocol(parent, &dp_guid, (void**)&p_dp)) || !p_dp) return false;
    if (EFI_ERROR(st->BootServices->HandleProtocol(child, &dp_guid, (void**)&c_dp)) || !c_dp) return false;

    uint32_t p_len = stp_dp_prefix_len(p_dp);
    uint32_t c_len = stp_dp_prefix_len(c_dp);
    if (p_len == 0 || c_len < p_len) return false;

    for (uint32_t i = 0; i < p_len; i++) {
        if (p_dp[i] != c_dp[i]) return false;
    }
    return true;
}

static bool stp_is_handle_readonly_media(EFI_SYSTEM_TABLE* st, EFI_HANDLE handle) {
    if (!st || !st->BootServices || !handle) return true;
    if (stp_is_cdrom_device_path(st, handle)) return true;

    EFI_GUID blk_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL* blk = NULL;
    EFI_STATUS status = st->BootServices->HandleProtocol(handle, &blk_guid, (void**)&blk);
    if (!EFI_ERROR(status) && blk && blk->Media && blk->Media->ReadOnly) {
        return true;
    }
    return false;
}

static bool stp_test_root_writable(EFI_FILE_HANDLE root) {
    if (!root) return false;
    EFI_FILE_HANDLE test_file = NULL;
    EFI_STATUS st = root->Open(
        root, &test_file, (CHAR16*)L".BLKWRT.TMP",
        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !test_file) return false;
    test_file->Delete(test_file);
    return true;
}

static int stp_scan_physical_disks(EFI_SYSTEM_TABLE* st, SetupDiskInfo* out_disks, int max_disks) {
    if (!st || !st->BootServices || !out_disks || max_disks <= 0) return 0;

    EFI_GUID blk_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN num_handles = 0;
    EFI_HANDLE* handles = NULL;
    EFI_STATUS status = st->BootServices->LocateHandleBuffer(
        ByProtocol, &blk_guid, NULL, &num_handles, &handles);

    if (EFI_ERROR(status) || !handles || num_handles == 0) {
        if (handles) st->BootServices->FreePool(handles);
        return 0;
    }

    int count = 0;
    for (UINTN i = 0; i < num_handles && count < max_disks; i++) {
        if (stp_is_cdrom_device_path(st, handles[i])) continue;

        EFI_BLOCK_IO_PROTOCOL* blk = NULL;
        status = st->BootServices->HandleProtocol(handles[i], &blk_guid, (void**)&blk);
        if (EFI_ERROR(status) || !blk || !blk->Media) continue;
        if (!blk->Media->MediaPresent || blk->Media->LogicalPartition || blk->Media->ReadOnly) continue;
        if (blk->Media->BlockSize == 0) continue;

        uint64_t total_blocks = (uint64_t)blk->Media->LastBlock + 1ULL;
        uint64_t size_mb = (total_blocks * (uint64_t)blk->Media->BlockSize) / (1024ULL * 1024ULL);

        out_disks[count].handle = handles[i];
        out_disks[count].blk = blk;
        out_disks[count].size_mb = size_mb;
        out_disks[count].block_size = blk->Media->BlockSize;
        out_disks[count].removable = (blk->Media->RemovableMedia != 0);
        out_disks[count].gpt_compatible = (blk->Media->BlockSize == 512 && blk->Media->LastBlock >= 80000);
        count++;
    }

    st->BootServices->FreePool(handles);
    return count;
}

static void stp_draw_header_steps(int card_x, int card_y, int card_w, int active_step) {
    const char* names[4] = { "1. Welcome", "2. Target Disk", "3. Install", "4. Complete" };
    int pill_w = 148;
    int pill_h = 28;
    int gap = 12;
    int row_w = 4 * pill_w + 3 * gap;
    int start_x = card_x + (card_w - row_w) / 2;
    int y = card_y + 24;

    for (int i = 0; i < 4; i++) {
        int px = start_x + i * (pill_w + gap);
        bool cur = (i == active_step);
        bool done = (i < active_step);
        uint32_t bg = cur ? THEME_ACCENT_CYAN : (done ? THEME_SURFACE_ELEVATED : THEME_SURFACE_INPUT);
        uint8_t alpha = cur ? 245 : (done ? 220 : 170);
        dui_rect_rounded(px, y, pill_w, pill_h, THEME_RADIUS_PILL, bg, alpha);

        uint32_t tc = cur ? THEME_TEXT_ON_ACCENT : (done ? THEME_ACCENT_TEAL : THEME_TEXT_MUTED);
        int tw = dui_text_width(names[i], 1);
        dui_text(px + (pill_w - tw) / 2, y + 6, names[i], tc, 1);
    }
}

static void stp_render_installing_frame(EFI_SYSTEM_TABLE* st, const SetupInstallProgress* prog) {
    (void)st;
    BlankInputState in;
    input_poll(&in);
    audio_tick();

    int sw = (int)(screen_width > 0 ? screen_width : 1024);
    int sh = (int)(screen_height > 0 ? screen_height : 768);

    dui_draw_wallpaper(dui_get_wallpaper_id());
    dui_rect(0, 0, sw, sh, THEME_BG_VOID, 92);

    int card_w = (sw >= 820) ? 736 : (sw - 64);
    int card_h = 468;
    int card_x = (sw - card_w) / 2;
    int card_y = (sh - card_h) / 2;

    dui_shadow(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, 12);
    dui_bento_card(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 236, true);

    stp_draw_header_steps(card_x, card_y, card_w, STEP_INSTALLING);

    int cx = card_x + card_w / 2;
    const char* hdr = "Installing BlankOS Horizon";
    int hw = dui_text_width(hdr, 2);
    dui_text(cx - hw / 2, card_y + 78, hdr, THEME_TEXT_PRIMARY, 2);

    float p = horizon_clampf(prog->progress, 0.0f, 1.0f);
    dui_progress_ring(cx, card_y + 178, 44, 5, p, THEME_SURFACE_ELEVATED, THEME_ACCENT_CYAN);

    int pct = (int)(p * 100.0f + 0.5f);
    if (pct > 100) pct = 100;
    char pct_str[8];
    stp_u64_to_str((uint64_t)pct, pct_str);
    stp_str_append(pct_str, "%", sizeof(pct_str));
    int pw = dui_text_width(pct_str, 1);
    dui_text(cx - pw / 2, card_y + 170, pct_str, THEME_TEXT_PRIMARY, 1);

    // Status + Current File
    int sw1 = dui_text_width(prog->status_line, 1);
    dui_text(cx - sw1 / 2, card_y + 244, prog->status_line, THEME_TEXT_PRIMARY, 1);

    if (prog->current_file[0]) {
        int fw = dui_text_width(prog->current_file, 1);
        dui_text(cx - fw / 2, card_y + 270, prog->current_file, THEME_ACCENT_CYAN, 1);
    }

    // Horizontal progress bar
    int bar_w = card_w - 112;
    int bar_x = card_x + 56;
    int bar_y = card_y + 308;
    dui_rect_rounded(bar_x, bar_y, bar_w, 8, 4, THEME_SURFACE_INPUT, 255);
    int fill_w = (int)((float)bar_w * p);
    if (fill_w >= 8) {
        dui_gradient_rounded(bar_x, bar_y, fill_w, 8, 4, THEME_ACCENT_CYAN, THEME_ACCENT_TEAL, 255);
    }

    // Live metrics card at bottom
    char bytes_str[32];
    stp_format_bytes(prog->bytes_copied, bytes_str, sizeof(bytes_str));
    char files_str[16];
    stp_u64_to_str((uint64_t)prog->files_copied, files_str);

    char metrics[96];
    stp_str_copy(metrics, "Files Written: ", sizeof(metrics));
    stp_str_append(metrics, files_str, sizeof(metrics));
    stp_str_append(metrics, "   |   Data Copied: ", sizeof(metrics));
    stp_str_append(metrics, bytes_str, sizeof(metrics));
    int mw = dui_text_width(metrics, 1);
    dui_text(cx - mw / 2, card_y + 344, metrics, THEME_TEXT_SECONDARY, 1);

    const char* note = "Do not power off your computer during installation.";
    int nw = dui_text_width(note, 1);
    dui_text(cx - nw / 2, card_y + 392, note, THEME_TEXT_MUTED, 1);

    blankUI_draw_cursor(in.mx, in.my, 0);
    swap_buffers();
}

static EFI_FILE_HANDLE stp_open_or_create_dir(EFI_FILE_HANDLE parent, const CHAR16* name) {
    if (!parent || !name) return NULL;
    EFI_FILE_HANDLE dir = NULL;
    // Try opening existing directory first; some UEFI FAT drivers reject CREATE on existing dirs
    EFI_STATUS st = parent->Open(parent, &dir, (CHAR16*)name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(st) && dir) return dir;
    st = parent->Open(
        parent, &dir, (CHAR16*)name,
        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
        EFI_FILE_DIRECTORY);
    if (EFI_ERROR(st)) return NULL;
    return dir;
}

static bool stp_write_small_file(EFI_FILE_HANDLE dir, const CHAR16* filename, const void* data, UINTN len) {
    if (!dir || !filename || !data || len == 0) return false;

    EFI_FILE_HANDLE existing = NULL;
    if (!EFI_ERROR(dir->Open(dir, &existing, (CHAR16*)filename, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && existing) {
        existing->Delete(existing); // Delete closes the handle
    }

    EFI_FILE_HANDLE file = NULL;
    EFI_STATUS st = dir->Open(
        dir, &file, (CHAR16*)filename,
        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(st) || !file) return false;

    UINTN written = len;
    st = file->Write(file, &written, (VOID*)data);
    if (file->Flush) file->Flush(file);
    file->Close(file);
    return (!EFI_ERROR(st) && written == len);
}

static bool stp_copy_file_chunked(
    EFI_SYSTEM_TABLE* st,
    EFI_FILE_HANDLE src_dir,
    EFI_FILE_HANDLE dst_dir,
    const CHAR16* filename,
    const char* display_name,
    void* chunk_buf,
    UINTN chunk_cap,
    SetupInstallProgress* prog,
    float progress_start,
    float progress_end,
    bool required
) {
    if (!src_dir || !dst_dir || !filename || !chunk_buf || chunk_cap == 0) return !required;

    EFI_FILE_HANDLE src_file = NULL;
    EFI_STATUS status = src_dir->Open(src_dir, &src_file, (CHAR16*)filename, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status) || !src_file) {
        return !required;
    }

    // Remove pre-existing target file so size never trails old bytes
    EFI_FILE_HANDLE old_dst = NULL;
    if (!EFI_ERROR(dst_dir->Open(dst_dir, &old_dst, (CHAR16*)filename, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && old_dst) {
        old_dst->Delete(old_dst);
    }

    EFI_FILE_HANDLE dst_file = NULL;
    status = dst_dir->Open(
        dst_dir, &dst_file, (CHAR16*)filename,
        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status) || !dst_file) {
        src_file->Close(src_file);
        return false;
    }

    stp_str_copy(prog->current_file, display_name, sizeof(prog->current_file));
    prog->progress = progress_start;
    stp_render_installing_frame(st, prog);

    bool ok = true;
    int chunk_idx = 0;
    while (1) {
        UINTN to_read = chunk_cap;
        EFI_STATUS r_st = src_file->Read(src_file, &to_read, chunk_buf);
        if (EFI_ERROR(r_st)) {
            ok = false;
            break;
        }
        if (to_read == 0) break; // EOF

        UINTN to_write = to_read;
        EFI_STATUS w_st = dst_file->Write(dst_file, &to_write, chunk_buf);
        if (EFI_ERROR(w_st) || to_write != to_read) {
            ok = false;
            break;
        }

        prog->bytes_copied += (uint64_t)to_write;
        chunk_idx++;
        float local_t = (float)chunk_idx / (float)(chunk_idx + 4);
        prog->progress = horizon_lerp(progress_start, progress_end, local_t);
        stp_render_installing_frame(st, prog);

        if (to_read < chunk_cap) break; // Final partial chunk
    }

    if (ok && dst_file->Flush) {
        dst_file->Flush(dst_file);
    }
    dst_file->Close(dst_file);
    src_file->Close(src_file);

    if (ok) {
        prog->files_copied++;
        prog->progress = progress_end;
        stp_render_installing_frame(st, prog);
    }
    return ok;
}

static bool stp_run_real_installation(
    EFI_SYSTEM_TABLE* SystemTable,
    EFI_HANDLE selected_disk_handle,
    EFI_BLOCK_IO_PROTOCOL* selected_blk,
    bool do_format,
    SetupInstallProgress* prog,
    char* error_out,
    int error_max
) {
    if (!SystemTable || !SystemTable->BootServices) {
        stp_str_copy(error_out, "UEFI BootServices unavailable.", error_max);
        return false;
    }

    // 1. Format target disk if requested + Reconnect UEFI Storage Controllers
    if (do_format) {
        if (!selected_disk_handle || !selected_blk) {
            stp_str_copy(error_out, "No physical target disk selected.", error_max);
            return false;
        }

        stp_str_copy(prog->status_line, "Partitioning GPT & Formatting FAT32...", sizeof(prog->status_line));
        stp_str_copy(prog->current_file, "Writing GPT header & FAT32 VBR", sizeof(prog->current_file));
        prog->progress = 0.08f;
        stp_render_installing_frame(SystemTable, prog);

        bool formatted = format_disk_gpt_fat32(SystemTable, selected_blk);
        if (!formatted) {
            stp_str_copy(error_out, "GPT/FAT32 format failed (requires 512B sectors & >= 40 MB).", error_max);
            return false;
        }

        stp_str_copy(prog->status_line, "Reconnecting UEFI storage & mounting ESP...", sizeof(prog->status_line));
        stp_str_copy(prog->current_file, "Remounting GPT + FAT32 partition driver", sizeof(prog->current_file));
        prog->progress = 0.16f;
        stp_render_installing_frame(SystemTable, prog);

        SystemTable->BootServices->DisconnectController(selected_disk_handle, NULL, NULL);
        SystemTable->BootServices->Stall(100000);
        SystemTable->BootServices->ConnectController(selected_disk_handle, NULL, NULL, TRUE);
        SystemTable->BootServices->Stall(200000);

        // Explicitly bind PartitionDxe/FatDxe onto all newly created child BlockIo handles
        EFI_GUID blk_guid_reconn = EFI_BLOCK_IO_PROTOCOL_GUID;
        UINTN bcnt = 0;
        EFI_HANDLE* bhandles = NULL;
        if (!EFI_ERROR(SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &blk_guid_reconn, NULL, &bcnt, &bhandles)) && bhandles) {
            for (UINTN bi = 0; bi < bcnt; bi++) {
                if (bhandles[bi] != selected_disk_handle && stp_dp_is_child(SystemTable, selected_disk_handle, bhandles[bi])) {
                    SystemTable->BootServices->ConnectController(bhandles[bi], NULL, NULL, TRUE);
                }
            }
            SystemTable->BootServices->FreePool(bhandles);
        }
        SystemTable->BootServices->Stall(150000);
    }

    // 2. Rescan all EFI_SIMPLE_FILE_SYSTEM_PROTOCOL handles
    stp_str_copy(prog->status_line, "Scanning UEFI filesystem volumes...", sizeof(prog->status_line));
    prog->current_file[0] = '\0';
    prog->progress = 0.22f;
    stp_render_installing_frame(SystemTable, prog);

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN num_fs = 0;
    EFI_HANDLE* fs_handles = NULL;
    EFI_STATUS status = SystemTable->BootServices->LocateHandleBuffer(
        ByProtocol, &fs_guid, NULL, &num_fs, &fs_handles);

    if (EFI_ERROR(status) || !fs_handles || num_fs == 0) {
        if (fs_handles) SystemTable->BootServices->FreePool(fs_handles);
        stp_str_copy(error_out, "No UEFI FAT32 filesystem volumes detected.", error_max);
        return false;
    }

    // Locate Source Volume containing \EFI\BOOT\BOOTX64.EFI
    EFI_HANDLE src_handle = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* src_fs = NULL;

    for (UINTN i = 0; i < num_fs; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(SystemTable->BootServices->HandleProtocol(fs_handles[i], &fs_guid, (void**)&fs)) || !fs) continue;
        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE efi_bin = NULL;
        EFI_STATUS o_st = root->Open(root, &efi_bin, (CHAR16*)L"EFI\\BOOT\\BOOTX64.EFI", EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(o_st) && efi_bin) {
            efi_bin->Close(efi_bin);
            src_handle = fs_handles[i];
            src_fs = fs;
            root->Close(root);
            break;
        }
        root->Close(root);
    }

    if (!src_fs) {
        SystemTable->BootServices->FreePool(fs_handles);
        stp_str_copy(error_out, "Install source \\EFI\\BOOT\\BOOTX64.EFI not found.", error_max);
        return false;
    }

    // Locate Writable Target Volume (prefer child of selected_disk_handle, else non-source writable FS)
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* dst_fs = NULL;

    if (selected_disk_handle != NULL) {
        for (UINTN i = 0; i < num_fs; i++) {
            if (stp_is_handle_readonly_media(SystemTable, fs_handles[i])) continue;
            if (!stp_dp_is_child(SystemTable, selected_disk_handle, fs_handles[i])) continue;

            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
            if (EFI_ERROR(SystemTable->BootServices->HandleProtocol(fs_handles[i], &fs_guid, (void**)&fs)) || !fs) continue;
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            bool writable = stp_test_root_writable(root);
            root->Close(root);
            if (writable) {
                dst_fs = fs;
                break;
            }
        }
    }

    if (!dst_fs) {
        for (UINTN i = 0; i < num_fs; i++) {
            if (fs_handles[i] == src_handle) continue;
            if (stp_is_handle_readonly_media(SystemTable, fs_handles[i])) continue;

            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
            if (EFI_ERROR(SystemTable->BootServices->HandleProtocol(fs_handles[i], &fs_guid, (void**)&fs)) || !fs) continue;
            EFI_FILE_HANDLE root = NULL;
            if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
            bool writable = stp_test_root_writable(root);
            root->Close(root);
            if (writable) {
                dst_fs = fs;
                break;
            }
        }
    }

    if (!dst_fs && do_format && selected_disk_handle != NULL) {
        // Retry once after connecting all handles in case PartitionDxe/FatDxe bound lazily
        SystemTable->BootServices->FreePool(fs_handles);
        fs_handles = NULL;
        num_fs = 0;

        UINTN all_cnt = 0;
        EFI_HANDLE* all_handles = NULL;
        if (!EFI_ERROR(SystemTable->BootServices->LocateHandleBuffer(AllHandles, NULL, NULL, &all_cnt, &all_handles)) && all_handles) {
            for (UINTN ai = 0; ai < all_cnt; ai++) {
                SystemTable->BootServices->ConnectController(all_handles[ai], NULL, NULL, TRUE);
            }
            SystemTable->BootServices->FreePool(all_handles);
        }
        SystemTable->BootServices->Stall(250000);

        status = SystemTable->BootServices->LocateHandleBuffer(ByProtocol, &fs_guid, NULL, &num_fs, &fs_handles);
        if (!EFI_ERROR(status) && fs_handles && num_fs > 0) {
            for (UINTN i = 0; i < num_fs; i++) {
                if (stp_is_handle_readonly_media(SystemTable, fs_handles[i])) continue;
                if (!stp_dp_is_child(SystemTable, selected_disk_handle, fs_handles[i])) continue;
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
                if (EFI_ERROR(SystemTable->BootServices->HandleProtocol(fs_handles[i], &fs_guid, (void**)&fs)) || !fs) continue;
                EFI_FILE_HANDLE root = NULL;
                if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
                bool writable = stp_test_root_writable(root);
                root->Close(root);
                if (writable) {
                    dst_fs = fs;
                    break;
                }
            }
        }
    }

    if (!dst_fs && !do_format && !stp_is_handle_readonly_media(SystemTable, src_handle)) {
        EFI_FILE_HANDLE root = NULL;
        if (!EFI_ERROR(src_fs->OpenVolume(src_fs, &root)) && root) {
            if (stp_test_root_writable(root)) dst_fs = src_fs;
            root->Close(root);
        }
    }

    if (!dst_fs) {
        SystemTable->BootServices->FreePool(fs_handles);
        stp_str_copy(error_out, "No writable FAT32 target partition found after scan.", error_max);
        return false;
    }

    // Allocate 256 KB streaming buffer
    void* chunk_buf = NULL;
    status = SystemTable->BootServices->AllocatePool(EfiLoaderData, SETUP_CHUNK_SIZE, &chunk_buf);
    if (EFI_ERROR(status) || !chunk_buf) {
        SystemTable->BootServices->FreePool(fs_handles);
        stp_str_copy(error_out, "UEFI AllocatePool failed for copy buffer.", error_max);
        return false;
    }

    EFI_FILE_HANDLE src_root = NULL;
    EFI_FILE_HANDLE dst_root = NULL;
    if (EFI_ERROR(src_fs->OpenVolume(src_fs, &src_root)) || !src_root ||
        EFI_ERROR(dst_fs->OpenVolume(dst_fs, &dst_root)) || !dst_root) {
        if (src_root) src_root->Close(src_root);
        if (dst_root) dst_root->Close(dst_root);
        SystemTable->BootServices->FreePool(chunk_buf);
        SystemTable->BootServices->FreePool(fs_handles);
        stp_str_copy(error_out, "Failed to open source or target FAT32 root volume.", error_max);
        return false;
    }

    bool ok = true;
    stp_str_copy(prog->status_line, "Creating EFI & system directories...", sizeof(prog->status_line));
    prog->progress = 0.26f;
    stp_render_installing_frame(SystemTable, prog);

    EFI_FILE_HANDLE dst_efi = stp_open_or_create_dir(dst_root, (CHAR16*)L"EFI");
    EFI_FILE_HANDLE dst_boot = dst_efi ? stp_open_or_create_dir(dst_efi, (CHAR16*)L"BOOT") : NULL;
    EFI_FILE_HANDLE dst_apps = dst_efi ? stp_open_or_create_dir(dst_efi, (CHAR16*)L"APPS") : NULL;
    EFI_FILE_HANDLE dst_assets = stp_open_or_create_dir(dst_root, (CHAR16*)L"assets");
    EFI_FILE_HANDLE dst_downloads = stp_open_or_create_dir(dst_root, (CHAR16*)L"DOWNLOADS");
    if (dst_downloads) dst_downloads->Close(dst_downloads);

    EFI_FILE_HANDLE src_efi = NULL;
    EFI_FILE_HANDLE src_boot = NULL;
    EFI_FILE_HANDLE src_assets = NULL;
    EFI_FILE_HANDLE src_apps = NULL;
    if (!EFI_ERROR(src_root->Open(src_root, &src_efi, (CHAR16*)L"EFI", EFI_FILE_MODE_READ, 0)) && src_efi) {
        src_efi->Open(src_efi, &src_boot, (CHAR16*)L"BOOT", EFI_FILE_MODE_READ, 0);
        src_efi->Open(src_efi, &src_apps, (CHAR16*)L"APPS", EFI_FILE_MODE_READ, 0);
    }
    src_root->Open(src_root, &src_assets, (CHAR16*)L"assets", EFI_FILE_MODE_READ, 0);

    if (!dst_boot || !src_boot) {
        ok = false;
        stp_str_copy(error_out, "Unable to open \\EFI\\BOOT directory on target volume.", error_max);
    }

    if (ok) {
        stp_str_copy(prog->status_line, "Installing UEFI kernel image...", sizeof(prog->status_line));
        if (!stp_copy_file_chunked(SystemTable, src_boot, dst_boot, (CHAR16*)L"BOOTX64.EFI",
                                   "\\EFI\\BOOT\\BOOTX64.EFI", chunk_buf, SETUP_CHUNK_SIZE,
                                   prog, 0.28f, 0.60f, true)) {
            ok = false;
            stp_str_copy(error_out, "Failed copying \\EFI\\BOOT\\BOOTX64.EFI.", error_max);
        } else {
            stp_copy_file_chunked(SystemTable, src_boot, dst_boot, (CHAR16*)L"BOOTIA32.EFI",
                                  "\\EFI\\BOOT\\BOOTIA32.EFI", chunk_buf, SETUP_CHUNK_SIZE,
                                  prog, 0.60f, 0.63f, false);
        }
    }

    if (ok && src_assets && dst_assets) {
        stp_str_copy(prog->status_line, "Installing Horizon fonts & visual assets...", sizeof(prog->status_line));
        stp_copy_file_chunked(SystemTable, src_assets, dst_assets, (CHAR16*)L"font.ttf",
                              "\\assets\\font.ttf", chunk_buf, SETUP_CHUNK_SIZE,
                              prog, 0.63f, 0.72f, false);
        stp_copy_file_chunked(SystemTable, src_assets, dst_assets, (CHAR16*)L"logo.jpg",
                              "\\assets\\logo.jpg", chunk_buf, SETUP_CHUNK_SIZE,
                              prog, 0.72f, 0.78f, false);
        stp_copy_file_chunked(SystemTable, src_assets, dst_assets, (CHAR16*)L"nano_banana.jpg",
                              "\\assets\\nano_banana.jpg", chunk_buf, SETUP_CHUNK_SIZE,
                              prog, 0.78f, 0.84f, false);
    }

    if (ok && dst_apps) {
        bool copied_cat = false;
        if (src_apps) {
            stp_str_copy(prog->status_line, "Installing BLOE App Store packages...", sizeof(prog->status_line));
            src_apps->SetPosition(src_apps, 0);
            uint8_t dir_info_buf[512];
            int guard = 0;
            while (guard++ < 48) {
                UINTN bsz = sizeof(dir_info_buf);
                EFI_STATUS st_d = src_apps->Read(src_apps, &bsz, dir_info_buf);
                if (EFI_ERROR(st_d) || bsz == 0) break;
                EFI_FILE_INFO* fi = (EFI_FILE_INFO*)dir_info_buf;
                if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
                if (fi->FileName[0] == L'.') continue;
                char app_disp[64] = "\\EFI\\APPS\\";
                int ap = 10;
                for (int k = 0; fi->FileName[k] && ap < 62; k++) {
                    app_disp[ap++] = (char)(fi->FileName[k] & 0x7F);
                }
                app_disp[ap] = '\0';
                stp_copy_file_chunked(SystemTable, src_apps, dst_apps, fi->FileName,
                                      app_disp, chunk_buf, SETUP_CHUNK_SIZE,
                                      prog, 0.84f, 0.94f, false);
                if ((fi->FileName[0] == L'c' || fi->FileName[0] == L'C') &&
                    (fi->FileName[1] == L'a' || fi->FileName[1] == L'A')) {
                    copied_cat = true;
                }
            }
        }
        if (!copied_cat) {
            const char json[] = "{\"store_name\":\"BlankOS App Store\",\"version\":\"3.1.0\"}";
            if (stp_write_small_file(dst_apps, (CHAR16*)L"catalog.json", json, sizeof(json) - 1)) {
                prog->files_copied++;
                prog->bytes_copied += sizeof(json) - 1;
            }
        }
    }

    if (ok) {
        stp_str_copy(prog->status_line, "Finalizing boot flag & Horizon registry...", sizeof(prog->status_line));
        stp_str_copy(prog->current_file, "\\EFI\\BOOT\\INSTALLED.FLG", sizeof(prog->current_file));
        prog->progress = 0.96f;
        stp_render_installing_frame(SystemTable, prog);

        const char flg[] = "BLANKOS_HORIZON_INSTALLED\r\n";
        if (!stp_write_small_file(dst_boot, (CHAR16*)L"INSTALLED.FLG", flg, sizeof(flg) - 1)) {
            ok = false;
            stp_str_copy(error_out, "Failed writing \\EFI\\BOOT\\INSTALLED.FLG.", error_max);
        } else {
            prog->files_copied++;
            prog->bytes_copied += sizeof(flg) - 1;
        }
    }

    if (ok) {
        bool reg_saved = blankReg_save(SystemTable);
        // Ensure REGISTRY.INI exists on the newly installed ESP even if firmware's first SFS handle was read-only
        EFI_FILE_HANDLE chk_reg = NULL;
        bool has_reg_on_dst = (!EFI_ERROR(dst_boot->Open(dst_boot, &chk_reg, (CHAR16*)L"REGISTRY.INI", EFI_FILE_MODE_READ, 0)) && chk_reg);
        if (chk_reg) chk_reg->Close(chk_reg);

        if (!reg_saved || !has_reg_on_dst) {
            const char default_reg[] =
                "# BlankOS Registry Hive (REGISTRY.INI)\r\n"
                "System/OSName=str:BlankOS Horizon\r\n"
                "UI/WallpaperTheme=int:0\r\n"
                "UI/DynamicIsland=int:1\r\n"
                "UI/DesktopWidgets=int:1\r\n";
            if (stp_write_small_file(dst_boot, (CHAR16*)L"REGISTRY.INI", default_reg, sizeof(default_reg) - 1)) {
                prog->files_copied++;
                prog->bytes_copied += sizeof(default_reg) - 1;
            }
        }

        prog->progress = 1.0f;
        stp_str_copy(prog->status_line, "Installation complete!", sizeof(prog->status_line));
        prog->current_file[0] = '\0';
        stp_render_installing_frame(SystemTable, prog);
        g_is_live_cd = false;
    }

    if (src_boot) src_boot->Close(src_boot);
    if (src_apps) src_apps->Close(src_apps);
    if (src_efi) src_efi->Close(src_efi);
    if (src_assets) src_assets->Close(src_assets);

    if (dst_boot) dst_boot->Close(dst_boot);
    if (dst_apps) dst_apps->Close(dst_apps);
    if (dst_efi) dst_efi->Close(dst_efi);
    if (dst_assets) dst_assets->Close(dst_assets);

    src_root->Close(src_root);
    dst_root->Close(dst_root);
    SystemTable->BootServices->FreePool(chunk_buf);
    SystemTable->BootServices->FreePool(fs_handles);

    return ok;
}

extern "C" bool is_os_installed(EFI_SYSTEM_TABLE* SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) return false;

    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN num_handles = 0;
    EFI_HANDLE* handle_buffer = NULL;

    EFI_STATUS status = SystemTable->BootServices->LocateHandleBuffer(
        ByProtocol, &fs_guid, NULL, &num_handles, &handle_buffer);

    if (EFI_ERROR(status) || num_handles == 0 || !handle_buffer) {
        if (handle_buffer) SystemTable->BootServices->FreePool(handle_buffer);
        return false;
    }

    bool found = false;
    for (UINTN i = 0; i < num_handles; i++) {
        // Must be a non-optical, writable filesystem handle
        if (stp_is_handle_readonly_media(SystemTable, handle_buffer[i])) continue;

        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        status = SystemTable->BootServices->HandleProtocol(handle_buffer[i], &fs_guid, (void**)&fs);
        if (EFI_ERROR(status) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_HANDLE file = NULL;
        status = root->Open(root, &file, (CHAR16*)L"EFI\\BOOT\\INSTALLED.FLG", EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(status) && file != NULL) {
            file->Close(file);

            // Confirm volume is genuinely writable (not read-only optical emulation)
            EFI_FILE_HANDLE rw_file = NULL;
            EFI_STATUS rw_st = root->Open(
                root, &rw_file, (CHAR16*)L"EFI\\BOOT\\INSTALLED.FLG",
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
            if (!EFI_ERROR(rw_st) && rw_file != NULL) {
                rw_file->Close(rw_file);
                found = true;
            } else if (stp_test_root_writable(root)) {
                found = true;
            }
        }
        root->Close(root);
        if (found) break;
    }

    SystemTable->BootServices->FreePool(handle_buffer);
    return found;
}

extern "C" void launch_setup_screen(EFI_SYSTEM_TABLE* SystemTable) {
    if (SystemTable && SystemTable->ConOut) {
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16*)L"[ OOBE ] Starting Horizon Setup Assistant...\r\n");
    }

    SetupDiskInfo disks[SETUP_MAX_DISKS];
    int num_disks = stp_scan_physical_disks(SystemTable, disks, SETUP_MAX_DISKS);
    int selected_disk_idx = (num_disks > 0) ? 0 : -1;
    bool format_selected = (num_disks > 0 && disks[0].gpt_compatible);

    SetupStep step = STEP_WELCOME;
    bool exit_to_desktop = false;
    bool install_ok = false;
    char install_err[128];
    install_err[0] = '\0';

    SetupInstallProgress prog;
    prog.phase_idx = 0;
    prog.total_phases = 4;
    prog.progress = 0.0f;
    prog.status_line[0] = '\0';
    prog.current_file[0] = '\0';
    prog.bytes_copied = 0;
    prog.files_copied = 0;

    BlankInputState in;

    while (!exit_to_desktop) {
        input_poll(&in);
        audio_tick();

        int sw = (int)(screen_width > 0 ? screen_width : 1024);
        int sh = (int)(screen_height > 0 ? screen_height : 768);

        int card_w = (sw >= 820) ? 720 : (sw - 64);
        int card_h = 480;
        int card_x = (sw - card_w) / 2;
        int card_y = (sh - card_h) / 2;

        // Render Aurora wallpaper + translucent dark scrim
        dui_set_pointer_down(in.left_down);
        dui_draw_wallpaper(dui_get_wallpaper_id());
        dui_rect(0, 0, sw, sh, THEME_BG_VOID, 92);

        dui_shadow(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, 12);
        dui_bento_card(card_x, card_y, card_w, card_h, THEME_RADIUS_CARD, THEME_SURFACE_BASE, 236, true);

        stp_draw_header_steps(card_x, card_y, card_w, (int)step);

        if (step == STEP_WELCOME) {
            int cx = card_x + card_w / 2;

            // Clean Lira seal emblem (zero glow)
            dui_circle(cx, card_y + 104, 24, THEME_SURFACE_CARD, 255);
            dui_circle_outline(cx, card_y + 104, 24, THEME_BORDER_SPECULAR, 200);
            dui_circle(cx, card_y + 104, 9, THEME_ACCENT_CYAN, 240);

            const char* title = "Welcome to BlankOS Horizon";
            int tw = dui_text_width(title, 2);
            dui_text(cx - tw / 2, card_y + 140, title, THEME_TEXT_PRIMARY, 2);

            const char* sub1 = "Install BlankOS Horizon onto a GPT/FAT32 disk for persistent storage,";
            const char* sub2 = "or launch the Live Desktop environment without modifying any drives.";
            dui_text(cx - dui_text_width(sub1, 1) / 2, card_y + 178, sub1, THEME_TEXT_SECONDARY, 1);
            dui_text(cx - dui_text_width(sub2, 1) / 2, card_y + 198, sub2, THEME_TEXT_SECONDARY, 1);

            // Asymmetric Lira Bento Grid: wide Hero Card (left) + 2 compact Cards (right)
            int grid_x = card_x + 44;
            int grid_w = card_w - 88;
            int grid_y = card_y + 230;
            int hero_w = (grid_w * 54) / 100;
            int side_w = grid_w - hero_w - 12;

            dui_bento_card(grid_x, grid_y, hero_w, 134, THEME_RADIUS_LG, THEME_SURFACE_CARD, 215, true);
            dui_dot_grid_overlay(grid_x + 12, grid_y + 12, hero_w - 24, 110, 14, THEME_TEXT_PRIMARY, 10);
            dui_draw_sys_icon(grid_x + 18, grid_y + 18, 20, 17, THEME_ACCENT_CYAN);
            dui_text(grid_x + 46, grid_y + 18, "Lira x Horizon Design", THEME_TEXT_PRIMARY, 1);
            dui_text(grid_x + 18, grid_y + 52, "Bento architecture, tactile bevels,", THEME_TEXT_SECONDARY, 1);
            dui_text(grid_x + 18, grid_y + 72, "collapsible pill chrome & acoustic cues.", THEME_TEXT_SECONDARY, 1);
            dui_status_pill(grid_x + 18, grid_y + 98, 136, 22, "Obsidian + Aurora", THEME_ACCENT_CYAN, THEME_SURFACE_INPUT, true);

            dui_bento_card(grid_x + hero_w + 12, grid_y, side_w, 61, THEME_RADIUS_LG, THEME_SURFACE_CARD, 210, false);
            dui_draw_sys_icon(grid_x + hero_w + 26, grid_y + 14, 16, 19, THEME_ACCENT_TEAL);
            dui_text(grid_x + hero_w + 50, grid_y + 12, "GPT + FAT32 ESP", THEME_TEXT_PRIMARY, 1);
            dui_text(grid_x + hero_w + 50, grid_y + 34, "Native UEFI disk boot", THEME_TEXT_MUTED, 1);

            dui_bento_card(grid_x + hero_w + 12, grid_y + 73, side_w, 61, THEME_RADIUS_LG, THEME_SURFACE_CARD, 210, false);
            dui_draw_sys_icon(grid_x + hero_w + 26, grid_y + 87, 16, 6, THEME_ACCENT_IRIS);
            dui_text(grid_x + hero_w + 50, grid_y + 85, "Live JS Browser", THEME_TEXT_PRIMARY, 1);
            dui_text(grid_x + hero_w + 50, grid_y + 107, "SNP HTTP + DOM scripts", THEME_TEXT_MUTED, 1);

            // Bottom Action Bar
            int btn_y = card_y + card_h - 64;
            if (dui_button(card_x + 44, btn_y, 200, 40, "Try Live Environment",
                           THEME_SURFACE_ELEVATED, false, in.mx, in.my, in.left_pressed) ||
                (in.has_key && in.key_char == ' ')) {
                play_system_sound((char*)"open");
                g_is_live_cd = true;
                exit_to_desktop = true;
            }

            if (dui_button(card_x + card_w - 244, btn_y, 200, 40, "Continue Setup ->",
                           THEME_ACCENT_CYAN, true, in.mx, in.my, in.left_pressed) ||
                (in.has_key && (in.key_char == '\r' || in.key_char == '\n'))) {
                play_system_sound((char*)"tab");
                num_disks = stp_scan_physical_disks(SystemTable, disks, SETUP_MAX_DISKS);
                if (num_disks > 0 && selected_disk_idx < 0) selected_disk_idx = 0;
                step = STEP_DISK_SELECT;
            }
        } else if (step == STEP_DISK_SELECT) {
            int pad_x = card_x + 44;
            int inner_w = card_w - 88;

            dui_text(pad_x, card_y + 76, "Select Target Disk", THEME_TEXT_PRIMARY, 2);
            if (dui_icon_pill(card_x + card_w - 152, card_y + 74, 108, 30, "Rescan", 16,
                              false, in.mx, in.my, in.left_pressed)) {
                play_system_sound((char*)"click");
                num_disks = stp_scan_physical_disks(SystemTable, disks, SETUP_MAX_DISKS);
                selected_disk_idx = (num_disks > 0) ? 0 : -1;
                if (selected_disk_idx >= 0) format_selected = disks[selected_disk_idx].gpt_compatible;
            }

            int list_y = card_y + 120;
            int max_show = 4;
            if (num_disks == 0) {
                dui_glass_card(pad_x, list_y, inner_w, 84, THEME_RADIUS_MD, THEME_SURFACE_CARD, 180, false);
                dui_text(pad_x + 18, list_y + 20, "No writable physical disks detected.", THEME_TEXT_PRIMARY, 1);
                dui_text(pad_x + 18, list_y + 44, "Attach a virtual disk (>= 50 MB) or choose Try Live Environment.", THEME_TEXT_SECONDARY, 1);
            } else {
                for (int i = 0; i < num_disks && i < max_show; i++) {
                    int ry = list_y + i * 54;
                    bool sel = (i == selected_disk_idx);
                    bool hovered = (in.mx >= pad_x && in.mx < pad_x + inner_w && in.my >= ry && in.my < ry + 46);

                    dui_glass_card(pad_x, ry, inner_w, 46, THEME_RADIUS_MD,
                                   sel ? THEME_SURFACE_ELEVATED : (hovered ? THEME_SURFACE_HOVER : THEME_SURFACE_CARD),
                                   sel ? 235 : 195, sel);

                    if (in.left_pressed && hovered) {
                        play_system_sound((char*)"click");
                        selected_disk_idx = i;
                        format_selected = disks[i].gpt_compatible;
                    }

                    // Radio dot
                    dui_circle_outline(pad_x + 22, ry + 23, 8, sel ? THEME_ACCENT_CYAN : THEME_BORDER_SPECULAR, 220);
                    if (sel) dui_circle(pad_x + 22, ry + 23, 4, THEME_ACCENT_CYAN, 255);

                    char row_lbl[64];
                    char num_buf[32];
                    stp_str_copy(row_lbl, "Disk ", sizeof(row_lbl));
                    stp_u64_to_str((uint64_t)i, num_buf);
                    stp_str_append(row_lbl, num_buf, sizeof(row_lbl));
                    stp_str_append(row_lbl, "  -  ", sizeof(row_lbl));
                    stp_u64_to_str(disks[i].size_mb, num_buf);
                    stp_str_append(row_lbl, num_buf, sizeof(row_lbl));
                    stp_str_append(row_lbl, " MB", sizeof(row_lbl));

                    dui_text(pad_x + 42, ry + 14, row_lbl, THEME_TEXT_PRIMARY, 1);

                    const char* badge = disks[i].removable ? "Removable" : "Fixed Drive";
                    if (!disks[i].gpt_compatible) badge = "Too Small / Non-512B";
                    int bw = dui_text_width(badge, 1);
                    dui_text(pad_x + inner_w - bw - 16, ry + 14, badge,
                             disks[i].gpt_compatible ? THEME_TEXT_SECONDARY : THEME_ACCENT_AMBER, 1);
                }
            }

            // Format toggle row
            int tog_y = card_y + 344;
            bool tog_hover = (in.mx >= pad_x && in.mx < pad_x + inner_w && in.my >= tog_y && in.my < tog_y + 44);
            dui_glass_card(pad_x, tog_y, inner_w, 44, THEME_RADIUS_MD, THEME_SURFACE_INPUT, 210, format_selected);
            if (in.left_pressed && tog_hover && selected_disk_idx >= 0) {
                play_system_sound((char*)"click");
                format_selected = !format_selected;
            }
            dui_toggle_switch(pad_x + 14, tog_y + 12, format_selected, THEME_ACCENT_CYAN);
            dui_text(pad_x + 64, tog_y + 13,
                     format_selected ? "Format disk with GPT + FAT32 (Erases all data on disk)"
                                     : "Install to existing FAT32 EFI volume (No format)",
                     format_selected ? THEME_ACCENT_AMBER : THEME_TEXT_SECONDARY, 1);

            // Bottom Action Bar
            int btn_y = card_y + card_h - 64;
            if (dui_button(pad_x, btn_y, 110, 40, "Back",
                           THEME_SURFACE_ELEVATED, false, in.mx, in.my, in.left_pressed) ||
                (in.has_key && in.scan_code == BLANK_SCAN_ESC)) {
                play_system_sound((char*)"click");
                step = STEP_WELCOME;
            }

            if (dui_button(pad_x + 124, btn_y, 180, 40, "Try Live Desktop",
                           THEME_SURFACE_ELEVATED, false, in.mx, in.my, in.left_pressed)) {
                play_system_sound((char*)"click");
                g_is_live_cd = true;
                exit_to_desktop = true;
            }

            bool can_install = (!format_selected) ||
                               (selected_disk_idx >= 0 && selected_disk_idx < num_disks &&
                                disks[selected_disk_idx].gpt_compatible);
            if (dui_button(card_x + card_w - 236, btn_y, 192, 40, "Install BlankOS",
                           can_install ? THEME_ACCENT_CYAN : THEME_SURFACE_CARD, can_install,
                           in.mx, in.my, in.left_pressed && can_install) ||
                (can_install && in.has_key && (in.key_char == '\r' || in.key_char == '\n'))) {
                play_system_sound((char*)"click");
                step = STEP_INSTALLING;
            }
        } else if (step == STEP_INSTALLING) {
            prog.bytes_copied = 0;
            prog.files_copied = 0;
            prog.progress = 0.04f;
            prog.current_file[0] = '\0';
            install_err[0] = '\0';

            EFI_HANDLE sel_handle = (selected_disk_idx >= 0 && selected_disk_idx < num_disks)
                                        ? disks[selected_disk_idx].handle : NULL;
            EFI_BLOCK_IO_PROTOCOL* sel_blk = (selected_disk_idx >= 0 && selected_disk_idx < num_disks)
                                                 ? disks[selected_disk_idx].blk : NULL;

            install_ok = stp_run_real_installation(
                SystemTable, sel_handle, sel_blk, format_selected,
                &prog, install_err, sizeof(install_err));

            play_system_sound(install_ok ? (char*)"success" : "error");
            step = STEP_COMPLETE;
            continue;
        } else if (step == STEP_COMPLETE) {
            int cx = card_x + card_w / 2;
            int pad_x = card_x + 48;

            if (install_ok) {
                dui_circle(cx, card_y + 112, 28, THEME_ACCENT_GREEN, 45);
                dui_draw_sys_icon(cx - 14, card_y + 98, 28, 11, THEME_ACCENT_GREEN);

                const char* title = "BlankOS Horizon Installed";
                dui_text(cx - dui_text_width(title, 2) / 2, card_y + 156, title, THEME_TEXT_PRIMARY, 2);

                char bytes_str[32];
                char files_str[16];
                stp_format_bytes(prog.bytes_copied, bytes_str, sizeof(bytes_str));
                stp_u64_to_str((uint64_t)prog.files_copied, files_str);

                char summary[96];
                stp_str_copy(summary, "Copied ", sizeof(summary));
                stp_str_append(summary, files_str, sizeof(summary));
                stp_str_append(summary, " system files (", sizeof(summary));
                stp_str_append(summary, bytes_str, sizeof(summary));
                stp_str_append(summary, ") to your GPT/FAT32 ESP.", sizeof(summary));
                dui_text(cx - dui_text_width(summary, 1) / 2, card_y + 204, summary, THEME_TEXT_SECONDARY, 1);

                const char* hint = "Remove installation ISO/USB before restarting to boot from disk.";
                dui_text(cx - dui_text_width(hint, 1) / 2, card_y + 244, hint, THEME_TEXT_MUTED, 1);

                int btn_y = card_y + card_h - 68;
                if (dui_button(pad_x, btn_y, 210, 42, "Continue to Desktop",
                               THEME_SURFACE_ELEVATED, false, in.mx, in.my, in.left_pressed)) {
                    play_system_sound((char*)"click");
                    g_is_live_cd = false;
                    exit_to_desktop = true;
                }
                if (dui_button(card_x + card_w - 258, btn_y, 210, 42, "Restart Now",
                               THEME_ACCENT_CYAN, true, in.mx, in.my, in.left_pressed) ||
                    (in.has_key && (in.key_char == '\r' || in.key_char == '\n'))) {
                    play_system_sound((char*)"click");
                    os_restart();
                }
            } else {
                dui_circle(cx, card_y + 112, 28, THEME_ACCENT_ROSE, 45);
                dui_draw_sys_icon(cx - 14, card_y + 98, 28, 12, THEME_ACCENT_ROSE);

                const char* title = "Installation Did Not Complete";
                dui_text(cx - dui_text_width(title, 2) / 2, card_y + 156, title, THEME_TEXT_PRIMARY, 2);

                const char* err_txt = install_err[0] ? install_err : "Unable to write system files to target disk.";
                dui_text(cx - dui_text_width(err_txt, 1) / 2, card_y + 206, err_txt, THEME_ACCENT_AMBER, 1);

                const char* hint = "You can retry with another disk or explore the Live Desktop.";
                dui_text(cx - dui_text_width(hint, 1) / 2, card_y + 244, hint, THEME_TEXT_MUTED, 1);

                int btn_y = card_y + card_h - 68;
                if (dui_button(pad_x, btn_y, 200, 42, "Back to Disk Select",
                               THEME_SURFACE_ELEVATED, false, in.mx, in.my, in.left_pressed)) {
                    play_system_sound((char*)"click");
                    step = STEP_DISK_SELECT;
                }
                if (dui_button(card_x + card_w - 248, btn_y, 200, 42, "Try Live Environment",
                               THEME_ACCENT_CYAN, true, in.mx, in.my, in.left_pressed)) {
                    play_system_sound((char*)"click");
                    g_is_live_cd = true;
                    exit_to_desktop = true;
                }
            }
        }

        blankUI_draw_cursor(in.mx, in.my, 0);
        swap_buffers();

        if (SystemTable && SystemTable->BootServices) {
            SystemTable->BootServices->Stall(16000);
        }
    }
}
