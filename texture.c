/*
 * Texture loading for arrival board UI.
 */
#include "texture.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>

static SDL_Surface *load_surface(const char *path) {
    return IMG_Load(path);
}

static SDL_Texture *surface_to_texture(SDL_Renderer *r, SDL_Surface *surf,
                                       const char *name, int force_rgba) {
    if (force_rgba) {
        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA8888, 0);
        if (rgba) { SDL_FreeSurface(surf); surf = rgba; }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        logf_("%s loaded", name);
    } else {
        logf_("Could not create texture for %s", name);
    }
    return tex;
}

static SDL_Texture *load_image(SDL_Renderer *r, const char *path,
                               const char *name, int force_rgba) {
    SDL_Surface *surf = load_surface(path);
    if (!surf) {
        logf_("%s not found: %s", name, IMG_GetError());
        return NULL;
    }
    return surface_to_texture(r, surf, name, force_rgba);
}

void texture_load(SDL_Renderer *r,
                  SDL_Texture **bg_tex, SDL_Texture **steam_tex, SDL_Texture **logo_tex,
                  SDL_Texture **wide_tile_tex, SDL_Texture **narrow_tile_tex) {
    *bg_tex = *steam_tex = *logo_tex = *wide_tile_tex = *narrow_tile_tex = NULL;

    const char *bg_path = getenv("BACKGROUND_IMAGE");
    if (!bg_path || !*bg_path) bg_path = "Steampunk bus image.png";
    /* force_rgba: some KMS/GL paths mishandle RGB24 from IMG_Load; match tile images. */
    *bg_tex = load_image(r, bg_path, "Background image", 1);

    SDL_Surface *steam_surf = load_surface("steam_puff.png");
    if (steam_surf) *steam_tex = surface_to_texture(r, steam_surf, "Steam puff", 0);

    *logo_tex       = load_image(r, "Damon Logo Large.png", "Logo", 0);
    *wide_tile_tex  = load_image(r, "tools/WideTile.png", "Wide tile bg", 1);
    *narrow_tile_tex = load_image(r, "tools/NarrowTile.png", "Narrow tile bg", 1);
}
