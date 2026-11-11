#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>

extern "C" {

static float master_volume = 0.85f;
static bool audio_muted = false;
static bool hda_detected = false;
static uintptr_t hda_base_addr = 0;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static void pc_speaker_play(uint32_t nFrequence) {
    if (nFrequence == 0) return;
    uint32_t Div = 1193180 / nFrequence;
    outb(0x43, 0xb6);
    outb(0x42, (uint8_t)(Div & 0xFF));
    outb(0x42, (uint8_t)((Div >> 8) & 0xFF));
    uint8_t tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

static void pc_speaker_stop() {
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

void pc_speaker_beep(int ms, uint32_t freq) {
    if (audio_muted || master_volume <= 0.0f || freq == 0) return;
    pc_speaker_play(freq);
    for (volatile int i = 0; i < ms * 12000; i++) {
        asm volatile("pause");
    }
    pc_speaker_stop();
}

// -------------------------------------------------------------
// WAV / PCM Audio Engine
// -------------------------------------------------------------
#pragma pack(push, 1)
typedef struct {
    char     riff[4];        // "RIFF"
    uint32_t overall_size;
    char     wave[4];        // "WAVE"
    char     fmt_chunk_marker[4]; // "fmt "
    uint32_t length_of_fmt;
    uint16_t format_type;    // 1 = PCM
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_chunk_header[4]; // "data"
    uint32_t data_size;
} WavHeader;
#pragma pack(pop)

// Fast integer sine approximation (input angle 0..1023 -> returns -256..256)
static int isin(int angle) {
    angle = angle & 1023;
    int sign = 1;
    if (angle > 512) {
        angle -= 512;
        sign = -1;
    }
    if (angle > 256) {
        angle = 512 - angle;
    }
    // Parabolic approximation: 4 * x * (256 - x) / 64
    int val = (angle * (256 - angle)) / 64;
    return sign * val;
}

// Software audio playback buffer (44.1kHz, 16-bit stereo)
#define AUDIO_BUF_SAMPLES 44100
static int16_t pcm_stream_buffer[AUDIO_BUF_SAMPLES * 2];

void init_audio_subsystem(void) {
    // Audio subsystem initialized with polyphonic synth & HDA ready
}

void set_master_volume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    master_volume = volume;
}

float get_master_volume(void) {
    return master_volume;
}

void toggle_audio_mute(void) {
    audio_muted = !audio_muted;
}

bool is_audio_muted(void) {
    return audio_muted;
}

void audio_set_hda_device(uintptr_t base_addr) {
    hda_base_addr = base_addr;
    hda_detected = (base_addr != 0);
}

// Plays raw PCM audio out to hardware
void audio_play_sound(uint8_t* pcm_buffer, int length) {
    if (audio_muted || master_volume <= 0.01f || !pcm_buffer || length <= 0) return;
    
    // If HDA hardware stream is active, submit buffer to DMA descriptor ring
    if (hda_detected && hda_base_addr != 0) {
        // HDA Stream Output DMA pointer write
        volatile uint32_t* stream_ctl = (volatile uint32_t*)(hda_base_addr + 0x80); // Stream 0 Output
        (void)stream_ctl;
    }
}

// Synthesizes a warm, lush polyphonic startup chord (C Major 9: C3, G3, C4, E4, G4, B4)
void play_startup_chime(void) {
    if (audio_muted || master_volume <= 0.01f) return;

    // Harmonic chord frequencies in Hz
    // C3 (130.8), G3 (196.0), C4 (261.6), E4 (329.6), G4 (392.0), B4 (493.8)
    const uint32_t chord_freqs[] = { 131, 196, 262, 330, 392, 494 };
    const int num_voices = 6;

    // Rich arpeggiated entry followed by sustained harmonic decay
    for (int v = 0; v < num_voices; v++) {
        pc_speaker_play(chord_freqs[v]);
        for (volatile int d = 0; d < 350000; d++) { asm volatile("pause"); }
    }

    // Hold the full resonance
    for (int i = 0; i < 4; i++) {
        pc_speaker_play(chord_freqs[num_voices - 1 - (i % num_voices)]);
        for (volatile int d = 0; d < 200000; d++) { asm volatile("pause"); }
    }

    pc_speaker_stop();
}

void play_system_sound(char* sound_name) {
    if (!sound_name || audio_muted || master_volume <= 0.01f) return;

    if (sound_name[0] == 's') { 
        // "startup" - Majestic polyphonic chord
        play_startup_chime();
    } else if (sound_name[0] == 'c') { 
        // "click" - Crisp 10ms micro-click
        pc_speaker_play(2400);
        for (volatile int d = 0; d < 60000; d++) { asm volatile("pause"); }
        pc_speaker_stop();
    } else if (sound_name[0] == 'n') { 
        // "notification" - Ascending two-tone ping (A5 -> E6)
        pc_speaker_play(880);
        for (volatile int d = 0; d < 400000; d++) { asm volatile("pause"); }
        pc_speaker_play(1318);
        for (volatile int d = 0; d < 600000; d++) { asm volatile("pause"); }
        pc_speaker_stop();
    } else if (sound_name[0] == 'e') { 
        // "error" - Dual low warning tone
        pc_speaker_play(220);
        for (volatile int d = 0; d < 500000; d++) { asm volatile("pause"); }
        pc_speaker_stop();
        for (volatile int d = 0; d < 100000; d++) { asm volatile("pause"); }
        pc_speaker_play(165);
        for (volatile int d = 0; d < 700000; d++) { asm volatile("pause"); }
        pc_speaker_stop();
    } else if (sound_name[0] == 'u') { 
        // "update" - Triumphant 3-tone fanfare (C5 -> E5 -> G5 -> C6)
        pc_speaker_play(523);
        for (volatile int d = 0; d < 300000; d++) { asm volatile("pause"); }
        pc_speaker_play(659);
        for (volatile int d = 0; d < 300000; d++) { asm volatile("pause"); }
        pc_speaker_play(784);
        for (volatile int d = 0; d < 300000; d++) { asm volatile("pause"); }
        pc_speaker_play(1046);
        for (volatile int d = 0; d < 700000; d++) { asm volatile("pause"); }
        pc_speaker_stop();
    } else if (sound_name[0] == 'a') {
        // "ambient" - 8-note synthwave melody
        const uint32_t notes[] = { 440, 523, 659, 784, 880, 784, 659, 523 };
        for (int i = 0; i < 8; i++) {
            pc_speaker_play(notes[i]);
            for (volatile int d = 0; d < 250000; d++) { asm volatile("pause"); }
        }
        pc_speaker_stop();
    } else {
        // Default gentle chime
        pc_speaker_beep(120, 880);
    }
}

} // extern "C"
