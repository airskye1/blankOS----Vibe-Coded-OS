#include <stdint.h>
#include <stdbool.h>

static float master_volume = 0.75f;

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static void pc_speaker_play(uint32_t nFrequence) {
    uint32_t Div;
    uint8_t tmp;
    Div = 1193180 / nFrequence;
    outb(0x43, 0xb6);
    outb(0x42, (uint8_t) (Div) );
    outb(0x42, (uint8_t) (Div >> 8));
    tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

static void pc_speaker_stop() {
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

void pc_speaker_beep(int ms, uint32_t freq) {
    if (master_volume <= 0.0f) return;
    pc_speaker_play(freq);
    for (volatile int i = 0; i < ms * 10000; i++) {
        asm volatile("pause");
    }
    pc_speaker_stop();
}

void init_audio_subsystem(void) {
    // Initializing Programmable Interval Timer (PIT) for PC Speaker Audio
}

void set_master_volume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    master_volume = volume;
}

float get_master_volume(void) {
    return master_volume;
}

void audio_play_sound(uint8_t* pcm_buffer, int length) {
    // HDA PCM playback stub
}

void play_system_sound(char* sound_name) {
    if (sound_name[0] == 's') { // startup
        pc_speaker_beep(200, 523); // C5
        pc_speaker_beep(200, 659); // E5
        pc_speaker_beep(400, 784); // G5
    } else if (sound_name[0] == 'e') { // error
        pc_speaker_beep(150, 150);
        pc_speaker_beep(250, 100);
    } else if (sound_name[0] == 'u') { // update
        pc_speaker_beep(100, 880);
        pc_speaker_beep(100, 1046);
    } else { // notification
        pc_speaker_beep(150, 880);
    }
}
