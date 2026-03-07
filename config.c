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

void config_from_env(AppConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    /* Body font: FONT_PATH env, else Noto, else DejaVu fallback. */
    const char *font_path = getenv("FONT_PATH");
    if (font_path && *font_path && access(font_path, R_OK) == 0)
        snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", font_path);
    else {
        snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf");
        if (access(cfg->font_path, R_OK) != 0)
            snprintf(cfg->font_path, sizeof(cfg->font_path), "%s", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    }

    /* Title font: TITLE_FONT_PATH env, then HOME path, then tools/, then exe-relative. */
    const char *title_font = getenv("TITLE_FONT_PATH");
    if (title_font && *title_font && access(title_font, R_OK) == 0)
        snprintf(cfg->title_font_path, sizeof(cfg->title_font_path), "%s", title_font);
    if (!cfg->title_font_path[0] && getenv("HOME")) {
        snprintf(cfg->title_font_path, sizeof(cfg->title_font_path), "%s/arrival_board/tools/fonts/Smythe-Regular.ttf", getenv("HOME"));
        if (access(cfg->title_font_path, R_OK) != 0) cfg->title_font_path[0] = '\0';
    }
    if (!cfg->title_font_path[0] && access("tools/fonts/Smythe-Regular.ttf", R_OK) == 0)
        snprintf(cfg->title_font_path, sizeof(cfg->title_font_path), "tools/fonts/Smythe-Regular.ttf");
#ifdef __linux__
    if (!cfg->title_font_path[0]) {
        /* exe_path size so exe_path + "/tools/fonts/Smythe-Regular.ttf" fits in title_font_path (512). */
        char exe_path[480];
        ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (n > 0) {
            exe_path[n] = '\0';
            char *slash = strrchr(exe_path, '/');
            if (slash) {
                *slash = '\0';
                snprintf(cfg->title_font_path, sizeof(cfg->title_font_path), "%s/tools/fonts/Smythe-Regular.ttf", exe_path);
                if (access(cfg->title_font_path, R_OK) != 0) cfg->title_font_path[0] = '\0';
            }
        }
    }
#endif
    resolve_absolute(cfg->title_font_path, sizeof(cfg->title_font_path));
    if (cfg->title_font_path[0])
        logf_("Title font path: %s", cfg->title_font_path);
    else
        logf_("Title font path: (none, using default)");

    /* Symbol font: SYMBOL_FONT_PATH env, else Noto Symbols2, else DejaVu fallback. */
    const char *sym = getenv("SYMBOL_FONT_PATH");
    if (sym && *sym && access(sym, R_OK) == 0)
        snprintf(cfg->symbol_font_path, sizeof(cfg->symbol_font_path), "%s", sym);
    else {
        snprintf(cfg->symbol_font_path, sizeof(cfg->symbol_font_path), "%s", "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf");
        if (access(cfg->symbol_font_path, R_OK) != 0)
            snprintf(cfg->symbol_font_path, sizeof(cfg->symbol_font_path), "%s", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    }

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

    /* Music path: single default under HOME/tools. */
    snprintf(cfg->music_path, sizeof(cfg->music_path), "%s/arrival_board/tools/Seaport_Steampunk_Final_Mix.wav", home_dir());
    if (access(cfg->music_path, R_OK) != 0) cfg->music_path[0] = '\0';

    /* Ferry (second loop): single default path. */
    snprintf(cfg->music_loop2_path, sizeof(cfg->music_loop2_path), "%s/arrival_board/tools/SI Ferry.wav", home_dir());
    if (access(cfg->music_loop2_path, R_OK) != 0) cfg->music_loop2_path[0] = '\0';
    resolve_absolute(cfg->music_path, sizeof(cfg->music_path));
    resolve_absolute(cfg->music_loop2_path, sizeof(cfg->music_loop2_path));

    /* Flip sound: single default path. */
    snprintf(cfg->flip_path, sizeof(cfg->flip_path), "%s/arrival_board/tools/ddsm.wav", home_dir());
    if (access(cfg->flip_path, R_OK) != 0) cfg->flip_path[0] = '\0';
    resolve_absolute(cfg->flip_path, sizeof(cfg->flip_path));
}
