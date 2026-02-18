/*
 * Arrival Board: MTA bus arrivals and weather on a full-screen display.
 * Build with: make [USE_SDL_IMAGE=1]. Config via environment (see arrival_board.env.example).
 */
#include "audio.h"
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
#include <unistd.h>

#ifdef USE_SDL_IMAGE
#include <SDL2/SDL_image.h>
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *font_path = getenv("FONT_PATH");
    if (!font_path || !*font_path) font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

    const char *mta_key = getenv("MTA_KEY");
    const char *stop_id = getenv("STOP_ID");
    const char *route_filter = getenv("ROUTE_FILTER");
    int poll = 10;
    const char *poll_s = getenv("POLL_SECONDS");
    if (poll_s && *poll_s) poll = atoi(poll_s);
    if (poll < 5) poll = 5;

    int max_tiles = 12;
    const char *max_s = getenv("MAX_TILES");
    if (max_s && *max_s) max_tiles = atoi(max_s);
    max_tiles = clampi(max_tiles, 1, 24);

    char stop_name[256];
    stop_name[0] = '\0';
    const char *stop_name_override = getenv("STOP_NAME");
    if (stop_name_override && *stop_name_override)
        snprintf(stop_name, sizeof(stop_name), "%s", stop_name_override);

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

    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
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

    SDL_Texture *bg_tex = NULL, *steam_tex = NULL, *logo_tex = NULL;
#ifdef USE_SDL_IMAGE
    texture_load(r, &bg_tex, &steam_tex, &logo_tex);
#endif

    Fonts fonts;
    if (tile_load_fonts(&fonts, font_path, H) != 0) {
        logf_("Failed to load font at %s", font_path);
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    const char *symbol_font_path = getenv("SYMBOL_FONT_PATH");
    if (!symbol_font_path || !*symbol_font_path) symbol_font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    float sym_scale = layout_scale(H);
    int sym_pt = clampi((int)(58.f * sym_scale), 26, 120);
    TTF_Font *symbol_font = TTF_OpenFont(symbol_font_path, sym_pt);
    if (!symbol_font) {
        const char *fallbacks[] = {
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            NULL
        };
        for (int i = 0; fallbacks[i] && !symbol_font; i++)
            symbol_font = TTF_OpenFont(fallbacks[i], sym_pt);
    }

    Weather wx;
    memset(&wx, 0, sizeof(wx));
    wx.precip_prob = -1;
    wx.precip_in = -1.0;

    Arrival arrivals[32];
    int n = 0;
    time_t last_fetch = 0;
    Arrival prev_arrivals[32];
    int n_prev = 0;

    /* Audio paths */
    const char *aplay_dev = getenv("APLAY_DEVICE");
    const char *music_env = getenv("MUSIC_FILE");
    if (!music_env || !*music_env) music_env = getenv("BACKGROUND_MUSIC");
    const char *flip_path = getenv("SOUND_FLIP");
    char music_path[512], flip_path_buf[512];
    music_path[0] = '\0';
    flip_path_buf[0] = '\0';
    if (music_env && *music_env) {
        snprintf(music_path, sizeof(music_path), "%s", music_env);
    } else if (getenv("HOME")) {
        snprintf(music_path, sizeof(music_path), "%s/arrival_board/tools/Seaport_Steampunk_Final_Mix.wav", getenv("HOME"));
        if (access(music_path, R_OK) != 0)
            snprintf(music_path, sizeof(music_path), "tools/Seaport_Steampunk_Final_Mix.wav");
    } else {
        snprintf(music_path, sizeof(music_path), "tools/Seaport_Steampunk_Final_Mix.wav");
    }
    if (!flip_path || !*flip_path) {
        if (getenv("HOME")) {
            snprintf(flip_path_buf, sizeof(flip_path_buf), "%s/arrival_board/tools/flip.wav", getenv("HOME"));
            if (access(flip_path_buf, R_OK) == 0) flip_path = flip_path_buf;
        }
        if ((!flip_path || !*flip_path) && access("tools/flip.wav", R_OK) == 0) flip_path = "tools/flip.wav";
    }
    if (aplay_dev && *aplay_dev && access(music_path, R_OK) == 0)
        audio_start_music(music_path, aplay_dev);

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))
                goto done;
        }

        time_t now = time(NULL);
        if (!last_fetch || difftime(now, last_fetch) >= poll) {
            memcpy(prev_arrivals, arrivals, sizeof(prev_arrivals));
            n_prev = n;
            char sn[256];
            sn[0] = '\0';
            n = fetch_mta_arrivals(arrivals, max_tiles, sn, sizeof(sn), mta_key, stop_id, route_filter);
            if (!stop_name[0] && sn[0]) snprintf(stop_name, sizeof(stop_name), "%s", sn);
            fetch_weather(&wx, stop_name[0] ? stop_name : NULL);
            if (n_prev > 0 && aplay_dev && *aplay_dev && flip_path && *flip_path &&
                audio_should_play_flip(arrivals, n, prev_arrivals, n_prev))
                audio_play_flip(flip_path, aplay_dev);
            last_fetch = now;
        }

        SDL_GetRendererOutputSize(r, &W, &H);
        ui_render(r, &fonts, W, H, stop_id ? stop_id : "--", stop_name, &wx, arrivals, n,
                  bg_tex, steam_tex, logo_tex, symbol_font);

        SDL_Delay(80);
    }

done:
    audio_stop_music();
    if (bg_tex) SDL_DestroyTexture(bg_tex);
    if (steam_tex) SDL_DestroyTexture(steam_tex);
    if (logo_tex) SDL_DestroyTexture(logo_tex);
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
