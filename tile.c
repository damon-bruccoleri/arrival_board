#include "tile.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int clampi(int v, int lo, int hi){ return (v<lo)?lo:(v>hi)?hi:v; }

static SDL_Texture* tex_from_text(SDL_Renderer *r, TTF_Font *font, const char *utf8, SDL_Color c,
                                  int *outw, int *outh) {
    if(!utf8) utf8 = "";
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, utf8, c);
    if(!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, s);
    if(outw) *outw = s->w;
    if(outh) *outh = s->h;
    SDL_FreeSurface(s);
    return t;
}

int tile_load_fonts(Fonts *f, const char *font_path, int screen_h) {
    if(!f) return -1;
    memset(f, 0, sizeof(*f));

    float scale = (screen_h > 0) ? ((float)screen_h / 2160.0f) : 1.0f;

    float user = 1.30f;
    const char *fs = getenv("FONT_SCALE");
    if(fs && *fs){
        float v = (float)atof(fs);
        if(v > 0.50f && v < 3.00f) user = v;
    }
    scale *= user;

    int h1 = (int)(86 * scale);
    int h2 = (int)(58 * scale);
    int tb = (int)(92 * scale);
    int tm = (int)(60 * scale);
    int ts = (int)(46 * scale);

    h1 = clampi(h1, 34, 160);
    h2 = clampi(h2, 26, 120);
    tb = clampi(tb, 30, 170);
    tm = clampi(tm, 22, 130);
    ts = clampi(ts, 18, 100);

    f->h1 = TTF_OpenFont(font_path, h1);
    f->h2 = TTF_OpenFont(font_path, h2);
    f->tile_big   = TTF_OpenFont(font_path, tb);
    f->tile_med   = TTF_OpenFont(font_path, tm);
    f->tile_small = TTF_OpenFont(font_path, ts);

    if(!f->h1 || !f->h2 || !f->tile_big || !f->tile_med || !f->tile_small) {
        return -1;
    }

    TTF_SetFontHinting(f->h1, TTF_HINTING_LIGHT);
    TTF_SetFontHinting(f->h2, TTF_HINTING_LIGHT);
    TTF_SetFontHinting(f->tile_big, TTF_HINTING_LIGHT);
    TTF_SetFontHinting(f->tile_med, TTF_HINTING_LIGHT);
    TTF_SetFontHinting(f->tile_small, TTF_HINTING_LIGHT);
    return 0;
}

void tile_free_fonts(Fonts *f){
    if(!f) return;
    if(f->h1) TTF_CloseFont(f->h1);
    if(f->h2) TTF_CloseFont(f->h2);
    if(f->tile_big) TTF_CloseFont(f->tile_big);
    if(f->tile_med) TTF_CloseFont(f->tile_med);
    if(f->tile_small) TTF_CloseFont(f->tile_small);
    memset(f, 0, sizeof(*f));
}

void text_size(TTF_Font *font, const char *utf8, int *out_w, int *out_h) {
    if(!utf8) utf8 = "";
    int w = 0, h = 0;
    if(TTF_SizeUTF8(font, utf8, &w, &h) == 0) {
        if(out_w) *out_w = w;
        if(out_h) *out_h = h;
    }
}

void draw_text(SDL_Renderer *r, TTF_Font *font, const char *utf8,
               int x, int y, SDL_Color c, int align) {
    int tw=0, th=0;
    SDL_Texture *t = tex_from_text(r, font, utf8, c, &tw, &th);
    if(!t) return;

    SDL_Rect dst = { x, y, tw, th };
    if(align == 1) dst.x = x - tw/2;
    if(align == 2) dst.x = x - tw;

    SDL_RenderCopy(r, t, NULL, &dst);
    SDL_DestroyTexture(t);
}

void draw_text_trunc(SDL_Renderer *r, TTF_Font *font, const char *utf8,
                     int x, int y, int max_w, SDL_Color c, int align) {
    if(!utf8) utf8 = "";
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", utf8);

    int w=0, h=0;
    if(TTF_SizeUTF8(font, buf, &w, &h) == 0 && w <= max_w) {
        draw_text(r, font, buf, x, y, c, align);
        return;
    }

    const char *ellipsis = "…";
    size_t n = strlen(buf);
    while(n > 0) {
        buf[n-1] = '\0';
        char tmp[520];
        snprintf(tmp, sizeof(tmp), "%s%s", buf, ellipsis);
        if(TTF_SizeUTF8(font, tmp, &w, &h) == 0 && w <= max_w) {
            draw_text(r, font, tmp, x, y, c, align);
            return;
        }
        n--;
    }
    draw_text(r, font, ellipsis, x, y, c, align);
}

/* Simple rounded-rect fill via scanlines */
void fill_round_rect(SDL_Renderer *r, SDL_Rect rc, int radius){
    if(radius <= 0){
        SDL_RenderFillRect(r, &rc);
        return;
    }
    radius = clampi(radius, 1, (rc.w < rc.h ? rc.w/2 : rc.h/2));
    for(int y=0; y<rc.h; y++){
        int dy_top = radius - y;
        int dy_bot = y - (rc.h - radius - 1);
        int dx = 0;
        if(dy_top > 0){
            int yy = dy_top;
            dx = (int)(radius - SDL_sqrtf((float)(radius*radius - yy*yy)));
        } else if(dy_bot > 0){
            int yy = dy_bot;
            dx = (int)(radius - SDL_sqrtf((float)(radius*radius - yy*yy)));
        }
        SDL_Rect line = { rc.x + dx, rc.y + y, rc.w - 2*dx, 1 };
        SDL_RenderFillRect(r, &line);
    }
}
