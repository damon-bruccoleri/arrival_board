/*
 * Audio: in-process SDL2 mixer.
 *
 * Implementation notes:
 * - A single SDL audio device is opened once; the callback mixes music + ferry + flip.
 * - Assets are loaded once at startup and converted to the device format.
 * - Flip triggers are non-blocking and ignore re-triggers while a flip is active.
 */
#include "audio.h"
#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_AudioDeviceID dev;
    SDL_AudioSpec have;
    int initialized;
    int paused;

    /* Pre-converted interleaved S16 samples in device format. */
    Uint8 *music_buf;
    uint32_t music_bytes;
    uint32_t music_pos_frames;     /* position within cycle */
    uint32_t music_cycle_frames;   /* cycle length (music+gap) */
    uint32_t music_file_frames;    /* frames in file */
    uint32_t music_cycles;         /* completed cycles */

    Uint8 *ferry_buf;
    uint32_t ferry_bytes;
    uint32_t ferry_pos_frames;
    int ferry_active;

    Uint8 *flip_buf;
    uint32_t flip_bytes;
    uint32_t flip_pos_frames;
    int flip_active;

    /* Reused mix accumulator to avoid callback-time heap churn/jitter. */
    int32_t *mix_acc;
    uint32_t mix_acc_frames;
} AudioState;

static AudioState g_audio;

int audio_debug_enabled(void) {
    const char *e = getenv("AUDIO_DEBUG");
    return e && strcmp(e, "1") == 0;
}

static uint32_t bytes_per_frame(const SDL_AudioSpec *s) {
    if (!s) return 0;
    return (uint32_t)(SDL_AUDIO_BITSIZE(s->format) / 8u) * (uint32_t)s->channels;
}

static void free_buf(Uint8 **p) {
    if (p && *p) {
        SDL_free(*p);
        *p = NULL;
    }
}

static void free_mix_acc(void) {
    if (g_audio.mix_acc) {
        SDL_free(g_audio.mix_acc);
        g_audio.mix_acc = NULL;
    }
    g_audio.mix_acc_frames = 0;
}

static int parse_env_int(const char *k, int defv, int lo, int hi) {
    const char *v = getenv(k);
    int out = (v && *v) ? atoi(v) : defv;
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return out;
}

static int load_and_convert(const char *path, const SDL_AudioSpec *dst,
                            Uint8 **out_buf, uint32_t *out_bytes) {
    if (!out_buf || !out_bytes) return -1;
    *out_buf = NULL;
    *out_bytes = 0;
    if (!path || !path[0]) return 0;

    SDL_AudioSpec src;
    Uint8 *src_buf = NULL;
    Uint32 src_len = 0;
    if (!SDL_LoadWAV(path, &src, &src_buf, &src_len)) {
        if (audio_debug_enabled())
            fprintf(stderr, "AUDIO_DEBUG: SDL_LoadWAV failed path=%s err=%s\n", path, SDL_GetError());
        return -1;
    }

    /* Convert via SDL_AudioStream to match the device format exactly. */
    SDL_AudioStream *st = SDL_NewAudioStream(src.format, src.channels, src.freq,
                                             dst->format, dst->channels, dst->freq);
    if (!st) {
        SDL_FreeWAV(src_buf);
        return -1;
    }
    if (SDL_AudioStreamPut(st, src_buf, (int)src_len) < 0) {
        SDL_FreeWAV(src_buf);
        SDL_FreeAudioStream(st);
        return -1;
    }
    SDL_FreeWAV(src_buf);
    SDL_AudioStreamFlush(st);

    int avail = SDL_AudioStreamAvailable(st);
    if (avail <= 0) {
        SDL_FreeAudioStream(st);
        return 0;
    }
    Uint8 *buf = (Uint8 *)SDL_malloc((size_t)avail);
    if (!buf) {
        SDL_FreeAudioStream(st);
        return -1;
    }
    int got = SDL_AudioStreamGet(st, buf, avail);
    SDL_FreeAudioStream(st);
    if (got <= 0) {
        SDL_free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_bytes = (uint32_t)got;
    return 0;
}

static int16_t clamp_s16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void mix_from_buf_s16(int32_t *acc, int frames, int channels,
                             const Uint8 *buf, uint32_t buf_frames,
                             uint32_t pos_frames, float gain) {
    if (!acc || frames <= 0 || channels <= 0 || !buf || buf_frames == 0)
        return;
    const int16_t *src = (const int16_t *)buf;
    for (int i = 0; i < frames; i++) {
        uint32_t si = pos_frames + (uint32_t)i;
        if (si >= buf_frames) break;
        const int16_t *sp = src + (size_t)si * (size_t)channels;
        int32_t *dp = acc + (size_t)i * (size_t)channels;
        for (int c = 0; c < channels; c++) {
            dp[c] += (int32_t)((float)sp[c] * gain);
        }
    }
}

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    if (!stream || len <= 0) return;

    if (!g_audio.initialized || g_audio.paused ||
        g_audio.have.format != AUDIO_S16SYS ||
        (g_audio.have.channels != 1 && g_audio.have.channels != 2)) {
        memset(stream, 0, (size_t)len);
        return;
    }

    const int channels = g_audio.have.channels;
    const int frames = (int)((uint32_t)len / bytes_per_frame(&g_audio.have));
    if (frames <= 0) {
        memset(stream, 0, (size_t)len);
        return;
    }

    if (!g_audio.mix_acc || g_audio.mix_acc_frames < (uint32_t)frames) {
        memset(stream, 0, (size_t)len);
        return;
    }
    int32_t *acc = g_audio.mix_acc;
    memset(acc, 0, (size_t)frames * (size_t)channels * sizeof(int32_t));

    if (!acc) {
        memset(stream, 0, (size_t)len);
        return;
    }

    /* --- Music (loop with optional gap) --- */
    if (g_audio.music_buf && g_audio.music_bytes > 0 && g_audio.music_cycle_frames > 0) {
        uint32_t cycle_pos = g_audio.music_pos_frames;
        uint32_t file_frames = g_audio.music_file_frames;
        uint32_t cycle_frames = g_audio.music_cycle_frames;

        for (int i = 0; i < frames; i++) {
            /* At the start of a new cycle, schedule ferry overlay (every 5 loops, first at loop 4). */
            if (cycle_pos == 0 && i == 0) {
                /* nothing: handled on wrap below to count completed cycle */
            }

            if (cycle_pos < file_frames) {
                mix_from_buf_s16(acc + (size_t)i * (size_t)channels, 1, channels,
                                 g_audio.music_buf, file_frames, cycle_pos, 0.50f);
            }
            cycle_pos++;
            if (cycle_pos >= cycle_frames) {
                cycle_pos = 0;
                g_audio.music_cycles++;
                /* Overlay ferry every 5 cycles, first time after 4 cycles. */
                if (g_audio.ferry_buf && g_audio.ferry_bytes > 0) {
                    if (g_audio.music_cycles == 4 || (g_audio.music_cycles > 4 && ((g_audio.music_cycles - 4) % 5) == 0)) {
                        g_audio.ferry_active = 1;
                        g_audio.ferry_pos_frames = 0;
                    }
                }
            }
        }
        g_audio.music_pos_frames = cycle_pos;
    }

    /* --- Ferry overlay (optional) --- */
    if (g_audio.ferry_active && g_audio.ferry_buf && g_audio.ferry_bytes > 0) {
        uint32_t ferry_frames = g_audio.ferry_bytes / bytes_per_frame(&g_audio.have);
        mix_from_buf_s16(acc, frames, channels, g_audio.ferry_buf, ferry_frames, g_audio.ferry_pos_frames, 0.35f);
        g_audio.ferry_pos_frames += (uint32_t)frames;
        if (g_audio.ferry_pos_frames >= ferry_frames) {
            g_audio.ferry_active = 0;
            g_audio.ferry_pos_frames = 0;
        }
    }

    /* --- Flip (one at a time; ignore retriggers while active) --- */
    if (g_audio.flip_active && g_audio.flip_buf && g_audio.flip_bytes > 0) {
        uint32_t flip_frames = g_audio.flip_bytes / bytes_per_frame(&g_audio.have);
        mix_from_buf_s16(acc, frames, channels, g_audio.flip_buf, flip_frames, g_audio.flip_pos_frames, 1.0f);
        g_audio.flip_pos_frames += (uint32_t)frames;
        if (g_audio.flip_pos_frames >= flip_frames) {
            g_audio.flip_active = 0;
            g_audio.flip_pos_frames = 0;
        }
    }

    /* Write out */
    int16_t *out = (int16_t *)stream;
    for (int i = 0; i < frames * channels; i++)
        out[i] = clamp_s16(acc[i]);
}

int audio_init(const char *music_path, const char *ferry_path, const char *flip_path) {
    if (g_audio.initialized)
        return 0;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        if (audio_debug_enabled())
            fprintf(stderr, "AUDIO_DEBUG: SDL_INIT_AUDIO failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = parse_env_int("AUDIO_SAMPLES", 2048, 256, 8192);
    want.callback = audio_callback;
    want.userdata = NULL;

    SDL_AudioSpec have;
    SDL_zero(have);
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                                SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!dev) {
        if (audio_debug_enabled())
            fprintf(stderr, "AUDIO_DEBUG: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.dev = dev;
    g_audio.have = have;
    g_audio.initialized = 1;
    g_audio.paused = 0;

    /* Allocate enough headroom for larger callback blocks to prevent runtime allocs. */
    g_audio.mix_acc_frames = (uint32_t)have.samples * 4u;
    if (g_audio.mix_acc_frames < 4096u)
        g_audio.mix_acc_frames = 4096u;
    g_audio.mix_acc = (int32_t *)SDL_calloc(
        (size_t)g_audio.mix_acc_frames * (size_t)have.channels, sizeof(int32_t));
    if (!g_audio.mix_acc) {
        if (audio_debug_enabled())
            fprintf(stderr, "AUDIO_DEBUG: mix accumulator alloc failed\n");
        SDL_CloseAudioDevice(dev);
        memset(&g_audio, 0, sizeof(g_audio));
        return -1;
    }

    /* Convert/preload assets */
    (void)load_and_convert(flip_path, &g_audio.have, &g_audio.flip_buf, &g_audio.flip_bytes);
    (void)load_and_convert(music_path, &g_audio.have, &g_audio.music_buf, &g_audio.music_bytes);
    (void)load_and_convert(ferry_path, &g_audio.have, &g_audio.ferry_buf, &g_audio.ferry_bytes);

    /* Configure looping music cycle/gap */
    uint32_t bpf = bytes_per_frame(&g_audio.have);
    if (g_audio.music_buf && g_audio.music_bytes > 0 && bpf > 0) {
        g_audio.music_file_frames = g_audio.music_bytes / bpf;
        int music_sec = parse_env_int("MUSIC_DURATION_SEC", 23, 1, 600);
        uint32_t cycle_frames = (uint32_t)music_sec * (uint32_t)g_audio.have.freq;
        if (cycle_frames < g_audio.music_file_frames)
            cycle_frames = g_audio.music_file_frames;
        g_audio.music_cycle_frames = cycle_frames;
    }

    if (audio_debug_enabled()) {
        fprintf(stderr, "AUDIO_DEBUG: device freq=%d ch=%d fmt=0x%x samples=%d\n",
                g_audio.have.freq, g_audio.have.channels, g_audio.have.format, g_audio.have.samples);
        fprintf(stderr, "AUDIO_DEBUG: mix_acc_frames=%u\n", g_audio.mix_acc_frames);
        fprintf(stderr, "AUDIO_DEBUG: music=%s bytes=%u\n", (music_path && *music_path) ? music_path : "(none)", g_audio.music_bytes);
        fprintf(stderr, "AUDIO_DEBUG: ferry=%s bytes=%u\n", (ferry_path && *ferry_path) ? ferry_path : "(none)", g_audio.ferry_bytes);
        fprintf(stderr, "AUDIO_DEBUG: flip=%s bytes=%u\n", (flip_path && *flip_path) ? flip_path : "(none)", g_audio.flip_bytes);
    }

    SDL_PauseAudioDevice(g_audio.dev, 0);
    return 0;
}

void audio_shutdown(void) {
    if (!g_audio.initialized)
        return;
    SDL_LockAudioDevice(g_audio.dev);
    g_audio.paused = 1;
    g_audio.flip_active = 0;
    g_audio.ferry_active = 0;
    SDL_UnlockAudioDevice(g_audio.dev);

    SDL_PauseAudioDevice(g_audio.dev, 1);
    SDL_CloseAudioDevice(g_audio.dev);

    free_buf(&g_audio.music_buf);
    free_buf(&g_audio.ferry_buf);
    free_buf(&g_audio.flip_buf);
    free_mix_acc();

    memset(&g_audio, 0, sizeof(g_audio));
}

void audio_set_paused(int paused) {
    if (!g_audio.initialized)
        return;
    SDL_LockAudioDevice(g_audio.dev);
    g_audio.paused = paused ? 1 : 0;
    SDL_UnlockAudioDevice(g_audio.dev);
}

void audio_trigger_flip(void) {
    if (!g_audio.initialized || !g_audio.flip_buf || g_audio.flip_bytes == 0)
        return;
    SDL_LockAudioDevice(g_audio.dev);
    if (!g_audio.flip_active) {
        g_audio.flip_active = 1;
        g_audio.flip_pos_frames = 0;
    }
    SDL_UnlockAudioDevice(g_audio.dev);
}

void audio_trigger_ferry(void) {
    if (!g_audio.initialized || !g_audio.ferry_buf || g_audio.ferry_bytes == 0)
        return;
    SDL_LockAudioDevice(g_audio.dev);
    if (!g_audio.ferry_active) {
        g_audio.ferry_active = 1;
        g_audio.ferry_pos_frames = 0;
    }
    SDL_UnlockAudioDevice(g_audio.dev);
}
