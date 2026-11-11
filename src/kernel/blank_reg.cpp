#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>
#include <efiprot.h>

// BlankOS Hierarchical Configuration Registry (blankReg) with FAT32 Persistence

#define BLANKREG_MAX_KEYS 128
#define BLANKREG_TYPE_INT 0
#define BLANKREG_TYPE_STR 1

extern "C" {
    __attribute__((weak)) EFI_SYSTEM_TABLE *gSystemTable = NULL;
}

typedef struct {
    bool in_use;
    char key_name[64];
    int value_type; // 0=INT, 1=STRING
    int int_val;
    char str_val[128];
} blankReg_Key;

static blankReg_Key registry[BLANKREG_MAX_KEYS];
static int reg_count = 0;
static bool reg_initialized = false;
static bool reg_suppress_autosave = false;
static EFI_SYSTEM_TABLE* g_reg_st = NULL;

static void reg_strncpy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    int i = 0;
    while (i < max_len - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool reg_streq(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

static int reg_find_slot(const char* key) {
    if (!key) return -1;
    for (int i = 0; i < BLANKREG_MAX_KEYS; i++) {
        if (registry[i].in_use && reg_streq(registry[i].key_name, key)) {
            return i;
        }
    }
    return -1;
}

static int reg_alloc_slot(const char* key) {
    int existing = reg_find_slot(key);
    if (existing >= 0) return existing;
    for (int i = 0; i < BLANKREG_MAX_KEYS; i++) {
        if (!registry[i].in_use) {
            registry[i].in_use = true;
            reg_strncpy(registry[i].key_name, key, 64);
            registry[i].value_type = BLANKREG_TYPE_INT;
            registry[i].int_val = 0;
            registry[i].str_val[0] = '\0';
            reg_count++;
            return i;
        }
    }
    return -1;
}

static int reg_int_to_str(int val, char* out) {
    char tmp[16];
    int pos = 0;
    bool neg = false;
    uint32_t uval;

    if (val < 0) {
        neg = true;
        uval = (uint32_t)(-val);
    } else {
        uval = (uint32_t)val;
    }

    if (uval == 0) {
        tmp[pos++] = '0';
    } else {
        while (uval > 0) {
            tmp[pos++] = (char)('0' + (uval % 10));
            uval /= 10;
        }
    }

    int out_idx = 0;
    if (neg) out[out_idx++] = '-';
    while (pos > 0) {
        out[out_idx++] = tmp[--pos];
    }
    out[out_idx] = '\0';
    return out_idx;
}

static bool reg_parse_int(const char* s, int* out_val) {
    if (!s || s[0] == '\0') return false;
    int i = 0;
    bool neg = false;
    if (s[i] == '-') {
        neg = true;
        i++;
    } else if (s[i] == '+') {
        i++;
    }
    if (s[i] == '\0') return false;
    int val = 0;
    while (s[i] != '\0') {
        if (s[i] < '0' || s[i] > '9') return false;
        val = val * 10 + (s[i] - '0');
        i++;
    }
    *out_val = neg ? -val : val;
    return true;
}

extern "C" {
    void blankReg_init(void);
    bool blankReg_save(EFI_SYSTEM_TABLE *st);
    bool blankReg_load(EFI_SYSTEM_TABLE *st);

    void blankReg_set_int(const char* key, int val) {
        if (!reg_initialized) blankReg_init();
        if (!key) return;
        int slot = reg_alloc_slot(key);
        if (slot < 0) return;
        registry[slot].value_type = BLANKREG_TYPE_INT;
        registry[slot].int_val = val;
        reg_int_to_str(val, registry[slot].str_val);
        if (!reg_suppress_autosave) {
            EFI_SYSTEM_TABLE* st = g_reg_st ? g_reg_st : gSystemTable;
            if (st) blankReg_save(st);
        }
    }

    int blankReg_get_int(const char* key, int default_val) {
        if (!reg_initialized) blankReg_init();
        int slot = reg_find_slot(key);
        if (slot < 0) return default_val;
        if (registry[slot].value_type == BLANKREG_TYPE_INT) {
            return registry[slot].int_val;
        }
        int parsed = default_val;
        if (reg_parse_int(registry[slot].str_val, &parsed)) {
            return parsed;
        }
        return default_val;
    }

    void blankReg_set_string(const char* key, const char* val) {
        if (!reg_initialized) blankReg_init();
        if (!key || !val) return;
        int slot = reg_alloc_slot(key);
        if (slot < 0) return;
        registry[slot].value_type = BLANKREG_TYPE_STR;
        reg_strncpy(registry[slot].str_val, val, 128);
        int parsed = 0;
        if (reg_parse_int(val, &parsed)) {
            registry[slot].int_val = parsed;
        } else {
            registry[slot].int_val = 0;
        }
        if (!reg_suppress_autosave) {
            EFI_SYSTEM_TABLE* st = g_reg_st ? g_reg_st : gSystemTable;
            if (st) blankReg_save(st);
        }
    }

    char* blankReg_get_string(const char* key, const char* default_val) {
        if (!reg_initialized) blankReg_init();
        int slot = reg_find_slot(key);
        if (slot < 0) return (char*)default_val;
        return registry[slot].str_val;
    }

    void blankReg_init(void) {
        if (reg_initialized) return;
        reg_initialized = true;
        reg_suppress_autosave = true;
        reg_count = 0;
        for (int i = 0; i < BLANKREG_MAX_KEYS; i++) {
            registry[i].in_use = false;
            registry[i].key_name[0] = '\0';
            registry[i].value_type = BLANKREG_TYPE_INT;
            registry[i].int_val = 0;
            registry[i].str_val[0] = '\0';
        }

        // Pre-populate default system & desktop configuration keys
        blankReg_set_string("System/OSName", "BlankOS Sequoia Twist");
        blankReg_set_string("System/KernelVersion", "2.4.0-x86_64-efi");
        blankReg_set_string("System/WindowServer", "BlankDUI QuartzGlass");
        blankReg_set_int("UI/WallpaperTheme", 0);
        blankReg_set_int("UI/DynamicIsland", 1);
        blankReg_set_int("UI/DesktopWidgets", 1);
        blankReg_set_int("UI/GlassOpacity", 220);
        blankReg_set_int("Audio/MasterVolume", 85);
        blankReg_set_int("Audio/StartupChime", 1);
        blankReg_set_int("Hardware/GPUAccel", 1);
        blankReg_set_string("Network/Hostname", "blankbook-pro.local");
        blankReg_set_string("Network/IPv4", "192.168.1.42");
        reg_suppress_autosave = false;

        EFI_SYSTEM_TABLE* st = g_reg_st ? g_reg_st : gSystemTable;
        if (st) {
            blankReg_load(st);
        }
    }

    int blankReg_get_count(void) {
        if (!reg_initialized) blankReg_init();
        return reg_count;
    }

    bool blankReg_get_entry(int index, char* key_out, int* type_out, int* int_out, char* str_out) {
        if (!reg_initialized) blankReg_init();
        if (index < 0 || index >= reg_count) return false;

        int seen = 0;
        for (int i = 0; i < BLANKREG_MAX_KEYS; i++) {
            if (registry[i].in_use) {
                if (seen == index) {
                    if (key_out)  reg_strncpy(key_out, registry[i].key_name, 64);
                    if (type_out) *type_out = registry[i].value_type;
                    if (int_out)  *int_out  = registry[i].int_val;
                    if (str_out)  reg_strncpy(str_out, registry[i].str_val, 128);
                    return true;
                }
                seen++;
            }
        }
        return false;
    }

    bool blankReg_delete_key(const char* key) {
        if (!reg_initialized) blankReg_init();
        int slot = reg_find_slot(key);
        if (slot < 0) return false;
        registry[slot].in_use = false;
        registry[slot].key_name[0] = '\0';
        registry[slot].str_val[0] = '\0';
        registry[slot].int_val = 0;
        if (reg_count > 0) reg_count--;
        if (!reg_suppress_autosave) {
            EFI_SYSTEM_TABLE* st = g_reg_st ? g_reg_st : gSystemTable;
            if (st) blankReg_save(st);
        }
        return true;
    }

    bool blankReg_save(EFI_SYSTEM_TABLE *st) {
        if (!reg_initialized) blankReg_init();
        if (st) g_reg_st = st;
        else st = g_reg_st ? g_reg_st : gSystemTable;
        if (!st || !st->BootServices) return false;

        static char save_buf[16384];
        int pos = 0;
        const char* header = "# BlankOS Registry Hive (REGISTRY.INI)\r\n";
        for (int i = 0; header[i] != '\0'; i++) {
            save_buf[pos++] = header[i];
        }

        for (int i = 0; i < BLANKREG_MAX_KEYS; i++) {
            if (!registry[i].in_use) continue;
            if (pos + 220 >= (int)sizeof(save_buf)) break;

            const char* k = registry[i].key_name;
            for (int j = 0; k[j] != '\0'; j++) save_buf[pos++] = k[j];
            save_buf[pos++] = '=';

            if (registry[i].value_type == BLANKREG_TYPE_INT) {
                save_buf[pos++] = 'i';
                save_buf[pos++] = 'n';
                save_buf[pos++] = 't';
                save_buf[pos++] = ':';
                char num_buf[16];
                int nlen = reg_int_to_str(registry[i].int_val, num_buf);
                for (int j = 0; j < nlen; j++) save_buf[pos++] = num_buf[j];
            } else {
                save_buf[pos++] = 's';
                save_buf[pos++] = 't';
                save_buf[pos++] = 'r';
                save_buf[pos++] = ':';
                const char* v = registry[i].str_val;
                for (int j = 0; v[j] != '\0'; j++) save_buf[pos++] = v[j];
            }
            save_buf[pos++] = '\r';
            save_buf[pos++] = '\n';
        }

        EFI_GUID sfs_guid = { 0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        EFI_STATUS status = st->BootServices->LocateProtocol(&sfs_guid, NULL, (VOID**)&fs);
        if (EFI_ERROR(status) || !fs) return false;

        EFI_FILE_HANDLE root = NULL;
        status = fs->OpenVolume(fs, &root);
        if (EFI_ERROR(status) || !root) return false;

        EFI_FILE_HANDLE file = NULL;
        status = root->Open(
            root,
            &file,
            (CHAR16*)L"EFI\\BOOT\\REGISTRY.INI",
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
            0
        );
        if (EFI_ERROR(status) || !file) {
            status = root->Open(
                root,
                &file,
                (CHAR16*)L"REGISTRY.INI",
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                0
            );
        }
        if (EFI_ERROR(status) || !file) {
            root->Close(root);
            return false;
        }

        UINTN write_size = (UINTN)pos;
        status = file->Write(file, &write_size, (VOID*)save_buf);
        file->Flush(file);
        file->Close(file);
        root->Close(root);
        return !EFI_ERROR(status);
    }

    bool blankReg_load(EFI_SYSTEM_TABLE *st) {
        if (!reg_initialized) blankReg_init();
        if (st) g_reg_st = st;
        else st = g_reg_st ? g_reg_st : gSystemTable;
        if (!st || !st->BootServices) return false;

        EFI_GUID sfs_guid = { 0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b} };
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        EFI_STATUS status = st->BootServices->LocateProtocol(&sfs_guid, NULL, (VOID**)&fs);
        if (EFI_ERROR(status) || !fs) return false;

        EFI_FILE_HANDLE root = NULL;
        status = fs->OpenVolume(fs, &root);
        if (EFI_ERROR(status) || !root) return false;

        EFI_FILE_HANDLE file = NULL;
        status = root->Open(
            root,
            &file,
            (CHAR16*)L"EFI\\BOOT\\REGISTRY.INI",
            EFI_FILE_MODE_READ,
            0
        );
        if (EFI_ERROR(status) || !file) {
            status = root->Open(
                root,
                &file,
                (CHAR16*)L"REGISTRY.INI",
                EFI_FILE_MODE_READ,
                0
            );
        }
        if (EFI_ERROR(status) || !file) {
            root->Close(root);
            return false;
        }

        static char load_buf[16384];
        UINTN read_size = sizeof(load_buf) - 1;
        status = file->Read(file, &read_size, (VOID*)load_buf);
        file->Close(file);
        root->Close(root);
        if (EFI_ERROR(status) || read_size == 0) return false;

        load_buf[read_size] = '\0';
        reg_suppress_autosave = true;

        UINTN i = 0;
        while (i < read_size) {
            while (i < read_size && (load_buf[i] == '\r' || load_buf[i] == '\n' || load_buf[i] == ' ' || load_buf[i] == '\t')) {
                i++;
            }
            if (i >= read_size) break;

            UINTN line_start = i;
            while (i < read_size && load_buf[i] != '\r' && load_buf[i] != '\n') {
                i++;
            }
            UINTN line_end = i;

            if (load_buf[line_start] == '#' || load_buf[line_start] == ';') {
                continue;
            }

            UINTN eq_pos = line_start;
            while (eq_pos < line_end && load_buf[eq_pos] != '=') {
                eq_pos++;
            }
            if (eq_pos >= line_end || eq_pos == line_start) {
                continue;
            }

            char key_buf[64];
            UINTN klen = eq_pos - line_start;
            if (klen >= sizeof(key_buf)) klen = sizeof(key_buf) - 1;
            for (UINTN k = 0; k < klen; k++) key_buf[k] = load_buf[line_start + k];
            key_buf[klen] = '\0';

            char val_buf[128];
            UINTN vstart = eq_pos + 1;
            UINTN vlen = (line_end > vstart) ? (line_end - vstart) : 0;
            if (vlen >= sizeof(val_buf)) vlen = sizeof(val_buf) - 1;
            for (UINTN v = 0; v < vlen; v++) val_buf[v] = load_buf[vstart + v];
            val_buf[vlen] = '\0';

            if (val_buf[0] == 'i' && val_buf[1] == 'n' && val_buf[2] == 't' && val_buf[3] == ':') {
                int iv = 0;
                if (reg_parse_int(&val_buf[4], &iv)) {
                    blankReg_set_int(key_buf, iv);
                }
            } else if (val_buf[0] == 's' && val_buf[1] == 't' && val_buf[2] == 'r' && val_buf[3] == ':') {
                blankReg_set_string(key_buf, &val_buf[4]);
            } else {
                int iv = 0;
                if (reg_parse_int(val_buf, &iv)) {
                    blankReg_set_int(key_buf, iv);
                } else {
                    blankReg_set_string(key_buf, val_buf);
                }
            }
        }
        reg_suppress_autosave = false;
        return true;
    }
}
