/*
 * Audio: background music (looped) and flip sound.
 * Uses aplay/sox; requires APLAY_DEVICE, SOUND_FLIP, MUSIC_FILE env.
 */
#pragma once

#include "types.h"

/* Start background music (loops); no-op if music_path or aplay_device empty. */
void audio_start_music(const char *music_path, const char *aplay_device);

/* Stop background music. */
void audio_stop_music(void);

/* Play flip sound (non-blocking). */
void audio_play_flip(const char *flip_path, const char *aplay_device);

/* Return 1 if flip should play: new bus on board, or bus just arrived (mins->0). */
int audio_should_play_flip(const Arrival *curr, int n_curr,
                           const Arrival *prev, int n_prev);
