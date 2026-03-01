/*
 * Texture loading for arrival board UI.
 * Requires USE_SDL_IMAGE (libsdl2-image-dev).
 */
#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_render.h>

#define STEAM_PUFF_SIZE 96

/* Load bg, steam puff, logo, and optional wide-tile (left) / narrow-tile (right) backgrounds. Paths: cwd or $HOME/arrival_board/. */
void texture_load(SDL_Renderer *r,
                  SDL_Texture **bg_tex, SDL_Texture **steam_tex, SDL_Texture **logo_tex,
                  SDL_Texture **wide_tile_tex, SDL_Texture **narrow_tile_tex);
