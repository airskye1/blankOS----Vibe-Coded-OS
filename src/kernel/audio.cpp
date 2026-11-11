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

static void speaker_set_freq(uint32_t freq) {
    if (audio_muted || master_volume <= 0 || freq == 0) {
        uint8_t tmp = inb(0x61);
        outb(0x61, tmp & 0xFC);
        g_active_freq = 0;
        return;
    }
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

    if (str_eq_local(name, "startup")) {
        // Warm Horizon 4-note ascending chime
        SeqNote notes[4] = { { 440, 4 }, { 554, 4 }, { 659, 4 }, { 880, 9 } };
        enqueue_sequence(notes, 4);
    } else if (str_eq_local(name, "click")) {
        SeqNote notes[1] = { { 1480, 1 } };
        enqueue_sequence(notes, 1);
    } else if (str_eq_local(name, "notify")) {
        SeqNote notes[2] = { { 659, 3 }, { 987, 5 } };
        enqueue_sequence(notes, 2);
    } else if (str_eq_local(name, "success") || str_eq_local(name, "download")) {
        SeqNote notes[3] = { { 523, 3 }, { 659, 3 }, { 1046, 6 } };
        enqueue_sequence(notes, 3);
    } else if (str_eq_local(name, "error")) {
        SeqNote notes[2] = { { 293, 4 }, { 220, 7 } };
        enqueue_sequence(notes, 2);
    } else if (str_eq_local(name, "lock")) {
        SeqNote notes[2] = { { 880, 2 }, { 587, 4 } };
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
    if (audio_muted) speaker_silence();
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

} // extern "C"
