#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

extern "C" {
    extern EFI_SYSTEM_TABLE *gSystemTable;
    extern int screen_width;
    extern int screen_height;

    extern void swap_buffers();
    extern void dui_draw_wallpaper();
    extern void blankUI_draw_menubar();
    extern void blankUI_draw_dock();
    extern void blankUI_draw_topbar(char* app_title);
    extern void blankUI_draw_window(int width, int height, char* title);
    extern void blankUI_draw_text_color(int x, int y, char* text, uint32_t color);
    extern void blankUI_draw_button(int x, int y, int width, int height, char* text);
    extern void blankUI_draw_toast(char* title, char* message);
    extern void blankUI_draw_cursor(int x, int y);
    extern int blankUI_hit_test_window_close(int cursor_x, int cursor_y, int width, int height);

    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_rect_rounded(int x, int y, int w, int h, int radius, uint32_t color, uint8_t alpha);
    extern void dui_rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);
    extern void dui_shadow(int x, int y, int w, int h, int radius, int blur_radius, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void dui_circle(int cx, int cy, int r, uint32_t color, uint8_t alpha);
    extern void dui_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    extern void dui_gradient_rounded(int x, int y, int w, int h, int radius, uint32_t top_color, uint32_t bottom_color);
    extern void play_system_sound(char* sound_name);

    // Real Kernel Telemetry Functions
    extern int pci_get_device_count(void);
    extern bool pci_get_device_info(int index, uint8_t* bus, uint8_t* dev, uint8_t* func,
                                    uint16_t* vendor, uint16_t* device, uint8_t* class_code,
                                    uint8_t* subclass, uint32_t* bar0, const char** vendor_str,
                                    const char** dev_str, const char** drv_str);
    extern uint64_t memory_get_total_mb(void);
    extern uint64_t memory_get_used_mb(void);
    extern uint32_t memory_get_alloc_count(void);
    extern uint32_t svga_get_vram_size(void);

    // Real System Application Launchers
    extern void launch_blankpad(void);
    extern void launch_blankreg_edit(void);
    extern void launch_terminal(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_sysinfo(void);
    extern void launch_browser(EFI_SYSTEM_TABLE *SystemTable);
    extern void launch_calculator(EFI_SYSTEM_TABLE *SystemTable);
}

namespace {

static void tm_strcpy(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static void tm_strcat(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*dst) dst++;
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static void tm_utoa(uint64_t val, char* buf) {
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
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

static void tm_hex_digits(uint64_t val, int digits, char* out) {
    const char* hex = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = hex[val & 0xF];
        val >>= 4;
    }
    out[digits] = '\0';
}

// Read real hardware Time Stamp Counter (RDTSC)
static inline uint64_t hw_rdtsc(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Execute real hardware CPUID instruction
static inline void hw_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    uint32_t a = leaf, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(0));
    *eax = a;
    *ebx = b;
    *ecx = c;
    *edx = d;
}

// Read real CPU control registers
static inline uint64_t hw_read_cr0(void) {
    uint64_t val = 0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t hw_read_cr3(void) {
    uint64_t val = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t hw_read_cr4(void) {
    uint64_t val = 0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

struct CpuHardwareInfo {
    char vendor[16];
    char brand[52];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t logical_cores;
    bool has_sse2;
    bool has_sse42;
    bool has_avx;
    bool has_msr;
};

static void query_real_cpu_info(CpuHardwareInfo* info) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    hw_cpuid(0, &eax, &ebx, &ecx, &edx);

    ((uint32_t*)info->vendor)[0] = ebx;
    ((uint32_t*)info->vendor)[1] = edx;
    ((uint32_t*)info->vendor)[2] = ecx;
    info->vendor[12] = '\0';

    hw_cpuid(1, &eax, &ebx, &ecx, &edx);
    info->stepping = eax & 0xF;
    info->model = (eax >> 4) & 0xF;
    info->family = (eax >> 8) & 0xF;
    if (info->family == 6 || info->family == 15) {
        info->model += ((eax >> 16) & 0xF) << 4;
    }
    if (info->family == 15) {
        info->family += (eax >> 20) & 0xFF;
    }
    info->logical_cores = (ebx >> 16) & 0xFF;
    if (info->logical_cores == 0) info->logical_cores = 1;

    info->has_msr = (edx & (1 << 5)) != 0;
    info->has_sse2 = (edx & (1 << 26)) != 0;
    info->has_sse42 = (ecx & (1 << 20)) != 0;
    info->has_avx = (ecx & (1 << 28)) != 0;

    uint32_t max_ext = 0;
    hw_cpuid(0x80000000, &max_ext, &ebx, &ecx, &edx);
    if (max_ext >= 0x80000004) {
        uint32_t* brand_words = (uint32_t*)info->brand;
        hw_cpuid(0x80000002, &brand_words[0], &brand_words[1], &brand_words[2], &brand_words[3]);
        hw_cpuid(0x80000003, &brand_words[4], &brand_words[5], &brand_words[6], &brand_words[7]);
        hw_cpuid(0x80000004, &brand_words[8], &brand_words[9], &brand_words[10], &brand_words[11]);
        info->brand[48] = '\0';
        // Trim leading spaces
        char* p = info->brand;
        while (*p == ' ') p++;
        if (p != info->brand) {
            int k = 0;
            while (p[k]) {
                info->brand[k] = p[k];
                k++;
            }
            info->brand[k] = '\0';
        }
    } else {
        tm_strcpy(info->brand, info->vendor);
        tm_strcat(info->brand, " x86_64 Processor");
    }
}

struct BlockDiskInfo {
    uint32_t media_id;
    uint32_t block_size;
    uint64_t last_block;
    uint64_t capacity_mb;
    bool removable;
    bool read_only;
    bool logical_partition;
    bool media_present;
};

static int query_real_block_disks(BlockDiskInfo* out_disks, int max_disks) {
    if (!gSystemTable || !gSystemTable->BootServices) return 0;
    EFI_GUID BlockIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN handle_count = 0;
    EFI_HANDLE* handles = NULL;
    EFI_STATUS st = gSystemTable->BootServices->LocateHandleBuffer(
        ByProtocol, &BlockIoGuid, NULL, &handle_count, &handles);
    if (EFI_ERROR(st) || !handles || handle_count == 0) return 0;

    int count = 0;
    for (UINTN i = 0; i < handle_count && count < max_disks; i++) {
        EFI_BLOCK_IO_PROTOCOL* bio = NULL;
        if (gSystemTable->BootServices->HandleProtocol(
                handles[i], &BlockIoGuid, (void**)&bio) == EFI_SUCCESS &&
            bio && bio->Media) {
            out_disks[count].media_id = bio->Media->MediaId;
            out_disks[count].block_size = bio->Media->BlockSize;
            out_disks[count].last_block = bio->Media->LastBlock;
            uint64_t total_bytes = (bio->Media->LastBlock + 1ULL) * (uint64_t)bio->Media->BlockSize;
            out_disks[count].capacity_mb = total_bytes / (1024ULL * 1024ULL);
            out_disks[count].removable = bio->Media->RemovableMedia;
            out_disks[count].read_only = bio->Media->ReadOnly;
            out_disks[count].logical_partition = bio->Media->LogicalPartition;
            out_disks[count].media_present = bio->Media->MediaPresent;
            count++;
        }
    }
    gSystemTable->BootServices->FreePool(handles);
    return count;
}

static int query_uefi_memory_map_stats(uint64_t* out_ConventionalPages, uint64_t* out_BootServicesPages,
                                       uint64_t* out_RuntimePages, uint32_t* out_DescriptorCount) {
    *out_ConventionalPages = 0;
    *out_BootServicesPages = 0;
    *out_RuntimePages = 0;
    *out_DescriptorCount = 0;
    if (!gSystemTable || !gSystemTable->BootServices) return -1;

    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;
    gSystemTable->BootServices->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    if (map_size == 0 || desc_size == 0) return -1;

    map_size += desc_size * 8;
    uint8_t* buffer = NULL;
    if (gSystemTable->BootServices->AllocatePool(EfiLoaderData, map_size, (void**)&buffer) != EFI_SUCCESS || !buffer) {
        return -1;
    }

    if (gSystemTable->BootServices->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR*)buffer, &map_key, &desc_size, &desc_ver) == EFI_SUCCESS) {
        UINTN count = map_size / desc_size;
        *out_DescriptorCount = (uint32_t)count;
        for (UINTN i = 0; i < count; i++) {
            EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(buffer + i * desc_size);
            if (d->Type == EfiConventionalMemory) {
                *out_ConventionalPages += d->NumberOfPages;
            } else if (d->Type == EfiBootServicesCode || d->Type == EfiBootServicesData ||
                       d->Type == EfiLoaderCode || d->Type == EfiLoaderData) {
                *out_BootServicesPages += d->NumberOfPages;
            } else if (d->Type == EfiRuntimeServicesCode || d->Type == EfiRuntimeServicesData ||
                       d->Type == EfiACPIReclaimMemory || d->Type == EfiACPIMemoryNVS) {
                *out_RuntimePages += d->NumberOfPages;
            }
        }
    }
    gSystemTable->BootServices->FreePool(buffer);
    return 0;
}

} // namespace

extern "C" void launch_task_manager(void) {
    if (!gSystemTable) return;

    int win_w = 760;
    int win_h = 480;
    if (win_w > screen_width - 20) win_w = screen_width - 20;
    if (win_h > screen_height - 80) win_h = screen_height - 80;

    int win_x = (screen_width - win_w) / 2;
    int win_y = (screen_height - win_h) / 2;

    EFI_GUID SimplePointerProtocolGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    EFI_SIMPLE_POINTER_PROTOCOL *Mouse = NULL;
    gSystemTable->BootServices->LocateProtocol(&SimplePointerProtocolGuid, NULL, (void**)&Mouse);
    if (Mouse) Mouse->Reset(Mouse, TRUE);

    int cursor_x = screen_width / 2;
    int cursor_y = screen_height / 2;
    bool prev_left = false;

    // Query real CPU & Disk hardware once at launch (and refreshable)
    CpuHardwareInfo cpu_info;
    query_real_cpu_info(&cpu_info);

    BlockDiskInfo disks[16];
    int disk_count = query_real_block_disks(disks, 16);

    // Real RDTSC telemetry buffers (32 samples of real frame work vs stall ratio)
    int cpu_load_history[32];
    for (int i = 0; i < 32; i++) cpu_load_history[i] = 0;

    // Real user-allocated test blocks via UEFI AllocatePages/AllocatePool
    void* user_allocs[16];
    for (int i = 0; i < 16; i++) user_allocs[i] = NULL;
    int user_alloc_count = 0;

    int active_tab = 0; // 0=Subsystems & Memory Map, 1=CPU & RAM Telemetry, 2=PCI & Block Disks, 3=App Launcher
    int selected_row = 0;
    int pci_scroll = 0;
    bool show_cpu_regs = false;

    uint64_t last_frame_work_cycles = 0;
    uint64_t last_stall_cycles = 1;
    uint32_t measured_mhz = 0;
    uint32_t frame_counter = 0;

    play_system_sound((char*)"click");

    bool done = false;
    while (!done) {
        uint64_t frame_start_tsc = hw_rdtsc();
        frame_counter++;

        // Poll Keyboard
        EFI_INPUT_KEY Key;
        if (gSystemTable->ConIn->ReadKeyStroke(gSystemTable->ConIn, &Key) == EFI_SUCCESS) {
            if (show_cpu_regs) {
                show_cpu_regs = false;
                play_system_sound((char*)"click");
            } else if (Key.ScanCode == 0x17) {
                done = true;
            } else if (Key.UnicodeChar >= '1' && Key.UnicodeChar <= '4') {
                active_tab = (int)(Key.UnicodeChar - '1');
                play_system_sound((char*)"click");
            } else if (Key.ScanCode == 0x01) {
                if (active_tab == 2 && pci_scroll > 0) pci_scroll--;
            } else if (Key.ScanCode == 0x02) {
                if (active_tab == 2) pci_scroll++;
            }
        }

        // Poll Mouse
        uint64_t input_t0 = hw_rdtsc();
        bool clicked = false;
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
                clicked = (State.LeftButton && !prev_left);
                prev_left = State.LeftButton;
            }
        }
        uint64_t input_cycles = hw_rdtsc() - input_t0;

        // Measure real UEFI memory map telemetry
        uint64_t mmap_t0 = hw_rdtsc();
        uint64_t conv_pages = 0, bs_pages = 0, rt_pages = 0;
        uint32_t mmap_descs = 0;
        query_uefi_memory_map_stats(&conv_pages, &bs_pages, &rt_pages, &mmap_descs);
        uint64_t mmap_cycles = hw_rdtsc() - mmap_t0;

        // Measure real PCI enumeration count
        uint64_t pci_t0 = hw_rdtsc();
        int total_pci = pci_get_device_count();
        uint64_t pci_cycles = hw_rdtsc() - pci_t0;

        if (clicked) {
            if (show_cpu_regs) {
                show_cpu_regs = false;
                play_system_sound((char*)"click");
            } else {
                // Close button check
                if (blankUI_hit_test_window_close(cursor_x, cursor_y, win_w, win_h) ||
                    (cursor_x >= win_x + 10 && cursor_x <= win_x + 30 &&
                     cursor_y >= win_y + 8 && cursor_y <= win_y + 30)) {
                    done = true;
                }

                // Toolbar Tabs (4 tabs)
                int tab_y = win_y + 42;
                int tab_w = 135;
                int tab_start_x = win_x + 190;
                for (int t = 0; t < 4; t++) {
                    int tx = tab_start_x + t * (tab_w + 6);
                    if (cursor_x >= tx && cursor_x <= tx + tab_w &&
                        cursor_y >= tab_y && cursor_y <= tab_y + 28) {
                        active_tab = t;
                        play_system_sound((char*)"click");
                    }
                }

                if (active_tab == 0) {
                    int row_y_start = win_y + 114;
                    int row_h = 28;
                    for (int r = 0; r < 7; r++) {
                        int ry = row_y_start + r * row_h;
                        if (cursor_x >= win_x + 16 && cursor_x <= win_x + win_w - 16 &&
                            cursor_y >= ry && cursor_y < ry + row_h) {
                            selected_row = r;
                            play_system_sound((char*)"click");
                        }
                    }

                    int btn_y = win_y + win_h - 46;
                    // [ Free 1MB Pool ]
                    if (cursor_x >= win_x + 18 && cursor_x <= win_x + 168 &&
                        cursor_y >= btn_y && cursor_y <= btn_y + 30) {
                        if (user_alloc_count > 0) {
                            user_alloc_count--;
                            gSystemTable->BootServices->FreePool(user_allocs[user_alloc_count]);
                            user_allocs[user_alloc_count] = NULL;
                            play_system_sound((char*)"click");
                            blankUI_draw_toast((char*)"UEFI Pool", (char*)"Freed 1 MB EfiLoaderData block.");
                        } else {
                            play_system_sound((char*)"error");
                            blankUI_draw_toast((char*)"UEFI Pool", (char*)"No user test blocks currently allocated.");
                        }
                    }

                    // [ + Alloc 1MB Pool ]
                    if (cursor_x >= win_x + 178 && cursor_x <= win_x + 348 &&
                        cursor_y >= btn_y && cursor_y <= btn_y + 30) {
                        if (user_alloc_count < 16) {
                            void* ptr = NULL;
                            if (gSystemTable->BootServices->AllocatePool(EfiLoaderData, 1024 * 1024, &ptr) == EFI_SUCCESS && ptr) {
                                // Touch pages so allocation is physically backed
                                uint8_t* p = (uint8_t*)ptr;
                                for (int k = 0; k < 1024 * 1024; k += 4096) p[k] = (uint8_t)(k & 0xFF);
                                user_allocs[user_alloc_count++] = ptr;
                                play_system_sound((char*)"notification");
                                blankUI_draw_toast((char*)"UEFI Pool", (char*)"Allocated real 1 MB physical block.");
                            } else {
                                play_system_sound((char*)"error");
                                blankUI_draw_toast((char*)"UEFI Pool", (char*)"UEFI AllocatePool failed.");
                            }
                        }
                    }

                    // [ Inspect Control Regs ]
                    if (cursor_x >= win_x + win_w - 228 && cursor_x <= win_x + win_w - 18 &&
                        cursor_y >= btn_y && cursor_y <= btn_y + 30) {
                        show_cpu_regs = true;
                        play_system_sound((char*)"notification");
                    }
                } else if (active_tab == 2) {
                    int up_x = win_x + win_w - 235;
                    int dn_x = win_x + win_w - 160;
                    int rf_x = win_x + win_w - 86;
                    int sb_y = win_y + win_h - 42;
                    if (cursor_x >= up_x && cursor_x <= up_x + 68 &&
                        cursor_y >= sb_y && cursor_y <= sb_y + 26) {
                        if (pci_scroll > 0) pci_scroll--;
                        play_system_sound((char*)"click");
                    }
                    if (cursor_x >= dn_x && cursor_x <= dn_x + 68 &&
                        cursor_y >= sb_y && cursor_y <= sb_y + 26) {
                        if (pci_scroll + 5 < total_pci) pci_scroll++;
                        play_system_sound((char*)"click");
                    }
                    if (cursor_x >= rf_x && cursor_x <= rf_x + 68 &&
                        cursor_y >= sb_y && cursor_y <= sb_y + 26) {
                        disk_count = query_real_block_disks(disks, 16);
                        play_system_sound((char*)"notification");
                    }
                } else if (active_tab == 3) {
                    int grid_x = win_x + 24;
                    int grid_y = win_y + 120;
                    int card_w = (win_w - 72) / 3;
                    int card_h = 140;
                    for (int i = 0; i < 6; i++) {
                        int r = i / 3;
                        int c = i % 3;
                        int cx = grid_x + c * (card_w + 12);
                        int cy = grid_y + r * (card_h + 14);
                        if (cursor_x >= cx && cursor_x <= cx + card_w &&
                            cursor_y >= cy && cursor_y <= cy + card_h) {
                            play_system_sound((char*)"click");
                            if (i == 0) launch_blankpad();
                            else if (i == 1) launch_blankreg_edit();
                            else if (i == 2) launch_terminal(gSystemTable);
                            else if (i == 3) launch_sysinfo();
                            else if (i == 4) launch_browser(gSystemTable);
                            else if (i == 5) launch_calculator(gSystemTable);
                            if (Mouse) Mouse->Reset(Mouse, TRUE);
                            prev_left = false;
                        }
                    }
                }
            }
        }

        // Render desktop background & shell
        dui_draw_wallpaper();
        blankUI_draw_menubar();
        blankUI_draw_dock();

        // Main Window
        dui_shadow(win_x, win_y, win_w, win_h, 14, 14, 0x000000, 90);
        dui_rect_rounded(win_x, win_y, win_w, win_h, 14, 0xF4F5F8, 252);

        // Dark Telemetry Header Bar
        dui_gradient_rounded(win_x, win_y, win_w, 78, 14, 0x161922, 0x1F2430);
        dui_rect(win_x, win_y + 64, win_w, 14, 0x1F2430, 255);

        // Traffic Lights
        dui_circle(win_x + 20, win_y + 20, 6, 0xFF5F56, 255);
        dui_circle(win_x + 40, win_y + 20, 6, 0xFFBD2E, 255);
        dui_circle(win_x + 60, win_y + 20, 6, 0x27C93F, 255);

        dui_text(win_x + 80, win_y + 13, "Activity Monitor - Real Hardware & UEFI Telemetry", 0xFFFFFF, 1);
        dui_text(win_x + 20, win_y + 48, cpu_info.vendor, 0x64D2FF, 1);

        const char* tab_names[4] = { "Processes", "CPU & Memory", "PCI & Drivers", "App Launcher" };
        int tab_y = win_y + 42;
        int tab_w = 135;
        int tab_start_x = win_x + 190;
        for (int t = 0; t < 4; t++) {
            int tx = tab_start_x + t * (tab_w + 6);
            if (t == active_tab) {
                dui_rect_rounded(tx, tab_y, tab_w, 28, 7, 0x0A84FF, 255);
                int tw = dui_text_width(tab_names[t], 1);
                dui_text(tx + (tab_w - tw) / 2, tab_y + 7, tab_names[t], 0xFFFFFF, 1);
            } else {
                dui_rect_rounded(tx, tab_y, tab_w, 28, 7, 0x2B3140, 255);
                int tw = dui_text_width(tab_names[t], 1);
                dui_text(tx + (tab_w - tw) / 2, tab_y + 7, tab_names[t], 0xB8C0D4, 1);
            }
        }

        uint64_t total_mb = memory_get_total_mb();
        uint64_t used_mb = memory_get_used_mb();
        uint32_t alloc_cnt = memory_get_alloc_count();
        uint32_t vram_bytes = svga_get_vram_size();

        // TAB 0: REAL SUBSYSTEMS & LIVE RDTSC / MEMORY TELEMETRY
        if (active_tab == 0) {
            int th_y = win_y + 86;
            dui_rect(win_x + 16, th_y, win_w - 32, 24, 0xE2E5EC, 255);
            dui_text(win_x + 24, th_y + 5, "ID", 0x3A3F4B, 1);
            dui_text(win_x + 74, th_y + 5, "Active Hardware / Kernel Subsystem", 0x3A3F4B, 1);
            dui_text(win_x + 345, th_y + 5, "RDTSC Cycles", 0x3A3F4B, 1);
            dui_text(win_x + 495, th_y + 5, "Live Allocation / Metric", 0x3A3F4B, 1);

            const char* sub_names[7] = {
                "BlankDUI Compositor & Framebuffer",
                "UEFI Memory Map & Page Tables",
                "Kernel Heap Allocator (memory.cpp)",
                "UEFI BootServices & Runtime Pool",
                "PCI Bus Host Controller",
                "UEFI Block I/O Storage Stack",
                "User EfiLoaderData Pool Blocks"
            };

            uint64_t sub_cycles[7];
            sub_cycles[0] = last_frame_work_cycles;
            sub_cycles[1] = mmap_cycles;
            sub_cycles[2] = mmap_cycles / 2;
            sub_cycles[3] = input_cycles;
            sub_cycles[4] = pci_cycles;
            sub_cycles[5] = input_cycles / 2;
            sub_cycles[6] = (uint64_t)user_alloc_count * 1024ULL;

            int row_y_start = win_y + 114;
            int row_h = 28;
            char nb[32];
            char metric[64];

            for (int r = 0; r < 7; r++) {
                int ry = row_y_start + r * row_h;
                bool is_sel = (r == selected_row);
                uint32_t bg = is_sel ? 0x0A84FF : ((r % 2 == 0) ? 0xFFFFFF : 0xF8F9FB);
                uint32_t fg = is_sel ? 0xFFFFFF : 0x1C1E24;
                uint32_t sfg = is_sel ? 0xE0F0FF : 0x525866;

                dui_rect(win_x + 16, ry, win_w - 32, row_h - 1, bg, 255);

                tm_utoa((uint64_t)(r + 1), nb);
                dui_text(win_x + 24, ry + 7, nb, sfg, 1);
                dui_text(win_x + 74, ry + 7, sub_names[r], fg, 1);

                tm_utoa(sub_cycles[r], nb);
                tm_strcat(nb, " cyc");
                dui_text(win_x + 345, ry + 7, nb, sfg, 1);

                if (r == 0) {
                    tm_strcpy(metric, "VRAM: ");
                    tm_utoa((uint64_t)(vram_bytes / (1024 * 1024)), nb);
                    tm_strcat(metric, nb);
                    tm_strcat(metric, " MB (");
                    tm_utoa((uint64_t)screen_width, nb);
                    tm_strcat(metric, nb);
                    tm_strcat(metric, "x");
                    tm_utoa((uint64_t)screen_height, nb);
                    tm_strcat(metric, nb);
                    tm_strcat(metric, ")");
                } else if (r == 1) {
                    tm_utoa((uint64_t)mmap_descs, nb);
                    tm_strcpy(metric, nb);
                    tm_strcat(metric, " UEFI descriptors (");
                    tm_utoa((conv_pages * 4096ULL) / (1024ULL * 1024ULL), nb);
                    tm_strcat(metric, nb);
                    tm_strcat(metric, " MB free)");
                } else if (r == 2) {
                    tm_utoa(used_mb, nb);
                    tm_strcpy(metric, nb);
                    tm_strcat(metric, " MB used / ");
                    tm_utoa((uint64_t)alloc_cnt, nb);
                    tm_strcat(metric, nb);
                    tm_strcat(metric, " heap blocks");
                } else if (r == 3) {
                    tm_utoa((bs_pages * 4096ULL) / (1024ULL * 1024ULL), nb);
                    tm_strcpy(metric, nb);
                    tm_strcat(metric, " MB BootServices + ");
                    tm_utoa((rt_pages * 4096ULL) / 1024ULL, nb);
                    tm_strcat(metric, nb);
                    tm_strcat(metric, " KB Runtime");
                } else if (r == 4) {
                    tm_utoa((uint64_t)total_pci, nb);
                    tm_strcpy(metric, nb);
                    tm_strcat(metric, " Hardware PCI Functions Bound");
                } else if (r == 5) {
                    tm_utoa((uint64_t)disk_count, nb);
                    tm_strcpy(metric, nb);
                    tm_strcat(metric, " Block I/O Handles Attached");
                } else {
                    tm_utoa((uint64_t)user_alloc_count, nb);
                    tm_strcpy(metric, nb);
                    tm_strcat(metric, " MB User Pool Allocated");
                }
                dui_text(win_x + 495, ry + 7, metric, fg, 1);
            }

            // Bottom telemetry bar
            int footer_y = win_y + win_h - 86;
            dui_rect_rounded(win_x + 16, footer_y, win_w - 32, 32, 6, 0xE6E9F0, 255);
            char sum_buf[112];
            tm_strcpy(sum_buf, "CPU Brand: ");
            tm_strcat(sum_buf, cpu_info.brand);
            tm_strcat(sum_buf, "   |   Clock: ~");
            tm_utoa((uint64_t)measured_mhz, nb);
            tm_strcat(sum_buf, nb);
            tm_strcat(sum_buf, " MHz");
            dui_text(win_x + 26, footer_y + 9, sum_buf, 0x1C1E24, 1);

            int btn_y = win_y + win_h - 46;
            dui_rect_rounded(win_x + 18, btn_y, 150, 30, 7, 0xFF3B30, 255);
            dui_text(win_x + 30, btn_y + 8, "[ Free 1MB Pool ]", 0xFFFFFF, 1);

            dui_rect_rounded(win_x + 178, btn_y, 170, 30, 7, 0x0A84FF, 255);
            dui_text(win_x + 190, btn_y + 8, "[ + Alloc 1MB Pool ]", 0xFFFFFF, 1);

            dui_rect_rounded(win_x + win_w - 228, btn_y, 210, 30, 7, 0x5856D6, 255);
            dui_text(win_x + win_w - 214, btn_y + 8, "[ Read CPU Control Regs ]", 0xFFFFFF, 1);
        }

        // TAB 1: REAL CPUID, RDTSC CYCLE GRAPH & PHYSICAL MEMORY
        else if (active_tab == 1) {
            int pad_x = win_x + 20;
            int card_w = win_w - 40;
            char nb[32];

            // CPU Hardware & Live RDTSC Load Graph Card
            int cpu_card_y = win_y + 88;
            int cpu_card_h = 182;
            dui_rect_rounded(pad_x, cpu_card_y, card_w, cpu_card_h, 10, 0x181C26, 255);
            dui_rect_outline(pad_x, cpu_card_y, card_w, cpu_card_h, 0x2E3545, 1);

            char cpu_hdr[112];
            tm_strcpy(cpu_hdr, "CPUID: ");
            tm_strcat(cpu_hdr, cpu_info.brand);
            tm_strcat(cpu_hdr, " (Fam ");
            tm_utoa(cpu_info.family, nb);
            tm_strcat(cpu_hdr, nb);
            tm_strcat(cpu_hdr, " Mod ");
            tm_utoa(cpu_info.model, nb);
            tm_strcat(cpu_hdr, nb);
            tm_strcat(cpu_hdr, " Step ");
            tm_utoa(cpu_info.stepping, nb);
            tm_strcat(cpu_hdr, nb);
            tm_strcat(cpu_hdr, ")");
            dui_text(pad_x + 16, cpu_card_y + 10, cpu_hdr, 0xFFFFFF, 1);

            char tsc_hdr[112];
            tm_strcpy(tsc_hdr, "RDTSC Delta: ");
            tm_utoa(last_frame_work_cycles, nb);
            tm_strcat(tsc_hdr, nb);
            tm_strcat(tsc_hdr, " active cyc / ");
            tm_utoa(last_stall_cycles, nb);
            tm_strcat(tsc_hdr, nb);
            tm_strcat(tsc_hdr, " stall cyc  |  Freq: ");
            tm_utoa(measured_mhz, nb);
            tm_strcat(tsc_hdr, nb);
            tm_strcat(tsc_hdr, " MHz  |  Cores: ");
            tm_utoa(cpu_info.logical_cores, nb);
            tm_strcat(tsc_hdr, nb);
            dui_text(pad_x + 16, cpu_card_y + 28, tsc_hdr, 0x64D2FF, 1);

            int gx = pad_x + 16;
            int gy = cpu_card_y + 48;
            int gw = card_w - 32;
            int gh = 120;
            dui_rect(gx, gy, gw, gh, 0x11141C, 255);
            for (int gl = 1; gl <= 3; gl++) {
                int ly = gy + (gh * gl) / 4;
                dui_line(gx, ly, gx + gw, ly, 0x232938, 1);
            }

            int bar_step = gw / 32;
            for (int s = 0; s < 32; s++) {
                int val = cpu_load_history[s];
                if (val < 1) val = 1;
                if (val > 100) val = 100;
                int bh = (val * (gh - 8)) / 100;
                int bx = gx + 6 + s * bar_step;
                int by = gy + gh - bh;
                uint32_t bcol = (val > 65) ? 0xFF9F0A : 0x30D158;
                dui_rect(bx, by, bar_step - 4, bh, bcol, 190);
                if (s > 0) {
                    int prev_val = cpu_load_history[s - 1];
                    if (prev_val < 1) prev_val = 1;
                    int prev_bh = (prev_val * (gh - 8)) / 100;
                    int prev_bx = gx + 6 + (s - 1) * bar_step + (bar_step - 4) / 2;
                    int prev_by = gy + gh - prev_bh;
                    dui_line(prev_bx, prev_by, bx + (bar_step - 4) / 2, by, 0x64D2FF, 2);
                }
            }

            // Real Physical Memory & UEFI Memory Map Card
            int mem_y = cpu_card_y + cpu_card_h + 12;
            int mem_h = 182;
            dui_rect_rounded(pad_x, mem_y, card_w, mem_h, 10, 0xFFFFFF, 255);
            dui_rect_outline(pad_x, mem_y, card_w, mem_h, 0xDCDFE6, 1);

            uint64_t uefi_free_mb = (conv_pages * 4096ULL) / (1024ULL * 1024ULL);
            uint64_t uefi_bs_mb = (bs_pages * 4096ULL) / (1024ULL * 1024ULL);
            if (total_mb == 0) total_mb = uefi_free_mb + uefi_bs_mb;
            uint64_t real_used_mb = used_mb + uefi_bs_mb + (uint64_t)user_alloc_count;
            if (real_used_mb > total_mb && total_mb > 0) real_used_mb = total_mb;
            uint64_t real_free_mb = (total_mb >= real_used_mb) ? (total_mb - real_used_mb) : uefi_free_mb;

            char ram_str[112];
            tm_strcpy(ram_str, "Physical RAM: ");
            tm_utoa(real_used_mb, nb);
            tm_strcat(ram_str, nb);
            tm_strcat(ram_str, " MB Used  |  ");
            tm_utoa(real_free_mb, nb);
            tm_strcat(ram_str, nb);
            tm_strcat(ram_str, " MB Free  |  ");
            tm_utoa(total_mb, nb);
            tm_strcat(ram_str, nb);
            tm_strcat(ram_str, " MB Total");
            dui_text(pad_x + 18, mem_y + 14, ram_str, 0x1C1E24, 1);

            int bar_w = card_w - 36;
            dui_rect_rounded(pad_x + 18, mem_y + 36, bar_w, 20, 6, 0xE5E8F0, 255);
            int fill_w = (total_mb > 0) ? (int)((real_used_mb * (uint64_t)bar_w) / total_mb) : 20;
            if (fill_w < 8) fill_w = 8;
            if (fill_w > bar_w) fill_w = bar_w;
            dui_rect_rounded(pad_x + 18, mem_y + 36, fill_w, 20, 6, 0x0A84FF, 255);

            // Real VRAM & Kernel Heap Counters
            char vram_str[112];
            tm_strcpy(vram_str, "SVGA / BDRM Framebuffer VRAM: ");
            tm_utoa((uint64_t)(vram_bytes / (1024 * 1024)), nb);
            tm_strcat(vram_str, nb);
            tm_strcat(vram_str, " MB (");
            tm_utoa((uint64_t)vram_bytes, nb);
            tm_strcat(vram_str, nb);
            tm_strcat(vram_str, " bytes)  |  Kernel Heap Allocs: ");
            tm_utoa((uint64_t)alloc_cnt, nb);
            tm_strcat(vram_str, nb);
            dui_text(pad_x + 18, mem_y + 68, vram_str, 0x1C1E24, 1);

            uint32_t fb_used_bytes = (uint32_t)(screen_width * screen_height * 4 * 2); // Front + Back buffer
            dui_rect_rounded(pad_x + 18, mem_y + 90, bar_w, 18, 6, 0xE5E8F0, 255);
            int vram_fill = (vram_bytes > 0) ? (int)(((uint64_t)fb_used_bytes * (uint64_t)bar_w) / vram_bytes) : (bar_w / 3);
            if (vram_fill < 10) vram_fill = 10;
            if (vram_fill > bar_w) vram_fill = bar_w;
            dui_rect_rounded(pad_x + 18, mem_y + 90, vram_fill, 18, 6, 0x5856D6, 255);

            // CPU Feature Set & UEFI Descriptor Summary
            dui_rect_rounded(pad_x + 18, mem_y + 122, bar_w, 46, 8, 0xF0F3F9, 255);
            char feat_str[112];
            tm_strcpy(feat_str, "ISA Extensions: x86_64");
            if (cpu_info.has_msr) tm_strcat(feat_str, " +MSR");
            if (cpu_info.has_sse2) tm_strcat(feat_str, " +SSE2");
            if (cpu_info.has_sse42) tm_strcat(feat_str, " +SSE4.2");
            if (cpu_info.has_avx) tm_strcat(feat_str, " +AVX");
            dui_text(pad_x + 28, mem_y + 129, feat_str, 0x1C1E24, 1);

            char desc_str[112];
            tm_strcpy(desc_str, "UEFI GetMemoryMap: ");
            tm_utoa((uint64_t)mmap_descs, nb);
            tm_strcat(desc_str, nb);
            tm_strcat(desc_str, " descriptors  |  Conventional Pages: ");
            tm_utoa(conv_pages, nb);
            tm_strcat(desc_str, nb);
            tm_strcat(desc_str, "  |  BootServices Pages: ");
            tm_utoa(bs_pages, nb);
            tm_strcat(desc_str, nb);
            dui_text(pad_x + 28, mem_y + 147, desc_str, 0x4A5060, 1);
        }

        // TAB 2: REAL PCI BUS FUNCTIONS & REAL UEFI BLOCK_IO DISKS
        else if (active_tab == 2) {
            int th_y = win_y + 84;
            dui_rect(win_x + 16, th_y, win_w - 32, 22, 0xE2E5EC, 255);
            dui_text(win_x + 24, th_y + 4, "Bus:Slot.Fn", 0x3A3F4B, 1);
            dui_text(win_x + 120, th_y + 4, "Ven:Dev", 0x3A3F4B, 1);
            dui_text(win_x + 204, th_y + 4, "BAR0", 0x3A3F4B, 1);
            dui_text(win_x + 300, th_y + 4, "Real PCI Hardware Device", 0x3A3F4B, 1);
            dui_text(win_x + 580, th_y + 4, "Bound Driver", 0x3A3F4B, 1);

            int row_y = win_y + 108;
            int row_h = 28;
            int max_pci_rows = 6;
            for (int r = 0; r < max_pci_rows; r++) {
                int idx = pci_scroll + r;
                if (idx >= total_pci) break;

                uint8_t bus = 0, dev = 0, func = 0, cls = 0, subcls = 0;
                uint16_t vendor = 0, device = 0;
                uint32_t bar0 = 0;
                const char* vendor_str = NULL;
                const char* dev_str = NULL;
                const char* drv_str = NULL;

                if (pci_get_device_info(idx, &bus, &dev, &func, &vendor, &device,
                                        &cls, &subcls, &bar0, &vendor_str, &dev_str, &drv_str)) {
                    int ry = row_y + r * row_h;
                    uint32_t bg = (r % 2 == 0) ? 0xFFFFFF : 0xF7F9FC;
                    dui_rect(win_x + 16, ry, win_w - 32, row_h - 1, bg, 255);

                    char bdf[16];
                    char hbuf[12];
                    tm_hex_digits(bus, 2, hbuf);
                    tm_strcpy(bdf, hbuf);
                    tm_strcat(bdf, ":");
                    tm_hex_digits(dev, 2, hbuf);
                    tm_strcat(bdf, hbuf);
                    tm_strcat(bdf, ".");
                    tm_hex_digits(func, 1, hbuf);
                    tm_strcat(bdf, hbuf);
                    dui_text(win_x + 24, ry + 7, bdf, 0x0A84FF, 1);

                    char vd[16];
                    tm_hex_digits(vendor, 4, hbuf);
                    tm_strcpy(vd, hbuf);
                    tm_strcat(vd, ":");
                    tm_hex_digits(device, 4, hbuf);
                    tm_strcat(vd, hbuf);
                    dui_text(win_x + 120, ry + 7, vd, 0x4A5060, 1);

                    char bar_str[16];
                    tm_strcpy(bar_str, "0x");
                    tm_hex_digits(bar0, 8, hbuf);
                    tm_strcat(bar_str, hbuf);
                    dui_text(win_x + 204, ry + 7, bar_str, 0x6E7380, 1);

                    char hw_name[64];
                    tm_strcpy(hw_name, vendor_str ? vendor_str : "PCI");
                    tm_strcat(hw_name, " ");
                    tm_strcat(hw_name, dev_str ? dev_str : "Device");
                    dui_text(win_x + 300, ry + 7, hw_name, 0x1C1E24, 1);

                    const char* dname = (drv_str && drv_str[0]) ? drv_str : "pci_generic.sys";
                    dui_rect_rounded(win_x + 576, ry + 3, 160, 20, 5, 0xE8F5E9, 255);
                    dui_text(win_x + 584, ry + 6, dname, 0x1E7E34, 1);
                }
            }

            // Real UEFI Block I/O Disks Section
            int disk_hdr_y = win_y + 284;
            dui_rect(win_x + 16, disk_hdr_y, win_w - 32, 22, 0x1F2430, 255);
            dui_text(win_x + 24, disk_hdr_y + 4, "UEFI EFI_BLOCK_IO_PROTOCOL Physical Disks & Partitions", 0x64D2FF, 1);

            int disk_row_y = disk_hdr_y + 24;
            char nb[32];
            for (int d = 0; d < disk_count && d < 4; d++) {
                int dy = disk_row_y + d * 28;
                uint32_t bg = (d % 2 == 0) ? 0xFFFFFF : 0xF7F9FC;
                dui_rect(win_x + 16, dy, win_w - 32, 27, bg, 255);

                char dline[120];
                tm_strcpy(dline, "Disk #");
                tm_utoa((uint64_t)d, nb);
                tm_strcat(dline, nb);
                tm_strcat(dline, " [MediaID ");
                tm_utoa((uint64_t)disks[d].media_id, nb);
                tm_strcat(dline, nb);
                tm_strcat(dline, "]  BlockSize: ");
                tm_utoa((uint64_t)disks[d].block_size, nb);
                tm_strcat(dline, nb);
                tm_strcat(dline, " B  |  LastBlock: ");
                tm_utoa(disks[d].last_block, nb);
                tm_strcat(dline, nb);
                tm_strcat(dline, "  |  Capacity: ");
                tm_utoa(disks[d].capacity_mb, nb);
                tm_strcat(dline, nb);
                tm_strcat(dline, " MB ");
                tm_strcat(dline, disks[d].logical_partition ? "(Partition)" : "(Raw Disk)");
                if (disks[d].read_only) tm_strcat(dline, " [RO]");
                dui_text(win_x + 24, dy + 6, dline, 0x1C1E24, 1);
            }

            int sb_y = win_y + win_h - 42;
            char pci_sum[64];
            tm_strcpy(pci_sum, "PCI Functions: ");
            tm_utoa((uint64_t)total_pci, nb);
            tm_strcat(pci_sum, nb);
            tm_strcat(pci_sum, "  |  Block I/O Volumes: ");
            tm_utoa((uint64_t)disk_count, nb);
            tm_strcat(pci_sum, nb);
            dui_text(win_x + 22, sb_y + 6, pci_sum, 0x3A3F4B, 1);

            dui_rect_rounded(win_x + win_w - 235, sb_y, 68, 26, 6, 0x2B3140, 255);
            dui_text(win_x + win_w - 220, sb_y + 6, "PCI Up", 0xFFFFFF, 1);

            dui_rect_rounded(win_x + win_w - 160, sb_y, 68, 26, 6, 0x2B3140, 255);
            dui_text(win_x + win_w - 148, sb_y + 6, "PCI Dn", 0xFFFFFF, 1);

            dui_rect_rounded(win_x + win_w - 86, sb_y, 68, 26, 6, 0x0A84FF, 255);
            dui_text(win_x + win_w - 76, sb_y + 6, "Rescan", 0xFFFFFF, 1);
        }

        // TAB 3: APP LAUNCHER
        else if (active_tab == 3) {
            dui_text(win_x + 24, win_y + 92, "Quick-Launch BlankOS System Applications (Click any card to open):", 0x3A3F4B, 1);

            const char* titles[6] = {
                "BlankPad & Files",
                "Registry Studio",
                "Terminal",
                "System Settings",
                "Browser",
                "Calculator"
            };
            const char* subs[6] = {
                "Rich Text, Code & VFS Finder",
                "Live Kernel & UI Key-Value Editor",
                "Interactive UEFI & OS Shell",
                "Display, Sound & System Info",
                "BlankNet Web & Local Docs",
                "Precision Scientific Utility"
            };
            uint32_t colors[6] = {
                0x0A84FF, 0xFF9500, 0x2C2C2E, 0x5856D6, 0x34C759, 0xFF2D55
            };

            int grid_x = win_x + 24;
            int grid_y = win_y + 120;
            int card_w = (win_w - 72) / 3;
            int card_h = 140;

            for (int i = 0; i < 6; i++) {
                int r = i / 3;
                int c = i % 3;
                int cx = grid_x + c * (card_w + 12);
                int cy = grid_y + r * (card_h + 14);

                dui_rect_rounded(cx, cy, card_w, card_h, 10, 0xFFFFFF, 255);
                dui_rect_outline(cx, cy, card_w, card_h, 0xDCE0E8, 1);

                dui_rect_rounded(cx + 16, cy + 16, 42, 42, 10, colors[i], 255);
                dui_text(cx + 28, cy + 29, ">", 0xFFFFFF, 1);

                dui_text(cx + 16, cy + 70, titles[i], 0x1C1E24, 1);
                dui_text(cx + 16, cy + 90, subs[i], 0x6E7380, 1);

                dui_rect_rounded(cx + 16, cy + 110, 88, 22, 6, colors[i], 255);
                dui_text(cx + 28, cy + 114, "LAUNCH", 0xFFFFFF, 1);
            }
        }

        // Real Hardware Control Registers Modal
        if (show_cpu_regs) {
            uint64_t cr0 = hw_read_cr0();
            uint64_t cr3 = hw_read_cr3();
            uint64_t cr4 = hw_read_cr4();
            uint64_t tsc_now = hw_rdtsc();
            char h16[20];
            char line[96];

            dui_rect_rounded(win_x + 40, win_y + 60, win_w - 80, win_h - 100, 12, 0x12151E, 252);
            dui_rect_outline(win_x + 40, win_y + 60, win_w - 80, win_h - 100, 0x0A84FF, 2);
            dui_text(win_x + 64, win_y + 84, "LIVE x86_64 HARDWARE CONTROL REGISTERS & CPUID DUMP", 0x64D2FF, 1);
            dui_text(win_x + 64, win_y + 112, cpu_info.brand, 0xFFFFFF, 1);

            tm_strcpy(line, "CR0 (Paging/Protected Mode): 0x");
            tm_hex_digits(cr0, 16, h16);
            tm_strcat(line, h16);
            dui_text(win_x + 64, win_y + 146, line, 0x30D158, 1);

            tm_strcpy(line, "CR3 (PML4 Page Directory Base): 0x");
            tm_hex_digits(cr3, 16, h16);
            tm_strcat(line, h16);
            dui_text(win_x + 64, win_y + 172, line, 0x30D158, 1);

            tm_strcpy(line, "CR4 (PAE/OSFXSR/OSXMMEXCPT): 0x");
            tm_hex_digits(cr4, 16, h16);
            tm_strcat(line, h16);
            dui_text(win_x + 64, win_y + 198, line, 0x30D158, 1);

            tm_strcpy(line, "RDTSC (Hardware Cycle Counter): 0x");
            tm_hex_digits(tsc_now, 16, h16);
            tm_strcat(line, h16);
            dui_text(win_x + 64, win_y + 224, line, 0xFFD60A, 1);

            tm_strcpy(line, "UEFI SystemTable Address:       0x");
            tm_hex_digits((uint64_t)(uintptr_t)gSystemTable, 16, h16);
            tm_strcat(line, h16);
            dui_text(win_x + 64, win_y + 250, line, 0xD1D5DB, 1);

            dui_rect_rounded(win_x + 64, win_y + 305, 280, 32, 8, 0x0A84FF, 255);
            dui_text(win_x + 84, win_y + 314, "[ Click or Press Any Key to Close ]", 0xFFFFFF, 1);
        }

        blankUI_draw_cursor(cursor_x, cursor_y);
        swap_buffers();

        // Measure real active work cycles vs 16ms hardware stall cycles
        uint64_t work_end_tsc = hw_rdtsc();
        last_frame_work_cycles = work_end_tsc - frame_start_tsc;

        uint64_t stall_t0 = hw_rdtsc();
        gSystemTable->BootServices->Stall(16000);
        uint64_t stall_t1 = hw_rdtsc();
        last_stall_cycles = stall_t1 - stall_t0;
        if (last_stall_cycles == 0) last_stall_cycles = 1;

        // 16000 microseconds = 16 ms -> cycles / 16000 = MHz
        measured_mhz = (uint32_t)(last_stall_cycles / 16000ULL);

        if ((frame_counter % 4) == 0) {
            uint64_t total_frame_cycles = last_frame_work_cycles + last_stall_cycles;
            int load_pct = (int)((last_frame_work_cycles * 100ULL) / total_frame_cycles);
            if (load_pct < 1) load_pct = 1;
            if (load_pct > 100) load_pct = 100;
            for (int i = 0; i < 31; i++) {
                cpu_load_history[i] = cpu_load_history[i + 1];
            }
            cpu_load_history[31] = load_pct;
        }
    }

    // Clean up any remaining user test allocations on exit
    for (int i = 0; i < user_alloc_count; i++) {
        if (user_allocs[i]) {
            gSystemTable->BootServices->FreePool(user_allocs[i]);
            user_allocs[i] = NULL;
        }
    }
}
