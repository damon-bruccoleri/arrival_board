/*
 * Audio: in-process SDL2 mixer.
 *
 * Rationale:
 * - Avoid fork/exec of paplay/aplay/sox per flip (high jitter on Pi Zero / Zero W).
 * - Keep audio work off the render loop; flip triggers must be non-blocking.
 * - Mix flip over background music; only one flip plays at a time.
 */
#pragma once

/* True only when AUDIO_DEBUG=1 (off in production by default). */
int audio_debug_enabled(void);

/* Initialize audio device and preload/prepare assets. Safe to call once per process. */
int audio_init(const char *music_path, const char *ferry_path, const char *flip_path);

/* Stop playback and release SDL audio resources (safe to call even if not initialized). */
void audio_shutdown(void);

/* Pause/unpause all audio output (0=play, 1=pause). Non-blocking. */
void audio_set_paused(int paused);

/* Trigger one flip sound. If a flip is already playing, this call is ignored. */
void audio_trigger_flip(void);

/* Trigger one ferry/event sound (uses the preloaded ferry asset if present). Ignored if already playing. */
void audio_trigger_ferry(void);
