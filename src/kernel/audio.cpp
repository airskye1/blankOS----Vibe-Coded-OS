#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include <stddef.h>

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

static int master_volume = 75;
static bool audio_muted = false;
static char audio_dev_name[64] = "PIT Precision Chime Synth";
static uint64_t hda_mmio_base = 0;

// Non-blocking multi-note sequencer state (~16ms per audio_tick)
#define MAX_SEQ_NOTES 8
typedef struct {
    uint32_t freq;
    int ticks_left; // 1 tick ~= 16ms
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
    int scale = master_volume;
    if (scale < 25) scale = 25;
    int b0 = (freq < 350) ? 92 : (freq < 520 ? 74 : 46);
    int b1 = (freq >= 350 && freq < 620) ? 95 : 64;
    int b2 = (freq >= 520 && freq < 760) ? 96 : (freq >= 760 ? 78 : 48);
    int b3 = (freq >= 660) ? 98 : 44;
    int vals[4] = {
        (b0 * scale) / 100,
        (b1 * scale) / 100,
        (b2 * scale) / 100,
        (b3 * scale) / 100
    };
    for (int i = 0; i < 4; i++) {
        if (vals[i] > 100) vals[i] = 100;
        if (vals[i] > g_eq_bands[i]) g_eq_bands[i] = vals[i];
    }
}

static void speaker_set_freq(uint32_t freq) {
    if (audio_muted || master_volume <= 0 || freq == 0) {
        uint8_t tmp = inb(0x61);
        outb(0x61, tmp & 0xFC);
        g_active_freq = 0;
        return;
    }
    audio_pulse_eq(freq);
    if (freq == g_active_freq) return;
    uint32_t div = 1193180 / freq;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) outb(0x61, tmp | 3);
    g_active_freq = freq;
}

static void speaker_silence(void) {
    uint8_t tmp = inb(0x61);
    if (tmp & 3) outb(0x61, tmp & 0xFC);
    g_active_freq = 0;
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
    for (int i = 0; i < g_seq_count; i++) {
        g_seq[i] = notes[i];
    }
    g_seq_idx = 0;
    if (g_seq_count > 0) {
        speaker_set_freq(g_seq[0].freq);
    }
}

extern "C" {

void init_audio_driver(EFI_SYSTEM_TABLE* st) {
    (void)st;
    hda_mmio_base = 0;
    copy_str_local(audio_dev_name, "PIT Precision Chime Synth", 64);

    // Probe PCI Bus 0 for Intel HDA (Class 0x04, Subclass 0x03)
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read_dword(0, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cls = pci_read_dword(0, dev, 0, 0x08);
        uint8_t base_cls = (uint8_t)((cls >> 24) & 0xFF);
        uint8_t sub_cls  = (uint8_t)((cls >> 16) & 0xFF);
        if (base_cls == 0x04 && sub_cls == 0x03) {
            uint32_t bar0 = pci_read_dword(0, dev, 0, 0x10);
            uint32_t bar1 = pci_read_dword(0, dev, 0, 0x14);
            hda_mmio_base = ((uint64_t)bar1 << 32) | (bar0 & ~0xFULL);
            copy_str_local(audio_dev_name, "Intel HD Audio + PIT Chime", 64);
            break;
        }
    }
    speaker_silence();
}

void audio_tick(void) {
    g_audio_tick_counter++;

    // Smoothly decay or animate equalizer bands
    for (int i = 0; i < 4; i++) {
        if (g_seq_idx < g_seq_count && !audio_muted && master_volume > 0 && g_active_freq > 0) {
            int jitter = (int)((g_audio_tick_counter * (uint32_t)(i + 3)) % 14) - 6;
            int target = g_eq_bands[i] + jitter;
            if (target < 18) target = 18;
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
    int ticks = (int)(duration_ms / 16);
    if (ticks < 1) ticks = 1;
    SeqNote n[1] = { { freq, ticks } };
    enqueue_sequence(n, 1);
}

void play_system_sound(const char* name) {
    if (!name || audio_muted || master_volume <= 0) return;

    uint32_t elapsed = g_audio_tick_counter - g_last_cue_tick;
    if (str_eq_local(name, g_last_cue_name)) {
        if ((str_eq_local(name, "hover") || str_eq_local(name, "tab")) && elapsed < 14) return;
        if ((str_eq_local(name, "tool") || str_eq_local(name, "click")) && elapsed < 3) return;
        if (elapsed < 10) return;
    }
    copy_str_local(g_last_cue_name, name, 24);
    g_last_cue_tick = g_audio_tick_counter;

    if (str_eq_local(name, "startup") || str_eq_local(name, "reward")) {
        // Lira reward / startup triad + octave (~64ms stagger)
        SeqNote notes[4] = { { 440, 4 }, { 554, 4 }, { 659, 4 }, { 880, 8 } };
        enqueue_sequence(notes, 4);
    } else if (str_eq_local(name, "send") || str_eq_local(name, "open")) {
        // Lira send / window open rising two-tone
        SeqNote notes[2] = { { 480, 4 }, { 620, 6 } };
        enqueue_sequence(notes, 2);
    } else if (str_eq_local(name, "reply") || str_eq_local(name, "notify")) {
        // Lira reply / notification bright fifth-ish interval
        SeqNote notes[2] = { { 620, 4 }, { 820, 7 } };
        enqueue_sequence(notes, 2);
    } else if (str_eq_local(name, "tool") || str_eq_local(name, "click")) {
        // Lira crisp low-pitched tactile detent thock (never piercing)
        SeqNote notes[1] = { { 420, 1 } };
        enqueue_sequence(notes, 1);
    } else if (str_eq_local(name, "hover") || str_eq_local(name, "tab")) {
        // Subtle Lira UI hover / tab switch pip
        SeqNote notes[1] = { { 520, 1 } };
        enqueue_sequence(notes, 1);
    } else if (str_eq_local(name, "success") || str_eq_local(name, "download")) {
        // Brisk Lira harmonic completion flourish
        SeqNote notes[4] = { { 440, 3 }, { 554, 3 }, { 659, 3 }, { 880, 7 } };
        enqueue_sequence(notes, 4);
    } else if (str_eq_local(name, "error")) {
        // Warm muted descending bass cue
        SeqNote notes[2] = { { 220, 4 }, { 180, 8 } };
        enqueue_sequence(notes, 2);
    } else if (str_eq_local(name, "close") || str_eq_local(name, "minimize") || str_eq_local(name, "lock")) {
        // Symmetric Lira exit cadence paired with open/send
        SeqNote notes[2] = { { 620, 3 }, { 480, 5 } };
        enqueue_sequence(notes, 2);
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
    hda_mmio_base = (uint64_t)bar0;
    if (hda_mmio_base != 0) {
        copy_str_local(audio_dev_name, "Intel HD Audio + PIT Chime", 64);
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
    return (!audio_muted && master_volume > 0 && (g_seq_idx < g_seq_count || g_eq_bands[0] > 4 || g_eq_bands[2] > 4));
}

} // extern "C"
