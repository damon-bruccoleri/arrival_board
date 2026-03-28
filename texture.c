/*
 * Texture loading for arrival board UI.
 */
#include "texture.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_SDL_IMAGE
#include <SDL2/SDL_image.h>

static SDL_Surface *load_surface(const char *path, const char *home_rel) {
    SDL_Surface *s = IMG_Load(path);
    if (s || !home_rel) return s;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char alt[512];
    snprintf(alt, sizeof(alt), "%s/arrival_board/%s", home, home_rel);
    return IMG_Load(alt);
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
                                const char *home_rel, const char *name, int force_rgba) {
    SDL_Surface *surf = load_surface(path, home_rel);
    if (!surf) {
        logf_("%s not found: %s", name, IMG_GetError());
        return NULL;
    }
    return surface_to_texture(r, surf, name, force_rgba);
}

static SDL_Surface *generate_steam_surface(void) {
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(
        0, STEAM_PUFF_SIZE, STEAM_PUFF_SIZE, 32, SDL_PIXELFORMAT_RGBA8888);
    if (!surf) return NULL;
    const float cen = (float)(STEAM_PUFF_SIZE / 2);
    const float rad = cen - 4.f;
    SDL_LockSurface(surf);
    Uint32 *pix = (Uint32 *)surf->pixels;
    for (int y = 0; y < STEAM_PUFF_SIZE; y++) {
        for (int x = 0; x < STEAM_PUFF_SIZE; x++) {
            float dx = (float)x - cen, dy = (float)y - cen;
            float d = sqrtf(dx * dx + dy * dy);
            Uint8 alpha = 0;
            if (d < rad) {
                float t = 1.f - (d / rad) * (d / rad);
                if (t > 0.f) { t = (float)pow((double)t, 1.5); alpha = (Uint8)(220.f * t); }
            }
            pix[y * surf->pitch / 4 + x] =
                (0xFFU << 24) | (255u << 16) | (255u << 8) | (unsigned)alpha;
        }
    }
    SDL_UnlockSurface(surf);
    return surf;
}

void texture_load(SDL_Renderer *r,
                  SDL_Texture **bg_tex, SDL_Texture **steam_tex, SDL_Texture **logo_tex,
                  SDL_Texture **wide_tile_tex, SDL_Texture **narrow_tile_tex) {
    *bg_tex = *steam_tex = *logo_tex = *wide_tile_tex = *narrow_tile_tex = NULL;

    const char *bg_path = getenv("BACKGROUND_IMAGE");
    if (!bg_path || !*bg_path) bg_path = "Steampunk bus image.png";
    *bg_tex = load_image(r, bg_path, "Steampunk bus image.png", "Background image", 0);

    SDL_Surface *steam_surf = load_surface("steam_puff.png", "steam_puff.png");
    if (!steam_surf) steam_surf = generate_steam_surface();
    if (steam_surf) *steam_tex = surface_to_texture(r, steam_surf, "Steam puff", 0);

    *logo_tex       = load_image(r, "Damon Logo Large.png", "Damon Logo Large.png", "Logo", 0);
    *wide_tile_tex  = load_image(r, "tools/WideTile.png", "tools/WideTile.png", "Wide tile bg", 1);
    *narrow_tile_tex = load_image(r, "tools/NarrowTile.png", "tools/NarrowTile.png", "Narrow tile bg", 1);
}

#else
void texture_load(SDL_Renderer *r,
                  SDL_Texture **bg_tex, SDL_Texture **steam_tex, SDL_Texture **logo_tex,
                  SDL_Texture **wide_tile_tex, SDL_Texture **narrow_tile_tex) {
    (void)r;
    *bg_tex = NULL;
    *steam_tex = NULL;
    *logo_tex = NULL;
    *wide_tile_tex = NULL;
    *narrow_tile_tex = NULL;
}
#endif
