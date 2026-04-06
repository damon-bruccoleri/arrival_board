/*
 * Arrival Board: MTA bus arrivals and weather on a full-screen display.
 * Build: make (requires libsdl2-image-dev). Config via environment (see arrival_board.env.example).
 */
#include "audio.h"
#include "config.h"
#include "gtfs.h"
#include "mta.h"
#include "tile.h"
#include "texture.h"
#include "types.h"
#include "ui.h"
#include "util.h"
#include "weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL_image.h>

typedef struct {
    const char *flip_path;
    const char *aplay_device;
    const char *music_path;
    const char *music_loop2_path;
} FlipSoundCtx;

typedef struct {
    const char *name;
    int consecutive_failures;
    int total_failures;
    time_t last_success;
    time_t last_failure;
    int initialized;
} SourceHealth;

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *renderer;
    Fonts         fonts;
    TTF_Font     *symbol_font;
    TTF_Font     *emoji_font;
    SDL_Texture  *bg_tex;
    SDL_Texture  *steam_tex;
    SDL_Texture  *logo_tex;
    SDL_Texture  *wide_tile_tex;
    SDL_Texture  *narrow_tile_tex;
} Resources;

static void resources_destroy(Resources *res) {
    if (res->bg_tex)          SDL_DestroyTexture(res->bg_tex);
    if (res->steam_tex)       SDL_DestroyTexture(res->steam_tex);
    if (res->logo_tex)        SDL_DestroyTexture(res->logo_tex);
    if (res->wide_tile_tex)   SDL_DestroyTexture(res->wide_tile_tex);
    if (res->narrow_tile_tex) SDL_DestroyTexture(res->narrow_tile_tex);
    if (res->symbol_font)     TTF_CloseFont(res->symbol_font);
    if (res->emoji_font)      TTF_CloseFont(res->emoji_font);
    tile_free_fonts(&res->fonts);
    if (res->renderer)        SDL_DestroyRenderer(res->renderer);
    if (res->win)             SDL_DestroyWindow(res->win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

static int fatal_font_error(Resources *res, const char *boot_msg,
                             const char *path, const char *env_var,
                             const char *install_hint) {
    log_to_boot_log("arrival_board: %s; terminating.", boot_msg);
    logf_("Failed to load font: %s", path[0] ? path : "(empty)");
    fprintf(stderr, "Arrival Board: Font missing.\nPath: %s\n"
            "Fix: Set %s to a valid .ttf file, or install (e.g. sudo apt install %s).\n"
            "See boot.log for details.\n", path[0] ? path : "(none)", env_var, install_hint);
    char msg[512];
    snprintf(msg, sizeof(msg),
             "Font could not be loaded.\nSet %s to a valid .ttf file, "
             "or install (e.g. sudo apt install %s).\nSee boot.log for details.",
             env_var, install_hint);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Arrival Board", msg, res->win);
    resources_destroy(res);
    return 1;
}

static void source_health_update(SourceHealth *s, int ok, const char *reason, time_t now) {
    if (!s) return;
    if (ok) {
        if (s->initialized && s->consecutive_failures > 0)
            logf_("SRC_RECOVER source=%s after_failures=%d reason=%s",
                  s->name, s->consecutive_failures, reason ? reason : "OK");
        s->consecutive_failures = 0;
        s->last_success = now;
    } else {
        s->consecutive_failures++;
        s->total_failures++;
        s->last_failure = now;
        if (!s->initialized || s->consecutive_failures == 1 || (s->consecutive_failures % 10) == 0)
            logf_("SRC_FAIL source=%s consecutive=%d total=%d reason=%s",
                  s->name, s->consecutive_failures, s->total_failures, reason ? reason : "UNKNOWN");
    }
    s->initialized = 1;
}

static void on_flip_ended(void *userdata) {
    FlipSoundCtx *ctx = (FlipSoundCtx *)userdata;
    if (!ctx || !ctx->flip_path || !ctx->flip_path[0]) return;
    if (getenv("AUDIO_DEBUG"))
        fprintf(stderr, "AUDIO_DEBUG: flip ended, playing sound\n");
    if (ctx->aplay_device && ctx->aplay_device[0]) {
        audio_stop_music();
        audio_play_flip(ctx->flip_path, ctx->aplay_device);
        if (ctx->music_path && access(ctx->music_path, R_OK) == 0)
            audio_start_music(ctx->music_path,
                              ctx->music_loop2_path && ctx->music_loop2_path[0] ? ctx->music_loop2_path : NULL,
                              ctx->aplay_device);
    } else {
        audio_play_flip(ctx->flip_path, NULL);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!getenv("AUDIO_DEBUG")) setenv("AUDIO_DEBUG", "1", 0);

    AppConfig cfg;
    config_from_env(&cfg);

    Resources res;
    memset(&res, 0, sizeof(res));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        logf_("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return 1;
    }
    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0)
        logf_("IMG_Init PNG failed: %s", IMG_GetError());

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    res.win = SDL_CreateWindow("Arrival Board",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!res.win) {
        logf_("CreateWindow failed: %s", SDL_GetError());
        resources_destroy(&res);
        return 1;
    }

    Uint32 rflags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    res.renderer = SDL_CreateRenderer(res.win, -1, rflags);
    if (!res.renderer) {
        logf_("CreateRenderer failed: %s", SDL_GetError());
        resources_destroy(&res);
        return 1;
    }
    SDL_Renderer *r = res.renderer;

    int W = 0, H = 0;
    SDL_GetRendererOutputSize(r, &W, &H);
    if (W <= 0 || H <= 0) SDL_GetWindowSize(res.win, &W, &H);

    texture_load(r, &res.bg_tex, &res.steam_tex, &res.logo_tex,
                 &res.wide_tile_tex, &res.narrow_tile_tex);
    if (!res.bg_tex)
        logf_("Background image not loaded (set BACKGROUND_IMAGE or add Steampunk bus image.png); body area will be solid color only.");
    if (!res.steam_tex) {
        log_to_boot_log("arrival_board: steam_puff.png missing, corrupt, or failed to load; terminating.");
        resources_destroy(&res);
        return 1;
    }

    if (tile_load_fonts(&res.fonts, cfg.font_path,
                        cfg.title_font_path[0] ? cfg.title_font_path : NULL, H) != 0)
        return fatal_font_error(&res, "body/title font failed to load",
                                cfg.font_path, "FONT_PATH", "fonts-noto-core");

    float sym_scale = layout_scale(H);
    int sym_pt = clampi((int)(58.f * sym_scale), 26, 120);
    /* symbol_font not currently used by any render path; skip loading to save RAM */
    res.symbol_font = NULL;

    int emoji_pt = clampi(sym_pt / 2, 12, 120);
    res.emoji_font = TTF_OpenFont(cfg.emoji_font_path, emoji_pt);
    if (!res.emoji_font)
        return fatal_font_error(&res, "emoji font failed to load",
                                cfg.emoji_font_path, "EMOJI_FONT_PATH", "fonts-noto-color-emoji");

    Weather wx;
    memset(&wx, 0, sizeof(wx));
    wx.precip_prob = -1;
    wx.precip_in = -1.0;
    wx.moon_phase = -1.f;

    Arrival arrivals[32];
    Arrival prev_arrivals[32];
    int n = 0;
    int prev_n = 0;
    time_t last_fetch = 0;
    time_t last_gtfs_load = 0;
    time_t last_health_log = 0;
    ScheduledDeparture scheduled[SCHEDULED_MAX];
    int n_scheduled = 0;
    SourceHealth mta_health = { "MTA", 0, 0, 0, 0, 0 };
    SourceHealth weather_health = { "WEATHER", 0, 0, 0, 0, 0 };
    SourceHealth gtfs_health = { "GTFS_LOAD", 0, 0, 0, 0, 0 };

    char stop_name[256];
    stop_name[0] = '\0';
    if (cfg.stop_name_override[0])
        snprintf(stop_name, sizeof(stop_name), "%s", cfg.stop_name_override);

    if (getenv("AUDIO_DEBUG")) {
        fprintf(stderr, "AUDIO_DEBUG: music=%s\n", cfg.music_path[0] ? cfg.music_path : "(none)");
        fprintf(stderr, "AUDIO_DEBUG: music_loop2=%s\n", cfg.music_loop2_path[0] ? cfg.music_loop2_path : "(none)");
        fprintf(stderr, "AUDIO_DEBUG: flip=%s\n", cfg.flip_path[0] ? cfg.flip_path : "(none)");
        fprintf(stderr, "AUDIO_DEBUG: device=%s\n", cfg.aplay_device[0] ? cfg.aplay_device : "(Pulse)");
    }
    if (access(cfg.music_path, R_OK) == 0)
        audio_start_music(cfg.music_path,
                          cfg.music_loop2_path[0] ? cfg.music_loop2_path : NULL,
                          cfg.aplay_device[0] ? cfg.aplay_device : NULL);
    else if (getenv("AUDIO_DEBUG"))
        fprintf(stderr, "AUDIO_DEBUG: music file not found, skipping audio\n");

    {
        SDL_SetRenderDrawColor(r, 10, 12, 16, 255);
        SDL_RenderClear(r);
        ui_render(r, &res.fonts, W, H,
                  cfg.stop_id[0] ? cfg.stop_id : "--", stop_name, &wx,
                  arrivals, 0,
                  scheduled, 0,
                  res.bg_tex, res.steam_tex, res.logo_tex,
                  res.wide_tile_tex, res.narrow_tile_tex,
                  res.symbol_font, res.emoji_font,
                  NULL, NULL);
    }

    gtfs_load(cfg.gtfs_url, cfg.gtfs_cache);
    source_health_update(&gtfs_health, gtfs_last_status() == 0, gtfs_last_status_str(), time(NULL));
    last_gtfs_load = time(NULL);

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))
                goto done;
        }

        time_t now = time(NULL);
        if (!last_fetch || difftime(now, last_fetch) >= cfg.poll_seconds) {
            /* Preserve previous arrivals to detect buses leaving the stop. */
            prev_n = n;
            if (prev_n > 32) prev_n = 32;
            if (prev_n > 0) memcpy(prev_arrivals, arrivals, sizeof(Arrival) * prev_n);

            char sn[256];
            sn[0] = '\0';
            int n_new = fetch_mta_arrivals(arrivals, cfg.max_tiles, sn, sizeof(sn),
                                           cfg.mta_key, cfg.stop_id,
                                           cfg.route_filter[0] ? cfg.route_filter : NULL);
            if (n_new < 0) {
                n = prev_n;
                if (n > 0) memcpy(arrivals, prev_arrivals, sizeof(Arrival) * (size_t)n);
            } else {
                n = n_new;
            }
            source_health_update(&mta_health, mta_last_status() == 0, mta_last_status_str(), now);
            if (!stop_name[0] && sn[0]) snprintf(stop_name, sizeof(stop_name), "%s", sn);
            fetch_weather(&wx, stop_name[0] ? stop_name : NULL);
            {
                int ws = weather_last_status();
                int weather_ok = (ws == 0 || ws == 1); /* THROTTLED is expected between successful polls. */
                source_health_update(&weather_health, weather_ok, weather_last_status_str(), now);
            }

            /* Flip sound is played when animation ends (via on_flip_ended callback), not here. */
            last_fetch = now;

            mta_log_realtime_express_routes(arrivals, n);

            /* If any bus that was at or about to leave the stop (mins<=1) disappeared from the feed, play ferry sound. */
            const char *ferry_path = cfg.music_loop2_path[0] ? cfg.music_loop2_path : cfg.flip_path;
            if (ferry_path && ferry_path[0] && prev_n > 0) {
                for (int pi = 0; pi < prev_n; pi++) {
                    const Arrival *pa = &prev_arrivals[pi];
                    if (pa->mins > 1 || pa->mins < 0 || !pa->bus[0]) continue;
                    int still_present = 0;
                    for (int ci = 0; ci < n; ci++) {
                        if (strcmp(arrivals[ci].bus, pa->bus) == 0) {
                            still_present = 1;
                            break;
                        }
                    }
                    if (!still_present) {
                        logf_("Bus left stop: playing ferry sound");
                        audio_play_flip(ferry_path,
                                        cfg.aplay_device[0] ? cfg.aplay_device : NULL);
                        break;
                    }
                }
            }

            n_scheduled = 0;
            if (cfg.stop_id[0]) {
                int nt = gtfs_next_departures(cfg.stop_id, NULL, scheduled, SCHEDULED_MAX);
                int out = 0;
                for (int i = 0; i < nt; i++) {
                    int in_realtime = 0;
                    for (int j = 0; j < n; j++)
                        if (strcmp(scheduled[i].route, arrivals[j].route) == 0) { in_realtime = 1; break; }
                    if (!in_realtime)
                        scheduled[out++] = scheduled[i];
                }
                n_scheduled = out;
            }
        }

        if (!last_gtfs_load || difftime(now, last_gtfs_load) >= 86400) {
            gtfs_load(cfg.gtfs_url, cfg.gtfs_cache);
            source_health_update(&gtfs_health, gtfs_last_status() == 0, gtfs_last_status_str(), now);
            last_gtfs_load = now;
        }

        if (!last_health_log || difftime(now, last_health_log) >= 600) {
            logf_("SRC_HEARTBEAT mta_n=%d weather_have=%d scheduled_n=%d mta_fail=%d weather_fail=%d gtfs_fail=%d",
                  n, wx.have, n_scheduled,
                  mta_health.consecutive_failures,
                  weather_health.consecutive_failures,
                  gtfs_health.consecutive_failures);
            last_health_log = now;
        }

        /* Reap zombie child processes (audio fork/exec) to prevent process table exhaustion. */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}

        SDL_GetRendererOutputSize(r, &W, &H);
        static FlipSoundCtx flip_ctx;
        if (cfg.flip_path[0]) {
            flip_ctx.flip_path = cfg.flip_path;
            flip_ctx.aplay_device = cfg.aplay_device;
            flip_ctx.music_path = cfg.music_path;
            flip_ctx.music_loop2_path = cfg.music_loop2_path;
        }
        n = arrivals_refresh_eta(arrivals, n, time(NULL));
        ui_render(r, &res.fonts, W, H,
                  cfg.stop_id[0] ? cfg.stop_id : "--", stop_name, &wx,
                  arrivals, n,
                  scheduled, n_scheduled,
                  res.bg_tex, res.steam_tex, res.logo_tex,
                  res.wide_tile_tex, res.narrow_tile_tex,
                  res.symbol_font, res.emoji_font,
                  cfg.flip_path[0] ? on_flip_ended : NULL,
                  cfg.flip_path[0] ? (void*)&flip_ctx : NULL);

        SDL_Delay(16);
    }

done:
    audio_stop_music();
    resources_destroy(&res);
    return 0;
}
