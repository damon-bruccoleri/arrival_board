/*
 * Audio: PipeWire/PulseAudio via paplay, or ALSA via aplay when APLAY_DEVICE is set.
 * When aplay_device is NULL, uses paplay so music and flip mix. Otherwise aplay to raw device.
 */
#pragma once

#include "types.h"

/* Return WAV duration in seconds (from file header), or -1 on error. */
int audio_wav_duration_seconds(const char *path);

/* Start background music (loops). aplay_device NULL = use paplay; else aplay -D aplay_device.
 * Ferry (music_loop2) on Pulse: mixed in at end of each loop; on ALSA: played after each loop. */
void audio_start_music(const char *music_path, const char *music_loop2, const char *aplay_device);

/* Stop background music. */
void audio_stop_music(void);

/* Play flip. With NULL device uses Pulse (mixes); with device uses ALSA (caller stops music first). */
void audio_play_flip(const char *flip_path, const char *aplay_device);

/* Return 1 if flip should play: any tile's left or right content changed (syncs with visual flip). */
int audio_should_play_flip(const Arrival *curr, int n_curr,
                           const Arrival *prev, int n_prev);
