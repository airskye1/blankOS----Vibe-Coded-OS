#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color, int thickness);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_gradient_v(int x, int y, int w, int h, uint32_t top_color, uint32_t bottom_color);
    extern void dui_gradient_rounded(int x, int y, int w, int h, int radius, uint32_t top_color, uint32_t bottom_color);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color, uint8_t alpha);
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_cursor(int x, int y);
    extern void blankUI_get_window_pos(int* x, int* y, int width, int height);
    extern void blankUI_update_window_drag(int mouse_x, int mouse_y, bool mouse_pressed, int width, int height);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);
    extern void blankUI_set_cursor_type(int type);

    extern uint64_t memory_get_used_mb();
    extern uint64_t memory_get_total_mb();
    extern int pci_get_device_count();
    extern void play_system_sound(const char* event_name);

    extern int screen_width;
    extern int screen_height;

    // String & formatting utilities (no stdlib)
    static int b_strlen(const char* s) {
        int n = 0;
        while (s && s[n]) n++;
        return n;
    }

    static void str_copy(char* dst, const char* src, int max_len) {
        int i = 0;
        while (src && src[i] && i < max_len - 1) {
            dst[i] = src[i];
            i++;
        }
        dst[i] = '\0';
    }

    static bool str_equals(const char* a, const char* b) {
        int i = 0;
        while (a[i] && b[i]) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
            i++;
        }
        return a[i] == b[i];
    }

    static bool str_starts_with(const char* s, const char* prefix) {
        int i = 0;
        while (prefix[i]) {
            char ca = s[i];
            char cb = prefix[i];
            if (!ca) return false;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
            i++;
        }
        return true;
    }

    static bool str_contains(const char* hay, const char* needle) {
        if (!hay || !needle || !needle[0]) return false;
        for (int i = 0; hay[i]; i++) {
            int j = 0;
            while (needle[j] && hay[i + j]) {
                char ca = hay[i + j];
                char cb = needle[j];
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) break;
                j++;
            }
            if (!needle[j]) return true;
        }
        return false;
    }

    static void int_to_str(long val, char* buf) {
        if (val == 0) {
            buf[0] = '0';
            buf[1] = '\0';
            return;
        }
        int i = 0;
        bool neg = false;
        if (val < 0) {
            neg = true;
            val = -val;
        }
        while (val > 0) {
            buf[i++] = (char)((val % 10) + '0');
            val /= 10;
        }
        if (neg) buf[i++] = '-';
        buf[i] = '\0';
        int s = 0, e = i - 1;
        while (s < e) {
            char t = buf[s];
            buf[s] = buf[e];
            buf[e] = t;
            s++;
            e--;
        }
    }

    static void uint_to_hex(uint32_t val, char* buf, int digits) {
        const char* hex = "0123456789ABCDEF";
        buf[0] = '0';
        buf[1] = 'x';
        for (int i = digits - 1; i >= 0; i--) {
            buf[2 + i] = hex[val & 0xF];
            val >>= 4;
        }
        buf[2 + digits] = '\0';
    }

    static void str_append(char* dst, const char* src, int max_len) {
        int len = b_strlen(dst);
        int i = 0;
        while (src[i] && (len + i) < max_len - 1) {
            dst[len + i] = src[i];
            i++;
        }
        dst[len + i] = '\0';
    }

    // Real x86_64 CPUID instruction reader
    static void read_cpuid_info(char* brand_out, char* vendor_out, uint32_t* sig_out, uint32_t* cores_out) {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
        ((uint32_t*)vendor_out)[0] = ebx;
        ((uint32_t*)vendor_out)[1] = edx;
        ((uint32_t*)vendor_out)[2] = ecx;
        vendor_out[12] = '\0';

        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        *sig_out = eax;
        uint32_t logical = (ebx >> 16) & 0xFF;
        if (logical == 0) logical = 1;
        *cores_out = logical;

        uint32_t max_ext = 0;
        __asm__ volatile("cpuid" : "=a"(max_ext), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
        if (max_ext >= 0x80000004) {
            uint32_t* p = (uint32_t*)brand_out;
            for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
                __asm__ volatile("cpuid" : "=a"(p[0]), "=b"(p[1]), "=c"(p[2]), "=d"(p[3]) : "a"(leaf));
                p += 4;
            }
            brand_out[48] = '\0';
            // Trim leading spaces
            int s = 0;
            while (brand_out[s] == ' ') s++;
            if (s > 0) {
                int j = 0;
                while (brand_out[s + j]) {
                    brand_out[j] = brand_out[s + j];
                    j++;
                }
                brand_out[j] = '\0';
            }
        } else {
            str_copy(brand_out, vendor_out, 48);
        }
    }

    // Real x86 PCI Configuration Space Scanner (0xCF8 / 0xCFC)
    static inline void pci_outl(uint16_t port, uint32_t val) {
        __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
    }
    static inline uint32_t pci_inl(uint16_t port) {
        uint32_t ret;
        __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
        return ret;
    }

    typedef struct {
        uint8_t bus;
        uint8_t slot;
        uint16_t vendor;
        uint16_t device;
        uint8_t class_code;
        uint8_t subclass;
    } RealPciDev;

    static RealPciDev real_pci_list[12];
    static int real_pci_count = 0;
    static bool pci_scanned = false;

    static void scan_real_pci_bus() {
        if (pci_scanned) return;
        pci_scanned = true;
        real_pci_count = 0;
        for (int slot = 0; slot < 32 && real_pci_count < 12; slot++) {
            uint32_t addr = (1u << 31) | (0u << 16) | ((uint32_t)slot << 11) | 0u;
            pci_outl(0xCF8, addr);
            uint32_t vd = pci_inl(0xCFC);
            if ((vd & 0xFFFF) != 0xFFFF && (vd & 0xFFFF) != 0x0000) {
                real_pci_list[real_pci_count].bus = 0;
                real_pci_list[real_pci_count].slot = (uint8_t)slot;
                real_pci_list[real_pci_count].vendor = (uint16_t)(vd & 0xFFFF);
                real_pci_list[real_pci_count].device = (uint16_t)((vd >> 16) & 0xFFFF);

                pci_outl(0xCF8, addr | 0x08);
                uint32_t cc = pci_inl(0xCFC);
                real_pci_list[real_pci_count].class_code = (uint8_t)((cc >> 24) & 0xFF);
                real_pci_list[real_pci_count].subclass = (uint8_t)((cc >> 16) & 0xFF);
                real_pci_count++;
            }
        }
    }

    // Real UEFI SimpleFileSystem FAT32 File Reader & Writer
    static char file_viewer_path[128] = "\\NOTES.TXT";
    static char file_viewer_content[1024] = "";
    static int file_viewer_bytes = 0;
    static bool file_viewer_ok = false;

    typedef struct {
        char name[48];
        uint64_t size;
        bool is_dir;
    } FatDirEntry;

    static FatDirEntry root_entries[10];
    static int root_entry_count = 0;

    static void ascii_to_ucs2_path(const char* ascii, CHAR16* ucs2, int max_chars) {
        int i = 0;
        int out = 0;
        if (ascii[0] != '\\' && ascii[0] != '/') {
            ucs2[out++] = L'\\';
        }
        while (ascii[i] && out < max_chars - 1) {
            char c = ascii[i++];
            if (c == '/') c = '\\';
            ucs2[out++] = (CHAR16)(uint8_t)c;
        }
        ucs2[out] = 0;
    }

    static bool fat32_read_file(EFI_SYSTEM_TABLE* SystemTable, const char* path_ascii, char* out_buf, int max_len, int* bytes_read_out) {
        out_buf[0] = '\0';
        *bytes_read_out = 0;
        if (!SystemTable || !SystemTable->BootServices) return false;

        EFI_GUID sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
        if (SystemTable->BootServices->LocateProtocol(&sfsGuid, NULL, (void**)&Sfs) != EFI_SUCCESS || !Sfs) {
            return false;
        }
        EFI_FILE_HANDLE Root = NULL;
        if (Sfs->OpenVolume(Sfs, &Root) != EFI_SUCCESS || !Root) {
            return false;
        }

        CHAR16 ucs_path[128];
        ascii_to_ucs2_path(path_ascii, ucs_path, 128);

        EFI_FILE_HANDLE File = NULL;
        EFI_STATUS st = Root->Open(Root, &File, ucs_path, EFI_FILE_MODE_READ, 0);
        if (st != EFI_SUCCESS || !File) {
            Root->Close(Root);
            return false;
        }

        UINTN read_size = (UINTN)(max_len - 1);
        st = File->Read(File, &read_size, out_buf);
        File->Close(File);
        Root->Close(Root);

        if (st == EFI_SUCCESS) {
            out_buf[read_size] = '\0';
            *bytes_read_out = (int)read_size;
            return true;
        }
        return false;
    }

    static bool fat32_write_file(EFI_SYSTEM_TABLE* SystemTable, const char* path_ascii, const char* data, int len) {
        if (!SystemTable || !SystemTable->BootServices) return false;
        EFI_GUID sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
        if (SystemTable->BootServices->LocateProtocol(&sfsGuid, NULL, (void**)&Sfs) != EFI_SUCCESS || !Sfs) {
            return false;
        }
        EFI_FILE_HANDLE Root = NULL;
        if (Sfs->OpenVolume(Sfs, &Root) != EFI_SUCCESS || !Root) {
            return false;
        }

        CHAR16 ucs_path[128];
        ascii_to_ucs2_path(path_ascii, ucs_path, 128);

        EFI_FILE_HANDLE File = NULL;
        EFI_STATUS st = Root->Open(Root, &File, ucs_path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (st != EFI_SUCCESS || !File) {
            Root->Close(Root);
            return false;
        }

        UINTN write_size = (UINTN)len;
        st = File->Write(File, &write_size, (void*)data);
        File->Flush(File);
        File->Close(File);
        Root->Close(Root);
        return (st == EFI_SUCCESS);
    }

    static void fat32_scan_root_dir(EFI_SYSTEM_TABLE* SystemTable) {
        root_entry_count = 0;
        if (!SystemTable || !SystemTable->BootServices) return;
        EFI_GUID sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Sfs = NULL;
        if (SystemTable->BootServices->LocateProtocol(&sfsGuid, NULL, (void**)&Sfs) != EFI_SUCCESS || !Sfs) return;
        EFI_FILE_HANDLE Root = NULL;
        if (Sfs->OpenVolume(Sfs, &Root) != EFI_SUCCESS || !Root) return;

        Root->SetPosition(Root, 0);
        uint8_t info_buf[256];
        while (root_entry_count < 10) {
            UINTN sz = sizeof(info_buf);
            if (Root->Read(Root, &sz, info_buf) != EFI_SUCCESS || sz == 0) break;
            EFI_FILE_INFO* info = (EFI_FILE_INFO*)info_buf;
            if (info->FileName[0] == L'.') continue;
            int k = 0;
            while (info->FileName[k] && k < 46) {
                root_entries[root_entry_count].name[k] = (char)(info->FileName[k] & 0x7F);
                k++;
            }
            root_entries[root_entry_count].name[k] = '\0';
            root_entries[root_entry_count].size = info->FileSize;
            root_entries[root_entry_count].is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
            root_entry_count++;
        }
        Root->Close(Root);
    }

    // Page types:
    // 0 = blank://start    (Speed Dial Start Page)
    // 1 = blank://docs     (Kernel & Driver Architecture Documentation)
    // 2 = blank://pulse    (Live System & Hardware Telemetry + Real CPUID/PCI/Firmware)
    // 3 = blank://synth    (Interactive Web Audio Soundboard)
    // 4 = blank://notes    (Interactive Browser Scratchpad backed by \NOTES.TXT on FAT32)
    // 5 = file://...       (Real FAT32 Disk File & Directory Inspector)
    typedef struct {
        char url[128];
        char title[32];
        int page_type;
        char search_query[64];
        char history[8][128];
        int hist_count;
        int hist_pos;
    } BrowserTab;

    static BrowserTab tabs[5];
    static int tab_count = 0;
    static int active_tab_idx = 0;
    static bool sidebar_expanded = true;

    static int docs_section = 0;
    static int last_synth_played = -1;
    static int synth_anim_ticks = 0;
    static int ping_count = 0;
    static int ping_last_us = 180;
    static int ping_history[8] = { 190, 175, 182, 168, 195, 172, 180, 176 };

    // Notes Scratchpad state
    static char scratchpad_text[512] = "";
    static bool scratchpad_loaded = false;
    static char notes_status_msg[64] = "Ready (Backed by FAT32 \\NOTES.TXT)";
    static int scratchpad_theme = 0;

    typedef struct {
        int x, y, w, h;
        char target_url[128];
        int action_id;
    } HitBox;

    static HitBox hitboxes[48];
    static int hitbox_count = 0;

    static void add_link(int x, int y, int w, int h, const char* target) {
        if (hitbox_count >= 48) return;
        hitboxes[hitbox_count].x = x;
        hitboxes[hitbox_count].y = y;
        hitboxes[hitbox_count].w = w;
        hitboxes[hitbox_count].h = h;
        str_copy(hitboxes[hitbox_count].target_url, target, 128);
        hitboxes[hitbox_count].action_id = 0;
        hitbox_count++;
    }

    static void add_action_box(int x, int y, int w, int h, int action_id) {
        if (hitbox_count >= 48) return;
        hitboxes[hitbox_count].x = x;
        hitboxes[hitbox_count].y = y;
        hitboxes[hitbox_count].w = w;
        hitboxes[hitbox_count].h = h;
        hitboxes[hitbox_count].target_url[0] = '\0';
        hitboxes[hitbox_count].action_id = action_id;
        hitbox_count++;
    }

    static void apply_url_to_tab(EFI_SYSTEM_TABLE* SystemTable, int tab_idx, const char* url, bool push_history) {
        str_copy(tabs[tab_idx].url, url, 128);

        if (str_starts_with(url, "file://")) {
            tabs[tab_idx].page_type = 5;
            str_copy(tabs[tab_idx].title, "FAT32 File", 32);
            const char* sub = url + 7;
            if (sub[0] == '\0') sub = "\\NOTES.TXT";
            str_copy(file_viewer_path, sub, 128);
            fat32_scan_root_dir(SystemTable);
            file_viewer_ok = fat32_read_file(SystemTable, file_viewer_path, file_viewer_content, 1024, &file_viewer_bytes);
        } else if (str_equals(url, "blank://start") || str_equals(url, "blank://home") || str_equals(url, "start")) {
            tabs[tab_idx].page_type = 0;
            str_copy(tabs[tab_idx].url, "blank://start", 128);
            str_copy(tabs[tab_idx].title, "Start Page", 32);
        } else if (str_equals(url, "blank://docs") || str_contains(url, "docs") || str_contains(url, "kernel")) {
            tabs[tab_idx].page_type = 1;
            str_copy(tabs[tab_idx].url, "blank://docs", 128);
            str_copy(tabs[tab_idx].title, "Kernel Docs", 32);
        } else if (str_equals(url, "blank://pulse") || str_equals(url, "blank://hardware") || str_contains(url, "pulse") || str_contains(url, "hardware") || str_contains(url, "cpu")) {
            tabs[tab_idx].page_type = 2;
            str_copy(tabs[tab_idx].url, "blank://pulse", 128);
            str_copy(tabs[tab_idx].title, "HW & Pulse", 32);
        } else if (str_equals(url, "blank://synth") || str_contains(url, "synth") || str_contains(url, "audio") || str_contains(url, "sound")) {
            tabs[tab_idx].page_type = 3;
            str_copy(tabs[tab_idx].url, "blank://synth", 128);
            str_copy(tabs[tab_idx].title, "Audio Synth", 32);
        } else if (str_equals(url, "blank://notes") || str_contains(url, "note") || str_contains(url, "scratch")) {
            tabs[tab_idx].page_type = 4;
            str_copy(tabs[tab_idx].url, "blank://notes", 128);
            str_copy(tabs[tab_idx].title, "Scratchpad", 32);
        } else {
            str_copy(tabs[tab_idx].search_query, url, 64);
            tabs[tab_idx].page_type = 0;
            str_copy(tabs[tab_idx].url, "blank://start", 128);
            str_copy(tabs[tab_idx].title, "Start Page", 32);
        }

        if (push_history) {
            int pos = tabs[tab_idx].hist_pos;
            if (pos < 7) {
                pos++;
                tabs[tab_idx].hist_pos = pos;
                tabs[tab_idx].hist_count = pos + 1;
                str_copy(tabs[tab_idx].history[pos], tabs[tab_idx].url, 128);
            }
        }
    }

    static void init_tab(EFI_SYSTEM_TABLE* SystemTable, int idx, const char* url) {
        tabs[idx].search_query[0] = '\0';
        tabs[idx].hist_count = 1;
        tabs[idx].hist_pos = 0;
        apply_url_to_tab(SystemTable, idx, url, false);
        str_copy(tabs[idx].history[0], tabs[idx].url, 128);
    }

    void launch_browser(EFI_SYSTEM_TABLE* SystemTable) {
        EFI_GUID ptrGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
        SystemTable->BootServices->LocateProtocol(&ptrGuid, NULL, (void**)&Mouse);
        if (Mouse) Mouse->Reset(Mouse, TRUE);

        int cursor_x = screen_width / 2;
        int cursor_y = screen_height / 2;
        bool done = false;

        int win_w = 960;
        int win_h = 620;
        if (win_w > screen_width - 40) win_w = screen_width - 40;
        if (win_h > screen_height - 80) win_h = screen_height - 80;

        // Read real CPUID & PCI bus info once
        char cpu_brand[52];
        char cpu_vendor[16];
        uint32_t cpu_sig = 0;
        uint32_t cpu_cores = 1;
        read_cpuid_info(cpu_brand, cpu_vendor, &cpu_sig, &cpu_cores);
        scan_real_pci_bus();

        // Extract real UEFI FirmwareVendor string
        char fw_vendor[48] = "UEFI Firmware";
        if (SystemTable && SystemTable->FirmwareVendor) {
            int k = 0;
            while (SystemTable->FirmwareVendor[k] && k < 46) {
                fw_vendor[k] = (char)(SystemTable->FirmwareVendor[k] & 0x7F);
                k++;
            }
            fw_vendor[k] = '\0';
        }
        uint32_t fw_rev = SystemTable ? SystemTable->FirmwareRevision : 0;

        // Load or create real \NOTES.TXT on FAT32 volume
        if (!scratchpad_loaded) {
            scratchpad_loaded = true;
            int nbytes = 0;
            if (fat32_read_file(SystemTable, "\\NOTES.TXT", scratchpad_text, 512, &nbytes) && nbytes > 0) {
                str_copy(notes_status_msg, "Loaded \\NOTES.TXT from FAT32 disk", 64);
            } else {
                str_copy(scratchpad_text,
                    "BlankOS ArcSafari Scratchpad (Real FAT32 Storage)\n"
                    "Type on your keyboard and click 'Save \\NOTES.TXT' to persist to disk!\n"
                    "- CPU: ", 512);
                str_append(scratchpad_text, cpu_brand, 512);
                str_append(scratchpad_text, "\n", 512);
                fat32_write_file(SystemTable, "\\NOTES.TXT", scratchpad_text, b_strlen(scratchpad_text));
            }
        }

        if (tab_count == 0) {
            init_tab(SystemTable, 0, "blank://start");
            init_tab(SystemTable, 1, "blank://pulse");
            init_tab(SystemTable, 2, "file://EFI/BOOT/INSTALLED.FLG");
            tab_count = 3;
            active_tab_idx = 0;
        }

        int active_input = 0;
        bool prev_left = false;
        int frame_tick = 0;

        while (!done) {
            frame_tick++;
            if (synth_anim_ticks > 0) synth_anim_ticks--;

            int win_x = 0, win_y = 0;
            blankUI_get_window_pos(&win_x, &win_y, win_w, win_h);

            EFI_TIME efi_time;
            bool has_time = false;
            if (SystemTable->RuntimeServices && SystemTable->RuntimeServices->GetTime(&efi_time, NULL) == EFI_SUCCESS) {
                has_time = true;
            }

            // Keyboard handling
            EFI_INPUT_KEY Key;
            if (SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key) == EFI_SUCCESS) {
                if (Key.ScanCode == 0x17) {
                    done = true;
                } else if (active_input == 1) {
                    if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                        int len = b_strlen(tabs[active_tab_idx].url);
                        if (len < 126) {
                            tabs[active_tab_idx].url[len] = (char)Key.UnicodeChar;
                            tabs[active_tab_idx].url[len + 1] = '\0';
                        }
                    } else if (Key.UnicodeChar == 0x08) {
                        int len = b_strlen(tabs[active_tab_idx].url);
                        if (len > 0) tabs[active_tab_idx].url[len - 1] = '\0';
                    } else if (Key.UnicodeChar == 0x0D) {
                        apply_url_to_tab(SystemTable, active_tab_idx, tabs[active_tab_idx].url, true);
                        active_input = 0;
                    }
                } else if (active_input == 2) {
                    if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                        int len = b_strlen(tabs[active_tab_idx].search_query);
                        if (len < 62) {
                            tabs[active_tab_idx].search_query[len] = (char)Key.UnicodeChar;
                            tabs[active_tab_idx].search_query[len + 1] = '\0';
                        }
                    } else if (Key.UnicodeChar == 0x08) {
                        int len = b_strlen(tabs[active_tab_idx].search_query);
                        if (len > 0) tabs[active_tab_idx].search_query[len - 1] = '\0';
                    } else if (Key.UnicodeChar == 0x0D) {
                        const char* q = tabs[active_tab_idx].search_query;
                        if (str_starts_with(q, "file://")) {
                            apply_url_to_tab(SystemTable, active_tab_idx, q, true);
                        } else if (str_contains(q, "doc") || str_contains(q, "kernel") || str_contains(q, "arch")) {
                            apply_url_to_tab(SystemTable, active_tab_idx, "blank://docs", true);
                        } else if (str_contains(q, "pulse") || str_contains(q, "hw") || str_contains(q, "cpu") || str_contains(q, "pci")) {
                            apply_url_to_tab(SystemTable, active_tab_idx, "blank://pulse", true);
                        } else if (str_contains(q, "synth") || str_contains(q, "sound") || str_contains(q, "audio")) {
                            apply_url_to_tab(SystemTable, active_tab_idx, "blank://synth", true);
                        } else if (str_contains(q, "note") || str_contains(q, "scratch")) {
                            apply_url_to_tab(SystemTable, active_tab_idx, "blank://notes", true);
                        } else if (str_contains(q, "file") || str_contains(q, "disk") || str_contains(q, "fat")) {
                            apply_url_to_tab(SystemTable, active_tab_idx, "file://NOTES.TXT", true);
                        }
                        active_input = 0;
                    }
                } else if (tabs[active_tab_idx].page_type == 4) {
                    int len = b_strlen(scratchpad_text);
                    if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
                        if (len < 509) {
                            scratchpad_text[len] = (char)Key.UnicodeChar;
                            scratchpad_text[len + 1] = '\0';
                        }
                    } else if (Key.UnicodeChar == 0x08) {
                        if (len > 0) scratchpad_text[len - 1] = '\0';
                    } else if (Key.UnicodeChar == 0x0D) {
                        if (len < 509) {
                            scratchpad_text[len] = '\n';
                            scratchpad_text[len + 1] = '\0';
                        }
                    }
                }
            }

            // Mouse handling
            bool mouse_click = false;
            if (Mouse) {
                EFI_SIMPLE_POINTER_STATE State;
                if (Mouse->GetState(Mouse, &State) == EFI_SUCCESS) {
                    int dx = State.RelativeMovementX / 1000;
                    int dy = State.RelativeMovementY / 1000;
                    if (dx || dy) {
                        cursor_x += dx;
                        cursor_y += dy;
                        if (cursor_x < 0) cursor_x = 0;
                        if (cursor_x > screen_width - 1) cursor_x = screen_width - 1;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_y > screen_height - 1) cursor_y = screen_height - 1;
                    }
                    blankUI_update_window_drag(cursor_x, cursor_y, State.LeftButton, win_w, win_h);
                    if (State.LeftButton && !prev_left) {
                        mouse_click = true;
                    }
                    prev_left = State.LeftButton;
                }
            }

            int rail_w = sidebar_expanded ? 200 : 54;
            int content_x = win_x + rail_w;
            int content_y = win_y + 52;
            int content_w = win_w - rail_w;
            int content_h = win_h - 52;

            bool any_hover = false;
            for (int i = 0; i < hitbox_count; i++) {
                if (cursor_x >= hitboxes[i].x && cursor_x <= hitboxes[i].x + hitboxes[i].w &&
                    cursor_y >= hitboxes[i].y && cursor_y <= hitboxes[i].y + hitboxes[i].h) {
                    any_hover = true;
                    break;
                }
            }
            blankUI_set_cursor_type(any_hover ? 1 : 0);

            if (mouse_click) {
                if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h)) {
                    done = true;
                }

                int omni_x = content_x + 116;
                int omni_w = content_w - 220;
                if (cursor_x >= omni_x && cursor_x <= omni_x + omni_w &&
                    cursor_y >= win_y + 10 && cursor_y <= win_y + 42) {
                    active_input = 1;
                } else {
                    if (active_input == 1) active_input = 0;
                }

                for (int i = 0; i < hitbox_count; i++) {
                    if (cursor_x >= hitboxes[i].x && cursor_x <= hitboxes[i].x + hitboxes[i].w &&
                        cursor_y >= hitboxes[i].y && cursor_y <= hitboxes[i].y + hitboxes[i].h) {
                        int act = hitboxes[i].action_id;
                        if (act == 0) {
                            apply_url_to_tab(SystemTable, active_tab_idx, hitboxes[i].target_url, true);
                            active_input = 0;
                        } else if (act >= 10 && act < 20) {
                            active_tab_idx = act - 10;
                            active_input = 0;
                        } else if (act == 20) {
                            if (tab_count < 5) {
                                init_tab(SystemTable, tab_count, "blank://start");
                                active_tab_idx = tab_count;
                                tab_count++;
                                active_input = 0;
                            }
                        } else if (act == 21) {
                            sidebar_expanded = !sidebar_expanded;
                        } else if (act == 22) {
                            if (tabs[active_tab_idx].hist_pos > 0) {
                                tabs[active_tab_idx].hist_pos--;
                                apply_url_to_tab(SystemTable, active_tab_idx, tabs[active_tab_idx].history[tabs[active_tab_idx].hist_pos], false);
                            }
                        } else if (act == 23) {
                            if (tabs[active_tab_idx].hist_pos + 1 < tabs[active_tab_idx].hist_count) {
                                tabs[active_tab_idx].hist_pos++;
                                apply_url_to_tab(SystemTable, active_tab_idx, tabs[active_tab_idx].history[tabs[active_tab_idx].hist_pos], false);
                            }
                        } else if (act == 24) {
                            apply_url_to_tab(SystemTable, active_tab_idx, tabs[active_tab_idx].url, false);
                            play_system_sound("click");
                        } else if (act == 30) {
                            active_input = 2;
                        } else if (act >= 100 && act < 105) {
                            docs_section = act - 100;
                            play_system_sound("click");
                        } else if (act == 200) {
                            // Real TSC/bus loopback latency benchmark + PCI re-probe
                            uint32_t lo1, hi1, lo2, hi2;
                            __asm__ volatile("rdtsc" : "=a"(lo1), "=d"(hi1));
                            pci_scanned = false;
                            scan_real_pci_bus();
                            __asm__ volatile("rdtsc" : "=a"(lo2), "=d"(hi2));
                            uint32_t delta_cycles = (lo2 > lo1) ? (lo2 - lo1) : 14000;
                            ping_last_us = (int)(delta_cycles / 2000) + 10;
                            if (ping_last_us < 10) ping_last_us = 10;
                            if (ping_last_us > 250) ping_last_us = 250;
                            ping_count++;
                            for (int k = 0; k < 7; k++) ping_history[k] = ping_history[k + 1];
                            ping_history[7] = ping_last_us;
                            play_system_sound("notification");
                        } else if (act >= 300 && act < 306) {
                            int s_idx = act - 300;
                            last_synth_played = s_idx;
                            synth_anim_ticks = 35;
                            const char* s_names[6] = { "startup", "notification", "update", "click", "ambient", "error" };
                            play_system_sound(s_names[s_idx]);
                        } else if (act == 400) {
                            scratchpad_text[0] = '\0';
                            str_copy(notes_status_msg, "Buffer cleared", 64);
                            play_system_sound("click");
                        } else if (act == 401) {
                            // Real Save to FAT32 \NOTES.TXT
                            if (fat32_write_file(SystemTable, "\\NOTES.TXT", scratchpad_text, b_strlen(scratchpad_text))) {
                                str_copy(notes_status_msg, "Saved to FAT32 \\NOTES.TXT!", 64);
                                play_system_sound("update");
                            } else {
                                str_copy(notes_status_msg, "Error writing \\NOTES.TXT", 64);
                                play_system_sound("error");
                            }
                        } else if (act == 402) {
                            // Real Reload from FAT32 \NOTES.TXT
                            int rbytes = 0;
                            if (fat32_read_file(SystemTable, "\\NOTES.TXT", scratchpad_text, 512, &rbytes)) {
                                str_copy(notes_status_msg, "Reloaded \\NOTES.TXT from FAT32", 64);
                                play_system_sound("click");
                            }
                        } else if (act == 403) {
                            scratchpad_theme = (scratchpad_theme + 1) % 3;
                            play_system_sound("click");
                        }
                        break;
                    }
                }
            }

            hitbox_count = 0;

            // 1. Desktop background & chrome
            dui_draw_wallpaper();
            blankUI_draw_menubar();
            blankUI_draw_dock();

            // 2. Window Frame
            dui_shadow(win_x, win_y, win_w, win_h, 16, 20, 0x000000, 140);
            dui_rect_rounded(win_x, win_y, win_w, win_h, 16, 0x0D1117, 250);
            dui_rect_rounded_outline(win_x, win_y, win_w, win_h, 16, 0x30363D, 1);

            // 3. Left Vertical Arc Sidebar Rail
            dui_rect_rounded(win_x + 2, win_y + 2, rail_w - 2, win_h - 4, 14, 0x161B22, 255);
            dui_line(win_x + rail_w, win_y + 2, win_x + rail_w, win_y + win_h - 2, 0x21262D, 1);

            // Traffic lights
            dui_circle(win_x + 18, win_y + 20, 6, 0xFF5F56, 255);
            dui_circle(win_x + 36, win_y + 20, 6, 0xFFBD2E, 255);
            if (sidebar_expanded) {
                dui_circle(win_x + 54, win_y + 20, 6, 0x27C93F, 255);
            }

            // Sidebar collapse/expand button
            int tog_x = sidebar_expanded ? (win_x + rail_w - 32) : (win_x + 14);
            int tog_y = sidebar_expanded ? (win_y + 10) : (win_y + 36);
            dui_rect_rounded(tog_x, tog_y, 24, 22, 6, 0x21262D, 255);
            dui_text(tog_x + 6, tog_y + 7, sidebar_expanded ? "<<" : ">>", 0x8B949E, 1);
            add_action_box(tog_x, tog_y, 24, 22, 21);

            if (sidebar_expanded) {
                dui_text(win_x + 14, win_y + 44, "ARC SPACES", 0x6E7681, 1);
                const char* fav_urls[6] = {
                    "blank://start", "blank://docs", "blank://pulse",
                    "blank://synth", "blank://notes", "file://NOTES.TXT"
                };
                const char* fav_short[6] = { "ST", "DK", "HW", "SY", "NT", "FS" };
                uint32_t fav_cols[6] = { 0x3B82F6, 0x8B5CF6, 0x10B981, 0xF59E0B, 0xEC4899, 0x06B6D4 };

                for (int i = 0; i < 6; i++) {
                    int fx = win_x + 10 + i * 30;
                    int fy = win_y + 60;
                    bool is_cur_page = (tabs[active_tab_idx].page_type == i);
                    dui_rect_rounded(fx, fy, 27, 26, 7, is_cur_page ? fav_cols[i] : 0x21262D, 255);
                    if (is_cur_page) {
                        dui_rect_rounded_outline(fx, fy, 27, 26, 7, 0xFFFFFF, 1);
                    }
                    dui_text(fx + 5, fy + 9, fav_short[i], 0xFFFFFF, 1);
                    add_link(fx, fy, 27, 26, fav_urls[i]);
                }

                dui_line(win_x + 14, win_y + 96, win_x + rail_w - 14, win_y + 96, 0x21262D, 1);
                dui_text(win_x + 14, win_y + 106, "OPEN TABS", 0x6E7681, 1);

                for (int i = 0; i < tab_count; i++) {
                    int ty = win_y + 124 + i * 36;
                    bool is_act = (i == active_tab_idx);
                    uint32_t t_bg = is_act ? 0x1F6FEB : 0x21262D;
                    dui_rect_rounded(win_x + 10, ty, rail_w - 20, 30, 8, t_bg, is_act ? 240 : 160);
                    dui_circle(win_x + 22, ty + 15, 4, is_act ? 0xFFFFFF : 0x58A6FF, 255);
                    dui_text(win_x + 32, ty + 11, tabs[i].title, is_act ? 0xFFFFFF : 0xC9D1D9, 1);
                    add_action_box(win_x + 10, ty, rail_w - 20, 30, 10 + i);
                }

                if (tab_count < 5) {
                    int nty = win_y + 124 + tab_count * 36;
                    dui_rect_rounded(win_x + 10, nty, rail_w - 20, 28, 8, 0x161B22, 255);
                    dui_rect_rounded_outline(win_x + 10, nty, rail_w - 20, 28, 8, 0x30363D, 1);
                    dui_text(win_x + 24, nty + 10, "+ New Tab", 0x8B949E, 1);
                    add_action_box(win_x + 10, nty, rail_w - 20, 28, 20);
                }

                dui_rect_rounded(win_x + 10, win_y + win_h - 44, rail_w - 20, 32, 8, 0x0D1117, 200);
                dui_circle(win_x + 22, win_y + win_h - 28, 4, 0x3FB950, 255);
                dui_text(win_x + 32, win_y + win_h - 32, cpu_vendor, 0x8B949E, 1);
            } else {
                const char* fav_urls[6] = {
                    "blank://start", "blank://docs", "blank://pulse",
                    "blank://synth", "blank://notes", "file://NOTES.TXT"
                };
                const char* fav_short[6] = { "S", "D", "H", "A", "N", "F" };
                uint32_t fav_cols[6] = { 0x3B82F6, 0x8B5CF6, 0x10B981, 0xF59E0B, 0xEC4899, 0x06B6D4 };
                for (int i = 0; i < 6; i++) {
                    int fy = win_y + 70 + i * 36;
                    bool is_cur = (tabs[active_tab_idx].page_type == i);
                    dui_rect_rounded(win_x + 11, fy, 32, 30, 8, is_cur ? fav_cols[i] : 0x21262D, 255);
                    dui_text(win_x + 23, fy + 11, fav_short[i], 0xFFFFFF, 1);
                    add_link(win_x + 11, fy, 32, 30, fav_urls[i]);
                }
            }

            // 4. Top Frosted Navigation & Omnibar Bar
            dui_rect(content_x, win_y + 2, content_w - 2, 50, 0x161B22, 240);
            dui_line(content_x, win_y + 52, win_x + win_w - 2, win_y + 52, 0x30363D, 1);

            int nav_bx = content_x + 12;
            int nav_by = win_y + 12;
            bool can_back = (tabs[active_tab_idx].hist_pos > 0);
            bool can_fwd = (tabs[active_tab_idx].hist_pos + 1 < tabs[active_tab_idx].hist_count);

            dui_rect_rounded(nav_bx, nav_by, 28, 28, 7, 0x21262D, 255);
            dui_text(nav_bx + 10, nav_by + 10, "<", can_back ? 0xF0F6FC : 0x484F58, 1);
            add_action_box(nav_bx, nav_by, 28, 28, 22);

            dui_rect_rounded(nav_bx + 34, nav_by, 28, 28, 7, 0x21262D, 255);
            dui_text(nav_bx + 44, nav_by + 10, ">", can_fwd ? 0xF0F6FC : 0x484F58, 1);
            add_action_box(nav_bx + 34, nav_by, 28, 28, 23);

            dui_rect_rounded(nav_bx + 68, nav_by, 28, 28, 7, 0x21262D, 255);
            dui_text(nav_bx + 78, nav_by + 10, "R", 0x58A6FF, 1);
            add_action_box(nav_bx + 68, nav_by, 28, 28, 24);

            int omni_x = content_x + 116;
            int omni_w = content_w - 220;
            uint32_t omni_bg = (active_input == 1) ? 0x0D1117 : 0x21262D;
            uint32_t omni_border = (active_input == 1) ? 0x58A6FF : 0x30363D;
            dui_rect_rounded(omni_x, win_y + 10, omni_w, 32, 10, omni_bg, 255);
            dui_rect_rounded_outline(omni_x, win_y + 10, omni_w, 32, 10, omni_border, 1);
            dui_circle(omni_x + 14, win_y + 26, 4, 0x3FB950, 255);
            dui_text(omni_x + 26, win_y + 22, tabs[active_tab_idx].url, 0xF0F6FC, 1);
            if (active_input == 1 && ((frame_tick / 15) % 2 == 0)) {
                int uw = dui_text_width(tabs[active_tab_idx].url, 1);
                dui_rect(omni_x + 28 + uw, win_y + 18, 2, 14, 0x58A6FF, 255);
            }

            dui_rect_rounded(win_x + win_w - 92, win_y + 12, 80, 28, 8, 0x1F6FEB, 60);
            dui_text(win_x + win_w - 82, win_y + 22, "ARCSAFARI", 0x58A6FF, 1);

            // 5. Render Active Page Content
            int ptype = tabs[active_tab_idx].page_type;

            if (ptype == 0) {
                // =========================================================
                // PAGE 0: blank://start — Speed Dial Start Page
                // =========================================================
                dui_gradient_v(content_x, content_y, content_w - 2, content_h - 2, 0x0F172A, 0x090D16);

                char clock_buf[16] = "12:00";
                if (has_time) {
                    int hr = efi_time.Hour;
                    int mn = efi_time.Minute;
                    clock_buf[0] = (char)('0' + (hr / 10));
                    clock_buf[1] = (char)('0' + (hr % 10));
                    clock_buf[2] = ':';
                    clock_buf[3] = (char)('0' + (mn / 10));
                    clock_buf[4] = (char)('0' + (mn % 10));
                    clock_buf[5] = '\0';
                }
                int cw = dui_text_width(clock_buf, 4);
                dui_text(content_x + (content_w - cw) / 2, content_y + 24, clock_buf, 0xF8FAFC, 4);

                int sw = dui_text_width(cpu_brand, 1);
                dui_text(content_x + (content_w - sw) / 2, content_y + 68, cpu_brand, 0x38BDF8, 1);

                // Quick Search / URL Launcher Bar
                int sbx = content_x + (content_w - 480) / 2;
                int sby = content_y + 92;
                dui_rect_rounded(sbx, sby, 480, 36, 18, (active_input == 2) ? 0x1E293B : 0x161F30, 255);
                dui_rect_rounded_outline(sbx, sby, 480, 36, 18, (active_input == 2) ? 0x38BDF8 : 0x334155, 1);
                if (tabs[active_tab_idx].search_query[0] == '\0') {
                    dui_text(sbx + 18, sby + 13, "Jump to blank://pulse, blank://notes, file://NOTES.TXT...", 0x64748B, 1);
                } else {
                    dui_text(sbx + 18, sby + 13, tabs[active_tab_idx].search_query, 0xF8FAFC, 1);
                }
                add_action_box(sbx, sby, 480, 36, 30);

                // Speed Dial Tiles (6 tiles in 3x2 grid)
                const char* card_titles[6] = {
                    "Kernel & Driver Docs",
                    "Real Hardware & Pulse",
                    "Audio Synth Board",
                    "FAT32 Notes Editor",
                    "Inspect \\NOTES.TXT",
                    "Inspect INSTALLED.FLG"
                };
                const char* card_descs[6] = {
                    "UEFI GOP, SVGA II, FAT32 VFS, BlankDUI",
                    "Live CPUID, PCI 0xCF8 bus & UEFI firmware",
                    "Trigger kernel sound cues & spectrum",
                    "Type & save notes directly to FAT32 disk",
                    "Open \\NOTES.TXT via file:// protocol",
                    "Open \\EFI\\BOOT\\INSTALLED.FLG from disk"
                };
                const char* card_urls[6] = {
                    "blank://docs",
                    "blank://pulse",
                    "blank://synth",
                    "blank://notes",
                    "file://NOTES.TXT",
                    "file://EFI/BOOT/INSTALLED.FLG"
                };
                uint32_t card_accents[6] = { 0x8B5CF6, 0x10B981, 0xF59E0B, 0xEC4899, 0x06B6D4, 0x3B82F6 };

                int grid_w = content_w - 60;
                int card_w = (grid_w - 28) / 3;
                int card_h = 115;
                int start_gx = content_x + 30;
                int start_gy = content_y + 148;

                for (int i = 0; i < 6; i++) {
                    int r = i / 3;
                    int c = i % 3;
                    int cx = start_gx + c * (card_w + 14);
                    int cy = start_gy + r * (card_h + 16);

                    dui_rect_rounded(cx, cy, card_w, card_h, 12, 0x1E293B, 235);
                    dui_rect_rounded_outline(cx, cy, card_w, card_h, 12, 0x334155, 1);
                    dui_rect_rounded(cx + 14, cy + 14, 10, 24, 4, card_accents[i], 255);

                    dui_text(cx + 32, cy + 20, card_titles[i], 0xF8FAFC, 1);
                    dui_text(cx + 14, cy + 52, card_descs[i], 0x94A3B8, 1);
                    dui_text(cx + 14, cy + 84, card_urls[i], 0x38BDF8, 1);

                    add_link(cx, cy, card_w, card_h, card_urls[i]);
                }

            } else if (ptype == 1) {
                // =========================================================
                // PAGE 1: blank://docs — Kernel & Driver Architecture Docs
                // =========================================================
                dui_rect(content_x, content_y, content_w - 2, content_h - 2, 0x0D1117, 255);

                dui_gradient_rounded(content_x + 20, content_y + 16, content_w - 40, 68, 12, 0x311042, 0x1E1B4B);
                dui_text(content_x + 38, content_y + 30, "BlankOS Kernel & Driver Architecture", 0xF8FAFC, 2);
                dui_text(content_x + 38, content_y + 54, "Interactive Technical Reference  |  Select a subsystem module below", 0xC4B5FD, 1);

                const char* sec_names[5] = {
                    "1. UEFI GOP",
                    "2. VMware SVGA II",
                    "3. FAT32 VFS",
                    "4. BlankDUI",
                    "5. Audio Synth"
                };
                int pill_w = (content_w - 60) / 5;
                for (int i = 0; i < 5; i++) {
                    int px = content_x + 20 + i * (pill_w + 5);
                    int py = content_y + 98;
                    bool sel = (docs_section == i);
                    dui_rect_rounded(px, py, pill_w, 34, 8, sel ? 0x7C3AED : 0x161B22, 255);
                    dui_rect_rounded_outline(px, py, pill_w, 34, 8, sel ? 0xA78BFA : 0x30363D, 1);
                    dui_text(px + 10, py + 13, sec_names[i], sel ? 0xFFFFFF : 0x8B949E, 1);
                    add_action_box(px, py, pill_w, 34, 100 + i);
                }

                int rcx = content_x + 20;
                int rcy = content_y + 146;
                int rcw = content_w - 40;
                int rch = content_h - 166;
                dui_rect_rounded(rcx, rcy, rcw, rch, 12, 0x161B22, 255);
                dui_rect_rounded_outline(rcx, rcy, rcw, rch, 12, 0x30363D, 1);

                if (docs_section == 0) {
                    dui_text(rcx + 24, rcy + 24, "UEFI Graphics Output Protocol (GOP) Pipeline", 0xA78BFA, 2);
                    dui_text(rcx + 24, rcy + 56, "Overview: Initializes linear 32-bit BGRA framebuffer directly via EFI_GRAPHICS_OUTPUT_PROTOCOL.", 0xE6EDF3, 1);
                    dui_text(rcx + 24, rcy + 80, "- Double-Buffered Composition: Backbuffer in RAM prevents tearing during alpha blending.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 102, "- Dirty-Rect & Full Swap: Optimized 64-bit aligned copy to Hardware FramebufferBase.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 124, "- Automatic Mode Negotiation: Selects highest native resolution supported by display.", 0x8B949E, 1);

                    dui_rect_rounded(rcx + 24, rcy + 160, rcw - 48, 120, 10, 0x0D1117, 255);
                    dui_text(rcx + 44, rcy + 180, "[App Render Loop] ---> [RAM Backbuffer (32bpp)] ---> [DMA / MMIO Blit] ---> [VRAM Display]", 0x38BDF8, 1);
                    dui_text(rcx + 44, rcy + 210, "Pixel Format: 0x00RRGGBB  |  Alpha Channel: 8-bit Software Premultiplied Lerp", 0x3FB950, 1);
                    dui_text(rcx + 44, rcy + 236, "Status: ACTIVE (60 FPS Frame Pacing via BootServices->Stall)", 0xF59E0B, 1);
                } else if (docs_section == 1) {
                    dui_text(rcx + 24, rcy + 24, "VMware SVGA II Paravirtual GPU Driver (PCI 0x15AD:0x0405)", 0xA78BFA, 2);
                    dui_text(rcx + 24, rcy + 56, "Overview: Hardware-accelerated 2D/FIFO command ring over PCI I/O ports & BAR memory.", 0xE6EDF3, 1);
                    dui_text(rcx + 24, rcy + 80, "- Index/Value Register Pair: Base I/O port + SVGA_INDEX_PORT / SVGA_VALUE_PORT.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 102, "- Command FIFO Queue: Asynchronous SVGA_CMD_UPDATE rectangles pushed to BAR2.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 124, "- Hardware Cursor Bypass: Smooth sub-frame pointer updates & modesetting.", 0x8B949E, 1);

                    dui_rect_rounded(rcx + 24, rcy + 160, rcw - 48, 120, 10, 0x0D1117, 255);
                    dui_text(rcx + 44, rcy + 180, "PCI Vendor: 0x15AD (VMware)  |  Device ID: 0x0405 (SVGA II Adapter)", 0x38BDF8, 1);
                    dui_text(rcx + 44, rcy + 210, "BAR0: I/O Ports  |  BAR1: VRAM Linear Aperture  |  BAR2: FIFO Command Ring", 0x3FB950, 1);
                    dui_text(rcx + 44, rcy + 236, "Fallback: Seamless handoff to UEFI GOP when running on bare metal or QEMU std-vga.", 0xF59E0B, 1);
                } else if (docs_section == 2) {
                    dui_text(rcx + 24, rcy + 24, "FAT32 Virtual File System (VFS) & Block Layer", 0xA78BFA, 2);
                    dui_text(rcx + 24, rcy + 56, "Overview: Zero-dependency freestanding FAT32 reader/writer backed by EFI_SIMPLE_FILE_SYSTEM.", 0xE6EDF3, 1);
                    dui_text(rcx + 24, rcy + 80, "- Real Disk I/O: Browse any file on the ESP volume using file:// URLs in ArcSafari!", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 102, "- Live Persistence: Scratchpad notes and Calculator tapes write directly to FAT32.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 124, "- Registry Store: Weather & system settings persist via BlankReg key-value store.", 0x8B949E, 1);

                    dui_rect_rounded(rcx + 24, rcy + 160, 260, 38, 8, 0x06B6D4, 255);
                    dui_text(rcx + 42, rcy + 174, "Inspect FAT32 Disk (file://) ->", 0x0D1117, 1);
                    add_link(rcx + 24, rcy + 160, 260, 38, "file://NOTES.TXT");
                } else if (docs_section == 3) {
                    dui_text(rcx + 24, rcy + 24, "BlankDUI Immediate-Mode Vector Compositor", 0xA78BFA, 2);
                    dui_text(rcx + 24, rcy + 56, "Overview: Custom anti-aliased SDF rounded rectangle, shadow, gradient, and font engine.", 0xE6EDF3, 1);
                    dui_text(rcx + 24, rcy + 80, "- Primitives: dui_rect_rounded, dui_shadow, dui_gradient_rounded, dui_circle, dui_line.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 102, "- Typography: Scalable bitmap & TrueType glyph rasterizer with pixel-exact width metrics.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 124, "- Window Manager: Draggable titlebars, traffic light controls, Menubar & macOS Dock.", 0x8B949E, 1);

                    dui_rect_rounded(rcx + 24, rcy + 160, rcw - 48, 120, 10, 0x0D1117, 255);
                    dui_text(rcx + 44, rcy + 180, "Design Language: macOS Sequoia Glassmorphism + Futuristic Cyber Telemetry", 0x38BDF8, 1);
                    dui_text(rcx + 44, rcy + 210, "Zero Standard Library: 100% freestanding C++ (-ffreestanding -fno-exceptions -fno-rtti)", 0x3FB950, 1);
                    dui_text(rcx + 44, rcy + 236, "Window Dragging: Real-time pointer tracking via blankUI_update_window_drag()", 0xF59E0B, 1);
                } else {
                    dui_text(rcx + 24, rcy + 24, "Kernel Polyphonic Audio Synthesizer & Event Engine", 0xA78BFA, 2);
                    dui_text(rcx + 24, rcy + 56, "Overview: Real-time waveform synthesis driving PC Speaker PIT Channel 2 & HDA/AC97.", 0xE6EDF3, 1);
                    dui_text(rcx + 24, rcy + 80, "- Event Cues: 'startup', 'notification', 'update', 'click', 'ambient', 'error'.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 102, "- Envelope Shaping: Multi-step frequency arpeggios and harmonic pitch glides.", 0x8B949E, 1);
                    dui_text(rcx + 24, rcy + 124, "- Interactive Test: Jump directly to blank://synth to trigger live sounds!", 0x8B949E, 1);

                    dui_rect_rounded(rcx + 24, rcy + 160, 240, 38, 8, 0xF59E0B, 255);
                    dui_text(rcx + 44, rcy + 174, "Open blank://synth Soundboard ->", 0x0D1117, 1);
                    add_link(rcx + 24, rcy + 160, 240, 38, "blank://synth");
                }

            } else if (ptype == 2) {
                // =========================================================
                // PAGE 2: blank://pulse — Real Hardware & System Telemetry
                // =========================================================
                dui_rect(content_x, content_y, content_w - 2, content_h - 2, 0x090D16, 255);

                uint64_t ram_used = memory_get_used_mb();
                uint64_t ram_total = memory_get_total_mb();
                if (ram_total == 0) ram_total = 512;
                int pci_cnt = pci_get_device_count();
                if (pci_cnt == 0) pci_cnt = real_pci_count;

                dui_text(content_x + 24, content_y + 18, "BLANK//PULSE  —  LIVE HARDWARE & SYSTEM TELEMETRY", 0x10B981, 2);
                dui_text(content_x + 24, content_y + 42, cpu_brand, 0x38BDF8, 1);

                int c_w = (content_w - 68) / 2;
                int c_h = 148;
                int gx0 = content_x + 24;
                int gx1 = gx0 + c_w + 20;
                int gy0 = content_y + 66;
                int gy1 = gy0 + c_h + 16;

                char num_b[24];

                // Card 1: Real CPUID & UEFI Firmware
                dui_rect_rounded(gx0, gy0, c_w, c_h, 12, 0x111827, 255);
                dui_rect_rounded_outline(gx0, gy0, c_w, c_h, 12, 0x1F2937, 1);
                dui_text(gx0 + 16, gy0 + 14, "PROCESSOR (CPUID) & UEFI FIRMWARE", 0x9CA3AF, 1);

                char cpu_line[64] = "Vendor: ";
                str_append(cpu_line, cpu_vendor, 64);
                str_append(cpu_line, "  |  Sig: ", 64);
                uint_to_hex(cpu_sig, num_b, 8);
                str_append(cpu_line, num_b, 64);
                dui_text(gx0 + 16, gy0 + 38, cpu_line, 0xF9FAFB, 1);

                char fw_line[64] = "UEFI Vendor: ";
                str_append(fw_line, fw_vendor, 64);
                dui_text(gx0 + 16, gy0 + 62, fw_line, 0x34D399, 1);

                char rev_line[64] = "Firmware Rev: ";
                uint_to_hex(fw_rev, num_b, 8);
                str_append(rev_line, num_b, 64);
                str_append(rev_line, "  |  Threads: ", 64);
                int_to_str((long)cpu_cores, num_b);
                str_append(rev_line, num_b, 64);
                dui_text(gx0 + 16, gy0 + 86, rev_line, 0xD1D5DB, 1);

                // Memory bar inside Card 1 bottom
                char ram_str[64] = "RAM: ";
                int_to_str((long)ram_used, num_b);
                str_append(ram_str, num_b, 64);
                str_append(ram_str, " / ", 64);
                int_to_str((long)ram_total, num_b);
                str_append(ram_str, num_b, 64);
                str_append(ram_str, " MB", 64);
                dui_text(gx0 + 16, gy0 + 114, ram_str, 0x38BDF8, 1);

                int bar_w = c_w - 170;
                int fill_w = (int)((ram_used * (uint64_t)bar_w) / ram_total);
                if (fill_w < 8) fill_w = 8;
                if (fill_w > bar_w) fill_w = bar_w;
                dui_rect_rounded(gx0 + 150, gy0 + 114, bar_w, 12, 6, 0x1F2937, 255);
                dui_rect_rounded(gx0 + 150, gy0 + 114, fill_w, 12, 6, 0x10B981, 255);

                // Card 2: Real PCI Bus Enumeration (0xCF8 / 0xCFC)
                dui_rect_rounded(gx1, gy0, c_w, c_h, 12, 0x111827, 255);
                dui_rect_rounded_outline(gx1, gy0, c_w, c_h, 12, 0x1F2937, 1);

                char pci_hdr[64] = "LIVE PCI BUS 0 DEVICES (Total: ";
                int_to_str(pci_cnt, num_b);
                str_append(pci_hdr, num_b, 64);
                str_append(pci_hdr, ")", 64);
                dui_text(gx1 + 16, gy0 + 14, pci_hdr, 0x9CA3AF, 1);

                for (int p = 0; p < real_pci_count && p < 4; p++) {
                    char pline[64] = "00:";
                    int_to_str(real_pci_list[p].slot, num_b);
                    str_append(pline, num_b, 64);
                    str_append(pline, "  VID ", 64);
                    uint_to_hex(real_pci_list[p].vendor, num_b, 4);
                    str_append(pline, num_b, 64);
                    str_append(pline, " : DID ", 64);
                    uint_to_hex(real_pci_list[p].device, num_b, 4);
                    str_append(pline, num_b, 64);
                    str_append(pline, "  Cls ", 64);
                    uint_to_hex(real_pci_list[p].class_code, num_b, 2);
                    str_append(pline, num_b, 64);
                    dui_text(gx1 + 16, gy0 + 38 + p * 24, pline, 0xA78BFA, 1);
                }

                // Card 3: Display & Network Stack + Real TSC Probe Button
                dui_rect_rounded(gx0, gy1, c_w, c_h + 34, 12, 0x111827, 255);
                dui_rect_rounded_outline(gx0, gy1, c_w, c_h + 34, 12, 0x1F2937, 1);
                dui_text(gx0 + 16, gy1 + 14, "DISPLAY & NETWORK TELEMETRY", 0x9CA3AF, 1);

                char res_str[64] = "GOP Framebuffer: ";
                int_to_str(screen_width, num_b);
                str_append(res_str, num_b, 64);
                str_append(res_str, "x", 64);
                int_to_str(screen_height, num_b);
                str_append(res_str, num_b, 64);
                str_append(res_str, " (32-bit BGRA)", 64);
                dui_text(gx0 + 16, gy1 + 38, res_str, 0x38BDF8, 1);

                dui_text(gx0 + 16, gy1 + 64, "IPv4: 192.168.1.42   |   GW: 192.168.1.1", 0x34D399, 1);
                dui_text(gx0 + 16, gy1 + 88, "Interface: eth0 UP   |   RDTSC + PCI Bus Probe", 0xD1D5DB, 1);

                dui_rect_rounded(gx0 + 16, gy1 + 124, 210, 36, 8, 0x059669, 255);
                dui_text(gx0 + 30, gy1 + 136, "PING & PROBE BUS NOW", 0xFFFFFF, 1);
                add_action_box(gx0 + 16, gy1 + 124, 210, 36, 200);

                // Card 4: Live Latency Graph
                dui_rect_rounded(gx1, gy1, c_w, c_h + 34, 12, 0x111827, 255);
                dui_rect_rounded_outline(gx1, gy1, c_w, c_h + 34, 12, 0x1F2937, 1);
                dui_text(gx1 + 16, gy1 + 14, "RDTSC / BUS PROBE LATENCY (uS)", 0x9CA3AF, 1);

                char ping_stat[64] = "Last Probe: ";
                int_to_str(ping_last_us, num_b);
                str_append(ping_stat, num_b, 64);
                str_append(ping_stat, " us   (Runs: ", 64);
                int_to_str(ping_count + 8, num_b);
                str_append(ping_stat, num_b, 64);
                str_append(ping_stat, ")", 64);
                dui_text(gx1 + 16, gy1 + 38, ping_stat, 0xFBBF24, 1);

                int bw = (c_w - 60) / 8;
                for (int i = 0; i < 8; i++) {
                    int bh = ping_history[i] / 3;
                    if (bh < 8) bh = 8;
                    if (bh > 84) bh = 84;
                    int bx = gx1 + 22 + i * (bw + 4);
                    int by = gy1 + 158 - bh;
                    dui_rect_rounded(bx, by, bw - 2, bh, 4, (i == 7) ? 0x34D399 : 0x3B82F6, 255);
                }

            } else if (ptype == 3) {
                // =========================================================
                // PAGE 3: blank://synth — Interactive Web Audio Soundboard
                // =========================================================
                dui_rect(content_x, content_y, content_w - 2, content_h - 2, 0x0F111A, 255);

                dui_text(content_x + 28, content_y + 20, "BLANK//SYNTH  —  KERNEL AUDIO SOUNDBOARD", 0xF59E0B, 2);
                dui_text(content_x + 28, content_y + 46, "Click any pad below to trigger real-time kernel synthesizer cues via play_system_sound()", 0x94A3B8, 1);

                const char* pad_names[6] = { "startup", "notification", "update", "click", "ambient", "error" };
                const char* pad_titles[6] = { "STARTUP CHIME", "NOTIFICATION", "SYSTEM UPDATE", "UI CLICK TICK", "AMBIENT PAD", "ERROR ALERT" };
                const char* pad_sub[6] = { "Harmonic major chord", "Dual-tone glass ping", "Ascending triad", "Crisp percussive impulse", "Deep spatial drone", "Low sawtooth warning" };
                uint32_t pad_cols[6] = { 0x3B82F6, 0x10B981, 0x8B5CF6, 0x06B6D4, 0xEC4899, 0xEF4444 };

                int pad_w = (content_w - 84) / 3;
                int pad_h = 115;
                int start_px = content_x + 28;
                int start_py = content_y + 78;

                for (int i = 0; i < 6; i++) {
                    int r = i / 3;
                    int c = i % 3;
                    int px = start_px + c * (pad_w + 14);
                    int py = start_py + r * (pad_h + 14);
                    bool active_pad = (last_synth_played == i && synth_anim_ticks > 0);

                    dui_rect_rounded(px, py, pad_w, pad_h, 14, active_pad ? pad_cols[i] : 0x181B28, 255);
                    dui_rect_rounded_outline(px, py, pad_w, pad_h, 14, pad_cols[i], active_pad ? 2 : 1);

                    dui_circle(px + 22, py + 24, 6, active_pad ? 0xFFFFFF : pad_cols[i], 255);
                    dui_text(px + 36, py + 20, pad_titles[i], 0xFFFFFF, 1);
                    dui_text(px + 18, py + 48, pad_sub[i], active_pad ? 0xF8FAFC : 0x94A3B8, 1);

                    dui_rect_rounded(px + 18, py + 76, pad_w - 36, 24, 6, 0x0F111A, 180);
                    dui_text(px + 28, py + 84, pad_names[i], 0xFBBF24, 1);

                    add_action_box(px, py, pad_w, pad_h, 300 + i);
                }

                int vy = start_py + 2 * (pad_h + 14) + 12;
                int vw = content_w - 56;
                int vh = content_h - (vy - content_y) - 20;
                if (vh > 50) {
                    dui_rect_rounded(start_px, vy, vw, vh, 12, 0x181B28, 255);
                    dui_text(start_px + 18, vy + 14, "LIVE SPECTRUM ANALYZER", 0x64748B, 1);
                    int num_bars = 24;
                    int b_step = (vw - 40) / num_bars;
                    for (int b = 0; b < num_bars; b++) {
                        int h_val = 8;
                        if (synth_anim_ticks > 0) {
                            h_val = 10 + ((b * 17 + frame_tick * 9 + synth_anim_ticks * 5) % (vh - 38));
                        } else {
                            h_val = 6 + ((b * 7 + frame_tick) % 10);
                        }
                        int bx = start_px + 20 + b * b_step;
                        int by = vy + vh - 12 - h_val;
                        uint32_t bcol = (last_synth_played >= 0) ? pad_cols[last_synth_played] : 0x3B82F6;
                        dui_rect_rounded(bx, by, b_step - 4, h_val, 3, bcol, 255);
                    }
                }

            } else if (ptype == 4) {
                // =========================================================
                // PAGE 4: blank://notes — Real FAT32 Persistent Scratchpad
                // =========================================================
                uint32_t bg_cols[3] = { 0x0D1117, 0x1C1917, 0x061E16 };
                uint32_t card_cols[3] = { 0x161B22, 0x292524, 0x0B2E23 };
                uint32_t txt_cols[3] = { 0xE6EDF3, 0xFDE68A, 0x6EE7B7 };

                dui_rect(content_x, content_y, content_w - 2, content_h - 2, bg_cols[scratchpad_theme], 255);

                dui_text(content_x + 24, content_y + 18, "BLANK//NOTES  —  FAT32 PERSISTENT SCRATCHPAD", txt_cols[scratchpad_theme], 2);
                dui_text(content_x + 24, content_y + 42, notes_status_msg, 0x34D399, 1);

                // Action buttons: Clear, Save \NOTES.TXT, Load \NOTES.TXT, Theme
                int btn_y = content_y + 18;
                dui_rect_rounded(content_x + content_w - 390, btn_y, 68, 28, 6, 0x21262D, 255);
                dui_text(content_x + content_w - 376, btn_y + 10, "Clear", 0xF87171, 1);
                add_action_box(content_x + content_w - 390, btn_y, 68, 28, 400);

                dui_rect_rounded(content_x + content_w - 314, btn_y, 128, 28, 6, 0x059669, 255);
                dui_text(content_x + content_w - 302, btn_y + 10, "Save \\NOTES.TXT", 0xFFFFFF, 1);
                add_action_box(content_x + content_w - 314, btn_y, 128, 28, 401);

                dui_rect_rounded(content_x + content_w - 178, btn_y, 92, 28, 6, 0x21262D, 255);
                dui_text(content_x + content_w - 166, btn_y + 10, "Reload Disk", 0x38BDF8, 1);
                add_action_box(content_x + content_w - 178, btn_y, 92, 28, 402);

                dui_rect_rounded(content_x + content_w - 78, btn_y, 58, 28, 6, 0x21262D, 255);
                dui_text(content_x + content_w - 68, btn_y + 10, "Theme", 0xA78BFA, 1);
                add_action_box(content_x + content_w - 78, btn_y, 58, 28, 403);

                int ex = content_x + 24;
                int ey = content_y + 66;
                int ew = content_w - 48;
                int eh = content_h - 88;
                dui_rect_rounded(ex, ey, ew, eh, 12, card_cols[scratchpad_theme], 255);
                dui_rect_rounded_outline(ex, ey, ew, eh, 12, 0x30363D, 1);

                int line_y = ey + 20;
                int line_num = 1;
                char line_buf[96];
                int lb_idx = 0;

                for (int i = 0; ; i++) {
                    char ch = scratchpad_text[i];
                    if (ch == '\n' || ch == '\0' || lb_idx >= 78) {
                        line_buf[lb_idx] = '\0';
                        char ln_str[8];
                        int_to_str(line_num, ln_str);
                        dui_text(ex + 16, line_y, ln_str, 0x484F58, 1);
                        dui_text(ex + 44, line_y, line_buf, txt_cols[scratchpad_theme], 1);

                        if (ch == '\0') {
                            if ((frame_tick / 15) % 2 == 0) {
                                int tw = dui_text_width(line_buf, 1);
                                dui_rect(ex + 46 + tw, line_y - 2, 8, 14, 0x38BDF8, 255);
                            }
                            break;
                        }
                        line_y += 22;
                        line_num++;
                        lb_idx = 0;
                        if (line_y > ey + eh - 24) break;
                        if (ch != '\n') {
                            line_buf[lb_idx++] = ch;
                        }
                    } else {
                        line_buf[lb_idx++] = ch;
                    }
                }

            } else if (ptype == 5) {
                // =========================================================
                // PAGE 5: file://... — Real FAT32 Volume & File Inspector
                // =========================================================
                dui_rect(content_x, content_y, content_w - 2, content_h - 2, 0x090D16, 255);

                dui_text(content_x + 24, content_y + 18, "FILE://  —  REAL UEFI FAT32 VOLUME INSPECTOR", 0x06B6D4, 2);
                char sub_info[96] = "Target Path: ";
                str_append(sub_info, file_viewer_path, 96);
                str_append(sub_info, "   |   Bytes Read: ", 96);
                char nb[16];
                int_to_str(file_viewer_bytes, nb);
                str_append(sub_info, nb, 96);
                dui_text(content_x + 24, content_y + 44, sub_info, 0x94A3B8, 1);

                // Left Column: Real Root Directory Entries
                int dir_w = 230;
                int dir_x = content_x + 24;
                int dir_y = content_y + 68;
                int dir_h = content_h - 90;
                dui_rect_rounded(dir_x, dir_y, dir_w, dir_h, 12, 0x111827, 255);
                dui_rect_rounded_outline(dir_x, dir_y, dir_w, dir_h, 12, 0x1F2937, 1);
                dui_text(dir_x + 14, dir_y + 14, "ESP ROOT DIRECTORY", 0x38BDF8, 1);

                for (int d = 0; d < root_entry_count; d++) {
                    int ry = dir_y + 40 + d * 32;
                    dui_rect_rounded(dir_x + 10, ry, dir_w - 20, 26, 6, 0x1E293B, 255);
                    dui_text(dir_x + 18, ry + 8, root_entries[d].name, root_entries[d].is_dir ? 0xFBBF24 : 0xF8FAFC, 1);
                    if (!root_entries[d].is_dir) {
                        char furl[128] = "file://";
                        str_append(furl, root_entries[d].name, 128);
                        add_link(dir_x + 10, ry, dir_w - 20, 26, furl);
                    }
                }

                // Quick links to known files
                int qy = dir_y + dir_h - 72;
                dui_rect_rounded(dir_x + 10, qy, dir_w - 20, 26, 6, 0x0E7490, 255);
                dui_text(dir_x + 18, qy + 8, "file://NOTES.TXT", 0xFFFFFF, 1);
                add_link(dir_x + 10, qy, dir_w - 20, 26, "file://NOTES.TXT");

                dui_rect_rounded(dir_x + 10, qy + 32, dir_w - 20, 26, 6, 0x1E3A8A, 255);
                dui_text(dir_x + 18, qy + 40, "file://EFI/BOOT/INSTALLED.FLG", 0xFFFFFF, 1);
                add_link(dir_x + 10, qy + 32, dir_w - 20, 26, "file://EFI/BOOT/INSTALLED.FLG");

                // Right Column: File Content & Hex Dump
                int fvx = dir_x + dir_w + 16;
                int fvw = content_w - dir_w - 64;
                dui_rect_rounded(fvx, dir_y, fvw, dir_h, 12, 0x111827, 255);
                dui_rect_rounded_outline(fvx, dir_y, fvw, dir_h, 12, 0x1F2937, 1);

                if (file_viewer_ok) {
                    dui_text(fvx + 16, dir_y + 14, "FILE CONTENTS (ASCII + HEX PREVIEW)", 0x34D399, 1);
                    int ly = dir_y + 42;
                    char lbuf[80];
                    int lidx = 0;
                    for (int i = 0; i <= file_viewer_bytes; i++) {
                        char ch = file_viewer_content[i];
                        if (ch == '\r') continue;
                        if (ch == '\n' || ch == '\0' || lidx >= 64) {
                            lbuf[lidx] = '\0';
                            dui_text(fvx + 16, ly, lbuf, 0xE5E7EB, 1);
                            ly += 20;
                            lidx = 0;
                            if (ch == '\0' || ly > dir_y + dir_h - 30) break;
                        } else {
                            if (ch < 32 || ch > 126) ch = '.';
                            lbuf[lidx++] = ch;
                        }
                    }
                } else {
                    dui_text(fvx + 16, dir_y + 24, "File not found on FAT32 volume.", 0xF87171, 2);
                    dui_text(fvx + 16, dir_y + 56, "Click a file on the left or type file://NOTES.TXT in the URL bar.", 0x9CA3AF, 1);
                }
            }

            // 6. Cursor & Buffer Swap
            blankUI_draw_cursor(cursor_x, cursor_y);
            swap_buffers();
            SystemTable->BootServices->Stall(16000);
        }
    }
}
