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
#define FLIP_DURATION_MS  500

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
        int line_gap = clampi((int)(8 * scale), 4, 14);
        int block_h = h1 + line_gap + h2;
        int top_y = center_y - block_h / 2;
        draw_text(r, mins_font, minsbuf, center_x, top_y, eta_color, 1);
        draw_text(r, f->tile_small, "min", center_x, top_y + h1 + line_gap, dim, 1);
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
        puffs[i].y -= puffs[i].rise * sdt;
        puffs[i].x += drift_right_per_up * puffs[i].rise * sdt;
        puffs[i].alpha -= fade_speed * sdt;
        puffs[i].scale += scale_grow * sdt;

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
                        TTF_Font *symbol_font, float scale) {
    (void)symbol_font;
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
        draw_text(r, f->h2, wline, right_x, hdr.y + pad + ts_h + right_line_gap, white, 2);
    } else {
        draw_text(r, f->h2, "Weather --", right_x, hdr.y + pad + ts_h + right_line_gap, dim, 2);
    }
}

static void draw_footer(SDL_Renderer *r, Fonts *f, int W, int H,
                        int pad, SDL_Texture *logo_tex, float scale,
                        int body_y, int body_h) {
    (void)H;
    SDL_Color dim = { 210, 210, 210, 255 };

    const int cols = TILE_COLS_FIXED;
    const int rows = TILE_ROWS_FIXED;
    int gap = clampi((int)(26 * scale), 2, 58);
    int tile_w = (W - 2 * pad - gap * (cols - 1)) / cols;
    int tile_h = (body_h - gap * (rows - 1)) / rows;
    /* Empty cell is bottom-right (col 1, row 5) - slot 11. */
    SDL_Rect cell = {
        pad + 1 * (tile_w + gap),
        body_y + 5 * (tile_h + gap),
        tile_w,
        tile_h
    };

    static const char copy_str[] = "\xC2\xA9 2026 Damon";
    int cw = 0, ch = 0;
    text_size(f->tile_small, copy_str, &cw, &ch);
    int copy_h = ch;
    int inset = clampi((int)(12 * scale), 6, 24);

    if (logo_tex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(logo_tex, NULL, NULL, &tw, &th);
        if (tw > 0 && th > 0) {
            int max_h = cell.h - copy_h - inset * 2;
            if (max_h > 20) {
                int dw = (int)((long)max_h * (long)tw / (long)th);
                if (dw > cell.w - inset * 2) dw = cell.w - inset * 2;
                int dh = (int)((long)dw * (long)th / (long)tw);
                if (dh > max_h) dh = max_h;
                SDL_Rect logo_dst = {
                    cell.x + (cell.w - dw) / 2,
                    cell.y + inset,
                    dw,
                    dh
                };
                SDL_RenderCopy(r, logo_tex, NULL, &logo_dst);
            }
        }
    }

    draw_text(r, f->tile_small, copy_str,
              cell.x + cell.w / 2,
              cell.y + cell.h - inset - copy_h / 2,
              dim, 1);
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
                           SDL_Texture *wide_tile_tex, SDL_Texture *narrow_tile_tex) {
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Color dim   = { 210, 210, 210, 255 };

    const int cols = TILE_COLS_FIXED;
    const int rows = TILE_ROWS_FIXED;
    /* Slot positions for 11 visible tiles (slot 11 = bottom-right is empty for logo). */
    static const int slot_col[11] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0 };
    static const int slot_row[11] = { 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5 };

    int gap = clampi((int)(26 * scale), 2, 58);
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
    static int right_animating[TILE_SLOTS_MAX];
    static float right_anim_t[TILE_SLOTS_MAX];
    static int last_tile_w, last_tile_h;
    static Uint32 last_flip_ticks;
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

        int left_chg = last_valid[i] && arrival_left_changed(&arr[i], &last_arrival[i]);
        int right_chg = last_valid[i] && arrival_right_changed(&arr[i], &last_arrival[i]);
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
            float t = left_anim_t[i] + dt_ms / (float)FLIP_DURATION_MS;
            if (t >= 1.f) { t = 1.f; left_animating[i] = 0; }
            left_anim_t[i] = t;
            float revealed = t * t * t;
            int h_new = (int)(revealed * (float)tile_h + 0.5f);
            if (h_new > tile_h) h_new = tile_h;
            if (h_new < 0) h_new = 0;

            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, tex_left_prev[i], NULL, &left_rect);
            if (h_new > 0) {
                float skew = 20.f * (1.f - t * t * t);
                SDL_Vertex verts[4] = {
                    { { (float)left_rect.x, (float)left_rect.y }, { 255,255,255,255 }, { 0.f, 0.f } },
                    { { (float)(left_rect.x + left_rect.w), (float)left_rect.y }, { 255,255,255,255 }, { 1.f, 0.f } },
                    { { (float)(left_rect.x + left_rect.w) - skew, (float)(left_rect.y + h_new) }, { 255,255,255,255 }, { 1.f, 1.f } },
                    { { (float)left_rect.x - skew, (float)(left_rect.y + h_new) }, { 255,255,255,255 }, { 0.f, 1.f } },
                };
                int indices[] = { 0, 1, 2, 0, 2, 3 };
                SDL_RenderGeometry(r, tex_left_display[i], verts, 4, indices, 6);
                if (h_new < tile_h) {
                    int yc = left_rect.y + h_new;
                    SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
                    SDL_RenderFillRect(r, &(SDL_Rect){ left_rect.x, yc - 1, left_rect.w, 1 });
                    SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
                    SDL_RenderFillRect(r, &(SDL_Rect){ left_rect.x, yc, left_rect.w, 2 });
                    SDL_SetRenderDrawColor(r, 255, 255, 255, 160);
                    SDL_RenderFillRect(r, &(SDL_Rect){ left_rect.x, yc + 2, left_rect.w, 1 });
                }
            }
        } else {
            if (left_chg) {
                SDL_Texture *tmp = tex_left_prev[i];
                tex_left_prev[i] = tex_left_display[i];
                tex_left_display[i] = tmp;
                render_left_to_texture(r, f, tex_left_display[i], left_w, tile_h, &arr[i], scale, white, dim, radius, wide_tile_tex);
                left_animating[i] = 1;
                left_anim_t[i] = 0.f;
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_left_prev[i], NULL, &left_rect);
            } else {
                render_left_to_texture(r, f, tex_left_display[i], left_w, tile_h, &arr[i], scale, white, dim, radius, wide_tile_tex);
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_left_display[i], NULL, &left_rect);
            }
        }

        /* --- Draw RIGHT part --- */
        if (right_animating[i]) {
            float t = right_anim_t[i] + dt_ms / (float)FLIP_DURATION_MS;
            if (t >= 1.f) { t = 1.f; right_animating[i] = 0; }
            right_anim_t[i] = t;
            float revealed = t * t * t;
            int h_new = (int)(revealed * (float)tile_h + 0.5f);
            if (h_new > tile_h) h_new = tile_h;
            if (h_new < 0) h_new = 0;

            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(r, tex_right_prev[i], NULL, &right_rect);
            if (h_new > 0) {
                float skew = 20.f * (1.f - t * t * t);
                SDL_Vertex verts[4] = {
                    { { (float)right_rect.x, (float)right_rect.y }, { 255,255,255,255 }, { 0.f, 0.f } },
                    { { (float)(right_rect.x + right_rect.w), (float)right_rect.y }, { 255,255,255,255 }, { 1.f, 0.f } },
                    { { (float)(right_rect.x + right_rect.w) - skew, (float)(right_rect.y + h_new) }, { 255,255,255,255 }, { 1.f, 1.f } },
                    { { (float)right_rect.x - skew, (float)(right_rect.y + h_new) }, { 255,255,255,255 }, { 0.f, 1.f } },
                };
                int indices[] = { 0, 1, 2, 0, 2, 3 };
                SDL_RenderGeometry(r, tex_right_display[i], verts, 4, indices, 6);
                if (h_new < tile_h) {
                    int yc = right_rect.y + h_new;
                    SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
                    SDL_RenderFillRect(r, &(SDL_Rect){ right_rect.x, yc - 1, right_rect.w, 1 });
                    SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
                    SDL_RenderFillRect(r, &(SDL_Rect){ right_rect.x, yc, right_rect.w, 2 });
                    SDL_SetRenderDrawColor(r, 255, 255, 255, 160);
                    SDL_RenderFillRect(r, &(SDL_Rect){ right_rect.x, yc + 2, right_rect.w, 1 });
                }
            }
        } else {
            if (right_chg) {
                SDL_Texture *tmp = tex_right_prev[i];
                tex_right_prev[i] = tex_right_display[i];
                tex_right_display[i] = tmp;
                render_right_to_texture(r, f, tex_right_display[i], right_w, tile_h, &arr[i], scale, white, dim, radius, narrow_tile_tex);
                right_animating[i] = 1;
                right_anim_t[i] = 0.f;
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_RenderCopy(r, tex_right_prev[i], NULL, &right_rect);
            } else {
                render_right_to_texture(r, f, tex_right_display[i], right_w, tile_h, &arr[i], scale, white, dim, radius, narrow_tile_tex);
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
}

void ui_render(SDL_Renderer *r, Fonts *f, int W, int H,
               const char *stop_id, const char *stop_name,
               Weather *wx, Arrival *arr, int n,
               Arrival *prev_arr, int n_prev,
               ScheduledDeparture *scheduled, int ns,
               SDL_Texture *bg_tex, SDL_Texture *steam_tex, SDL_Texture *logo_tex,
               SDL_Texture *wide_tile_tex, SDL_Texture *narrow_tile_tex,
               TTF_Font *symbol_font) {
    (void)prev_arr;
    (void)n_prev;
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

    body_y = pad + header_h + pad;
    body_h = H - body_y - pad;
    if (body_h < 100) body_h = 100;

    draw_footer(r, f, W, H, pad, logo_tex, scale, body_y, body_h);

    if (n <= 0 && ( !scheduled || ns <= 0)) {
        draw_text(r, f->h1, "No upcoming buses", W / 2, body_y + body_h / 2, white, 1);
        SDL_RenderPresent(r);
        return;
    }

    draw_tile_grid(r, f, W, body_y, body_h, pad, arr, n, scheduled ? scheduled : (ScheduledDeparture *)0, scheduled ? ns : 0, scale, wide_tile_tex, narrow_tile_tex);
    SDL_RenderPresent(r);
}
