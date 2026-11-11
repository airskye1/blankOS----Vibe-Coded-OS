#ifndef BLANKOS_AUDIO_H
#define BLANKOS_AUDIO_H

#include <efi.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Non-blocking Horizon Audio Subsystem (HDA Probe + PIT Chime Sequencer)
void init_audio_driver(EFI_SYSTEM_TABLE* st);
void audio_tick(void);                  // Advances multi-note chimes non-blockingly each frame
void play_system_sound(const char* name); // "startup", "click", "notify", "success", "error", "download"
void play_beep(uint32_t freq, uint32_t duration_ms);

void set_master_volume(int vol);
int get_master_volume(void);
void toggle_audio_mute(void);
bool is_audio_muted(void);
const char* get_audio_device_name(void);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_AUDIO_H
