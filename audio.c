/*
 * Audio: background music and flip via PipeWire/PulseAudio (paplay) or ALSA (aplay).
 * When aplay_device is NULL, use paplay so music and flip mix. Otherwise use aplay to raw device.
 *
 * For PCM WAVs already at 16-bit LE, 44.1 kHz or 48 kHz, mono or stereo, paplay/aplay is used
 * directly (no sox) unless AUDIO_FORCE_SOX=1. sox is only for gain/volume or odd formats.
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

static pid_t music_pid = -1;
static pid_t ferry_pid = -1;

int audio_debug_enabled(void) {
    const char *e = getenv("AUDIO_DEBUG");
    return e && strcmp(e, "1") == 0;
}

static int audio_force_sox(void) {
    const char *e = getenv("AUDIO_FORCE_SOX");
    return e && strcmp(e, "1") == 0;
}

/* Read fmt chunk: PCM (1), 16-bit LE, 44.1k or 48k, 1–2 channels — fine for paplay/aplay without sox. */
static int wav_ok_direct_play(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[12];
    uint32_t riff_size = 0;
    long pos;
    if (fread(buf, 1, 12, f) != 12) goto bad;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) goto bad;
    riff_size = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    pos = 12;
    while (1) {
        if (fseek(f, pos, SEEK_SET) != 0) goto bad;
        if (fread(buf, 1, 8, f) != 8) goto bad;
        uint32_t chunk_size = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        if (memcmp(buf, "fmt ", 4) == 0 && chunk_size >= 16) {
            unsigned char fmt[16];
            if (fread(fmt, 1, 16, f) != 16) goto bad;
            uint16_t audio_format = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            uint16_t channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            uint32_t rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            uint16_t bits = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
            fclose(f);
            if (audio_format != 1u) return 0;
            if (bits != 16u) return 0;
            if (channels != 1u && channels != 2u) return 0;
            if (rate != 44100u && rate != 48000u) return 0;
            return 1;
        }
        if (memcmp(buf, "data", 4) == 0)
            break;
        pos += 8 + (long)((chunk_size + 1u) / 2u * 2u);
        if (pos < 12 || (riff_size > 0 && (uint32_t)pos > riff_size + 8)) goto bad;
    }
bad:
    fclose(f);
    return 0;
}

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

void audio_play_flip(const char *flip_path, const char *aplay_device) {
    if (!flip_path || !flip_path[0]) return;

    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    int audio_debug = audio_debug_enabled();
    int direct = !audio_force_sox() && wav_ok_direct_play(flip_path);
    if (audio_debug) {
        fprintf(stderr, "AUDIO_DEBUG: audio_play_flip path=%s device=%s direct=%d\n",
                flip_path, aplay_device && aplay_device[0] ? aplay_device : "(paplay)", direct);
    }

    if (!aplay_device || !aplay_device[0]) {
        pid_t pid = fork();
        if (pid < 0) return;
        if (pid == 0) {
            (void)freopen("/dev/null", "r", stdin);
            (void)freopen("/dev/null", "w", stdout);
            if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
            if (direct) {
                (void)execlp("paplay", "paplay", flip_path, (char *)NULL);
                _exit(127);
            }
            execl("/bin/sh", "sh", "-c",
                  "command -v paplay >/dev/null 2>&1 && ( command -v sox >/dev/null 2>&1 && sox -q -v 5.0 \"$1\" -t wav - 2>/dev/null | paplay 2>/dev/null ) || paplay \"$1\" 2>/dev/null",
                  "sh", flip_path, (char *)NULL);
            _exit(127);
        }
        return;
    }

    if (audio_debug) fprintf(stderr, "AUDIO_DEBUG: flip via aplay (ALSA)\n");
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
        if (direct) {
            if (execlp("aplay", "aplay", "-q", "-D", aplay_device, flip_path, (char *)NULL) < 0)
                _exit(127);
            _exit(0);
        }
        if (audio_debug) {
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

    int audio_debug = audio_debug_enabled();
    int use_pulse = (!aplay_device || !aplay_device[0]);
    int music_direct = !audio_force_sox() && wav_ok_direct_play(music_path);
    int ferry_direct = (!music_loop2 || !music_loop2[0]) ? 0 : (!audio_force_sox() && wav_ok_direct_play(music_loop2));
    ferry_pid = -1;

    int music_sec = 23;
    const char *env_music = getenv("MUSIC_DURATION_SEC");
    if (env_music && *env_music) {
        int v = atoi(env_music);
        if (v > 0) music_sec = v;
    }
    int delay_sec = music_sec * 4;
    int interval_sec = music_sec * 5;

    int file_sec = audio_wav_duration_seconds(music_path);
    if (file_sec <= 0 || file_sec > 300) {
        const char *env_file = getenv("MUSIC_FILE_SEC");
        if (env_file && *env_file) { int v = atoi(env_file); if (v > 0) file_sec = v; }
        else file_sec = 21;
    }
    int gap_sec = music_sec - file_sec;
    if (gap_sec < 0) gap_sec = 0;

    if (music_loop2 && music_loop2[0]) {
        char delay_buf[16], interval_buf[16];
        snprintf(delay_buf, sizeof(delay_buf), "%d", delay_sec);
        snprintf(interval_buf, sizeof(interval_buf), "%d", interval_sec);
        ferry_pid = fork();
        if (ferry_pid == 0) {
            setsid();
            (void)freopen("/dev/null", "r", stdin);
            (void)freopen("/dev/null", "w", stdout);
            if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
            if (use_pulse) {
                if (ferry_direct) {
                    execl("/bin/sh", "sh", "-c",
                          "command -v paplay >/dev/null 2>&1 && sleep \"$1\" && while true; do paplay \"$2\"; sleep \"$3\"; done",
                          "sh", delay_buf, music_loop2, interval_buf, (char *)NULL);
                } else {
                    execl("/bin/sh", "sh", "-c",
                          "command -v paplay >/dev/null 2>&1 && sleep \"$1\" && while true; do ( command -v sox >/dev/null 2>&1 && sox -q -v 0.5 \"$2\" -t wav - 2>/dev/null | paplay 2>/dev/null ) || paplay \"$2\" 2>/dev/null; sleep \"$3\"; done",
                          "sh", delay_buf, music_loop2, interval_buf, (char *)NULL);
                }
            } else {
                if (ferry_direct) {
                    execl("/bin/sh", "sh", "-c",
                          "sleep \"$1\" && while true; do aplay -q -D \"$2\" \"$3\"; sleep \"$4\"; done",
                          "sh", delay_buf, aplay_device, music_loop2, interval_buf, (char *)NULL);
                } else {
                    execl("/bin/sh", "sh", "-c",
                          "sleep \"$1\" && while true; do ( command -v sox >/dev/null 2>&1 && sox -q -v 0.5 \"$3\" -t wav - 2>/dev/null | aplay -q -D \"$2\" - 2>/dev/null ) || aplay -q -D \"$2\" \"$3\" 2>/dev/null; sleep \"$4\"; done",
                          "sh", delay_buf, aplay_device, music_loop2, interval_buf, (char *)NULL);
                }
            }
            _exit(127);
        }
        if (ferry_pid < 0) ferry_pid = -1;
        else if (audio_debug)
            fprintf(stderr, "AUDIO_DEBUG: ferry overlay every 5 loops (first at %ds, then every %ds)\n", delay_sec, interval_sec);
    }

    char gap_buf[16];
    snprintf(gap_buf, sizeof(gap_buf), "%d", gap_sec);
    if (audio_debug)
        fprintf(stderr, "AUDIO_DEBUG: cycle=%ds file~%ds gap=%ds music_direct=%d\n", music_sec, file_sec, gap_sec, music_direct);

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        if (!audio_debug) (void)freopen("/dev/null", "w", stderr);
        if (use_pulse) {
            if (music_direct) {
                if (audio_debug)
                    execl("/bin/sh", "sh", "-c",
                          "command -v paplay >/dev/null 2>&1 && while true; do paplay \"$1\"; sleep \"$2\"; done",
                          "sh", music_path, gap_buf, (char *)NULL);
                else
                    execl("/bin/sh", "sh", "-c",
                          "command -v paplay >/dev/null 2>&1 && while true; do paplay \"$1\" 2>/dev/null; sleep \"$2\"; done",
                          "sh", music_path, gap_buf, (char *)NULL);
            } else {
                if (audio_debug)
                    execl("/bin/sh", "sh", "-c",
                          "command -v paplay >/dev/null 2>&1 && while true; do ( command -v sox >/dev/null 2>&1 && sox -q -v 0.5 \"$1\" -t wav - | paplay ) || paplay \"$1\"; sleep \"$2\"; done",
                          "sh", music_path, gap_buf, (char *)NULL);
                else
                    execl("/bin/sh", "sh", "-c",
                          "command -v paplay >/dev/null 2>&1 && while true; do ( command -v sox >/dev/null 2>&1 && sox -q -v 0.5 \"$1\" -t wav - 2>/dev/null | paplay 2>/dev/null ) || paplay \"$1\" 2>/dev/null; sleep \"$2\"; done",
                          "sh", music_path, gap_buf, (char *)NULL);
            }
        } else {
            if (music_direct) {
                if (audio_debug)
                    execl("/bin/sh", "sh", "-c",
                          "while true; do aplay -q -D \"$1\" \"$2\"; sleep \"$3\"; done",
                          "sh", aplay_device, music_path, gap_buf, (char *)NULL);
                else
                    execl("/bin/sh", "sh", "-c",
                          "while true; do aplay -q -D \"$1\" \"$2\" 2>/dev/null; sleep \"$3\"; done",
                          "sh", aplay_device, music_path, gap_buf, (char *)NULL);
            } else {
                if (audio_debug)
                    execl("/bin/sh", "sh", "-c",
                          "while true; do ( command -v sox >/dev/null 2>&1 && sox -q -v 0.5 \"$2\" -t wav - | aplay -q -D \"$1\" - ) || aplay -q -D \"$1\" \"$2\"; sleep \"$3\"; done",
                          "sh", aplay_device, music_path, gap_buf, (char *)NULL);
                else
                    execl("/bin/sh", "sh", "-c",
                          "while true; do ( command -v sox >/dev/null 2>&1 && sox -q -v 0.5 \"$2\" -t wav - 2>/dev/null | aplay -q -D \"$1\" - 2>/dev/null ) || aplay -q -D \"$1\" \"$2\" 2>/dev/null; sleep \"$3\"; done",
                          "sh", aplay_device, music_path, gap_buf, (char *)NULL);
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
}
