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
#define EYE_PULSE_HZ      2.2f
#define EYE_ALPHA_LO      140
#define EYE_ALPHA_HI      240

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

    SDL_Color route_color = white;
    TTF_Font *route_font = f->tile_big;
    if (strstr(route, "QM8 Super Express")) {
        route_color = (SDL_Color){ 255, 60, 60, 255 };
        route_font = f->tile_big_bold ? f->tile_big_bold : f->tile_big;
    } else if (strstr(route, "QM8")) {
        route_color = (SDL_Color){ 255, 60, 60, 255 };
    } else if (strstr(route, "QM5")) {
        route_color = (SDL_Color){ 60, 120, 255, 255 };
    }

    int eta_urgent = (a->mins >= 0 && a->mins <= 3);
    SDL_Color eta_color = eta_urgent ? (SDL_Color){ 255, 60, 60, 255 } : white;

    int route_w = 0, eta_w = 0;
    text_size(route_font, route, &route_w, NULL);
    text_size(f->tile_big, minsbuf, &eta_w, NULL);
    int line1_gap = clampi((int)(10 * scale), 6, 20);
    int max_dest_w = rect.w - 2 * inner - route_w - eta_w - line1_gap * 2;
    if (max_dest_w < 40) max_dest_w = 40;

    draw_text(r, route_font, route, x, y, route_color, 0);
    char dest_line[256];
    snprintf(dest_line, sizeof(dest_line), " - %s", dest);
    draw_text_trunc(r, f->tile_med, dest_line, x + route_w + line1_gap, y, max_dest_w, dim, 0);
    draw_text(r, f->tile_big, minsbuf, rect.x + rect.w - inner, y, eta_color, 2);

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

void ui_render(SDL_Renderer *r, Fonts *f, int W, int H,
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

    body_y = pad + header_h + pad;
    body_h = H - body_y - pad;
    if (body_h < 100) body_h = 100;

    draw_footer(r, f, W, H, pad, logo_tex, scale);

    if (n <= 0) {
        draw_text(r, f->h1, "No upcoming buses", W / 2, body_y + body_h / 2, white, 1);
        SDL_RenderPresent(r);
        return;
    }

    draw_tile_grid(r, f, W, body_y, body_h, pad, arr, n, scale);
    SDL_RenderPresent(r);
}
