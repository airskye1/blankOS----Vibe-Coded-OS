#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "audio.h"

extern "C" {
    extern uint32_t pci_read_dword(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static EFI_SYSTEM_TABLE* g_audio_st = NULL;
static int master_volume = 78;
static bool audio_muted = false;
static char audio_dev_name[64] = "Lira Acoustic PIT Synth";
static uint64_t hda_mmio_base = 0;

// ============================================================================
// Intel HD Audio (HDA 1.0a) 48kHz 16-bit Stereo Polyphonic PCM Synth
// ============================================================================
#define HDA_REG_GCAP      0x00
#define HDA_REG_GCTL      0x08
#define HDA_REG_STATESTS  0x0E
#define HDA_REG_ICW       0x60
#define HDA_REG_IRR       0x64
#define HDA_REG_IRS       0x68

#define HDA_SD_CTL        0x00
#define HDA_SD_STS        0x03
#define HDA_SD_LPIB       0x04
#define HDA_SD_CBL        0x08
#define HDA_SD_LVI        0x0C
#define HDA_SD_FMT        0x12
#define HDA_SD_BDPL       0x18
#define HDA_SD_BDPU       0x1C

#define HDA_RING_FRAMES   4096
#define HDA_RING_BYTES    (HDA_RING_FRAMES * 4) // 16384 bytes (stereo 16-bit @ 48kHz = ~85.3ms)
#define HDA_CHUNK_FRAMES  2048
#define MAX_HDA_VOICES    8

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
} __attribute__((packed)) HdaBdlEntry;

typedef struct {
    bool     active;
    uint32_t phase;      // 32-bit phase accumulator
    uint32_t phase_inc;  // freq * (2^32 / 48000)
    uint32_t env_q15;    // 0..32767 exponential decay amplitude
    uint32_t decay_q15;  // per-32-sample decay multiplier (e.g. 32600..32760)
    int32_t  harm2_q15;  // second/third harmonic warmth mix
    uint32_t delay_samples; // stagger start for chords/arpeggios
} HdaVoice;

static bool        g_hda_pcm_ready = false;
static uint32_t    g_hda_sd_off = 0;
static uint8_t*    g_hda_dma_virt = NULL;
static HdaBdlEntry* g_hda_bdl = NULL;
static int16_t*    g_hda_ring = NULL;
static uint32_t    g_hda_write_frame = 0;
static HdaVoice    g_voices[MAX_HDA_VOICES];

static inline volatile uint8_t* hda_ptr(uint32_t off) {
    return (volatile uint8_t*)(uintptr_t)(hda_mmio_base + (uint64_t)off);
}
static inline uint32_t hda_r32(uint32_t off) { return *(volatile uint32_t*)hda_ptr(off); }
static inline uint16_t hda_r16(uint32_t off) { return *(volatile uint16_t*)hda_ptr(off); }
static inline uint8_t  hda_r8(uint32_t off)  { return *(volatile uint8_t*)hda_ptr(off); }
static inline void hda_w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)hda_ptr(off) = v; }
static inline void hda_w16(uint32_t off, uint16_t v) { *(volatile uint16_t*)hda_ptr(off) = v; }
static inline void hda_w8(uint32_t off, uint8_t v)   { *(volatile uint8_t*)hda_ptr(off) = v; }

static void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t addr = 0x80000000U | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | 0x04U;
    outl(0xCF8, addr);
    uint32_t cmd = inl(0xCFC);
    cmd |= 0x06U; // Memory Space + Bus Master Enable
    outl(0xCF8, addr);
    outl(0xCFC, cmd);
}

static bool hda_send_verb(uint8_t cad, uint8_t nid, uint32_t verb20, uint32_t* out_resp) {
    for (int t = 0; t < 4000; t++) {
        if ((hda_r16(HDA_REG_IRS) & 0x01U) == 0) break;
    }
    hda_w16(HDA_REG_IRS, 0x02U); // Clear IRV sticky bit
    uint32_t pkt = ((uint32_t)(cad & 0x0FU) << 28) | ((uint32_t)(nid & 0xFFU) << 20) | (verb20 & 0xFFFFFU);
    hda_w32(HDA_REG_ICW, pkt);
    hda_w16(HDA_REG_IRS, 0x01U); // Trigger ICB
    for (int t = 0; t < 6000; t++) {
        uint16_t st = hda_r16(HDA_REG_IRS);
        if ((st & 0x01U) == 0 && (st & 0x02U) != 0) {
            if (out_resp) *out_resp = hda_r32(HDA_REG_IRR);
            return true;
        }
    }
    return false;
}

// Fast quarter-wave parabolic sine approximation returning Q15 (-32767..32767)
static inline int32_t synth_sin_q15(uint32_t phase) {
    uint32_t q = phase >> 30;
    int32_t x = (int32_t)((phase >> 15) & 0x7FFFU); // 0..32767 in quadrant
    if (q & 1U) x = 32767 - x;
    // y ~ x * (65536 - x) / 32768 scaled smoothly -> smooth warm sine
    int32_t t = (x * (65535 - x)) >> 15;
    // Slight Chebyshev cubic blend to match pure sine within <1%
    int32_t s = (t * 4 + ((t * t) >> 15)) / 5;
    if (s > 32767) s = 32767;
    return (q >= 2U) ? -s : s;
}

static void hda_spawn_voice(uint32_t freq, uint32_t env_q15, uint32_t decay_q15, int32_t harm2_q15, uint32_t delay_ms) {
    if (!g_hda_pcm_ready || freq == 0) return;
    int slot = 0;
    uint32_t min_env = 0xFFFFFFFFU;
    for (int i = 0; i < MAX_HDA_VOICES; i++) {
        if (!g_voices[i].active) { slot = i; break; }
        if (g_voices[i].env_q15 < min_env) {
            min_env = g_voices[i].env_q15;
            slot = i;
        }
    }
    g_voices[slot].active = true;
    g_voices[slot].phase = 0;
    g_voices[slot].phase_inc = (uint32_t)(((uint64_t)freq * 4294967296ULL) / 48000ULL);
    g_voices[slot].env_q15 = env_q15;
    g_voices[slot].decay_q15 = decay_q15;
    g_voices[slot].harm2_q15 = harm2_q15;
    g_voices[slot].delay_samples = delay_ms * 48U;
}

static void hda_render_chunk(uint32_t frames_to_render) {
    if (!g_hda_pcm_ready || !g_hda_ring || frames_to_render == 0) return;
    int vol = (audio_muted || master_volume <= 0) ? 0 : master_volume;

    for (uint32_t f = 0; f < frames_to_render; f++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;
        bool update_env = ((f & 31U) == 0U);

        for (int v = 0; v < MAX_HDA_VOICES; v++) {
            HdaVoice* vc = &g_voices[v];
            if (!vc->active) continue;
            if (vc->delay_samples > 0) {
                vc->delay_samples--;
                continue;
            }
            vc->phase += vc->phase_inc;
            int32_t s1 = synth_sin_q15(vc->phase);
            int32_t s2 = (vc->harm2_q15 > 0) ? synth_sin_q15(vc->phase * 2U) : 0;
            int32_t samp = s1 + ((s2 * vc->harm2_q15) >> 15);
            samp = (samp * (int32_t)vc->env_q15) >> 16;

            // Gentle odd/even stereo pan spread across voice index
            int32_t pan_l = (v & 1) ? 14 : 18;
            int32_t pan_r = (v & 1) ? 18 : 14;
            mix_l += (samp * pan_l) >> 4;
            mix_r += (samp * pan_r) >> 4;

            if (update_env) {
                vc->env_q15 = (vc->env_q15 * vc->decay_q15) >> 15;
                if (vc->env_q15 < 45) vc->active = false;
            }
        }

        mix_l = (mix_l * vol) / 100;
        mix_r = (mix_r * vol) / 100;
        if (mix_l > 32767) mix_l = 32767;
        if (mix_l < -32767) mix_l = -32767;
        if (mix_r > 32767) mix_r = 32767;
        if (mix_r < -32767) mix_r = -32767;

        uint32_t pos = (g_hda_write_frame & (HDA_RING_FRAMES - 1U)) * 2U;
        g_hda_ring[pos + 0] = (int16_t)mix_l;
        g_hda_ring[pos + 1] = (int16_t)mix_r;
        g_hda_write_frame++;
    }
}

static void hda_pump_audio(void) {
    if (!g_hda_pcm_ready || !g_hda_ring) return;
    uint32_t lpib_bytes = hda_r32(g_hda_sd_off + HDA_SD_LPIB);
    uint32_t play_frame = (lpib_bytes >> 2) & (HDA_RING_FRAMES - 1U);
    uint32_t ahead = (g_hda_write_frame - play_frame) & (HDA_RING_FRAMES - 1U);
    const uint32_t target_ahead = 1280U; // ~26.6 ms lookahead
    if (ahead < target_ahead) {
        uint32_t need = target_ahead - ahead;
        if (need > 2048U) need = 2048U;
        hda_render_chunk(need);
    }
}

static bool hda_try_init_pcm(EFI_SYSTEM_TABLE* st) {
    if (hda_mmio_base == 0 || !st || !st->BootServices) return false;

    // Reset HDA Controller
    hda_w32(HDA_REG_GCTL, 0x00U);
    for (int t = 0; t < 2000; t++) {
        if ((hda_r32(HDA_REG_GCTL) & 0x01U) == 0) break;
        st->BootServices->Stall(10);
    }
    hda_w32(HDA_REG_GCTL, 0x01U);
    for (int t = 0; t < 5000; t++) {
        if ((hda_r32(HDA_REG_GCTL) & 0x01U) != 0) break;
        st->BootServices->Stall(10);
    }
    if ((hda_r32(HDA_REG_GCTL) & 0x01U) == 0) return false;

    st->BootServices->Stall(1500);
    uint16_t states = hda_r16(HDA_REG_STATESTS);
    if (states == 0) return false;

    uint8_t cad = 0;
    while (cad < 15 && ((states >> cad) & 1U) == 0) cad++;

    uint16_t gcap = hda_r16(HDA_REG_GCAP);
    uint32_t iss = (gcap >> 8) & 0x0FU;
    uint32_t oss = (gcap >> 12) & 0x0FU;
    if (oss == 0) return false;
    g_hda_sd_off = 0x80U + iss * 0x20U;

    // Enumerate Codec Root -> AFG -> Audio Output + Pin Complex
    uint32_t root_sub = 0;
    if (!hda_send_verb(cad, 0, 0xF0004U, &root_sub)) return false;
    uint8_t fg_start = (uint8_t)((root_sub >> 16) & 0xFFU);
    uint8_t fg_count = (uint8_t)(root_sub & 0xFFU);

    uint8_t afg_nid = 0;
    for (uint8_t i = 0; i < fg_count; i++) {
        uint32_t fg_type = 0;
        if (hda_send_verb(cad, (uint8_t)(fg_start + i), 0xF0005U, &fg_type)) {
            if ((fg_type & 0xFFU) == 0x01U) { afg_nid = (uint8_t)(fg_start + i); break; }
        }
    }
    if (afg_nid == 0) return false;

    hda_send_verb(cad, afg_nid, 0x70500U, NULL); // D0 power state
    uint32_t wsub = 0;
    if (!hda_send_verb(cad, afg_nid, 0xF0004U, &wsub)) return false;
    uint8_t w_start = (uint8_t)((wsub >> 16) & 0xFFU);
    uint8_t w_count = (uint8_t)(wsub & 0xFFU);

    uint8_t dac_nid = 0;
    for (uint8_t i = 0; i < w_count; i++) {
        uint8_t nid = (uint8_t)(w_start + i);
        uint32_t wcap = 0;
        if (!hda_send_verb(cad, nid, 0xF0009U, &wcap)) continue;
        uint8_t wtype = (uint8_t)((wcap >> 20) & 0x0FU);
        hda_send_verb(cad, nid, 0x70500U, NULL);
        if (wtype == 0x0U && dac_nid == 0) {
            dac_nid = nid;
            hda_send_verb(cad, nid, 0x70610U, NULL); // Stream 1, Channel 0
            hda_send_verb(cad, nid, 0x20011U, NULL); // 48kHz, 16-bit, 2ch
            hda_send_verb(cad, nid, 0x3B07FU, NULL); // Output amp max unmuted
        } else if (wtype == 0x4U) {
            uint32_t pcap = 0;
            if (hda_send_verb(cad, nid, 0xF000CU, &pcap) && (pcap & 0x10U)) {
                hda_send_verb(cad, nid, 0x70740U, NULL); // Out Enable
                hda_send_verb(cad, nid, 0x70C02U, NULL); // EAPD Enable
                hda_send_verb(cad, nid, 0x3B07FU, NULL); // Out Amp unmuted
            }
        }
    }
    if (dac_nid == 0) return false;

    // Allocate 5 pages (16KB DMA ring + 4KB BDL)
    EFI_PHYSICAL_ADDRESS dma_phys = 0;
    if (EFI_ERROR(st->BootServices->AllocatePages(AllocateAnyPages, EfiBootServicesData, 5, &dma_phys)) || dma_phys == 0) {
        return false;
    }
    g_hda_dma_virt = (uint8_t*)(uintptr_t)dma_phys;
    for (uint32_t i = 0; i < 5U * 4096U; i++) g_hda_dma_virt[i] = 0;

    g_hda_ring = (int16_t*)g_hda_dma_virt;
    g_hda_bdl  = (HdaBdlEntry*)(g_hda_dma_virt + 16384U);
    uint64_t ring_phys = (uint64_t)dma_phys;

    g_hda_bdl[0].addr = ring_phys;
    g_hda_bdl[0].len  = HDA_CHUNK_FRAMES * 4U;
    g_hda_bdl[0].ioc  = 0;
    g_hda_bdl[1].addr = ring_phys + (uint64_t)(HDA_CHUNK_FRAMES * 4U);
    g_hda_bdl[1].len  = HDA_CHUNK_FRAMES * 4U;
    g_hda_bdl[1].ioc  = 0;

    // Reset Output Stream 0
    hda_w8(g_hda_sd_off + HDA_SD_CTL, 0x01U);
    for (int t = 0; t < 2000; t++) {
        if ((hda_r8(g_hda_sd_off + HDA_SD_CTL) & 0x01U) != 0) break;
    }
    hda_w8(g_hda_sd_off + HDA_SD_CTL, 0x00U);
    for (int t = 0; t < 2000; t++) {
        if ((hda_r8(g_hda_sd_off + HDA_SD_CTL) & 0x01U) == 0) break;
    }

    uint64_t bdl_phys = ring_phys + 16384ULL;
    hda_w32(g_hda_sd_off + HDA_SD_CBL, HDA_RING_BYTES);
    hda_w16(g_hda_sd_off + HDA_SD_LVI, 1);
    hda_w16(g_hda_sd_off + HDA_SD_FMT, 0x0011U); // 48kHz, 16-bit, Stereo
    hda_w32(g_hda_sd_off + HDA_SD_BDPL, (uint32_t)(bdl_phys & 0xFFFFFFFFULL));
    hda_w32(g_hda_sd_off + HDA_SD_BDPU, (uint32_t)(bdl_phys >> 32));
    hda_w32(g_hda_sd_off + HDA_SD_CTL, (1U << 20) | 0x02U); // Stream 1 + RUN

    g_hda_write_frame = 512U;
    g_hda_pcm_ready = true;
    return true;
}

// ============================================================================
// Acoustic PIT Precision Synth + Equalizer Telemetry
// ============================================================================
#define MAX_SEQ_NOTES 12
typedef struct {
    uint32_t freq;
    int ticks_left; // ~16ms per audio_tick; freq==0 is soft breath rest
} SeqNote;

static SeqNote g_seq[MAX_SEQ_NOTES];
static int g_seq_count = 0;
static int g_seq_idx = 0;
static uint32_t g_active_freq = 0;
static uint32_t g_audio_tick_counter = 0;
static uint32_t g_last_cue_tick = 0;
static char g_last_cue_name[24] = {0};
static int g_eq_bands[4] = {0, 0, 0, 0};

static void audio_pulse_eq(uint32_t freq) {
    if (freq == 0 || audio_muted || master_volume <= 0) return;
    int scale = (master_volume < 25) ? 25 : master_volume;
    int b0 = (freq < 350) ? 94 : (freq < 540 ? 72 : 42);
    int b1 = (freq >= 340 && freq < 640) ? 96 : 60;
    int b2 = (freq >= 520 && freq < 780) ? 95 : (freq >= 780 ? 74 : 46);
    int b3 = (freq >= 660) ? 98 : 44;
    int vals[4] = {
        (b0 * scale) / 100, (b1 * scale) / 100,
        (b2 * scale) / 100, (b3 * scale) / 100
    };
    for (int i = 0; i < 4; i++) {
        if (vals[i] > 100) vals[i] = 100;
        if (vals[i] > g_eq_bands[i]) g_eq_bands[i] = vals[i];
    }
}

static void speaker_set_freq(uint32_t freq) {
    if (audio_muted || master_volume <= 0 || freq == 0) {
        uint8_t tmp = inb(0x61);
        if (tmp & 3) outb(0x61, tmp & 0xFC);
        g_active_freq = 0;
        return;
    }
    audio_pulse_eq(freq);
    if (g_hda_pcm_ready) {
        // When HDA PCM is streaming, avoid loud PC-speaker buzz on top of stereo audio
        g_active_freq = freq;
        return;
    }
    if (freq == g_active_freq) return;
    uint32_t div = 1193180U / freq;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFFU));
    outb(0x42, (uint8_t)((div >> 8) & 0xFFU));
    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) outb(0x61, tmp | 3);
    g_active_freq = freq;
}

static void speaker_silence(void) {
    uint8_t tmp = inb(0x61);
    if (tmp & 3) outb(0x61, tmp & 0xFC);
    g_active_freq = 0;
}

static void pit_micro_detent(uint32_t f1, uint32_t f2, uint32_t us1, uint32_t us2) {
    if (g_hda_pcm_ready || audio_muted || master_volume <= 0 || !g_audio_st || !g_audio_st->BootServices) return;
    audio_pulse_eq(f1);
    uint32_t div1 = 1193180U / f1;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div1 & 0xFFU));
    outb(0x42, (uint8_t)((div1 >> 8) & 0xFFU));
    uint8_t tmp = inb(0x61);
    outb(0x61, tmp | 3);
    g_audio_st->BootServices->Stall(us1);

    if (f2 > 0 && us2 > 0) {
        uint32_t div2 = 1193180U / f2;
        outb(0x43, 0xB6);
        outb(0x42, (uint8_t)(div2 & 0xFFU));
        outb(0x42, (uint8_t)((div2 >> 8) & 0xFFU));
        g_audio_st->BootServices->Stall(us2);
    }
    speaker_silence();
}

static bool str_eq_local(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static void copy_str_local(char* dst, const char* src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void enqueue_sequence(const SeqNote* notes, int count) {
    g_seq_count = (count > MAX_SEQ_NOTES) ? MAX_SEQ_NOTES : count;
    for (int i = 0; i < g_seq_count; i++) g_seq[i] = notes[i];
    g_seq_idx = 0;
    if (g_seq_count > 0) speaker_set_freq(g_seq[0].freq);
}

extern "C" {

void init_audio_driver(EFI_SYSTEM_TABLE* st) {
    g_audio_st = st;
    hda_mmio_base = 0;
    g_hda_pcm_ready = false;
    copy_str_local(audio_dev_name, "Lira Acoustic PIT Synth", 64);

    // Probe PCI Bus 0..7 for Intel HDA (Class 0x04, Subclass 0x03)
    for (uint8_t bus = 0; bus < 4 && hda_mmio_base == 0; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read_dword(bus, dev, 0, 0x00);
            if ((id & 0xFFFFU) == 0xFFFFU) continue;
            uint32_t cls = pci_read_dword(bus, dev, 0, 0x08);
            uint8_t base_cls = (uint8_t)((cls >> 24) & 0xFFU);
            uint8_t sub_cls  = (uint8_t)((cls >> 16) & 0xFFU);
            if (base_cls == 0x04 && sub_cls == 0x03) {
                pci_enable_bus_master(bus, dev, 0);
                uint32_t bar0 = pci_read_dword(bus, dev, 0, 0x10);
                uint32_t bar1 = pci_read_dword(bus, dev, 0, 0x14);
                hda_mmio_base = ((uint64_t)bar1 << 32) | (bar0 & ~0xFULL);
                if (hda_try_init_pcm(st)) {
                    copy_str_local(audio_dev_name, "Intel HDA Stereo 48kHz PCM", 64);
                } else {
                    copy_str_local(audio_dev_name, "Intel HDA + Lira PIT Synth", 64);
                }
                break;
            }
        }
    }
    speaker_silence();
}

void audio_tick(void) {
    g_audio_tick_counter++;

    if (g_hda_pcm_ready) {
        hda_pump_audio();
    }

    bool any_voice = false;
    for (int i = 0; i < MAX_HDA_VOICES; i++) {
        if (g_voices[i].active) { any_voice = true; break; }
    }

    // Smoothly decay or animate equalizer bands
    for (int i = 0; i < 4; i++) {
        if ((g_seq_idx < g_seq_count || any_voice) && !audio_muted && master_volume > 0 && (g_active_freq > 0 || any_voice)) {
            int jitter = (int)((g_audio_tick_counter * (uint32_t)(i + 3)) % 14) - 6;
            int target = g_eq_bands[i] + jitter;
            if (target < 22) target = 22;
            if (target > 100) target = 100;
            g_eq_bands[i] = (g_eq_bands[i] * 3 + target) / 4;
        } else {
            g_eq_bands[i] = (g_eq_bands[i] * 3) / 4;
            if (g_eq_bands[i] < 2) g_eq_bands[i] = 0;
        }
    }

    if (g_seq_idx >= g_seq_count) {
        speaker_silence();
        return;
    }
    if (audio_muted || master_volume <= 0) {
        speaker_silence();
        g_seq_count = 0;
        return;
    }
    if (g_seq[g_seq_idx].ticks_left > 0) {
        g_seq[g_seq_idx].ticks_left--;
        if (g_seq[g_seq_idx].ticks_left == 0) {
            g_seq_idx++;
            if (g_seq_idx < g_seq_count) {
                speaker_set_freq(g_seq[g_seq_idx].freq);
            } else {
                speaker_silence();
                g_seq_count = 0;
            }
        }
    }
}

void play_beep(uint32_t freq, uint32_t duration_ms) {
    if (audio_muted || master_volume <= 0) return;
    if (g_hda_pcm_ready) {
        uint32_t dec = (duration_ms > 200) ? 32720U : 32450U;
        hda_spawn_voice(freq, 22000U, dec, 6000, 0);
        audio_pulse_eq(freq);
        return;
    }
    int ticks = (int)(duration_ms / 16);
    if (ticks < 1) ticks = 1;
    SeqNote n[1] = { { freq, ticks } };
    enqueue_sequence(n, 1);
}

void play_system_sound(const char* name) {
    if (!name || audio_muted || master_volume <= 0) return;

    uint32_t elapsed = g_audio_tick_counter - g_last_cue_tick;
    if (str_eq_local(name, g_last_cue_name)) {
        if ((str_eq_local(name, "hover") || str_eq_local(name, "tab")) && elapsed < 12) return;
        if ((str_eq_local(name, "tool") || str_eq_local(name, "click")) && elapsed < 3) return;
        if (elapsed < 8) return;
    }
    copy_str_local(g_last_cue_name, name, 24);
    g_last_cue_tick = g_audio_tick_counter;

    if (str_eq_local(name, "startup") || str_eq_local(name, "reward")) {
        // Warm A-major-9 Lira chord arpeggio (A4 -> C#5 -> E5 -> G#5 -> B5)
        if (g_hda_pcm_ready) {
            hda_spawn_voice(440, 18000, 32690, 5000, 0);
            hda_spawn_voice(554, 16500, 32695, 4500, 65);
            hda_spawn_voice(659, 16000, 32705, 4000, 130);
            hda_spawn_voice(831, 14000, 32710, 3500, 195);
            hda_spawn_voice(988, 12500, 32718, 3000, 260);
            audio_pulse_eq(659);
        } else {
            SeqNote notes[7] = { { 440, 3 }, { 0, 1 }, { 554, 3 }, { 0, 1 }, { 659, 3 }, { 831, 3 }, { 988, 6 } };
            enqueue_sequence(notes, 7);
        }
    } else if (str_eq_local(name, "send") || str_eq_local(name, "open")) {
        // Organic rising fifth + shimmer octave
        if (g_hda_pcm_ready) {
            hda_spawn_voice(523, 18000, 32540, 5200, 0);
            hda_spawn_voice(784, 15000, 32620, 3800, 48);
            audio_pulse_eq(784);
        } else {
            SeqNote notes[3] = { { 523, 2 }, { 0, 1 }, { 784, 4 } };
            enqueue_sequence(notes, 3);
        }
    } else if (str_eq_local(name, "reply") || str_eq_local(name, "notify")) {
        // Glass bell crystalline fourth-to-octave notification
        if (g_hda_pcm_ready) {
            hda_spawn_voice(659, 17500, 32620, 7500, 0);
            hda_spawn_voice(880, 15500, 32660, 4200, 72);
            audio_pulse_eq(880);
        } else {
            SeqNote notes[3] = { { 659, 2 }, { 0, 1 }, { 880, 4 } };
            enqueue_sequence(notes, 3);
        }
    } else if (str_eq_local(name, "tool") || str_eq_local(name, "click")) {
        // Tactile mechanical wood-switch detent (2.5ms, never harsh)
        if (g_hda_pcm_ready) {
            hda_spawn_voice(310, 21000, 30800, 8200, 0);
            hda_spawn_voice(620, 12000, 29500, 2000, 0);
            audio_pulse_eq(420);
        } else {
            pit_micro_detent(780, 310, 900, 1400);
        }
    } else if (str_eq_local(name, "hover") || str_eq_local(name, "tab")) {
        // Ultra-subtle water-droplet UI pip (1.5ms)
        if (g_hda_pcm_ready) {
            hda_spawn_voice(680, 11500, 30200, 3000, 0);
            audio_pulse_eq(680);
        } else {
            pit_micro_detent(660, 0, 1300, 0);
        }
    } else if (str_eq_local(name, "success") || str_eq_local(name, "download")) {
        // Bright Lydian completion triad (C5 -> E5 -> G5 -> C6)
        if (g_hda_pcm_ready) {
            hda_spawn_voice(523, 17000, 32630, 5000, 0);
            hda_spawn_voice(659, 16000, 32645, 4500, 52);
            hda_spawn_voice(784, 15500, 32660, 4000, 104);
            hda_spawn_voice(1046, 14000, 32680, 3000, 156);
            audio_pulse_eq(784);
        } else {
            SeqNote notes[5] = { { 523, 2 }, { 659, 2 }, { 0, 1 }, { 784, 2 }, { 1046, 5 } };
            enqueue_sequence(notes, 5);
        }
    } else if (str_eq_local(name, "error")) {
        // Soft damped descending double-thump
        if (g_hda_pcm_ready) {
            hda_spawn_voice(210, 20000, 32400, 4500, 0);
            hda_spawn_voice(155, 19000, 32480, 3500, 85);
            audio_pulse_eq(210);
        } else {
            SeqNote notes[3] = { { 220, 3 }, { 0, 1 }, { 165, 5 } };
            enqueue_sequence(notes, 3);
        }
    } else if (str_eq_local(name, "close") || str_eq_local(name, "minimize") || str_eq_local(name, "lock")) {
        // Gentle descending fifth closure
        if (g_hda_pcm_ready) {
            hda_spawn_voice(659, 15500, 32520, 4200, 0);
            hda_spawn_voice(440, 14500, 32580, 3600, 50);
            audio_pulse_eq(440);
        } else {
            SeqNote notes[3] = { { 659, 2 }, { 0, 1 }, { 440, 4 } };
            enqueue_sequence(notes, 3);
        }
    }
}

void set_master_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    master_volume = vol;
    if (master_volume == 0) speaker_silence();
}

int get_master_volume(void) {
    return master_volume;
}

void toggle_audio_mute(void) {
    audio_muted = !audio_muted;
    if (audio_muted) {
        speaker_silence();
        for (int i = 0; i < 4; i++) g_eq_bands[i] = 0;
    } else {
        play_system_sound((char*)"tool");
    }
}

void audio_set_hda_device(uintptr_t bar0) {
    if (hda_mmio_base == 0 && bar0 != 0) {
        hda_mmio_base = (uint64_t)bar0;
        if (hda_try_init_pcm(g_audio_st)) {
            copy_str_local(audio_dev_name, "Intel HDA Stereo 48kHz PCM", 64);
        } else {
            copy_str_local(audio_dev_name, "Intel HDA + Lira PIT Synth", 64);
        }
    }
}

bool is_audio_muted(void) {
    return audio_muted;
}

const char* get_audio_device_name(void) {
    return audio_dev_name;
}

void audio_get_eq_bands(int* out_bands, int count) {
    if (!out_bands || count <= 0) return;
    int n = (count > 4) ? 4 : count;
    for (int i = 0; i < n; i++) {
        out_bands[i] = (audio_muted || master_volume <= 0) ? 0 : g_eq_bands[i];
    }
    for (int i = n; i < count; i++) out_bands[i] = 0;
}

bool audio_is_playing(void) {
    bool any_voice = false;
    for (int i = 0; i < MAX_HDA_VOICES; i++) {
        if (g_voices[i].active) { any_voice = true; break; }
    }
    return (!audio_muted && master_volume > 0 && (g_seq_idx < g_seq_count || any_voice || g_eq_bands[0] > 4 || g_eq_bands[2] > 4));
}

} // extern "C"
