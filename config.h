/*
 * Application config from environment.
 * Resolves paths (music, flip, GTFS cache) and fills a single struct for main.
 */
#pragma once

typedef struct AppConfig {
    char font_path[512];
    char title_font_path[512];  /* optional: set TITLE_FONT_PATH for "Arrival Board" header; no fallback */
    char symbol_font_path[512];
    char emoji_font_path[512];  /* required for moon/weather glyphs; no fallback */
    char mta_key[128];
    char stop_id[64];
    char route_filter[256];
    int poll_seconds;
    int max_tiles;
    char stop_name_override[256];
    char gtfs_url[512];
    char gtfs_cache[512];
    char music_path[512];
    char music_loop2_path[512];
    char flip_path[512];
    char aplay_device[128];
} AppConfig;

/* Fill config from environment. Paths resolved with realpath where applicable. */
void config_from_env(AppConfig *cfg);
