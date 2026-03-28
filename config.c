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

static void env_str(char *dst, size_t dstsz, const char *key, const char *fallback) {
    const char *v = getenv(key);
    if (v && *v)
        snprintf(dst, dstsz, "%s", v);
    else if (fallback)
        snprintf(dst, dstsz, "%s", fallback);
}

static void env_font(char *dst, size_t dstsz, const char *key, const char *fallback) {
    const char *v = getenv(key);
    if (v && *v && access(v, R_OK) == 0)
        snprintf(dst, dstsz, "%s", v);
    else if (fallback)
        snprintf(dst, dstsz, "%s", fallback);
}

static int env_int(const char *key, int fallback, int lo, int hi) {
    const char *v = getenv(key);
    int val = (v && *v) ? atoi(v) : fallback;
    return clampi(val, lo, hi);
}

static void resolve_audio_path(char *dst, size_t dstsz, const char *rel) {
    snprintf(dst, dstsz, "%s/arrival_board/%s", home_dir(), rel);
    if (access(dst, R_OK) != 0) dst[0] = '\0';
    resolve_absolute(dst, dstsz);
}

void config_from_env(AppConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    env_font(cfg->font_path, sizeof(cfg->font_path), "FONT_PATH",
             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");

    env_font(cfg->title_font_path, sizeof(cfg->title_font_path), "TITLE_FONT_PATH", NULL);
    resolve_absolute(cfg->title_font_path, sizeof(cfg->title_font_path));
    if (cfg->title_font_path[0])
        logf_("Title font path: %s", cfg->title_font_path);

    env_font(cfg->symbol_font_path, sizeof(cfg->symbol_font_path), "SYMBOL_FONT_PATH",
             "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf");

    env_font(cfg->emoji_font_path, sizeof(cfg->emoji_font_path), "EMOJI_FONT_PATH",
             "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf");

    env_str(cfg->mta_key, sizeof(cfg->mta_key), "MTA_KEY", NULL);
    env_str(cfg->stop_id, sizeof(cfg->stop_id), "STOP_ID", NULL);
    env_str(cfg->route_filter, sizeof(cfg->route_filter), "ROUTE_FILTER", NULL);
    env_str(cfg->stop_name_override, sizeof(cfg->stop_name_override), "STOP_NAME", NULL);
    env_str(cfg->aplay_device, sizeof(cfg->aplay_device), "APLAY_DEVICE", NULL);

    cfg->poll_seconds = env_int("POLL_SECONDS", 10, 5, 3600);
    cfg->max_tiles = env_int("MAX_TILES", 12, 1, 24);

    env_str(cfg->gtfs_url, sizeof(cfg->gtfs_url), "GTFS_BUS_URL",
            "https://rrgtfsfeeds.s3.amazonaws.com/gtfs_busco.zip");

    const char *gtfs_cache_env = getenv("GTFS_CACHE_PATH");
    if (gtfs_cache_env && *gtfs_cache_env)
        snprintf(cfg->gtfs_cache, sizeof(cfg->gtfs_cache), "%s", gtfs_cache_env);
    else
        snprintf(cfg->gtfs_cache, sizeof(cfg->gtfs_cache), "%s/arrival_board/gtfs_bus_cache.zip", home_dir());

    resolve_audio_path(cfg->music_path, sizeof(cfg->music_path), "tools/Seaport_Steampunk_Final_Mix.wav");
    resolve_audio_path(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), "tools/SI Ferry.wav");
    resolve_audio_path(cfg->flip_path, sizeof(cfg->flip_path), "tools/ddsm.wav");
}
