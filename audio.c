/*
 * Audio: background music and flip via PipeWire/PulseAudio (paplay) or ALSA (aplay).
 * When aplay_device is NULL, use paplay so music and flip mix. Otherwise use aplay to raw device.
 */
#include "audio.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(USE_PULSE)
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

static pid_t music_pid = -1;
static pid_t ferry_pid = -1;
static char mixed_path_buf[512]; /* sox mixed file; unlink on stop */

/* Parse RIFF WAV and return duration in seconds; -1 on error. */
int audio_wav_duration_seconds(const char *path) {
    if (!path || !path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char buf[12];
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t data_bytes = 0;
    uint32_t riff_size = 0;
    long pos;
    if (fread(buf, 1, 12, f) != 12) goto fail;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) goto fail;
    riff_size = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    pos = 12;
    while (1) {
        if (fseek(f, pos, SEEK_SET) != 0) goto fail;
        if (fread(buf, 1, 8, f) != 8) goto fail;
        {
            uint32_t chunk_size = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
            if (memcmp(buf, "fmt ", 4) == 0 && chunk_size >= 16) {
                unsigned char fmt[16];
                if (fread(fmt, 1, 16, f) != 16) goto fail;
                channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
                rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
                bits = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
            } else if (memcmp(buf, "data", 4) == 0) {
                data_bytes = chunk_size;
                /* Some WAVs have data chunk size 0; use rest of file. RIFF size = file size - 8, data starts at pos+8. */
                if (data_bytes == 0 && riff_size > (uint32_t)pos)
                    data_bytes = riff_size - (uint32_t)pos;
                break;
            }
            pos += 8 + (chunk_size + 1) / 2 * 2;
        }
    }
    fclose(f);
    if (rate == 0 || channels == 0 || bits == 0) return -1;
    return (int)(data_bytes / (rate * channels * (bits / 8)));
fail:
    fclose(f);
    return -1;
}

#if defined(USE_PULSE)
static int use_pulse_for_flip(const char *flip_path) {
    pa_simple *s = NULL;
    FILE *f;
    unsigned char header[44];
    uint32_t rate;
    uint16_t channels;
    pa_sample_spec ss;
    char buf[1024];
    size_t n;
    int pa_err = 0;
    int r = -1;

    if (!flip_path || !flip_path[0]) return 0;
    f = fopen(flip_path, "rb");
    if (!f) {
        if (getenv("AUDIO_DEBUG")) fprintf(stderr, "AUDIO_DEBUG: flip file open failed: %s\n", flip_path);
        return 0;
    }
    if (fread(header, 1, 44, f) != 44) goto out;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) goto out;
    channels = (uint16_t)((unsigned char)header[22] | ((unsigned char)header[23] << 8));
    rate = (uint32_t)((unsigned char)header[24] | ((unsigned char)header[25] << 8) | ((unsigned char)header[26] << 16) | ((unsigned char)header[27] << 24));
    if (channels == 0 || channels > 2 || rate < 4000 || rate > 192000) goto out;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = rate;
    ss.channels = channels;
    s = pa_simple_new(NULL, "arrival_board", PA_STREAM_PLAYBACK, NULL, "flip", &ss, NULL, NULL, &pa_err);
    if (!s) goto out;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (pa_simple_write(s, buf, n, &pa_err) < 0) break;
    }
    pa_simple_drain(s, &pa_err);
    r = 0;
out:
    if (s) pa_simple_free(s);
    if (f) fclose(f);
    if (getenv("AUDIO_DEBUG") && r != 0) fprintf(stderr, "AUDIO_DEBUG: Pulse flip failed, will use paplay/aplay\n");
    return (r == 0);
}
#endif

void audio_play_flip(const char *flip_path, const char *aplay_device) {
    if (!flip_path || !flip_path[0]) return;

    int audio_debug = (getenv("AUDIO_DEBUG") != NULL);
    if (audio_debug)
        fprintf(stderr, "AUDIO_DEBUG: audio_play_flip path=%s device=%s\n",
                flip_path, aplay_device && aplay_device[0] ? aplay_device : "(Pulse)");

#if defined(USE_PULSE)
    if (use_pulse_for_flip(flip_path)) {
        if (audio_debug) fprintf(stderr, "AUDIO_DEBUG: flip played via Pulse (libpulse)\n");
        return;
    }
#endif

    if (!aplay_device || !aplay_device[0]) {
        /* Pulse path: paplay in background, mixes with music */
        pid_t pid = fork();
        if (pid < 0) return;
        if (pid == 0) {
            (void)freopen("/dev/null", "r", stdin);
            (void)freopen("/dev/null", "w", stdout);
            if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
            execl("/bin/sh", "sh", "-c", "command -v paplay >/dev/null 2>&1 && paplay \"$1\" 2>/dev/null &", "sh", flip_path, (char *)NULL);
            _exit(127);
        }
        (void)waitpid(pid, NULL, WNOHANG);
        return;
    }

    /* ALSA path: block (music already stopped by caller) */
    if (audio_debug) fprintf(stderr, "AUDIO_DEBUG: flip via aplay (ALSA)\n");
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
        if (audio_debug) {
            /* Show aplay/sox errors when troubleshooting */
            if (execl("/bin/sh", "sh", "-c",
                      "( command -v sox >/dev/null 2>&1 && sox -q -v 5.0 \"$1\" -c 2 -r 44100 -t wav - | aplay -q -D \"$2\" -f S16_LE -c 2 -r 44100 - ) || aplay -q -D \"$2\" \"$1\"",
                      "sh", flip_path, aplay_device, (char *)NULL) < 0)
                _exit(127);
        } else if (execl("/bin/sh", "sh", "-c",
                  "( command -v sox >/dev/null 2>&1 && sox -q -v 5.0 \"$1\" -c 2 -r 44100 -t wav - | aplay -q -D \"$2\" -f S16_LE -c 2 -r 44100 - 2>/dev/null ) || aplay -q -D \"$2\" \"$1\" 2>/dev/null",
                  "sh", flip_path, aplay_device, (char *)NULL) < 0)
            _exit(127);
        _exit(0);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
}

void audio_start_music(const char *music_path, const char *music_loop2, const char *aplay_device) {
    if (!music_path || !music_path[0]) return;

    int audio_debug = (getenv("AUDIO_DEBUG") != NULL);
    int use_pulse = (!aplay_device || !aplay_device[0]);
    const char *play_path = music_path;
    mixed_path_buf[0] = '\0';
    ferry_pid = -1;

    /* Pre-mix music + ferry into one file so one stream plays continuously (music never stops). */
    if (music_loop2 && music_loop2[0]) {
        int music_sec = audio_wav_duration_seconds(music_path);
        int ferry_sec = audio_wav_duration_seconds(music_loop2);
        const char *env_music = getenv("MUSIC_DURATION_SEC");
        const char *env_ferry = getenv("FERRY_DURATION_SEC");
        if (env_music && *env_music) { int v = atoi(env_music); if (v > 0) music_sec = v; }
        if (env_ferry && *env_ferry) { int v = atoi(env_ferry); if (v >= 0) ferry_sec = v; }
        if (ferry_sec < 0) ferry_sec = 5; /* .mp3 or other format: assume ~5s if not set via env */
        if (music_sec > 0 && ferry_sec >= 0) {
            /* Ferry at end of music only: pad ferry with (music_len - ferry_len) sec so it overlays the last ferry_len sec. */
            int delay_sec = music_sec - ferry_sec;
            if (delay_sec < 0) delay_sec = 0;
            snprintf(mixed_path_buf, sizeof(mixed_path_buf), "/tmp/arrival_board_mixed_%d.wav", (int)getpid());
            char delay_buf[16];
            snprintf(delay_buf, sizeof(delay_buf), "%d", delay_sec);
            /* Normalize ferry to 48kHz 16-bit stereo for compatibility (Pi/sox). */
            char ferry_norm_buf[512];
            snprintf(ferry_norm_buf, sizeof(ferry_norm_buf), "/tmp/arrival_board_ferry_%d.wav", (int)getpid());
            const char *ferry_for_mix = music_loop2;
            pid_t norm_pid = fork();
            if (norm_pid == 0) {
                (void)freopen("/dev/null", "r", stdin);
                (void)freopen("/dev/null", "w", stdout);
                if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
                execl("/bin/sh", "sh", "-c",
                      "command -v sox >/dev/null 2>&1 && sox \"$1\" -r 48000 -c 2 -b 16 \"$2\"",
                      "sh", music_loop2, ferry_norm_buf, (char *)NULL);
                _exit(1);
            }
            if (norm_pid > 0) {
                int st;
                while (waitpid(norm_pid, &st, 0) < 0 && errno == EINTR) { }
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                    struct stat sb;
                    if (stat(ferry_norm_buf, &sb) == 0)
                        ferry_for_mix = ferry_norm_buf;
                }
            }
            /* Mix full music with ferry over the last ferry_sec: pad ferry with delay_sec, then mix (output = full music length). */
            pid_t sox_pid = fork();
            if (sox_pid == 0) {
                (void)freopen("/dev/null", "r", stdin);
                (void)freopen("/dev/null", "w", stdout);
                if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
                /* Boost ferry -v 4.4, music -v 0.3 so it’s audible over the music */
                execl("/bin/sh", "sh", "-c",
                      "command -v sox >/dev/null 2>&1 && sox \"$2\" -p pad \"$3\" 0 | sox -m -v 0.3 \"$1\" -v 4.4 - \"$4\"",
                      "sh", music_path, ferry_for_mix, delay_buf, mixed_path_buf, (char *)NULL);
                _exit(1);
            }
            if (sox_pid > 0) {
                int st;
                while (waitpid(sox_pid, &st, 0) < 0 && errno == EINTR) { }
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                    struct stat sb;
                    if (stat(mixed_path_buf, &sb) == 0) {
                        play_path = mixed_path_buf;
                        if (audio_debug) fprintf(stderr, "AUDIO_DEBUG: sox mix ok, ferry at end of loop\n");
                    }
                }
                if (play_path != mixed_path_buf) {
                    mixed_path_buf[0] = '\0';
                    if (audio_debug) fprintf(stderr, "AUDIO_DEBUG: sox mix failed (sox exit %d), music only\n",
                            sox_pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1);
                }
            }
            if (ferry_for_mix == ferry_norm_buf)
                (void)unlink(ferry_norm_buf);
        }
    }

    /* If no pre-mixed file, use separate ferry process on Pulse only. */
    int ferry_mix = use_pulse && music_loop2 && music_loop2[0] && play_path == music_path;
    if (ferry_mix) {
        int music_sec = audio_wav_duration_seconds(music_path);
        int ferry_sec = audio_wav_duration_seconds(music_loop2);
        if (music_sec > 0 && ferry_sec >= 0) {
            int delay_sec = music_sec - ferry_sec;
            if (delay_sec < 0) delay_sec = 0;
            char delay_buf[16];
            snprintf(delay_buf, sizeof(delay_buf), "%d", delay_sec);
            ferry_pid = fork();
            if (ferry_pid == 0) {
                setsid();
                (void)freopen("/dev/null", "r", stdin);
                (void)freopen("/dev/null", "w", stdout);
                if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
                execl("/bin/sh", "sh", "-c",
                      "command -v paplay >/dev/null 2>&1 && while true; do sleep \"$1\"; paplay \"$2\" 2>/dev/null; done",
                      "sh", delay_buf, music_loop2, (char *)NULL);
                _exit(127);
            }
            if (ferry_pid < 0) ferry_pid = -1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
        if (use_pulse) {
            if (audio_debug)
                execl("/bin/sh", "sh", "-c",
                      "command -v paplay >/dev/null 2>&1 && while true; do paplay \"$1\"; done",
                      "sh", play_path, (char *)NULL);
            else
                execl("/bin/sh", "sh", "-c",
                      "command -v paplay >/dev/null 2>&1 && while true; do paplay \"$1\" 2>/dev/null; done",
                      "sh", play_path, (char *)NULL);
        } else {
            /* ALSA: play_path is mixed file (ferry mixed in) or music only. Never play ferry in sequence. */
            if (play_path != music_path) {
                if (audio_debug)
                    execl("/bin/sh", "sh", "-c",
                          "while true; do aplay -q -D \"$1\" \"$2\"; done",
                          "sh", aplay_device, play_path, (char *)NULL);
                else
                    execl("/bin/sh", "sh", "-c",
                          "while true; do aplay -q -D \"$1\" \"$2\" 2>/dev/null; done",
                          "sh", aplay_device, play_path, (char *)NULL);
            } else {
                execl("/bin/sh", "sh", "-c",
                      "command -v sox >/dev/null 2>&1 && "
                      "while true; do sox -q -v 0.15 \"$1\" -t alsa \"$2\" 2>/dev/null || aplay -q -D \"$2\" \"$1\" 2>/dev/null; done || "
                      "while true; do aplay -q -D \"$2\" \"$1\" 2>/dev/null; done",
                      "sh", music_path, aplay_device, (char *)NULL);
            }
        }
        _exit(127);
    }
    music_pid = pid;
}

void audio_stop_music(void) {
    if (ferry_pid > 0) {
        kill(ferry_pid, SIGTERM);
        waitpid(ferry_pid, NULL, WNOHANG);
        ferry_pid = -1;
    }
    if (music_pid > 0) {
        kill(music_pid, SIGTERM);
        waitpid(music_pid, NULL, WNOHANG);
        music_pid = -1;
    }
    if (mixed_path_buf[0]) {
        (void)unlink(mixed_path_buf);
        mixed_path_buf[0] = '\0';
    }
}

int audio_should_play_flip(const Arrival *curr, int n_curr,
                           const Arrival *prev, int n_prev) {
    for (int i = 0; i < n_curr; i++) {
        const Arrival *a = &curr[i];
        int is_new = 1;
        int transition_to_now = 0;
        for (int j = 0; j < n_prev; j++) {
            const Arrival *b = &prev[j];
            if (strcmp(a->bus, b->bus) == 0 && strcmp(a->route, b->route) == 0) {
                is_new = 0;
                if (a->mins == 0 && b->mins > 0) transition_to_now = 1;
                break;
            }
        }
        if (is_new || transition_to_now) return 1;
    }
    return 0;
}
