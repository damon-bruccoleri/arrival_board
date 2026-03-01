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

static SDL_Surface *load_surface(const char *path, const char *alt_path) {
    SDL_Surface *s = IMG_Load(path);
    if (!s && alt_path) s = IMG_Load(alt_path);
    return s;
}

void texture_load(SDL_Renderer *r,
                  SDL_Texture **bg_tex, SDL_Texture **steam_tex, SDL_Texture **logo_tex,
                  SDL_Texture **wide_tile_tex, SDL_Texture **narrow_tile_tex) {
    *bg_tex = NULL;
    *steam_tex = NULL;
    *logo_tex = NULL;
    *wide_tile_tex = NULL;
    *narrow_tile_tex = NULL;

    const char *bg_path = getenv("BACKGROUND_IMAGE");
    if (!bg_path || !*bg_path) bg_path = "Steampunk bus image.png";
    char bg_alt[512];
    if (getenv("HOME"))
        snprintf(bg_alt, sizeof(bg_alt), "%s/arrival_board/Steampunk bus image.png", getenv("HOME"));
    else
        bg_alt[0] = '\0';
    SDL_Surface *bg_surf = load_surface(bg_path, bg_alt[0] ? bg_alt : NULL);
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
    char steam_alt[512];
    if (getenv("HOME"))
        snprintf(steam_alt, sizeof(steam_alt), "%s/arrival_board/steam_puff.png", getenv("HOME"));
    else
        steam_alt[0] = '\0';
    SDL_Surface *steam_surf = load_surface(steam_path, steam_alt[0] ? steam_alt : NULL);
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
    char logo_alt[512];
    if (getenv("HOME"))
        snprintf(logo_alt, sizeof(logo_alt), "%s/arrival_board/Damon Logo Large.png", getenv("HOME"));
    else
        logo_alt[0] = '\0';
    SDL_Surface *logo_surf = load_surface(logo_path, logo_alt[0] ? logo_alt : NULL);
    if (logo_surf) {
        *logo_tex = SDL_CreateTextureFromSurface(r, logo_surf);
        SDL_FreeSurface(logo_surf);
        if (*logo_tex) {
            SDL_SetTextureBlendMode(*logo_tex, SDL_BLENDMODE_BLEND);
            logf_("Logo loaded: Damon Logo Large.png");
        }
    } else
        logf_("Logo not found: %s", IMG_GetError());

    const char *wide_path = "tools/WideTile.png";
    char wide_alt[512];
    if (getenv("HOME"))
        snprintf(wide_alt, sizeof(wide_alt), "%s/arrival_board/tools/WideTile.png", getenv("HOME"));
    else
        wide_alt[0] = '\0';
    SDL_Surface *wide_surf = load_surface(wide_path, wide_alt[0] ? wide_alt : NULL);
    if (wide_surf) {
        /* Force RGBA format so alpha is preserved (some renderers create RGB texture from surface). */
        SDL_Surface *rgba_surf = SDL_ConvertSurfaceFormat(wide_surf, SDL_PIXELFORMAT_RGBA8888, 0);
        if (rgba_surf) {
            SDL_FreeSurface(wide_surf);
            wide_surf = rgba_surf;
        }
        *wide_tile_tex = SDL_CreateTextureFromSurface(r, wide_surf);
        SDL_FreeSurface(wide_surf);
        if (*wide_tile_tex) {
            SDL_SetTextureBlendMode(*wide_tile_tex, SDL_BLENDMODE_BLEND);
            logf_("Wide tile background loaded: tools/WideTile.png");
        }
    } else
        logf_("Wide tile image not found: %s", IMG_GetError());

    const char *narrow_path = "tools/NarrowTile.png";
    char narrow_alt[512];
    if (getenv("HOME"))
        snprintf(narrow_alt, sizeof(narrow_alt), "%s/arrival_board/tools/NarrowTile.png", getenv("HOME"));
    else
        narrow_alt[0] = '\0';
    SDL_Surface *narrow_surf = load_surface(narrow_path, narrow_alt[0] ? narrow_alt : NULL);
    if (narrow_surf) {
        SDL_Surface *rgba_surf = SDL_ConvertSurfaceFormat(narrow_surf, SDL_PIXELFORMAT_RGBA8888, 0);
        if (rgba_surf) {
            SDL_FreeSurface(narrow_surf);
            narrow_surf = rgba_surf;
        }
        *narrow_tile_tex = SDL_CreateTextureFromSurface(r, narrow_surf);
        SDL_FreeSurface(narrow_surf);
        if (*narrow_tile_tex) {
            SDL_SetTextureBlendMode(*narrow_tile_tex, SDL_BLENDMODE_BLEND);
            logf_("Narrow tile background loaded: tools/NarrowTile.png");
        }
    } else
        logf_("Narrow tile image not found: %s", IMG_GetError());
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
