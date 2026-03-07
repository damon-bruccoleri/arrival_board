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

void audio_play_flip(const char *flip_path, const char *aplay_device) {
    if (!flip_path || !flip_path[0]) return;

    int audio_debug = (getenv("AUDIO_DEBUG") != NULL);
    if (audio_debug)
        fprintf(stderr, "AUDIO_DEBUG: audio_play_flip path=%s device=%s\n",
                flip_path, aplay_device && aplay_device[0] ? aplay_device : "(paplay)");

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
    mixed_path_buf[0] = '\0';
    ferry_pid = -1;

    /* Total cycle length 23s (music + gap). Ferry every 5th cycle. */
    int music_sec = 23;
    const char *env_music = getenv("MUSIC_DURATION_SEC");
    if (env_music && *env_music) {
        int v = atoi(env_music);
        if (v > 0) music_sec = v;
    }
    int delay_sec = music_sec * 4;    /* first ferry at start of 5th loop */
    int interval_sec = music_sec * 5; /* then every 5 loops */

    /* Actual file length: for gap so each loop = music_sec. If header wrong, use MUSIC_FILE_SEC or 21. */
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
            if (use_pulse)
                execl("/bin/sh", "sh", "-c",
                      "command -v paplay >/dev/null 2>&1 && sleep \"$1\" && while true; do paplay \"$2\" 2>/dev/null; sleep \"$3\"; done",
                      "sh", delay_buf, music_loop2, interval_buf, (char *)NULL);
            else
                execl("/bin/sh", "sh", "-c",
                      "sleep \"$1\" && while true; do aplay -q -D \"$2\" \"$3\" 2>/dev/null; sleep \"$4\"; done",
                      "sh", delay_buf, aplay_device, music_loop2, interval_buf, (char *)NULL);
            _exit(127);
        }
        if (ferry_pid < 0) ferry_pid = -1;
        else if (audio_debug)
            fprintf(stderr, "AUDIO_DEBUG: ferry overlay every 5 loops (first at %ds, then every %ds)\n", delay_sec, interval_sec);
    }

    char gap_buf[16];
    snprintf(gap_buf, sizeof(gap_buf), "%d", gap_sec);
    if (audio_debug)
        fprintf(stderr, "AUDIO_DEBUG: cycle=%ds file~%ds gap=%ds\n", music_sec, file_sec, gap_sec);

    /* Music: play file then gap so each cycle = music_sec. Never stop. */
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
                      "command -v paplay >/dev/null 2>&1 && while true; do paplay \"$1\"; sleep \"$2\"; done",
                      "sh", music_path, gap_buf, (char *)NULL);
            else
                execl("/bin/sh", "sh", "-c",
                      "command -v paplay >/dev/null 2>&1 && while true; do paplay \"$1\" 2>/dev/null; sleep \"$2\"; done",
                      "sh", music_path, gap_buf, (char *)NULL);
        } else {
            if (audio_debug)
                execl("/bin/sh", "sh", "-c",
                      "while true; do aplay -q -D \"$1\" \"$2\"; sleep \"$3\"; done",
                      "sh", aplay_device, music_path, gap_buf, (char *)NULL);
            else
                execl("/bin/sh", "sh", "-c",
                      "while true; do aplay -q -D \"$1\" \"$2\" 2>/dev/null; sleep \"$3\"; done",
                      "sh", aplay_device, music_path, gap_buf, (char *)NULL);
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

/* True if left-part fields changed (route, dest, bus, stops, ppl, miles). */
static int arrival_left_changed(const Arrival *a, const Arrival *b) {
    return strcmp(a->route, b->route) != 0 || strcmp(a->dest, b->dest) != 0 ||
           strcmp(a->bus, b->bus) != 0 || a->stops_away != b->stops_away ||
           a->ppl_est != b->ppl_est || (a->miles_away != b->miles_away);
}

/* True if right-part (mins) changed. */
static int arrival_right_changed(const Arrival *a, const Arrival *b) {
    return a->mins != b->mins;
}

int audio_should_play_flip(const Arrival *curr, int n_curr,
                           const Arrival *prev, int n_prev) {
    int slots = n_curr < n_prev ? n_curr : n_prev;
    for (int i = 0; i < slots; i++) {
        if (arrival_left_changed(&curr[i], &prev[i]) || arrival_right_changed(&curr[i], &prev[i]))
            return 1;
    }
    return 0;
}
