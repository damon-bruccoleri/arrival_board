/*
 * Arrival Board: MTA bus arrivals and weather on a full-screen display.
 * Build with: make [USE_SDL_IMAGE=1]. Config via environment (see arrival_board.env.example).
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
#include <time.h>
#include <unistd.h>

#ifdef USE_SDL_IMAGE
#include <SDL2/SDL_image.h>
#endif

typedef struct {
    const char *flip_path;
    const char *aplay_device;
    const char *music_path;
    const char *music_loop2_path;
} FlipSoundCtx;

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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        logf_("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return 1;
    }
#ifdef USE_SDL_IMAGE
    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0)
        logf_("IMG_Init PNG failed: %s", IMG_GetError());
#endif

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    SDL_Window *win = SDL_CreateWindow("Arrival Board",
                                      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      1280, 720,
                                      SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        logf_("CreateWindow failed: %s", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Uint32 rflags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer *r = SDL_CreateRenderer(win, -1, rflags);
    if (!r) {
        logf_("CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    int W = 0, H = 0;
    SDL_GetRendererOutputSize(r, &W, &H);
    if (W <= 0 || H <= 0) SDL_GetWindowSize(win, &W, &H);

    SDL_Texture *bg_tex = NULL, *steam_tex = NULL, *logo_tex = NULL, *wide_tile_tex = NULL, *narrow_tile_tex = NULL;
#ifdef USE_SDL_IMAGE
    texture_load(r, &bg_tex, &steam_tex, &logo_tex, &wide_tile_tex, &narrow_tile_tex);
#endif

    Fonts fonts;
    if (tile_load_fonts(&fonts, cfg.font_path, cfg.title_font_path[0] ? cfg.title_font_path : NULL, H) != 0) {
        logf_("Failed to load font at %s", cfg.font_path);
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    float sym_scale = layout_scale(H);
    int sym_pt = clampi((int)(58.f * sym_scale), 26, 120);
    TTF_Font *symbol_font = TTF_OpenFont(cfg.symbol_font_path, sym_pt);
    if (!symbol_font) {
        logf_("Failed to load symbol font at %s: %s", cfg.symbol_font_path, TTF_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "Arrival Board",
                                 "Failed to load symbol font.\n"
                                 "Check SYMBOL_FONT_PATH.\n"
                                 "See boot.log for details.",
                                 win);
        tile_free_fonts(&fonts);
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Weather wx;
    memset(&wx, 0, sizeof(wx));
    wx.precip_prob = -1;
    wx.precip_in = -1.0;
    wx.moon_phase = -1.f;

    Arrival arrivals[32];
    int n = 0;
    time_t last_fetch = 0;
    time_t last_gtfs_load = 0;
    Arrival prev_arrivals[32];
    int n_prev = 0;
    ScheduledDeparture scheduled[SCHEDULED_MAX];
    int n_scheduled = 0;

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

    gtfs_load(cfg.gtfs_url, cfg.gtfs_cache);
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
            memcpy(prev_arrivals, arrivals, sizeof(prev_arrivals));
            n_prev = n;
            char sn[256];
            sn[0] = '\0';
            n = fetch_mta_arrivals(arrivals, cfg.max_tiles, sn, sizeof(sn),
                                   cfg.mta_key, cfg.stop_id,
                                   cfg.route_filter[0] ? cfg.route_filter : NULL);
            if (!stop_name[0] && sn[0]) snprintf(stop_name, sizeof(stop_name), "%s", sn);
            fetch_weather(&wx, stop_name[0] ? stop_name : NULL);

            /* Flip sound is played when animation ends (via on_flip_ended callback), not here. */
            last_fetch = now;

            mta_log_realtime_express_routes(arrivals, n);

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
            last_gtfs_load = now;
        }

        SDL_GetRendererOutputSize(r, &W, &H);
        static FlipSoundCtx flip_ctx;
        if (cfg.flip_path[0]) {
            flip_ctx.flip_path = cfg.flip_path;
            flip_ctx.aplay_device = cfg.aplay_device;
            flip_ctx.music_path = cfg.music_path;
            flip_ctx.music_loop2_path = cfg.music_loop2_path;
        }
        ui_render(r, &fonts, W, H,
                  cfg.stop_id[0] ? cfg.stop_id : "--", stop_name, &wx,
                  arrivals, n, prev_arrivals, n_prev,
                  scheduled, n_scheduled,
                  bg_tex, steam_tex, logo_tex, wide_tile_tex, narrow_tile_tex, symbol_font,
                  cfg.flip_path[0] ? on_flip_ended : NULL,
                  cfg.flip_path[0] ? (void*)&flip_ctx : NULL);

        SDL_Delay(16);
    }

done:
    audio_stop_music();
    if (bg_tex) SDL_DestroyTexture(bg_tex);
    if (steam_tex) SDL_DestroyTexture(steam_tex);
    if (logo_tex) SDL_DestroyTexture(logo_tex);
    if (wide_tile_tex) SDL_DestroyTexture(wide_tile_tex);
    if (narrow_tile_tex) SDL_DestroyTexture(narrow_tile_tex);
    if (symbol_font) TTF_CloseFont(symbol_font);
    tile_free_fonts(&fonts);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
#ifdef USE_SDL_IMAGE
    IMG_Quit();
#endif
    TTF_Quit();
    SDL_Quit();
    return 0;
}
