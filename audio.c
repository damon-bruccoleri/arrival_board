/*
 * Audio: background music (looped) and flip sound.
 */
#include "audio.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t music_pid = -1;

void audio_play_flip(const char *flip_path, const char *aplay_device) {
    if (!flip_path || !flip_path[0] || !aplay_device || !aplay_device[0]) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        /* sox -v 5.0 for much louder than background (which is 0.15) */
        execl("/bin/sh", "sh", "-c",
              "if command -v sox >/dev/null 2>&1; then "
              "sox -q -v 5.0 \"$1\" -t alsa \"$2\" 2>/dev/null; "
              "else aplay -q -D \"$2\" \"$1\"; fi",
              "sh", flip_path, aplay_device, (char *)NULL);
        _exit(127);
    }
}

void audio_start_music(const char *music_path, const char *aplay_device) {
    if (!music_path || !music_path[0] || !aplay_device || !aplay_device[0]) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execl("/bin/sh", "sh", "-c",
              "command -v sox >/dev/null 2>&1 && "
              "while true; do sox -q -v 0.15 \"$1\" -t alsa \"$2\" 2>/dev/null || aplay -q -D \"$2\" \"$1\" 2>/dev/null; done || "
              "while true; do aplay -q -D \"$2\" \"$1\" 2>/dev/null; done",
              "sh", music_path, aplay_device, (char *)NULL);
        _exit(127);
    }
    music_pid = pid;
}

void audio_stop_music(void) {
    if (music_pid > 0) {
        kill(music_pid, SIGTERM);
        waitpid(music_pid, NULL, WNOHANG);
        music_pid = -1;
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
