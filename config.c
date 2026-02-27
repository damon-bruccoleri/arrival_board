/*
 * Application config from environment and path resolution.
 */
#include "config.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

static void resolve_absolute(char *path, size_t sz) {
    if (!path || !path[0]) return;
    char abs_buf[512];
    if (realpath(path, abs_buf))
        snprintf(path, sz, "%s", abs_buf);
}

static void try_same_dir_as_music(char *out, size_t outsz, const char *music_path, const char *filename) {
    if (!music_path || !music_path[0] || !out || outsz == 0) return;
    const char *last_slash = strrchr(music_path, '/');
    if (!last_slash || (size_t)(last_slash - music_path) >= outsz) return;
    snprintf(out, outsz, "%.*s/%s", (int)(last_slash - music_path), music_path, filename);
}

void config_from_env(AppConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    const char *font_path = getenv("FONT_PATH");
    if (!font_path || !*font_path) font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", font_path);

    const char *sym = getenv("SYMBOL_FONT_PATH");
    if (!sym || !*sym) sym = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    snprintf(cfg->symbol_font_path, sizeof(cfg->symbol_font_path), "%s", sym);

    const char *mta_key = getenv("MTA_KEY");
    if (mta_key) snprintf(cfg->mta_key, sizeof(cfg->mta_key), "%s", mta_key);
    const char *stop_id = getenv("STOP_ID");
    if (stop_id) snprintf(cfg->stop_id, sizeof(cfg->stop_id), "%s", stop_id);
    const char *route_filter = getenv("ROUTE_FILTER");
    if (route_filter) snprintf(cfg->route_filter, sizeof(cfg->route_filter), "%s", route_filter);

    int poll = 10;
    const char *poll_s = getenv("POLL_SECONDS");
    if (poll_s && *poll_s) poll = atoi(poll_s);
    cfg->poll_seconds = poll < 5 ? 5 : poll;

    int max_tiles = 12;
    const char *max_s = getenv("MAX_TILES");
    if (max_s && *max_s) max_tiles = atoi(max_s);
    cfg->max_tiles = clampi(max_tiles, 1, 24);

    const char *stop_name_override = getenv("STOP_NAME");
    if (stop_name_override && *stop_name_override)
        snprintf(cfg->stop_name_override, sizeof(cfg->stop_name_override), "%s", stop_name_override);

    const char *gtfs_url = getenv("GTFS_BUS_URL");
    if (!gtfs_url || !*gtfs_url) gtfs_url = "https://rrgtfsfeeds.s3.amazonaws.com/gtfs_busco.zip";
    snprintf(cfg->gtfs_url, sizeof(cfg->gtfs_url), "%s", gtfs_url);

    const char *gtfs_cache_env = getenv("GTFS_CACHE_PATH");
    if (gtfs_cache_env && *gtfs_cache_env)
        snprintf(cfg->gtfs_cache, sizeof(cfg->gtfs_cache), "%s", gtfs_cache_env);
    else
        snprintf(cfg->gtfs_cache, sizeof(cfg->gtfs_cache), "%s/arrival_board/gtfs_bus_cache.zip", home_dir());

    const char *aplay_dev = getenv("APLAY_DEVICE");
    if (aplay_dev) snprintf(cfg->aplay_device, sizeof(cfg->aplay_device), "%s", aplay_dev);

    /* Music path: MUSIC_FILE / BACKGROUND_MUSIC, then default under HOME/tools */
    const char *music_env = getenv("MUSIC_FILE");
    if (!music_env || !*music_env) music_env = getenv("BACKGROUND_MUSIC");
    if (music_env && *music_env) {
        snprintf(cfg->music_path, sizeof(cfg->music_path), "%s", music_env);
    } else {
        snprintf(cfg->music_path, sizeof(cfg->music_path), "%s/arrival_board/tools/Seaport_Steampunk_Final_Mix.wav", home_dir());
        if (access(cfg->music_path, R_OK) != 0)
            snprintf(cfg->music_path, sizeof(cfg->music_path), "tools/Seaport_Steampunk_Final_Mix.wav");
    }

    /* Ferry (second loop): SI Ferry.wav / .mp3 under HOME/tools or same dir as music */
    snprintf(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), "%s/arrival_board/tools/SI Ferry.wav", home_dir());
    if (access(cfg->music_loop2_path, R_OK) != 0) {
        snprintf(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), "%s/arrival_board/tools/SI Ferry.mp3", home_dir());
        if (access(cfg->music_loop2_path, R_OK) != 0) cfg->music_loop2_path[0] = '\0';
    }
    if (!cfg->music_loop2_path[0] && access("tools/SI Ferry.wav", R_OK) == 0)
        snprintf(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), "tools/SI Ferry.wav");
    if (!cfg->music_loop2_path[0] && access("tools/SI Ferry.mp3", R_OK) == 0)
        snprintf(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), "tools/SI Ferry.mp3");
    resolve_absolute(cfg->music_path, sizeof(cfg->music_path));
    resolve_absolute(cfg->music_loop2_path, sizeof(cfg->music_loop2_path));
    if (cfg->music_path[0] && !cfg->music_loop2_path[0]) {
        try_same_dir_as_music(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), cfg->music_path, "SI Ferry.wav");
        if (access(cfg->music_loop2_path, R_OK) != 0) {
            try_same_dir_as_music(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), cfg->music_path, "SI Ferry.mp3");
            if (access(cfg->music_loop2_path, R_OK) != 0) cfg->music_loop2_path[0] = '\0';
        }
        if (cfg->music_loop2_path[0]) resolve_absolute(cfg->music_loop2_path, sizeof(cfg->music_loop2_path));
    }

    /* Flip sound: SOUND_FLIP env, then ddsm.wav / flip.wav in tools or same dir as music */
    const char *env_flip = getenv("SOUND_FLIP");
    if (env_flip && *env_flip && access(env_flip, R_OK) == 0)
        snprintf(cfg->flip_path, sizeof(cfg->flip_path), "%s", env_flip);
    if (!cfg->flip_path[0]) {
        snprintf(cfg->flip_path, sizeof(cfg->flip_path), "%s/arrival_board/tools/ddsm.wav", home_dir());
        if (access(cfg->flip_path, R_OK) != 0) cfg->flip_path[0] = '\0';
    }
    if (!cfg->flip_path[0] && access("tools/ddsm.wav", R_OK) == 0)
        snprintf(cfg->flip_path, sizeof(cfg->flip_path), "tools/ddsm.wav");
    if (!cfg->flip_path[0]) {
        snprintf(cfg->flip_path, sizeof(cfg->flip_path), "%s/arrival_board/flip.wav", home_dir());
        if (access(cfg->flip_path, R_OK) != 0) cfg->flip_path[0] = '\0';
    }
    if (!cfg->flip_path[0] && access("tools/flip.wav", R_OK) == 0)
        snprintf(cfg->flip_path, sizeof(cfg->flip_path), "tools/flip.wav");
    if (!cfg->flip_path[0] && cfg->music_path[0]) {
        try_same_dir_as_music(cfg->flip_path, sizeof(cfg->flip_path), cfg->music_path, "ddsm.wav");
        if (access(cfg->flip_path, R_OK) != 0) {
            try_same_dir_as_music(cfg->flip_path, sizeof(cfg->flip_path), cfg->music_path, "flip.wav");
            if (access(cfg->flip_path, R_OK) != 0) cfg->flip_path[0] = '\0';
        }
    }
    resolve_absolute(cfg->flip_path, sizeof(cfg->flip_path));
}
