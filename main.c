/*
 * Arrival Board: MTA bus arrivals and weather on a full-screen display.
 * Build with: make [USE_SDL_IMAGE=1]. Config via environment (see arrival_board.env.example).
 */
#include "mta.h"
#include "tile.h"
#include "types.h"
#include "util.h"
#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_SDL_IMAGE
#include <SDL2/SDL_image.h>
#endif

#define LAYOUT_VER "2"

/* Steam puff effect: number of puffs and size of the source texture. */
#define STEAM_PUFFS       2
#define STEAM_PUFF_SIZE   96

typedef struct SteamPuff {
    float x, y;
    float alpha;
    float scale;
    float rise;
} SteamPuff;

/* --- Layout constants (scaled from LAYOUT_REF_HEIGHT in types.h) --- */
static float layout_scale(int H) {
    return (H > 0) ? ((float)H / (float)LAYOUT_REF_HEIGHT) : 1.0f;
}

/* --- Tile content: one bus card (route, destination, ETA, meta line) --- */
static void draw_tile_content(SDL_Renderer *r, Fonts *f, const Arrival *a,
                              SDL_Rect rect, float scale,
                              SDL_Color white, SDL_Color dim, int radius) {
    int inner = clampi((int)(32 * scale), 12, 60);
    int x = rect.x + inner;
    int y = rect.y + clampi((int)(20 * scale), 8, 40);

    SDL_SetRenderDrawColor(r, 18, 20, 26, 255);
    fill_round_rect(r, rect, radius);

    char busnum[64];
    const char *busid = a->bus;
    if (!busid || !busid[0])
        snprintf(busnum, sizeof(busnum), "--");
    else {
        const char *p = strrchr(busid, '_');
        if (p && p[1]) busid = p + 1;
        else {
            const char *sp = strrchr(busid, ' ');
            if (sp && sp[1]) busid = sp + 1;
        }
        snprintf(busnum, sizeof(busnum), "%s", busid);
    }

    const char *route = a->route[0] ? a->route : "--";
    const char *dest  = a->dest[0]  ? a->dest  : "--";
    char minsbuf[16];
    if (a->mins == 0) snprintf(minsbuf, sizeof(minsbuf), "NOW");
    else if (a->mins > 0) snprintf(minsbuf, sizeof(minsbuf), "%d", a->mins);
    else snprintf(minsbuf, sizeof(minsbuf), "--");

    int route_w = 0, eta_w = 0;
    text_size(f->tile_big, route, &route_w, NULL);
    text_size(f->tile_big, minsbuf, &eta_w, NULL);
    int line1_gap = clampi((int)(10 * scale), 6, 20);
    int max_dest_w = rect.w - 2 * inner - route_w - eta_w - line1_gap * 2;
    if (max_dest_w < 40) max_dest_w = 40;

    draw_text(r, f->tile_big, route, x, y, white, 0);
    char dest_line[256];
    snprintf(dest_line, sizeof(dest_line), " - %s", dest);
    draw_text_trunc(r, f->tile_med, dest_line, x + route_w + line1_gap, y, max_dest_w, dim, 0);
    draw_text(r, f->tile_big, minsbuf, rect.x + rect.w - inner, y, white, 2);

    int y2 = y + clampi((int)(120 * scale), 70, 190);
    char stopsbuf[32], milesbuf[32];
    if (a->stops_away >= 0) snprintf(stopsbuf, sizeof(stopsbuf), "%d", a->stops_away);
    else snprintf(stopsbuf, sizeof(stopsbuf), "--");
    if (a->miles_away >= 0.0) snprintf(milesbuf, sizeof(milesbuf), "%.1f", (double)a->miles_away);
    else snprintf(milesbuf, sizeof(milesbuf), "--");
    char meta[256];
    snprintf(meta, sizeof(meta), "%s stops  •  %d ppl  •  BUS %s  •  %s mi",
             stopsbuf, a->ppl_est, busnum, milesbuf);
    draw_text(r, f->tile_small, meta, x, y2, dim, 0);
    if (a->mins != 0)
        draw_text(r, f->tile_small, "min", rect.x + rect.w - inner, y2, dim, 2);
}

/* --- Draw background image and steam puffs (above background, below header) --- */
static void draw_background_and_steam(SDL_Renderer *r, int W, int H,
                                      int body_y, int body_h,
                                      SDL_Texture *bg_tex, SDL_Texture *steam_tex) {
    if (bg_tex) {
        SDL_Rect dst = { 0, body_y, W, H - body_y };
        SDL_SetTextureAlphaMod(bg_tex, 76);
        SDL_RenderCopy(r, bg_tex, NULL, &dst);
        SDL_SetTextureAlphaMod(bg_tex, 255);
    }

    if (!steam_tex) return;

    static SteamPuff puffs[STEAM_PUFFS];
    static int init;

    const float exhaust_img_x[STEAM_PUFFS] = { 0.22f, 0.78f };
    const float exhaust_img_y[STEAM_PUFFS] = { 0.88f, 0.88f };
    const float rise_speed = 4.4f;
    const float fade_speed = 0.28f;
    const float scale_grow = 0.0012f;
    const float puff_size_mult = 2.f;
    const float start_alpha = 64.f;
    const float origin_dx[STEAM_PUFFS] = { -70.f, 290.f };
    const float origin_dy[STEAM_PUFFS] = { -470.f, -660.f };
    const float drift_right_per_up = 1.0f;

    if (!init) {
        for (int i = 0; i < STEAM_PUFFS; i++) {
            float ex_x = (float)W * exhaust_img_x[i] + origin_dx[i];
            float ex_y = (float)body_y + (float)body_h * exhaust_img_y[i] + origin_dy[i];
            puffs[i].x = ex_x + (float)((i * 17) % 21 - 10);
            puffs[i].y = ex_y + (float)((i * 11) % 12);
            puffs[i].alpha = start_alpha;
            puffs[i].scale = 0.7f + (float)(i % 3) * 0.1f;
            puffs[i].rise = rise_speed + (float)(i % 2) * 1.2f;
        }
        init = 1;
    }

    SDL_SetTextureBlendMode(steam_tex, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < STEAM_PUFFS; i++) {
        puffs[i].y -= puffs[i].rise;
        puffs[i].x += drift_right_per_up * puffs[i].rise;
        puffs[i].alpha -= fade_speed;
        puffs[i].scale += scale_grow;

        if (puffs[i].alpha <= 0.f || puffs[i].y < (float)(body_y - 120)) {
            float ex_x = (float)W * exhaust_img_x[i] + origin_dx[i];
            float ex_y = (float)body_y + (float)body_h * exhaust_img_y[i] + origin_dy[i];
            puffs[i].x = ex_x + (float)((i * 17) % 21 - 10);
            puffs[i].y = ex_y + (float)((i * 11) % 12);
            puffs[i].alpha = start_alpha;
            puffs[i].scale = 0.6f + (float)(i % 3) * 0.1f;
            puffs[i].rise = rise_speed + (float)(i % 2) * 1.0f;
        }

        int a = (int)puffs[i].alpha;
        if (a > 0) {
            int sz = (int)(STEAM_PUFF_SIZE * puffs[i].scale * puff_size_mult);
            if (sz < 12) sz = 12;
            SDL_Rect dst = {
                (int)puffs[i].x - sz / 2,
                (int)puffs[i].y - sz / 2,
                sz, sz
            };
            SDL_SetTextureAlphaMod(steam_tex, a > 255 ? 255 : a);
            SDL_RenderCopy(r, steam_tex, NULL, &dst);
        }
    }
}

/* --- Robot eyes (pulsing circles) aligned over the background image --- */
static void draw_eyes(SDL_Renderer *r, int W, int H, int body_y, float scale) {
    const float eye_left_fx = 0.38f, eye_left_fy = 0.22f;
    const float eye_right_fx = 0.62f, eye_right_fy = 0.22f;
    const int eye_left_dx = -49, eye_left_dy = 366;
    const int eye_right_dx = -924, eye_right_dy = 356;

    int eye_radius = clampi((int)(18 * scale), 8, 36);
    int cx_left  = (int)((float)W * eye_left_fx + 0.5f) + eye_left_dx;
    int cy_left  = (int)(body_y + (float)(H - body_y) * eye_left_fy + 0.5f) + eye_left_dy;
    int cx_right = (int)((float)W * eye_right_fx + 0.5f) + eye_right_dx;
    int cy_right = (int)(body_y + (float)(H - body_y) * eye_right_fy + 0.5f) + eye_right_dy;

    float t = (float)SDL_GetTicks() * 0.001f;
    float pulse = 0.5f + 0.5f * sinf(t * 6.283185f * 2.2f);
    int alpha = 140 + (int)(100.f * pulse);
    if (alpha > 255) alpha = 255;
    SDL_Color cyan = { 0, 200, 255, (Uint8)alpha };
    draw_filled_circle(r, cx_left,  cy_left,  eye_radius, cyan);
    draw_filled_circle(r, cx_right, cy_right, eye_radius, cyan);
}

/* --- Header: title, stop name, date/time, weather --- */
static void draw_header(SDL_Renderer *r, Fonts *f, int W, int pad, int header_h,
                        const char *stop_id, const char *stop_name, Weather *wx,
                        TTF_Font *symbol_font, float scale) {
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Color dim   = { 210, 210, 210, 255 };

    SDL_Rect hdr = { pad, pad, W - 2 * pad, header_h };
    SDL_SetRenderDrawColor(r, 22, 26, 34, 255);
    fill_round_rect(r, hdr, clampi((int)(24 * scale), 10, 40));

    int title_y = hdr.y + clampi((int)(22 * scale), 10, 36);
    draw_text(r, f->h1, "Arrival Board", hdr.x + hdr.w / 2, title_y, white, 1);

    char left1[256];
    if (stop_name && *stop_name) snprintf(left1, sizeof(left1), "%s", stop_name);
    else snprintf(left1, sizeof(left1), "Stop %s", stop_id ? stop_id : "--");

    int left_x = hdr.x + pad;
    int top_y  = hdr.y + clampi((int)(52 * scale), 28, 80);
    draw_text_trunc(r, f->h2, left1, left_x, top_y, hdr.w - 2 * pad - (int)(560 * scale), white, 0);

    char left2[256];
    snprintf(left2, sizeof(left2), "Stop %s", stop_id ? stop_id : "--");
    draw_text(r, f->h2, left2, left_x, top_y + clampi((int)(78 * scale), 44, 120), dim, 0);

    int right_x = hdr.x + hdr.w - pad;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char ts[64];
    strftime(ts, sizeof(ts), "%a %b %-d  %-I:%M %p", &lt);
    int ts_h = 0;
    text_size(f->h2, ts, NULL, &ts_h);
    int right_line_gap = clampi((int)(12 * scale), 6, 24);
    draw_text(r, f->h2, ts, right_x, hdr.y + pad, white, 2);

    if (wx && wx->have) {
        char wline[128];
        if (wx->precip_prob >= 0)
            snprintf(wline, sizeof(wline), "%s  %d°F   Precip %d%%", wx->icon, wx->temp_f, wx->precip_prob);
        else if (wx->precip_in >= 0)
            snprintf(wline, sizeof(wline), "%s  %d°F   Precip %.2f in", wx->icon, wx->temp_f, wx->precip_in);
        else
            snprintf(wline, sizeof(wline), "%s  %d°F   Precip --", wx->icon, wx->temp_f);
        TTF_Font *wf = symbol_font ? symbol_font : f->h2;
        draw_text(r, wf, wline, right_x, hdr.y + pad + ts_h + right_line_gap, white, 2);
    } else {
        draw_text(r, f->h2, "Weather --", right_x, hdr.y + pad + ts_h + right_line_gap, dim, 2);
    }
}

/* --- Footer: logo (bottom left) and copyright (bottom center) --- */
static void draw_footer_logo_copyright(SDL_Renderer *r, Fonts *f, int W, int H,
                                      int pad, SDL_Texture *logo_tex, float scale) {
    SDL_Color dim = { 210, 210, 210, 255 };

    int logo_max_h = clampi((int)(280 * scale), 120, 440);
    if (logo_tex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(logo_tex, NULL, NULL, &tw, &th);
        if (tw > 0 && th > 0) {
            int dw = (int)((long)logo_max_h * (long)tw / (long)th);
            if (dw > W - 2 * pad) dw = W - 2 * pad;
            int dh = (int)((long)dw * (long)th / (long)tw);
            SDL_Rect logo_dst = { pad, H - pad - dh, dw, dh };
            SDL_RenderCopy(r, logo_tex, NULL, &logo_dst);
        }
    }

    static const char copy_str[] = "\xC2\xA9 2026 Damon";
    int cw = 0, ch = 0;
    text_size(f->tile_small, copy_str, &cw, &ch);
    draw_text(r, f->tile_small, copy_str, W / 2, H - pad - ch / 2, dim, 1);
}

/* --- Tile grid: fixed columns x rows, only tiles with data --- */
static void draw_tile_grid(SDL_Renderer *r, Fonts *f, int W, int body_y, int body_h,
                           int pad, Arrival *arr, int n, float scale) {
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Color dim   = { 210, 210, 210, 255 };

    const int cols = TILE_COLS_FIXED;
    const int rows = TILE_ROWS_FIXED;
    int gap = clampi((int)(38 * scale), 14, 70);
    int tile_w = (W - 2 * pad - gap * (cols - 1)) / cols;
    int tile_h = (body_h - gap * (rows - 1)) / rows;
    int radius = clampi((int)(26 * scale), 10, 42);

    int show_n = n < TILE_SLOTS_MAX ? n : TILE_SLOTS_MAX;
    for (int i = 0; i < show_n; i++) {
        int c = i % cols;
        int rr = i / cols;
        SDL_Rect trc = {
            pad + c * (tile_w + gap),
            body_y + rr * (tile_h + gap),
            tile_w,
            tile_h
        };
        draw_tile_content(r, f, &arr[i], trc, scale, white, dim, radius);
    }
}

/* --- Full UI: background, steam, eyes, header, footer, then tiles or "no buses" --- */
static void render_ui(SDL_Renderer *r, Fonts *f,
                      int W, int H,
                      const char *stop_id, const char *stop_name,
                      Weather *wx, Arrival *arr, int n,
                      SDL_Texture *bg_tex, SDL_Texture *steam_tex, SDL_Texture *logo_tex,
                      TTF_Font *symbol_font) {
    SDL_Color white = { 255, 255, 255, 255 };

    SDL_SetRenderDrawColor(r, 10, 12, 16, 255);
    SDL_RenderClear(r);

    float scale = layout_scale(H);
    int pad = clampi((int)(46 * scale), 18, 90);
    int header_h = clampi((int)(260 * scale), 140, 420);
    int body_y = pad + header_h + pad;
    int body_h = H - body_y - pad;
    if (body_h < 100) body_h = 100;

    draw_background_and_steam(r, W, H, body_y, body_h, bg_tex, steam_tex);
    draw_eyes(r, W, H, body_y, scale);
    draw_header(r, f, W, pad, header_h, stop_id, stop_name, wx, symbol_font, scale);

    /* Body area starts below header. */
    body_y = pad + header_h + pad;
    body_h = H - body_y - pad;
    if (body_h < 100) body_h = 100;

    draw_footer_logo_copyright(r, f, W, H, pad, logo_tex, scale);

    if (n <= 0) {
        draw_text(r, f->h1, "No upcoming buses", W / 2, body_y + body_h / 2, white, 1);
        SDL_RenderPresent(r);
        return;
    }

    draw_tile_grid(r, f, W, body_y, body_h, pad, arr, n, scale);
    SDL_RenderPresent(r);
}

#ifdef USE_SDL_IMAGE
/* Load background, steam puff, and logo textures. Paths: cwd or $HOME/arrival_board/. */
static void load_textures(SDL_Renderer *r,
                          SDL_Texture **bg_tex, SDL_Texture **steam_tex, SDL_Texture **logo_tex) {
    *bg_tex = NULL;
    *steam_tex = NULL;
    *logo_tex = NULL;

    const char *bg_path = getenv("BACKGROUND_IMAGE");
    if (!bg_path || !*bg_path) bg_path = "Steampunk bus image.png";
    SDL_Surface *bg_surf = IMG_Load(bg_path);
    if (!bg_surf && getenv("HOME")) {
        char alt[512];
        snprintf(alt, sizeof(alt), "%s/arrival_board/Steampunk bus image.png", getenv("HOME"));
        bg_surf = IMG_Load(alt);
        if (bg_surf) bg_path = alt;
    }
    if (bg_surf) {
        *bg_tex = SDL_CreateTextureFromSurface(r, bg_surf);
        SDL_FreeSurface(bg_surf);
        if (*bg_tex) {
            SDL_SetTextureBlendMode(*bg_tex, SDL_BLENDMODE_BLEND);
            logf_("Background image loaded: %s", bg_path);
        } else
            logf_("Could not create texture from background image");
    } else
        logf_("Could not load background image '%s': %s", bg_path, IMG_GetError());

    const char *steam_path = "steam_puff.png";
    SDL_Surface *steam_surf = IMG_Load(steam_path);
    if (!steam_surf && getenv("HOME")) {
        char steam_alt[512];
        snprintf(steam_alt, sizeof(steam_alt), "%s/arrival_board/steam_puff.png", getenv("HOME"));
        steam_surf = IMG_Load(steam_alt);
        if (steam_surf) steam_path = steam_alt;
    }
    if (!steam_surf) {
        steam_surf = SDL_CreateRGBSurfaceWithFormat(0, STEAM_PUFF_SIZE, STEAM_PUFF_SIZE, 32, SDL_PIXELFORMAT_RGBA8888);
        if (steam_surf) {
            const float cen = (float)(STEAM_PUFF_SIZE / 2);
            const float rad = cen - 4.f;
            SDL_LockSurface(steam_surf);
            Uint32 *pix = (Uint32 *)steam_surf->pixels;
            for (int y = 0; y < STEAM_PUFF_SIZE; y++) {
                for (int x = 0; x < STEAM_PUFF_SIZE; x++) {
                    float dx = (float)x - cen, dy = (float)y - cen;
                    float d = sqrtf(dx * dx + dy * dy);
                    Uint8 alpha = 0;
                    if (d < rad) {
                        float t = 1.f - (d / rad) * (d / rad);
                        if (t > 0.f) { t = (float)pow((double)t, 1.5); alpha = (Uint8)(220.f * t); }
                    }
                    pix[y * steam_surf->pitch / 4 + x] = (0xFFU << 24) | (255u << 16) | (255u << 8) | (unsigned)alpha;
                }
            }
            SDL_UnlockSurface(steam_surf);
            if (IMG_SavePNG(steam_surf, steam_path) == 0) logf_("Created %s", steam_path);
            SDL_FreeSurface(steam_surf);
            steam_surf = IMG_Load(steam_path);
        }
    }
    if (steam_surf) {
        *steam_tex = SDL_CreateTextureFromSurface(r, steam_surf);
        SDL_FreeSurface(steam_surf);
        if (*steam_tex) {
            SDL_SetTextureBlendMode(*steam_tex, SDL_BLENDMODE_BLEND);
            logf_("Steam puff texture loaded");
        }
    }

    const char *logo_path = "Damon Logo Large.png";
    SDL_Surface *logo_surf = IMG_Load(logo_path);
    if (!logo_surf && getenv("HOME")) {
        char alt[512];
        snprintf(alt, sizeof(alt), "%s/arrival_board/Damon Logo Large.png", getenv("HOME"));
        logo_surf = IMG_Load(alt);
    }
    if (logo_surf) {
        *logo_tex = SDL_CreateTextureFromSurface(r, logo_surf);
        SDL_FreeSurface(logo_surf);
        if (*logo_tex) {
            SDL_SetTextureBlendMode(*logo_tex, SDL_BLENDMODE_BLEND);
            logf_("Logo loaded: Damon Logo Large.png");
        }
    } else
        logf_("Logo not found: %s", IMG_GetError());
}
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
    load_textures(r, &bg_tex, &steam_tex, &logo_tex);
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
    const char *loaded_symbol_path = symbol_font ? symbol_font_path : NULL;
    if (!symbol_font) {
        const char *fallbacks[] = {
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            NULL
        };
        for (int i = 0; fallbacks[i] && !symbol_font; i++) {
            symbol_font = TTF_OpenFont(fallbacks[i], sym_pt);
            if (symbol_font) loaded_symbol_path = fallbacks[i];
        }
    }
    if (symbol_font)
        logf_("Symbol font loaded: %s (pt %d) for weather icon", loaded_symbol_path, sym_pt);
    else
        logf_("Symbol font failed (weather icon may show tofu): %s - %s", symbol_font_path, TTF_GetError());

    Weather wx;
    memset(&wx, 0, sizeof(wx));
    wx.precip_prob = -1;
    wx.precip_in = -1.0;

    Arrival arrivals[32];
    int n = 0;
    time_t last_fetch = 0;

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q))
                goto done;
        }

        time_t now = time(NULL);
        if (!last_fetch || difftime(now, last_fetch) >= poll) {
            char sn[256];
            sn[0] = '\0';
            n = fetch_mta_arrivals(arrivals, max_tiles, sn, sizeof(sn), mta_key, stop_id, route_filter);
            if (!stop_name[0] && sn[0]) snprintf(stop_name, sizeof(stop_name), "%s", sn);
            fetch_weather(&wx, stop_name[0] ? stop_name : NULL);
            last_fetch = now;
        }

        SDL_GetRendererOutputSize(r, &W, &H);
        render_ui(r, &fonts, W, H, stop_id ? stop_id : "--", stop_name, &wx, arrivals, n,
                 bg_tex, steam_tex, logo_tex, symbol_font);

        SDL_Delay(80);
    }

done:
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
