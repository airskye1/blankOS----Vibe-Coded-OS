#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern "C" {
    extern bool svga_is_active(void);
    extern uint32_t svga_get_vram_size(void);
    extern int pci_get_device_count(void);
    extern bool pci_get_device_info(
        int index,
        uint8_t* bus,
        uint8_t* dev,
        uint8_t* func,
        uint16_t* vendor,
        uint16_t* device,
        uint8_t* class_code,
        uint8_t* subclass,
        uint32_t* bar0,
        const char** vendor_str,
        const char** dev_str,
        const char** drv_str
    );
}

typedef struct {
    bool initialized;
    bool hdr_enabled;
    int max_luminance_nits;
    bool freesync_enabled;
    bool gsync_enabled;
    bool night_shift_enabled;
    int night_shift_warmth; // 0..100
    uint64_t frame_count;
    uint16_t gpu_vendor_id;
    uint16_t gpu_device_id;
    uint32_t vram_bytes;
    uint8_t gamma_lut_r[256];
    uint8_t gamma_lut_g[256];
    uint8_t gamma_lut_b[256];
    char icc_profile_name[64];
} DisplayCapabilities;

static DisplayCapabilities current_display;

static void bdrm_recompute_color_lut(void) {
    int warmth = current_display.night_shift_enabled ? current_display.night_shift_warmth : 0;
    // Warmth reduces blue channel up to 40% and green channel up to 12% for Kelvin shift
    int blue_scale  = 256 - (warmth * 102) / 100;
    int green_scale = 256 - (warmth * 30) / 100;

    for (int i = 0; i < 256; i++) {
        int r = i;
        int g = (i * green_scale) >> 8;
        int b = (i * blue_scale) >> 8;
        if (current_display.hdr_enabled) {
            // Apply PQ/HDR tone boost to upper highlights
            r = (r * 268) >> 8;
            if (r > 255) r = 255;
        }
        current_display.gamma_lut_r[i] = (uint8_t)r;
        current_display.gamma_lut_g[i] = (uint8_t)(g > 255 ? 255 : g);
        current_display.gamma_lut_b[i] = (uint8_t)(b > 255 ? 255 : b);
    }
}

extern "C" {
    void bdrm_init(void) {
        current_display.initialized = true;
        current_display.hdr_enabled = false;
        current_display.freesync_enabled = false;
        current_display.gsync_enabled = false;
        current_display.max_luminance_nits = 400;
        current_display.night_shift_enabled = false;
        current_display.night_shift_warmth = 35;
        current_display.frame_count = 0;
        current_display.gpu_vendor_id = 0;
        current_display.gpu_device_id = 0;
        current_display.vram_bytes = svga_get_vram_size();

        const char* default_icc = "Display P3 - BlankOS Quartz Reference";
        int k = 0;
        while (default_icc[k] != '\0' && k < 63) {
            current_display.icc_profile_name[k] = default_icc[k];
            k++;
        }
        current_display.icc_profile_name[k] = '\0';

        // Probe real PCI display controller
        int count = pci_get_device_count();
        for (int i = 0; i < count; i++) {
            uint16_t vendor = 0, device = 0;
            uint8_t cls = 0, sub = 0;
            if (pci_get_device_info(i, NULL, NULL, NULL, &vendor, &device, &cls, &sub, NULL, NULL, NULL, NULL)) {
                if (cls == 0x03) {
                    current_display.gpu_vendor_id = vendor;
                    current_display.gpu_device_id = device;
                    if (vendor == 0x10DE) {
                        current_display.gsync_enabled = true;
                    } else if (vendor == 0x1022 || vendor == 0x1002 || vendor == 0x8086) {
                        current_display.freesync_enabled = true;
                    }
                    break;
                }
            }
        }

        bdrm_recompute_color_lut();
    }

    void bdrm_load_icc_profile(char* profile_buffer) {
        if (!profile_buffer) return;
        int i = 0;
        while (profile_buffer[i] != '\0' && i < 63) {
            current_display.icc_profile_name[i] = profile_buffer[i];
            i++;
        }
        current_display.icc_profile_name[i] = '\0';
        bdrm_recompute_color_lut();
    }

    void bdrm_enable_hdr(void) {
        current_display.hdr_enabled = true;
        current_display.max_luminance_nits = 1000;
        bdrm_recompute_color_lut();
    }

    void bdrm_enable_vrr(bool prefer_gsync) {
        if (prefer_gsync) {
            current_display.gsync_enabled = true;
            current_display.freesync_enabled = false;
        } else {
            current_display.freesync_enabled = true;
            current_display.gsync_enabled = false;
        }
    }

    void bdrm_present_frame(void) {
        current_display.frame_count++;
    }

    uint64_t bdrm_get_frame_count(void) {
        return current_display.frame_count;
    }

    bool bdrm_is_hdr_enabled(void) {
        return current_display.hdr_enabled;
    }

    void bdrm_set_night_shift(bool enabled, int warmth_percent) {
        current_display.night_shift_enabled = enabled;
        if (warmth_percent < 0) warmth_percent = 0;
        if (warmth_percent > 100) warmth_percent = 100;
        current_display.night_shift_warmth = warmth_percent;
        bdrm_recompute_color_lut();
    }

    bool bdrm_get_night_shift(void) {
        return current_display.night_shift_enabled;
    }
}
