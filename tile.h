#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

typedef struct Fonts {
    TTF_Font *h1;
    TTF_Font *h2;
    TTF_Font *tile_big;
    TTF_Font *tile_med;
    TTF_Font *tile_small;
} Fonts;

int  tile_load_fonts(Fonts *f, const char *font_path, int screen_h);
void tile_free_fonts(Fonts *f);

void text_size(TTF_Font *font, const char *utf8, int *out_w, int *out_h);

void draw_text(SDL_Renderer *r, TTF_Font *font, const char *utf8,
               int x, int y, SDL_Color c, int align /*0=L 1=C 2=R*/);

void draw_text_trunc(SDL_Renderer *r, TTF_Font *font, const char *utf8,
                     int x, int y, int max_w, SDL_Color c, int align);

void fill_round_rect(SDL_Renderer *r, SDL_Rect rc, int radius);
