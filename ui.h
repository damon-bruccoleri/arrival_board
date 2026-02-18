/*
 * UI rendering: header, footer, steam puffs, eyes, tile grid.
 */
#pragma once

#include "tile.h"
#include "types.h"
#include <SDL2/SDL.h>

/* Render full UI: background, steam, eyes, header, footer, tiles. */
void ui_render(SDL_Renderer *r, Fonts *f, int W, int H,
               const char *stop_id, const char *stop_name,
               Weather *wx, Arrival *arr, int n,
               SDL_Texture *bg_tex, SDL_Texture *steam_tex, SDL_Texture *logo_tex,
               TTF_Font *symbol_font);
