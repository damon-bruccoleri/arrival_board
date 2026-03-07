/*
 * UI rendering: header, footer, steam puffs, eyes, tile grid.
 */
#include "ui.h"
#include "texture.h"
#include "types.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STEAM_PUFFS  2
#define STEAM_SPHERE_OFFSET 60

typedef struct SteamPuff {
    float x, y;
    float alpha;
    float scale;
    float rise;
} SteamPuff;

typedef struct {
    float fx, fy;
    int dx, dy;
} EyeLayout;

#define EYE_RADIUS_SCALE  18
#define EYE_PULSE_HZ      (2.2f * 2.0f / 15.0f)
#define EYE_ALPHA_LO      140
#define EYE_ALPHA_HI      240

    /* Mechanical flip: duration and ease-in (split goes 1 -> 0). */
/* 1.0s drop time for the split curve. */
#define FLIP_DURATION_MS  1000

/* Palette: distinct colors for route names. Same route => same color (real-time and scheduled). Regular and express share palette. */
#define ROUTE_PALETTE_SIZE 48
static const SDL_Color route_palette[ROUTE_PALETTE_SIZE] = {
    { 255, 100, 100, 255 }, { 100, 180, 255, 255 }, { 100, 255, 140, 255 }, { 255, 200, 80,  255 },
    { 200, 120, 255, 255 }, { 80,  255, 255, 255 }, { 255, 140, 200, 255 }, { 255, 220, 100, 255 },
    { 140, 255, 180, 255 }, { 180, 140, 255, 255 }, { 255, 160, 100, 255 }, { 100, 220, 255, 255 },
    { 220, 255, 140, 255 }, { 255, 120, 180, 255 }, { 160, 255, 220, 255 }, { 255, 180, 140, 255 },
    { 200, 200, 255, 255 }, { 255, 255, 140, 255 }, { 180, 255, 200, 255 }, { 255, 140, 140, 255 },
    { 120, 255, 120, 255 }, { 255, 100, 200, 255 }, { 100, 200, 255, 255 }, { 255, 200, 120, 255 },
    { 200, 255, 100, 255 }, { 220, 180, 255, 255 }, { 255, 220, 180, 255 }, { 140, 255, 255, 255 },
    { 255, 180, 220, 255 }, { 180, 255, 140, 255 }, { 255, 140, 255, 255 }, { 140, 200, 255, 255 },
    { 255, 255, 100, 255 }, { 100, 255, 200, 255 }, { 255, 100, 140, 255 }, { 200, 255, 180, 255 },
    { 160, 200, 255, 255 }, { 255, 200, 200, 255 }, { 200, 160, 255, 255 }, { 255, 160, 255, 255 },
    { 120, 255, 200, 255 }, { 255, 120, 120, 255 }, { 180, 220, 255, 255 }, { 255, 180, 100, 255 },
    { 220, 255, 220, 255 }, { 255, 100, 255, 255 }, { 100, 255, 100, 255 }, { 255, 220, 255, 255 },
};

static SDL_Color route_color_for(const char *route) {
    if (!route || !route[0]) return (SDL_Color){ 255, 255, 255, 255 };
    if (strcmp(route, "QM8") == 0) return (SDL_Color){ 200, 60, 60, 255 }; /* QM8: red */
    unsigned hash = 5381u;
    for (const unsigned char *p = (const unsigned char *)route; *p; p++)
        hash = ((hash << 5u) + hash) + (unsigned)*p;
    return route_palette[hash % ROUTE_PALETTE_SIZE];
}

/* Compute left and right tile rects from full tile rect. */
static void tile_split_rects(SDL_Rect full, float scale, Fonts *f,
                            SDL_Rect *left_out, SDL_Rect *right_out) {
    int inner = clampi((int)(32 * scale), 12, 60);
    int gap = clampi((int)(8 * scale), 4, 16);
    int eta_w = 0, min_w = 0;
    text_size(f->tile_big, "99", &eta_w, NULL);
    text_size(f->tile_small, "min", &min_w, NULL);
    int right_w = (eta_w > min_w ? eta_w : min_w) + 2 * inner;
    if (right_w < (int)(full.w * 0.15f)) right_w = (int)(full.w * 0.15f);
    int left_w = full.w - right_w - gap;
    if (left_w < 80) { left_w = full.w - right_w; gap = 0; }
    *left_out  = (SDL_Rect){ full.x, full.y, left_w, full.h };
    *right_out = (SDL_Rect){ full.x + left_w + gap, full.y, right_w, full.h };
}

static void draw_tile_left_content(SDL_Renderer *r, Fonts *f, const Arrival *a,
                                  SDL_Rect left_rect, float scale,
                                  SDL_Color white, SDL_Color dim, int radius,
                                  SDL_Texture *wide_tile_tex) {
    (void)white;
    if (wide_tile_tex) {
        /* WideTile is drawn in draw_tile_grid; here we draw only text on transparent. */
    } else {
        SDL_SetRenderDrawColor(r, 18, 20, 26, 255);
        fill_round_rect(r, left_rect, radius);
    }
    int inner = clampi((int)(32 * scale), 12, 60);
    int y = left_rect.y + clampi((int)(20 * scale), 8, 40) + 23;
    int y2 = y + clampi((int)(120 * scale), 70, 190);
    int line1_gap = clampi((int)(10 * scale), 6, 20);
    int left_w = left_rect.w;

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
    SDL_Color route_color = route_color_for(route);
    TTF_Font *route_font = f->tile_big;
    if (strstr(route, "QM8 Super Express") && f->tile_big_bold)
        route_font = f->tile_big_bold;

    int route_w = 0;
    text_size(route_font, route, &route_w, NULL);
    int max_dest_w = left_w - 2 * inner - route_w - line1_gap;
    if (max_dest_w < 40) max_dest_w = 40;

    int left_x = left_rect.x + inner + 90;
    draw_text(r, route_font, route, left_x, y, route_color, 0);
    char dest_line[256];
    snprintf(dest_line, sizeof(dest_line), " - %s", dest);
    draw_text_trunc(r, f->tile_small, dest_line, left_x + route_w + line1_gap, y + 45, max_dest_w, dim, 0);

    char stopsbuf[32], milesbuf[32];
    if (a->stops_away >= 0) snprintf(stopsbuf, sizeof(stopsbuf), "%d", a->stops_away);
    else snprintf(stopsbuf, sizeof(stopsbuf), "--");
    if (a->miles_away >= 0.0) snprintf(milesbuf, sizeof(milesbuf), "%.1f", (double)a->miles_away);
    else snprintf(milesbuf, sizeof(milesbuf), "--");
    char meta[256];
    snprintf(meta, sizeof(meta), "%s stops  •  %d ppl  •  BUS %s  •  %s mi",
             stopsbuf, a->ppl_est, busnum, milesbuf);
    draw_text(r, f->tile_small, meta, left_x, y2, dim, 0);
}

static void draw_tile_right_content(SDL_Renderer *r, Fonts *f, const Arrival *a,
                                   SDL_Rect right_rect, float scale,
                                   SDL_Color white, SDL_Color dim, int radius,
                                   SDL_Texture *narrow_tile_tex) {

    if (!narrow_tile_tex) {
        SDL_SetRenderDrawColor(r, 18, 20, 26, 255);
        fill_round_rect(r, right_rect, radius);
    }

    char minsbuf[16];
    if (a->mins == 0) snprintf(minsbuf, sizeof(minsbuf), "NOW");
    else if (a->mins > 0) snprintf(minsbuf, sizeof(minsbuf), "%d", a->mins);
    else snprintf(minsbuf, sizeof(minsbuf), "--");

    int eta_urgent = (a->mins >= 0 && a->mins <= 3);
    SDL_Color eta_color = eta_urgent ? (SDL_Color){ 255, 60, 60, 255 } : white;

    /* Use smaller font for "NOW" so it fits in the narrow tile. */
    TTF_Font *mins_font = (a->mins == 0) ? f->tile_med : f->tile_big;
    int center_x = right_rect.x + right_rect.w / 2;
    int center_y = right_rect.y + right_rect.h / 2;

    if (a->mins == 0 || a->mins < 0) {
        /* Single line "NOW" or "--": center horizontally and vertically. */
        int h1 = 0;
        text_size(mins_font, minsbuf, NULL, &h1);
        draw_text(r, mins_font, minsbuf, center_x, center_y - h1 / 2, eta_color, 1);
    } else {
        /* Two lines: number and "min", both centered; block centered vertically. */
        int h1 = 0, h2 = 0;
        text_size(mins_font, minsbuf, NULL, &h1);
        text_size(f->tile_small, "min", NULL, &h2);
        /* Tighten vertical spacing. SDL_ttf glyph boxes can include extra vertical
         * whitespace; allowing a small negative gap tucks the lines together while
         * keeping the two-line block vertically centered. */
        int gap_lo = -(int)(24 * scale);
        if (gap_lo > -6) gap_lo = -6;
        int gap_hi = (int)(4 * scale);
        if (gap_hi < 2) gap_hi = 2;
        int line_gap = clampi((int)(-0.28f * (float)h2), gap_lo, gap_hi);
        int block_h = h1 + line_gap + h2;
        int top_y = center_y - block_h / 2;
        draw_text(r, mins_font, minsbuf, center_x, top_y, eta_color, 1);
        draw_text(r, f->tile_small, "min", center_x, top_y + h1 + line_gap - 20, dim, 1);
    }
}

/* True if left-part fields changed (route, dest, bus, stops, ppl, miles). */
static int arrival_left_changed(const Arrival *a, const Arrival *b) {
    return strcmp(a->route, b->route) != 0 || strcmp(a->dest, b->dest) != 0 ||
           strcmp(a->bus, b->bus) != 0 || a->stops_away != b->stops_away ||
           a->ppl_est != b->ppl_est || (a->miles_away != b->miles_away);
}

/* True if right-part (mins) changed. */
static int arrival_right_changed(const Arrival *a, const Arrival *b) {
    return a->mins != b->mins;
}

/* Render left or right part into a texture (render target). */
static void render_left_to_texture(SDL_Renderer *r, Fonts *f, SDL_Texture *tex,
                                   int w, int h, const Arrival *a, float scale,
                                   SDL_Color white, SDL_Color dim, int radius,
                                   SDL_Texture *wide_tile_tex) {
    SDL_SetRenderTarget(r, tex);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    /* Bake background into texture so the flip reveal is opaque (no transparent holes). */
    if (wide_tile_tex) {
        SDL_Rect dst = { 0, 0, w, h };
        SDL_RenderCopy(r, wide_tile_tex, NULL, &dst);
    }
    SDL_Rect rect = { 0, 0, w, h };
    draw_tile_left_content(r, f, a, rect, scale, white, dim, radius, wide_tile_tex);
    SDL_SetRenderTarget(r, NULL);
}

static void render_right_to_texture(SDL_Renderer *r, Fonts *f, SDL_Texture *tex,
                                    int w, int h, const Arrival *a, float scale,
                                    SDL_Color white, SDL_Color dim, int radius,
                                    SDL_Texture *narrow_tile_tex) {
    SDL_SetRenderTarget(r, tex);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    /* Bake background into texture so the flip reveal is opaque. */
    if (narrow_tile_tex) {
        SDL_Rect dst = { 0, 0, w, h };
        SDL_RenderCopy(r, narrow_tile_tex, NULL, &dst);
    }
    SDL_Rect rect = { 0, 0, w, h };
    draw_tile_right_content(r, f, a, rect, scale, white, dim, radius, narrow_tile_tex);
    SDL_SetRenderTarget(r, NULL);
}

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
    static Uint32 last_ticks;

    const float exhaust_img_x[STEAM_PUFFS] = { 0.22f, 0.78f };
    const float exhaust_img_y[STEAM_PUFFS] = { 0.88f, 0.88f };
    const float rise_speed = 4.4f;
    const float fade_speed = 0.28f;
    const float scale_grow = 0.0012f;
    const float puff_size_mult = 2.f;
    const float start_alpha = 64.f;
    const float origin_dx[STEAM_PUFFS] = { -85.f, 280.f };   /* -10px x each */
    const float origin_dy[STEAM_PUFFS] = { -490.f, -670.f }; /* -10px y each */
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
        last_ticks = SDL_GetTicks();
    }

    Uint32 now = SDL_GetTicks();
    float dt = (now - last_ticks) * 0.001f;
    last_ticks = now;
    float frame = 1.0f / 60.0f;
    float dt_norm = dt > 0.f ? dt / frame : 1.f;
    if (dt_norm < 0.25f) dt_norm = 0.25f;
    if (dt_norm > 4.0f) dt_norm = 4.0f;

    SDL_SetTextureBlendMode(steam_tex, SDL_BLENDMODE_BLEND);
    const float speed_scale = (1.0f / 25.0f) * 1.3f;
    for (int i = 0; i < STEAM_PUFFS; i++) {
        float sdt = dt_norm * speed_scale;
        /* Right puff travels 10% faster than left. */
        float speed_mult = (i == 1) ? 1.1f : 1.0f;
        puffs[i].y -= puffs[i].rise * sdt * speed_mult;
        puffs[i].x += drift_right_per_up * puffs[i].rise * sdt * speed_mult;
        puffs[i].alpha -= fade_speed * sdt;
        puffs[i].scale += scale_grow * sdt;

        /* Respawn when faded out, too high, or (for right puff) when its center moves off-screen. */
        if (puffs[i].alpha <= 0.f ||
            puffs[i].y < (float)(body_y - 120) ||
            (i == 1 && puffs[i].x > (float)W)) {
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
            const int alpha_mod = a > 255 ? 255 : a;
            SDL_SetTextureAlphaMod(steam_tex, alpha_mod);
            SDL_Rect dst1 = {
                (int)puffs[i].x - sz / 2,
                (int)puffs[i].y - sz / 2,
                sz, sz
            };
            SDL_Rect dst2 = {
                dst1.x + STEAM_SPHERE_OFFSET,
                dst1.y + STEAM_SPHERE_OFFSET,
                sz, sz
            };
            SDL_RenderCopy(r, steam_tex, NULL, &dst1);
            SDL_RenderCopy(r, steam_tex, NULL, &dst2);
        }
    }
}

static void draw_eyes(SDL_Renderer *r, int W, int H, int body_y, float scale) {
    static const EyeLayout eyes[] = {
        { 0.38f, 0.22f,  -49, 366 },
        { 0.62f, 0.22f, -867, 356 },   /* -3px x */
    };
    const int n_eyes = (int)(sizeof(eyes) / sizeof(eyes[0]));
    const int body_h = H - body_y;
    const int radius = clampi((int)(EYE_RADIUS_SCALE * scale), 8, 36);

    float t = (float)SDL_GetTicks() * 0.001f;
    float pulse = 0.5f + 0.5f * sinf(t * 6.283185f * EYE_PULSE_HZ);
    int alpha = EYE_ALPHA_LO + (int)((EYE_ALPHA_HI - EYE_ALPHA_LO) * pulse);
    if (alpha > 255) alpha = 255;
    SDL_Color cyan = { 0, 200, 255, (Uint8)alpha };

    for (int i = 0; i < n_eyes; i++) {
        const EyeLayout *e = &eyes[i];
        int cx = (int)((float)W * e->fx + 0.5f) + e->dx;
        int cy = (int)(body_y + (float)body_h * e->fy + 0.5f) + e->dy;
        draw_filled_circle(r, cx, cy, radius, cyan);
    }
}

static void draw_header(SDL_Renderer *r, Fonts *f, int W, int pad, int header_h,
                        const char *stop_id, const char *stop_name, Weather *wx,
                        TTF_Font *symbol_font, TTF_Font *emoji_font, float scale) {
    (void)symbol_font;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Color dim   = { 210, 210, 210, 255 };

    SDL_Rect hdr = { pad, pad, W - 2 * pad, header_h };
    SDL_SetRenderDrawColor(r, 22, 26, 34, 255);
    fill_round_rect(r, hdr, clampi((int)(24 * scale), 10, 40));

    int title_y = hdr.y + clampi((int)(22 * scale), 10, 36);
    TTF_Font *title_font = (f->title_font) ? f->title_font : f->h1;
    draw_text(r, title_font, "Arrival Board", hdr.x + hdr.w / 2, title_y, white, 1);

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
    int ts_w = 0, ts_h = 0;
    text_size(f->h2, ts, &ts_w, &ts_h);
    int time_moon_gap = clampi((int)(10 * scale), 6, 20);
    int first_line_y = hdr.y + pad;

    /* Moon phase glyphs (Noto Color Emoji): U+1F311..U+1F318, UTF-8. */
    static const char moon_phase_utf8[8][5] = {
        "\xF0\x9F\x8C\x91", "\xF0\x9F\x8C\x92", "\xF0\x9F\x8C\x93", "\xF0\x9F\x8C\x94",
        "\xF0\x9F\x8C\x95", "\xF0\x9F\x8C\x96", "\xF0\x9F\x8C\x97", "\xF0\x9F\x8C\x98"
    };
    int moon_w = 0;
    const char *moon_utf8 = NULL;
    if (wx && wx->have && wx->moon_phase >= 0.f && emoji_font) {
        int idx = (int)(wx->moon_phase * 8) % 8;
        moon_utf8 = moon_phase_utf8[idx];
        text_size(emoji_font, moon_utf8, &moon_w, NULL);
        moon_w = (int)(moon_w * 0.5f + 0.5f);  /* layout uses scaled width */
    }

    /* First line: date/time then moon glyph, right-justified (moon after time). */
    if (moon_utf8 && moon_w > 0) {
        int time_right_x = right_x - moon_w - time_moon_gap;
        draw_text(r, f->h2, ts, time_right_x, first_line_y, white, 2);
        draw_text_scaled(r, emoji_font, moon_utf8, right_x, first_line_y + 20, white, 2, 0.5f);
    } else {
        draw_text(r, f->h2, ts, right_x, first_line_y, white, 2);
    }

    int right_line_gap = clampi((int)(12 * scale), 6, 24);
    /* Weather line: icon + temp + precip, moved up 10px from default. */
    const int weather_line_offset = -20;

    if (wx && wx->have) {
        TTF_Font *w_icon_font = emoji_font;
        char info[96];
        if (wx->precip_prob >= 0)
            snprintf(info, sizeof(info), "%d°F   Precip %d%%", wx->temp_f, wx->precip_prob);
        else if (wx->precip_in >= 0)
            snprintf(info, sizeof(info), "%d°F   Precip %.2f in", wx->temp_f, wx->precip_in);
        else
            snprintf(info, sizeof(info), "%d°F   Precip --", wx->temp_f);

        int info_w = 0;
        text_size(f->h2, info, &info_w, NULL);
        int icon_w = 0;
        text_size(w_icon_font, wx->icon, &icon_w, NULL);
        icon_w = (int)(icon_w * 0.5f + 0.5f);
        int gap_icon = clampi((int)(8 * scale), 4, 16);
        int y = hdr.y + pad + ts_h + right_line_gap + weather_line_offset;

        int text_left = right_x - info_w;
        int icon_x = text_left - gap_icon - icon_w;
        draw_text_scaled(r, w_icon_font, wx->icon, icon_x, y + 20, white, 0, 0.5f);
        draw_text(r, f->h2, info, right_x, y, white, 2);
    } else {
        draw_text(r, f->h2, "Weather --", right_x, hdr.y + pad + ts_h + right_line_gap + weather_line_offset, dim, 2);
    }
}

static void draw_footer(SDL_Renderer *r, Fonts *f, int W, int H,
                        int pad, SDL_Texture *logo_tex, float scale,
                        int body_y, int body_h) {
    (void)H;
    SDL_Color dim = { 210, 210, 210, 255 };

    const int cols = TILE_COLS_FIXED;
    const int rows = TILE_ROWS_FIXED;
    int gap = clampi((int)(20 * scale), 2, 48);
    int tile_w = (W - 2 * pad - gap * (cols - 1)) / cols;
    int tile_h = (body_h - gap * (rows - 1)) / rows;
    /* Empty cell is bottom-right (col 1, row 5) - slot 11. */
    SDL_Rect cell = {
        pad + 1 * (tile_w + gap),
        body_y + 5 * (tile_h + gap),
        tile_w,
        tile_h
    };

    /* Copyright text: "(C) 2026 " in small font; name in small Smythe (title_small) at larger size. */
    static const char copy_left[] = "\xC2\xA9 2026 ";
    static const char copy_name[] = "D Bruccoleri";
    int left_w = 0, left_h = 0;
    text_size(f->tile_small, copy_left, &left_w, &left_h);
    TTF_Font *name_font = (f->title_small) ? f->title_small : f->tile_small;
    int name_w = 0, name_h = 0;
    text_size(name_font, copy_name, &name_w, &name_h);
    int copy_w = left_w + name_w;
    int copy_h = (left_h > name_h) ? left_h : name_h;
    int inset = clampi((int)(12 * scale), 6, 24);
    int logo_copy_gap = clampi((int)(16 * scale), 8, 32);

    int logo_w = 0, logo_h = 0;
    if (logo_tex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(logo_tex, NULL, NULL, &tw, &th);
        if (tw > 0 && th > 0) {
            /* Logo height fills the cell (with inset). */
            logo_h = cell.h - 2 * inset;
            if (logo_h > 0) {
                logo_w = (int)((long)logo_h * (long)tw / (long)th);
                if (logo_w > cell.w - inset * 2) {
                    logo_w = cell.w - inset * 2;
                    logo_h = (int)((long)logo_w * (long)th / (long)tw);
                }
            }
        }
    }

    /* Center logo + copyright side by side in the cell. */
    int total_w = logo_w + logo_copy_gap + copy_w;
    int start_x = cell.x + (cell.w - total_w) / 2;
    int logo_x = start_x;
    int copy_x = start_x + logo_w + logo_copy_gap;
    if (logo_w == 0) copy_x = cell.x + (cell.w - copy_w) / 2;

    if (logo_tex && logo_w > 0 && logo_h > 0) {
        int tw = 0, th = 0;
        SDL_QueryTexture(logo_tex, NULL, NULL, &tw, &th);
        SDL_Rect logo_dst = {
            logo_x,
            cell.y + (cell.h - logo_h) / 2,
            logo_w,
            logo_h
        };
        SDL_RenderCopy(r, logo_tex, NULL, &logo_dst);
    }

    /* Copyright: vertically centered with logo. */
    int copy_y = cell.y + (cell.h - copy_h) / 2;
    /* Left part: © and year in small font, vertically centered within combined block. */
    int left_y = copy_y + (copy_h - left_h) / 2;
    draw_text(r, f->tile_small, copy_left,
              copy_x,
              left_y,
              dim, 0);
    /* Name: small Smythe at same point size as left side, vertically centered. */
    int name_y = copy_y + (copy_h - name_h) / 2;
    draw_text(r, name_font, copy_name,
              copy_x + left_w,
              name_y,
              dim, 0);
}

/* Format scheduled when (America/New_York): today = "2:30 PM", tomorrow = "tomorrow 2:30 PM", else "Wed 2:30 PM". */
static void format_scheduled_time(time_t when, char *buf, size_t bufsz) {
    setenv("TZ", "America/New_York", 1);
    tzset();
    time_t now = time(NULL);
    struct tm tm_when, tm_now;
    localtime_r(&when, &tm_when);
    localtime_r(&now, &tm_now);
    int when_ymd = (tm_when.tm_year + 1900) * 10000 + (tm_when.tm_mon + 1) * 100 + tm_when.tm_mday;
    int now_ymd = (tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    time_t tomorrow_sec = now + 24 * 3600;
    struct tm tm_tomorrow;
    localtime_r(&tomorrow_sec, &tm_tomorrow);
    int tomorrow_ymd = (tm_tomorrow.tm_year + 1900) * 10000 + (tm_tomorrow.tm_mon + 1) * 100 + tm_tomorrow.tm_mday;

    char time_part[32];
    strftime(time_part, sizeof(time_part), "%I:%M %p", &tm_when);
    if (time_part[0] == '0') time_part[0] = ' ';

    if (when_ymd == now_ymd) {
        snprintf(buf, bufsz, "Scheduled %s", time_part);
    } else if (when_ymd == tomorrow_ymd) {
        snprintf(buf, bufsz, "Scheduled tomorrow %s", time_part);
    } else {
        char day[8];
        strftime(day, sizeof(day), "%a", &tm_when);
        snprintf(buf, bufsz, "Scheduled %s %s", day, time_part);
    }
}

static void draw_scheduled_tile_content(SDL_Renderer *r, Fonts *f, const ScheduledDeparture *s,
                                        SDL_Rect rect, float scale, int radius,
                                        SDL_Texture *wide_tile_tex) {
    SDL_Color dim   = { 210, 210, 210, 255 };
    int inner = clampi((int)(32 * scale), 12, 60);
    int x = rect.x + inner + 200;
    int y = rect.y + clampi((int)(20 * scale), 8, 40) + 25;

    if (!wide_tile_tex) {
        SDL_SetRenderDrawColor(r, 18, 20, 26, 255);
        fill_round_rect(r, rect, radius);
    }

    const char *route = s->route[0] ? s->route : "--";
    const char *dest  = s->dest[0]  ? s->dest  : "--";
    SDL_Color route_color = route_color_for(route);
    int route_w = 0;
    text_size(f->tile_big, route, &route_w, NULL);
    int line1_gap = clampi((int)(10 * scale), 6, 20);
    int max_dest_w = rect.w - 2 * inner - route_w - line1_gap * 2;
    if (max_dest_w < 40) max_dest_w = 40;

    draw_text(r, f->tile_big, route, x, y, route_color, 0);
    char dest_line[256];
    snprintf(dest_line, sizeof(dest_line), " - %s", dest);
    draw_text_trunc(r, f->tile_small, dest_line, x + route_w + line1_gap, y + 45, max_dest_w, dim, 0);

    char line2[128];
    format_scheduled_time(s->when, line2, sizeof(line2));
    int y2 = y + clampi((int)(120 * scale), 70, 190);
    draw_text(r, f->tile_small, line2, x, y2, dim, 0);
}

static void draw_tile_grid(SDL_Renderer *r, Fonts *f, int W, int body_y, int body_h,
                           int pad, Arrival *arr, int n,
                           ScheduledDeparture *scheduled, int ns, float scale,
                           SDL_Texture *wide_tile_tex, SDL_Texture *narrow_tile_tex,
                           void (*on_flip_ended)(void*), void *flip_userdata) {
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Color dim   = { 210, 210, 210, 255 };

    const int cols = TILE_COLS_FIXED;
    const int rows = TILE_ROWS_FIXED;
    /* Slot positions for 11 visible tiles (slot 11 = bottom-right is empty for logo). */
    static const int slot_col[11] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 };
    static const int slot_row[11] = { 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5 };

    int gap = clampi((int)(20 * scale), 2, 48);
    int tile_w = (W - 2 * pad - gap * (cols - 1)) / cols;
    int tile_h = (body_h - gap * (rows - 1)) / rows;
    int radius = clampi((int)(26 * scale), 10, 42);

    int scheduled_count = ns < TILE_SLOTS_VISIBLE ? ns : TILE_SLOTS_VISIBLE;
    int realtime_count = TILE_SLOTS_VISIBLE - scheduled_count;
    if (n < realtime_count) realtime_count = n;
    if (realtime_count < 0) realtime_count = 0;

    /* Per-slot, per-part flip state (realtime tiles only). */
    static SDL_Texture *tex_left_display[TILE_SLOTS_MAX];
    static SDL_Texture *tex_left_prev[TILE_SLOTS_MAX];
    static SDL_Texture *tex_right_display[TILE_SLOTS_MAX];
    static SDL_Texture *tex_right_prev[TILE_SLOTS_MAX];
    static Arrival last_arrival[TILE_SLOTS_MAX];
    static int last_valid[TILE_SLOTS_MAX];
    static int left_animating[TILE_SLOTS_MAX];
    static float left_anim_t[TILE_SLOTS_MAX];
    static float left_anim_delay_ms[TILE_SLOTS_MAX];  /* hold t=0 for 0.2s before animating */
    static int right_animating[TILE_SLOTS_MAX];
    static float right_anim_t[TILE_SLOTS_MAX];
    static float right_anim_delay_ms[TILE_SLOTS_MAX];
    static int last_tile_w, last_tile_h;
    static Uint32 last_flip_ticks;
    int flip_ended_this_frame = 0;
    Uint32 now = SDL_GetTicks();
    float dt_ms = (float)(now - last_flip_ticks);
    last_flip_ticks = now;
    if (dt_ms <= 0.f || dt_ms > 200.f) dt_ms = 16.f;

    /* Recreate part textures if size changed (run once per frame). */
    if (tile_w != last_tile_w || tile_h != last_tile_h) {
        for (int j = 0; j < TILE_SLOTS_MAX; j++) {
            if (tex_left_display[j])  { SDL_DestroyTexture(tex_left_display[j]);  tex_left_display[j] = NULL; }
            if (tex_left_prev[j])    { SDL_DestroyTexture(tex_left_prev[j]);    tex_left_prev[j] = NULL; }
            if (tex_right_display[j]) { SDL_DestroyTexture(tex_right_display[j]); tex_right_display[j] = NULL; }
            if (tex_right_prev[j])   { SDL_DestroyTexture(tex_right_prev[j]);   tex_right_prev[j] = NULL; }
            left_animating[j] = right_animating[j] = 0;
            left_anim_delay_ms[j] = right_anim_delay_ms[j] = 0.f;
        }
        last_tile_w = tile_w;
        last_tile_h = tile_h;
    }

    for (int i = 0; i < realtime_count; i++) {
        int c = slot_col[i];
        int rr = slot_row[i];
        SDL_Rect trc = {
            pad + c * (tile_w + gap),
            body_y + rr * (tile_h + gap),
            tile_w,
            tile_h
        };
        SDL_Rect left_rect, right_rect;
        tile_split_rects(trc, scale, f, &left_rect, &right_rect);
        int left_w = left_rect.w;
        int right_w = right_rect.w;

        /* Ensure we have render targets for this slot. */
        if (!tex_left_display[i]) {
            tex_left_display[i]  = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, left_w, tile_h);
            tex_left_prev[i]     = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, left_w, tile_h);
            tex_right_display[i] = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, right_w, tile_h);
            tex_right_prev[i]    = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, right_w, tile_h);
            if (!tex_left_display[i] || !tex_left_prev[i] || !tex_right_display[i] || !tex_right_prev[i]) continue;
            /* So transparent areas (e.g. left tile when using WideTile) show content behind. */
            SDL_SetTextureBlendMode(tex_left_display[i], SDL_BLENDMODE_BLEND);
            SDL_SetTextureBlendMode(tex_left_prev[i], SDL_BLENDMODE_BLEND);
            SDL_SetTextureBlendMode(tex_right_display[i], SDL_BLENDMODE_BLEND);
            SDL_SetTextureBlendMode(tex_right_prev[i], SDL_BLENDMODE_BLEND);
            /* Clear prev textures to transparent so they are never drawn uninitialized. */
            for (int j = 0; j < 2; j++) {
                SDL_Texture *p = j ? tex_right_prev[i] : tex_left_prev[i];
                SDL_SetRenderTarget(r, p);
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
                SDL_RenderClear(r);
                SDL_SetRenderTarget(r, NULL);
            }
            last_valid[i] = 0;
        }

        int left_chg = 0;
        int right_chg = last_valid[i] ? arrival_right_changed(&arr[i], &last_arrival[i]) : 1;
        /* Left tile only flips when time changed; then check if left info also changed. */
        if (right_chg)
            left_chg = arrival_left_changed(&arr[i], &last_arrival[i]);
        last_arrival[i] = arr[i];
        last_valid[i] = 1;

        /* Draw WideTile under the left part (above other layers, below text) for both static and flip. */
        if (wide_tile_tex) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, wide_tile_tex, NULL, &left_rect);
        }
        /* Draw NarrowTile under the right part. */
        if (narrow_tile_tex) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, narrow_tile_tex, NULL, &right_rect);
        }

        /* --- Draw LEFT part --- */
        if (left_animating[i]) {
            /* Hold at t=0 for 0.2s before starting the GPU animation. */
            if (left_anim_delay_ms[i] > 0.f) {
                left_anim_delay_ms[i] -= dt_ms;
                if (left_anim_delay_ms[i] < 0.f) left_anim_delay_ms[i] = 0.f;
            } else {
                float t = left_anim_t[i] + dt_ms / (float)FLIP_DURATION_MS;
                if (t >= 1.f) { t = 1.f; left_animating[i] = 0; flip_ended_this_frame = 1; }
                left_anim_t[i] = t;
            }
            float t = left_anim_t[i];
            float revealed = t * t * t;
            int h_new = (int)(revealed * (float)tile_h + 0.5f);
            if (h_new > tile_h) h_new = tile_h;
            if (h_new < 0) h_new = 0;

            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, tex_left_prev[i], NULL, &left_rect);
            /* Quad uses minimum height 5px so the parallelogram is never degenerate on the first frame. */
            int h_draw = (h_new > 0) ? h_new : 5;
            if (h_draw > tile_h) h_draw = tile_h;
            {
                /* New tile: parallelogram; use only the revealed height in texture so the strip shows
                 * the top of the tile. Quad geometry skews the texture so image fits the parallelogram. */
                float skew = 100.f * (1.f - t * t * t);
                float v = (tile_h > 0) ? ((float)h_draw / (float)tile_h) : 1.f;
                if (v > 1.f) v = 1.f;
                SDL_Vertex verts[4] = {
                    { { (float)left_rect.x,                 (float)left_rect.y },          { 255,255,255,255 }, { 0.f, 0.f } },
                    { { (float)(left_rect.x + left_rect.w), (float)left_rect.y },          { 255,255,255,255 }, { 1.f, 0.f } },
                    { { (float)(left_rect.x + left_rect.w) + skew, (float)(left_rect.y + h_draw) }, { 255,255,255,255 }, { 1.f, v } },
                    { { (float)left_rect.x + skew,          (float)(left_rect.y + h_draw) }, { 255,255,255,255 }, { 0.f, v } },
                };
                int indices[] = { 0, 1, 2, 0, 2, 3 };
                SDL_RenderGeometry(r, tex_left_display[i], verts, 4, indices, 6);
            }
        } else {
            if (left_chg) {
                SDL_Texture *tmp = tex_left_prev[i];
                tex_left_prev[i] = tex_left_display[i];
                tex_left_display[i] = tmp;
                render_left_to_texture(r, f, tex_left_display[i], left_w, tile_h, &arr[i], scale, white, dim, radius, wide_tile_tex);
                left_animating[i] = 1;
                left_anim_t[i] = 0.f;
                left_anim_delay_ms[i] = 200.f;
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_left_prev[i], NULL, &left_rect);
            } else if (right_chg) {
                /* Remaining time changed: update large tile text and show without animating. */
                render_left_to_texture(r, f, tex_left_display[i], left_w, tile_h, &arr[i], scale, white, dim, radius, wide_tile_tex);
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_left_display[i], NULL, &left_rect);
            } else {
                /* No time change: do not change large tile text; just redraw existing. */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_left_display[i], NULL, &left_rect);
            }
        }

        /* --- Draw RIGHT part --- */
        if (right_animating[i]) {
            /* Hold at t=0 for 0.2s before starting the GPU animation. */
            if (right_anim_delay_ms[i] > 0.f) {
                right_anim_delay_ms[i] -= dt_ms;
                if (right_anim_delay_ms[i] < 0.f) right_anim_delay_ms[i] = 0.f;
            } else {
                float t = right_anim_t[i] + dt_ms / (float)FLIP_DURATION_MS;
                if (t >= 1.f) { t = 1.f; right_animating[i] = 0; flip_ended_this_frame = 1; }
                right_anim_t[i] = t;
            }
            float t = right_anim_t[i];
            float revealed = t * t * t;
            int h_new = (int)(revealed * (float)tile_h + 0.5f);
            if (h_new > tile_h) h_new = tile_h;
            if (h_new < 0) h_new = 0;

            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, tex_right_prev[i], NULL, &right_rect);
            /* Quad uses minimum height 5px so the parallelogram is never degenerate on the first frame. */
            int h_draw = (h_new > 0) ? h_new : 5;
            if (h_draw > tile_h) h_draw = tile_h;
            {
                /* Right part: same parallelogram; revealed height v so strip shows top of tile, skewed. */
                float skew = 100.f * (1.f - t * t * t);
                float v = (tile_h > 0) ? ((float)h_draw / (float)tile_h) : 1.f;
                if (v > 1.f) v = 1.f;
                SDL_Vertex verts[4] = {
                    { { (float)right_rect.x,                 (float)right_rect.y },          { 255,255,255,255 }, { 0.f, 0.f } },
                    { { (float)(right_rect.x + right_rect.w), (float)right_rect.y },          { 255,255,255,255 }, { 1.f, 0.f } },
                    { { (float)(right_rect.x + right_rect.w) + skew, (float)(right_rect.y + h_draw) }, { 255,255,255,255 }, { 1.f, v } },
                    { { (float)right_rect.x + skew,          (float)(right_rect.y + h_draw) }, { 255,255,255,255 }, { 0.f, v } },
                };
                int indices[] = { 0, 1, 2, 0, 2, 3 };
                SDL_RenderGeometry(r, tex_right_display[i], verts, 4, indices, 6);
            }
        } else {
            if (right_chg) {
                SDL_Texture *tmp = tex_right_prev[i];
                tex_right_prev[i] = tex_right_display[i];
                tex_right_display[i] = tmp;
                render_right_to_texture(r, f, tex_right_display[i], right_w, tile_h, &arr[i], scale, white, dim, radius, narrow_tile_tex);
                right_animating[i] = 1;
                right_anim_t[i] = 0.f;
                right_anim_delay_ms[i] = 200.f;
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_right_prev[i], NULL, &right_rect);
            } else {
                /* Time unchanged: just redraw existing right tile. */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_right_display[i], NULL, &right_rect);
            }
        }
    }

    /* Scheduled tiles: bottom slots, grow up. */
    for (int i = 0; i < scheduled_count; i++) {
        int slot_idx = TILE_SLOTS_VISIBLE - scheduled_count + i;
        int c = slot_col[slot_idx];
        int rr = slot_row[slot_idx];
        SDL_Rect trc = {
            pad + c * (tile_w + gap),
            body_y + rr * (tile_h + gap),
            tile_w,
            tile_h
        };
        if (wide_tile_tex) {
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, wide_tile_tex, NULL, &trc);
        }
        draw_scheduled_tile_content(r, f, &scheduled[i], trc, scale, radius, wide_tile_tex);
    }

    if (flip_ended_this_frame && on_flip_ended)
        on_flip_ended(flip_userdata);
}

void ui_render(SDL_Renderer *r, Fonts *f, int W, int H,
               const char *stop_id, const char *stop_name,
               Weather *wx, Arrival *arr, int n,
               ScheduledDeparture *scheduled, int ns,
               SDL_Texture *bg_tex, SDL_Texture *steam_tex, SDL_Texture *logo_tex,
               SDL_Texture *wide_tile_tex, SDL_Texture *narrow_tile_tex,
               TTF_Font *symbol_font, TTF_Font *emoji_font,
               void (*on_flip_ended)(void*), void *flip_userdata) {
    SDL_Color white = { 255, 255, 255, 255 };

    SDL_SetRenderDrawColor(r, 10, 12, 16, 255);
    SDL_RenderClear(r);

    float scale = layout_scale(H);
    int pad = clampi((int)(46 * scale), 18, 90);
    int header_h = clampi((int)(220 * scale), 120, 380);
    int body_y = pad + header_h + pad;
    int body_h = H - body_y - pad;
    if (body_h < 100) body_h = 100;

    draw_background_and_steam(r, W, H, body_y, body_h, bg_tex, steam_tex);
    draw_eyes(r, W, H, body_y, scale);
    draw_header(r, f, W, pad, header_h, stop_id, stop_name, wx, symbol_font, emoji_font, scale);

    body_y = pad + header_h + pad;
    body_h = H - body_y - pad;
    if (body_h < 100) body_h = 100;

    draw_footer(r, f, W, H, pad, logo_tex, scale, body_y, body_h);

    if (n <= 0 && ( !scheduled || ns <= 0)) {
        draw_text(r, f->h1, "No upcoming buses", W / 2, body_y + body_h / 2, white, 1);
        SDL_RenderPresent(r);
        return;
    }

    draw_tile_grid(r, f, W, body_y, body_h, pad, arr, n, scheduled ? scheduled : (ScheduledDeparture *)0, scheduled ? ns : 0, scale, wide_tile_tex, narrow_tile_tex, on_flip_ended, flip_userdata);
    SDL_RenderPresent(r);
}
