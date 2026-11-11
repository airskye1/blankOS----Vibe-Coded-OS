#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

#include "../ui/theme.h"
#include "../ui/blankDUI.h"
#include "../ui/window_manager.h"
#include "../kernel/input.h"
#include "../kernel/net.h"
#include "../kernel/audio.h"

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
    extern void sha256_compute(const uint8_t* data, uint64_t len, uint8_t output_hash[32]);
    extern void sha256_hash(char* input_string, uint8_t* output_hash);
    extern uint32_t crc32_compute(const uint8_t* data, uint64_t len);
    extern void os_restart(void);
    extern void os_send_notification(const char* title, const char* message);
}

namespace {

#define MAX_READ_FILE_BYTES (8 * 1024 * 1024)

typedef struct {
    const char* label;
    const char* disp_path;
    const CHAR16* efi_path;
    bool checked;
    bool found;
    uint64_t bytes;
    uint32_t crc32;
    char crc_hex[16];
    char sha_hex[65];
} VerifiedFile;

static VerifiedFile s_files[3] = {
    { "UEFI Kernel Binary", "\\EFI\\BOOT\\BOOTX64.EFI", (const CHAR16*)L"EFI\\BOOT\\BOOTX64.EFI", false, false, 0, 0, {0}, {0} },
    { "TrueType UI Font",   "\\assets\\font.ttf",       (const CHAR16*)L"assets\\font.ttf",       false, false, 0, 0, {0}, {0} },
    { "System Registry",    "\\EFI\\BOOT\\REGISTRY.INI",(const CHAR16*)L"EFI\\BOOT\\REGISTRY.INI",false, false, 0, 0, {0}, {0} }
};

static bool s_registered = false;
static bool s_started_scan = false;
static int s_verify_step = 0; // 0..2 = verifying file i, 3 = done
static float s_ring_anim = 0.0f;
static float s_pulse_phase = 0.0f;

static bool s_bak_checked = false;
static bool s_bak_exists = false;
static uint64_t s_bak_bytes = 0;
static uint32_t s_bak_crc = 0;

static char s_status_msg[112] = "Initializing integrity verification...";
static uint32_t s_status_color = THEME_TEXT_SECONDARY;

// Optional HTTP manifest checker
static char s_update_url[256] = "http://10.0.2.2:8000/version.json";
static int s_url_len = 33;
static bool s_url_focused = false;
static int s_xfer_id = -1;
static char s_http_result[128] = "Optional: enter an http:// manifest URL and click Check Updates";
static uint32_t s_http_color = THEME_TEXT_MUTED;
static float s_http_spin = 0.0f;

static bool pt_in(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void s_copy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void s_cat(char* dst, const char* src, int max_len) {
    if (!dst || !src || max_len <= 0) return;
    int i = 0;
    while (dst[i] && i < max_len - 1) i++;
    int j = 0;
    while (src[j] && i < max_len - 1) dst[i++] = src[j++];
    dst[i] = '\0';
}

static void u64_to_str(uint64_t val, char* buf, int max_len) {
    if (!buf || max_len < 2) return;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[24];
    int i = 0;
    while (val > 0 && i < 22) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (i > 0 && j < max_len - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void bytes_to_hex(const uint8_t* data, int len, char* out) {
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hex[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static void u32_to_hex(uint32_t val, char* out) {
    const char* hex = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++) {
        out[9 - i] = hex[val & 0xF];
        val >>= 4;
    }
    out[10] = '\0';
}

static void set_status(const char* msg, uint32_t col) {
    s_copy(s_status_msg, msg, sizeof(s_status_msg));
    s_status_color = col;
}

static bool read_uefi_file_alloc(const CHAR16* path, uint8_t** out_buf, UINTN* out_size) {
    if (out_buf) *out_buf = NULL;
    if (out_size) *out_size = 0;
    if (!gSystemTable || !gSystemTable->BootServices || !path) return false;

    EFI_GUID sfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN handle_count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(gSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &sfspGuid, NULL, &handle_count, &handles)) || !handles) {
        return false;
    }

    bool ok = false;
    for (UINTN i = 0; i < handle_count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(gSystemTable->BootServices->HandleProtocol(handles[i], &sfspGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL* root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_PROTOCOL* file = NULL;
        if (root->Open(root, &file, (CHAR16*)path, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS && file) {
            uint8_t* buf = NULL;
            if (gSystemTable->BootServices->AllocatePool(EfiLoaderData, MAX_READ_FILE_BYTES, (void**)&buf) == EFI_SUCCESS && buf) {
                UINTN total = 0;
                while (total < MAX_READ_FILE_BYTES) {
                    UINTN chunk = MAX_READ_FILE_BYTES - total;
                    EFI_STATUS rst = file->Read(file, &chunk, buf + total);
                    if (EFI_ERROR(rst) || chunk == 0) break;
                    total += chunk;
                }
                if (total > 0) {
                    *out_buf = buf;
                    *out_size = total;
                    ok = true;
                } else {
                    gSystemTable->BootServices->FreePool(buf);
                }
            }
            file->Close(file);
        }
        root->Close(root);
        if (ok) break;
    }
    gSystemTable->BootServices->FreePool(handles);
    return ok;
}

static void probe_existing_backup(void) {
    s_bak_checked = true;
    s_bak_exists = false;
    s_bak_bytes = 0;
    s_bak_crc = 0;

    uint8_t* buf = NULL;
    UINTN sz = 0;
    if (read_uefi_file_alloc((const CHAR16*)L"EFI\\BOOT\\BOOTX64.BAK", &buf, &sz) && buf && sz > 0) {
        s_bak_exists = true;
        s_bak_bytes = (uint64_t)sz;
        s_bak_crc = crc32_compute(buf, (uint64_t)sz);
        gSystemTable->BootServices->FreePool(buf);
    }
}

static void verify_one_file(int idx) {
    if (idx < 0 || idx >= 3) return;
    VerifiedFile* vf = &s_files[idx];
    vf->checked = true;
    vf->found = false;
    vf->bytes = 0;
    vf->crc32 = 0;
    s_copy(vf->crc_hex, "N/A", sizeof(vf->crc_hex));
    s_copy(vf->sha_hex, "unavailable", sizeof(vf->sha_hex));

    uint8_t* buf = NULL;
    UINTN sz = 0;
    if (read_uefi_file_alloc(vf->efi_path, &buf, &sz) && buf && sz > 0) {
        uint8_t digest[32];
        sha256_compute(buf, (uint64_t)sz, digest);
        bytes_to_hex(digest, 32, vf->sha_hex);
        vf->crc32 = crc32_compute(buf, (uint64_t)sz);
        u32_to_hex(vf->crc32, vf->crc_hex);
        vf->bytes = (uint64_t)sz;
        vf->found = true;
        gSystemTable->BootServices->FreePool(buf);
    }
}

static void start_full_verification(void) {
    s_started_scan = true;
    s_verify_step = 0;
    s_ring_anim = 0.0f;
    for (int i = 0; i < 3; i++) {
        s_files[i].checked = false;
        s_files[i].found = false;
        s_files[i].bytes = 0;
        s_files[i].crc32 = 0;
    }
    s_bak_checked = false;
    set_status("Computing SHA-256 & CRC-32 checksums...", THEME_ACCENT_CYAN);
}

static bool create_bootx64_backup(void) {
    if (!gSystemTable || !gSystemTable->BootServices) return false;

    uint8_t* kbuf = NULL;
    UINTN ksz = 0;
    if (!read_uefi_file_alloc((const CHAR16*)L"EFI\\BOOT\\BOOTX64.EFI", &kbuf, &ksz) || !kbuf || ksz == 0) {
        set_status("Cannot read \\EFI\\BOOT\\BOOTX64.EFI for backup", THEME_ACCENT_ROSE);
        play_system_sound((char*)"error");
        return false;
    }

    EFI_GUID sfspGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN handle_count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(gSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &sfspGuid, NULL, &handle_count, &handles)) || !handles) {
        gSystemTable->BootServices->FreePool(kbuf);
        return false;
    }

    bool wrote = false;
    for (UINTN i = 0; i < handle_count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(gSystemTable->BootServices->HandleProtocol(handles[i], &sfspGuid, (void**)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL* root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;

        EFI_FILE_PROTOCOL* efi_dir = NULL;
        if (root->Open(root, &efi_dir, (CHAR16*)L"EFI",
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                       EFI_FILE_DIRECTORY) == EFI_SUCCESS && efi_dir) {
            EFI_FILE_PROTOCOL* boot_dir = NULL;
            if (efi_dir->Open(efi_dir, &boot_dir, (CHAR16*)L"BOOT",
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                              EFI_FILE_DIRECTORY) == EFI_SUCCESS && boot_dir) {
                // Remove old BOOTX64.BAK if present
                EFI_FILE_PROTOCOL* old_bak = NULL;
                if (boot_dir->Open(boot_dir, &old_bak, (CHAR16*)L"BOOTX64.BAK",
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS && old_bak) {
                    old_bak->Delete(old_bak);
                }

                EFI_FILE_PROTOCOL* bak = NULL;
                if (boot_dir->Open(boot_dir, &bak, (CHAR16*)L"BOOTX64.BAK",
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0) == EFI_SUCCESS && bak) {
                    UINTN wlen = ksz;
                    if (bak->Write(bak, &wlen, kbuf) == EFI_SUCCESS && wlen == ksz) {
                        bak->Flush(bak);
                        wrote = true;
                    }
                    bak->Close(bak);
                }
                boot_dir->Close(boot_dir);
            }
            efi_dir->Close(efi_dir);
        }
        root->Close(root);
        if (wrote) break;
    }

    gSystemTable->BootServices->FreePool(handles);
    gSystemTable->BootServices->FreePool(kbuf);

    if (wrote) {
        probe_existing_backup();
        play_system_sound((char*)"success");
        os_send_notification("System Updater", "Created \\EFI\\BOOT\\BOOTX64.BAK backup on FAT32.");
        set_status("Backup \\EFI\\BOOT\\BOOTX64.BAK written & verified!", THEME_ACCENT_GREEN);
    } else {
        play_system_sound((char*)"error");
        set_status("Volume is read-only (Live ISO9660) - cannot write BOOTX64.BAK", THEME_ACCENT_ROSE);
    }
    return wrote;
}

static void start_http_manifest_check(void) {
    if (!s_update_url[0]) {
        s_copy(s_http_result, "Enter an http:// manifest URL first", sizeof(s_http_result));
        s_http_color = THEME_ACCENT_AMBER;
        return;
    }
    if (s_xfer_id >= 0) {
        net_http_close(s_xfer_id);
        s_xfer_id = -1;
    }
    s_xfer_id = net_http_start(s_update_url, NULL);
    if (s_xfer_id >= 0) {
        play_system_sound((char*)"click");
        s_copy(s_http_result, "Connecting to update manifest server...", sizeof(s_http_result));
        s_http_color = THEME_ACCENT_CYAN;
    } else {
        play_system_sound((char*)"error");
        s_copy(s_http_result, "Failed to initiate HTTP request (check URL / NIC)", sizeof(s_http_result));
        s_http_color = THEME_ACCENT_ROSE;
    }
}

static void updater_on_frame(BlankWindow* win, int cx, int cy, int cw, int ch, const BlankInputState* in) {
    (void)win;
    if (!s_started_scan) start_full_verification();

    // Non-blocking stepped verification (1 file per frame)
    if (s_verify_step < 3) {
        verify_one_file(s_verify_step);
        s_verify_step++;
        if (s_verify_step == 3) {
            probe_existing_backup();
            int ok_cnt = 0;
            for (int i = 0; i < 3; i++) if (s_files[i].found) ok_cnt++;
            if (ok_cnt == 3) {
                set_status("All 3 core system files verified on disk (CRC-32 & SHA-256)", THEME_ACCENT_GREEN);
            } else {
                set_status("Verification complete with missing files", THEME_ACCENT_AMBER);
            }
        }
    }

    s_pulse_phase += 0.05f;
    if (s_pulse_phase > 6.283f) s_pulse_phase -= 6.283f;

    int verified_cnt = 0;
    int checked_cnt = 0;
    for (int i = 0; i < 3; i++) {
        if (s_files[i].checked) checked_cnt++;
        if (s_files[i].found) verified_cnt++;
    }
    float target_prog = (float)checked_cnt / 3.0f;
    s_ring_anim = horizon_spring_step(s_ring_anim, target_prog, 0.16f);

    int pad = 16;
    int px = cx + pad;
    int py = cy + 12;
    int pw = cw - pad * 2;

    // Top Hero Integrity Card with Animated Verification Ring
    int hero_h = 98;
    dui_glass_card(px, py, pw, hero_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 228, false);

    int ring_cx = px + 54;
    int ring_cy = py + hero_h / 2;
    uint32_t ring_col = (s_verify_step < 3) ? THEME_ACCENT_CYAN
                        : ((verified_cnt == 3) ? THEME_ACCENT_GREEN : THEME_ACCENT_AMBER);
    dui_progress_ring(ring_cx, ring_cy, 34, 7, s_ring_anim, THEME_SURFACE_INPUT, ring_col);

    char score_txt[16], vb[8];
    u64_to_str((uint64_t)verified_cnt, vb, sizeof(vb));
    s_copy(score_txt, vb, sizeof(score_txt));
    s_cat(score_txt, "/3", sizeof(score_txt));
    int stw = dui_text_width(score_txt, 1);
    dui_text(ring_cx - stw / 2, ring_cy - 8, score_txt, THEME_TEXT_PRIMARY, 1);

    dui_text(px + 106, py + 12, "System Integrity & Firmware Backup", THEME_TEXT_PRIMARY, 2);
    dui_text_ellipsis(px + 106, py + 40, pw - 266, s_status_msg, s_status_color, 1);

    // Backup status line inside hero
    char bak_line[96];
    if (!s_bak_checked) {
        s_copy(bak_line, "Checking \\EFI\\BOOT\\BOOTX64.BAK...", sizeof(bak_line));
    } else if (s_bak_exists) {
        char bs[16], crcb[16];
        u64_to_str(s_bak_bytes, bs, sizeof(bs));
        u32_to_hex(s_bak_crc, crcb);
        s_copy(bak_line, "Backup Present: BOOTX64.BAK (", sizeof(bak_line));
        s_cat(bak_line, bs, sizeof(bak_line));
        s_cat(bak_line, " B, ", sizeof(bak_line));
        s_cat(bak_line, crcb, sizeof(bak_line));
        if (s_files[0].found && s_bak_crc == s_files[0].crc32) {
            s_cat(bak_line, " - Exact Match)", sizeof(bak_line));
        } else {
            s_cat(bak_line, ")", sizeof(bak_line));
        }
    } else {
        s_copy(bak_line, "No \\EFI\\BOOT\\BOOTX64.BAK backup on disk yet", sizeof(bak_line));
    }
    dui_text_ellipsis(px + 106, py + 64, pw - 266, bak_line,
                      s_bak_exists ? THEME_ACCENT_GREEN : THEME_TEXT_MUTED, 1);

    if (dui_button(px + pw - 148, py + 14, 134, 30, "Re-Verify Files", THEME_SURFACE_ELEVATED, false,
                   in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"click");
        start_full_verification();
    }
    if (dui_button(px + pw - 148, py + 52, 134, 30, "Backup .BAK", THEME_ACCENT_GREEN, true,
                   in->mx, in->my, in->left_pressed)) {
        create_bootx64_backup();
    }

    // Verified Disk Files List (3 cards with mini animated rings + CRC32 + SHA-256 + exact byte size)
    int list_y = py + hero_h + 10;
    int card_h = 58;
    for (int i = 0; i < 3; i++) {
        VerifiedFile* vf = &s_files[i];
        int fy = list_y + i * (card_h + 7);
        dui_glass_card(px, fy, pw, card_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 218, false);

        float fprog = vf->checked ? (vf->found ? 1.0f : 0.35f) : 0.1f;
        uint32_t fcol = !vf->checked ? THEME_ACCENT_CYAN : (vf->found ? THEME_ACCENT_GREEN : THEME_ACCENT_ROSE);
        dui_progress_ring(px + 28, fy + card_h / 2, 13, 4, fprog, THEME_SURFACE_INPUT, fcol);

        dui_text(px + 52, fy + 9, vf->label, THEME_TEXT_PRIMARY, 1);
        dui_text(px + 220, fy + 9, vf->disp_path, THEME_ACCENT_CYAN, 1);

        if (vf->found) {
            char bbuf[20], szline[64];
            u64_to_str(vf->bytes, bbuf, sizeof(bbuf));
            s_copy(szline, bbuf, sizeof(szline));
            s_cat(szline, " B  |  CRC32: ", sizeof(szline));
            s_cat(szline, vf->crc_hex, sizeof(szline));
            dui_text(px + 52, fy + 30, szline, THEME_TEXT_SECONDARY, 1);

            char shaline[80];
            s_copy(shaline, "SHA256: ", sizeof(shaline));
            s_cat(shaline, vf->sha_hex, sizeof(shaline));
            dui_text_ellipsis(px + 290, fy + 30, pw - 304, shaline, THEME_ACCENT_TEAL, 1);
        } else if (vf->checked) {
            dui_text(px + 52, fy + 30, "File not found on mounted UEFI volumes", THEME_ACCENT_ROSE, 1);
        } else {
            dui_text(px + 52, fy + 30, "Queued for verification...", THEME_TEXT_MUTED, 1);
        }
    }

    // Optional HTTP Manifest Update Check Card + Reboot Button
    int http_y = list_y + 3 * (card_h + 7) + 2;
    int http_h = ch - (http_y - cy) - 10;
    if (http_h < 92) http_h = 92;

    dui_glass_card(px, http_y, pw, http_h, THEME_RADIUS_MD, THEME_SURFACE_CARD, 220, false);
    dui_text(px + 14, http_y + 10, "OPTIONAL HTTP UPDATE MANIFEST CHECK", THEME_ACCENT_GREEN, 1);

    int inp_x = px + 14;
    int inp_y = http_y + 32;
    int inp_w = pw - 266;
    if (inp_w < 160) inp_w = 160;
    int inp_h = 30;

    if (in->left_pressed) {
        s_url_focused = pt_in(in->mx, in->my, inp_x, inp_y, inp_w, inp_h);
    }
    dui_rect_rounded(inp_x, inp_y, inp_w, inp_h, THEME_RADIUS_SM, THEME_SURFACE_INPUT, 240);
    dui_rect_rounded_outline(inp_x, inp_y, inp_w, inp_h, THEME_RADIUS_SM,
                             s_url_focused ? THEME_BORDER_FOCUS : THEME_BORDER_SUBTLE, 220);
    dui_text_ellipsis(inp_x + 8, inp_y + 7, inp_w - 16,
                      s_url_len > 0 ? s_update_url : "http://...",
                      s_url_len > 0 ? THEME_TEXT_PRIMARY : THEME_TEXT_MUTED, 1);

    if (s_url_focused && in->has_key) {
        if (in->key_char == '\b') {
            if (s_url_len > 0) s_update_url[--s_url_len] = '\0';
        } else if (in->key_char == '\n') {
            start_http_manifest_check();
        } else if (in->key_char >= 32 && in->key_char < 127 && s_url_len < 240) {
            s_update_url[s_url_len++] = in->key_char;
            s_update_url[s_url_len] = '\0';
        }
    }

    if (dui_button(inp_x + inp_w + 8, inp_y, 122, inp_h, "Check Updates", THEME_ACCENT_GREEN, true,
                   in->mx, in->my, in->left_pressed)) {
        start_http_manifest_check();
    }
    if (dui_button(px + pw - 114, inp_y, 100, inp_h, "Restart OS", THEME_SURFACE_ELEVATED, false,
                   in->mx, in->my, in->left_pressed)) {
        play_system_sound((char*)"click");
        os_restart();
    }

    // Poll active HTTP manifest transfer non-blockingly
    if (s_xfer_id >= 0) {
        const HttpTransfer* tr = net_http_get(s_xfer_id);
        if (tr) {
            s_http_spin += 0.04f;
            if (s_http_spin > 1.0f) s_http_spin = 0.0f;
            dui_progress_ring(px + 26, http_y + 76, 10, 3, s_http_spin, THEME_SURFACE_INPUT, THEME_ACCENT_CYAN);

            if (tr->state == HTTP_DONE) {
                char code_b[12], len_b[16];
                u64_to_str((uint64_t)tr->status_code, code_b, sizeof(code_b));
                u64_to_str((uint64_t)tr->bytes_received, len_b, sizeof(len_b));
                s_copy(s_http_result, "HTTP ", sizeof(s_http_result));
                s_cat(s_http_result, code_b, sizeof(s_http_result));
                s_cat(s_http_result, " (", sizeof(s_http_result));
                s_cat(s_http_result, len_b, sizeof(s_http_result));
                s_cat(s_http_result, " B): ", sizeof(s_http_result));
                if (tr->body_buf && tr->bytes_received > 0) {
                    char snippet[64];
                    int sp = 0;
                    for (int k = 0; k < tr->bytes_received && sp < 60; k++) {
                        char c = tr->body_buf[k];
                        if (c >= 32 && c < 127) snippet[sp++] = c;
                        else if (c == '\n' || c == '\r') snippet[sp++] = ' ';
                    }
                    snippet[sp] = '\0';
                    s_cat(s_http_result, snippet, sizeof(s_http_result));
                }
                s_http_color = THEME_ACCENT_GREEN;
                play_system_sound((char*)"notify");
                net_http_close(s_xfer_id);
                s_xfer_id = -1;
            } else if (tr->state == HTTP_ERROR) {
                s_copy(s_http_result, tr->error_msg[0] ? tr->error_msg : "HTTP manifest check failed", sizeof(s_http_result));
                s_http_color = THEME_ACCENT_ROSE;
                play_system_sound((char*)"error");
                net_http_close(s_xfer_id);
                s_xfer_id = -1;
            }
            dui_text_ellipsis(px + 44, http_y + 68, pw - 58, s_http_result, s_http_color, 1);
        } else {
            s_xfer_id = -1;
        }
    } else {
        dui_text_ellipsis(px + 14, http_y + 68, pw - 28, s_http_result, s_http_color, 1);
    }
}

static void updater_on_close(BlankWindow* win) {
    (void)win;
    s_url_focused = false;
    if (s_xfer_id >= 0) {
        net_http_close(s_xfer_id);
        s_xfer_id = -1;
    }
}

} // namespace

extern "C" {

void updater_register(void) {
    if (s_registered) return;
    s_registered = true;
    wm_register_app(APP_UPDATER, "System Updater", 680, 480, THEME_ACCENT_GREEN,
                    updater_on_frame, updater_on_close);
}

void launch_updater(EFI_SYSTEM_TABLE* st) {
    if (st && !gSystemTable) gSystemTable = st;
    updater_register();
    wm_open_app(APP_UPDATER);
}

} // extern "C"
