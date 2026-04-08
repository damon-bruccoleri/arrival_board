/*
 * Arrival Board: MTA bus arrivals and weather on a full-screen display.
 * Build: make (requires libsdl2-image-dev). Config via environment (see arrival_board.env.example).
 *
 * All blocking I/O (MTA API, weather API, GTFS parsing) runs on a background
 * thread so the SDL render loop stays smooth at ~60 fps even on a Pi Zero W.
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

#include <pthread.h>
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

/* Mutex-protected state shared between the fetch thread and the render loop. */
typedef struct {
    pthread_mutex_t lock;
    pthread_t       tid;

    Arrival             arrivals[TILE_SLOTS_MAX];
    int                 n;
    ScheduledDeparture  scheduled[SCHEDULED_MAX];
    int                 n_scheduled;
    Weather             weather;
    char                stop_name[256];
    int                 generation;

    AppConfig           cfg;
    volatile int        running;
} FetchCtx;

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

/* ---- Background fetch thread ------------------------------------------------
 * Runs MTA, weather, and GTFS queries in a loop.  Results are published to
 * FetchCtx under the mutex; the render thread copies them each frame.
 * -------------------------------------------------------------------------- */
static void *fetch_loop(void *arg) {
    FetchCtx *ctx = (FetchCtx *)arg;
    AppConfig *cfg = &ctx->cfg;

    SourceHealth mta_h    = { "MTA",       0, 0, 0, 0, 0 };
    SourceHealth wx_h     = { "WEATHER",   0, 0, 0, 0, 0 };
    SourceHealth gtfs_h   = { "GTFS_LOAD", 0, 0, 0, 0, 0 };
    time_t last_gtfs_load = 0;
    time_t last_health_log = 0;

    Weather persist_wx;
    memset(&persist_wx, 0, sizeof(persist_wx));
    persist_wx.precip_prob = -1;
    persist_wx.precip_in   = -1.0;
    persist_wx.moon_phase  = -1.f;

    gtfs_load(cfg->gtfs_url, cfg->gtfs_cache);
    source_health_update(&gtfs_h, gtfs_last_status() == 0, gtfs_last_status_str(), time(NULL));
    last_gtfs_load = time(NULL);

    while (ctx->running) {
        time_t now = time(NULL);

        /* --- MTA real-time arrivals --- */
        Arrival local_arr[TILE_SLOTS_MAX];
        memset(local_arr, 0, sizeof(local_arr));
        char sn[256] = {0};
        int n_new = fetch_mta_arrivals(local_arr, cfg->max_tiles, sn, sizeof(sn),
                                       cfg->mta_key, cfg->stop_id,
                                       cfg->route_filter[0] ? cfg->route_filter : NULL);
        now = time(NULL);
        source_health_update(&mta_h, mta_last_status() == 0, mta_last_status_str(), now);
        mta_log_realtime_express_routes(local_arr, n_new >= 0 ? n_new : 0);

        /* --- Weather --- */
        char wx_name[256] = {0};
        pthread_mutex_lock(&ctx->lock);
        snprintf(wx_name, sizeof(wx_name), "%s", ctx->stop_name);
        pthread_mutex_unlock(&ctx->lock);
        if (!wx_name[0] && sn[0])
            snprintf(wx_name, sizeof(wx_name), "%s", sn);

        fetch_weather(&persist_wx, wx_name[0] ? wx_name : NULL);
        source_health_update(&wx_h,
                             (weather_last_status() == 0 || weather_last_status() == 1),
                             weather_last_status_str(), time(NULL));

        /* --- GTFS scheduled departures, excluding routes already in real-time --- */
        ScheduledDeparture local_sched[SCHEDULED_MAX];
        int n_sched = 0;
        if (cfg->stop_id[0]) {
            int nt = gtfs_next_departures(cfg->stop_id, NULL, local_sched, SCHEDULED_MAX);
            int out = 0;
            int n_rt = (n_new >= 0) ? n_new : 0;
            for (int i = 0; i < nt; i++) {
                int dup = 0;
                for (int j = 0; j < n_rt; j++)
                    if (strcmp(local_sched[i].route, local_arr[j].route) == 0) { dup = 1; break; }
                if (!dup) local_sched[out++] = local_sched[i];
            }
            n_sched = out;
        }

        /* --- Publish results --- */
        pthread_mutex_lock(&ctx->lock);
        if (n_new >= 0) {
            memcpy(ctx->arrivals, local_arr, sizeof(Arrival) * (size_t)n_new);
            ctx->n = n_new;
        }
        if (!ctx->stop_name[0] && sn[0])
            snprintf(ctx->stop_name, sizeof(ctx->stop_name), "%s", sn);
        ctx->weather = persist_wx;
        memcpy(ctx->scheduled, local_sched, sizeof(ScheduledDeparture) * (size_t)n_sched);
        ctx->n_scheduled = n_sched;
        ctx->generation++;
        pthread_mutex_unlock(&ctx->lock);

        /* --- Daily GTFS zip refresh --- */
        now = time(NULL);
        if (difftime(now, last_gtfs_load) >= 86400) {
            gtfs_load(cfg->gtfs_url, cfg->gtfs_cache);
            source_health_update(&gtfs_h, gtfs_last_status() == 0, gtfs_last_status_str(), now);
            last_gtfs_load = now;
        }

        /* --- Heartbeat --- */
        if (!last_health_log || difftime(now, last_health_log) >= 600) {
            pthread_mutex_lock(&ctx->lock);
            int hn = ctx->n, hns = ctx->n_scheduled, hwx = ctx->weather.have;
            pthread_mutex_unlock(&ctx->lock);
            logf_("SRC_HEARTBEAT mta_n=%d weather_have=%d scheduled_n=%d mta_fail=%d weather_fail=%d gtfs_fail=%d",
                  hn, hwx, hns, mta_h.consecutive_failures, wx_h.consecutive_failures, gtfs_h.consecutive_failures);
            last_health_log = now;
        }

        /* Sleep in 1-second chunks so the thread exits promptly on shutdown. */
        for (int s = 0; s < cfg->poll_seconds && ctx->running; s++)
            sleep(1);
    }
    return NULL;
}

/* ---- Main ---------------------------------------------------------------- */

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
    (void)sym_pt;
    res.symbol_font = NULL;

    int emoji_pt = clampi((int)(58.f * sym_scale) / 2, 12, 120);
    res.emoji_font = TTF_OpenFont(cfg.emoji_font_path, emoji_pt);
    if (!res.emoji_font)
        return fatal_font_error(&res, "emoji font failed to load",
                                cfg.emoji_font_path, "EMOJI_FONT_PATH", "fonts-noto-color-emoji");

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

    /* Draw one immediate frame so the display is not blank while the fetch
     * thread performs its initial GTFS download and first MTA poll. */
    char init_sn[256] = {0};
    if (cfg.stop_name_override[0])
        snprintf(init_sn, sizeof(init_sn), "%s", cfg.stop_name_override);
    {
        Weather empty_wx;
        memset(&empty_wx, 0, sizeof(empty_wx));
        empty_wx.precip_prob = -1;
        empty_wx.precip_in   = -1.0;
        empty_wx.moon_phase  = -1.f;
        SDL_SetRenderDrawColor(r, 10, 12, 16, 255);
        SDL_RenderClear(r);
        ui_render(r, &res.fonts, W, H,
                  cfg.stop_id[0] ? cfg.stop_id : "--", init_sn, &empty_wx,
                  NULL, 0, NULL, 0,
                  res.bg_tex, res.steam_tex, res.logo_tex,
                  res.wide_tile_tex, res.narrow_tile_tex,
                  res.symbol_font, res.emoji_font,
                  NULL, NULL);
    }

    /* ---- Start background fetch thread ----------------------------------- */
    static FetchCtx fctx;
    memset(&fctx, 0, sizeof(fctx));
    pthread_mutex_init(&fctx.lock, NULL);
    fctx.cfg     = cfg;
    fctx.running = 1;
    fctx.weather.precip_prob = -1;
    fctx.weather.precip_in   = -1.0;
    fctx.weather.moon_phase  = -1.f;
    if (cfg.stop_name_override[0])
        snprintf(fctx.stop_name, sizeof(fctx.stop_name), "%s", cfg.stop_name_override);
    pthread_create(&fctx.tid, NULL, fetch_loop, &fctx);

    /* ---- Render-loop local state ----------------------------------------- */
    Arrival local_arr[TILE_SLOTS_MAX];
    memset(local_arr, 0, sizeof(local_arr));
    int local_n = 0;

    ScheduledDeparture local_sched[SCHEDULED_MAX];
    int local_ns = 0;

    Weather local_wx;
    memset(&local_wx, 0, sizeof(local_wx));
    local_wx.precip_prob = -1;
    local_wx.precip_in   = -1.0;
    local_wx.moon_phase  = -1.f;

    char local_sn[256] = {0};
    if (cfg.stop_name_override[0])
        snprintf(local_sn, sizeof(local_sn), "%s", cfg.stop_name_override);

    int local_gen = -1;
    const char *ferry_path = cfg.music_loop2_path[0] ? cfg.music_loop2_path : cfg.flip_path;

    /* ---- Main render loop ------------------------------------------------ */
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))
                goto done;
        }

        /* Reap zombie child processes (audio fork/exec). */
        while (waitpid(-1, NULL, WNOHANG) > 0) {}

        /* Pick up new data from the fetch thread (fast: just a memcpy under lock). */
        int play_ferry = 0;
        pthread_mutex_lock(&fctx.lock);
        if (fctx.generation != local_gen) {
            /* Detect bus departures (bus with mins<=1 that disappeared). */
            if (ferry_path && ferry_path[0] && local_n > 0) {
                for (int pi = 0; pi < local_n; pi++) {
                    if (local_arr[pi].mins > 1 || local_arr[pi].mins < 0 || !local_arr[pi].bus[0]) continue;
                    int still = 0;
                    for (int ci = 0; ci < fctx.n; ci++)
                        if (strcmp(fctx.arrivals[ci].bus, local_arr[pi].bus) == 0) { still = 1; break; }
                    if (!still) { play_ferry = 1; break; }
                }
            }
            memcpy(local_arr, fctx.arrivals, sizeof(Arrival) * (size_t)fctx.n);
            local_n = fctx.n;
            memcpy(local_sched, fctx.scheduled, sizeof(ScheduledDeparture) * (size_t)fctx.n_scheduled);
            local_ns = fctx.n_scheduled;
            local_wx = fctx.weather;
            if (fctx.stop_name[0])
                snprintf(local_sn, sizeof(local_sn), "%s", fctx.stop_name);
            local_gen = fctx.generation;
        }
        pthread_mutex_unlock(&fctx.lock);

        if (play_ferry) {
            logf_("Bus left stop: playing ferry sound");
            audio_play_flip(ferry_path, cfg.aplay_device[0] ? cfg.aplay_device : NULL);
        }

        SDL_GetRendererOutputSize(r, &W, &H);
        static FlipSoundCtx flip_ctx;
        if (cfg.flip_path[0]) {
            flip_ctx.flip_path       = cfg.flip_path;
            flip_ctx.aplay_device    = cfg.aplay_device;
            flip_ctx.music_path      = cfg.music_path;
            flip_ctx.music_loop2_path = cfg.music_loop2_path;
        }
        local_n = arrivals_refresh_eta(local_arr, local_n, time(NULL));
        ui_render(r, &res.fonts, W, H,
                  cfg.stop_id[0] ? cfg.stop_id : "--", local_sn, &local_wx,
                  local_arr, local_n,
                  local_sched, local_ns,
                  res.bg_tex, res.steam_tex, res.logo_tex,
                  res.wide_tile_tex, res.narrow_tile_tex,
                  res.symbol_font, res.emoji_font,
                  cfg.flip_path[0] ? on_flip_ended : NULL,
                  cfg.flip_path[0] ? (void *)&flip_ctx : NULL);

        SDL_Delay(16);
    }

done:
    fctx.running = 0;
    pthread_join(fctx.tid, NULL);
    pthread_mutex_destroy(&fctx.lock);
    audio_stop_music();
    resources_destroy(&res);
    return 0;
}
