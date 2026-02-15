#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  SDL_Window   *win;
  SDL_Renderer *ren;
  int w, h;
} Video;

static int clampi(int v, int lo, int hi){
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int env_is_true(const char *k){
  const char *v = getenv(k);
  if (!v || !*v) return 0;
  return (strcmp(v,"1")==0 || strcasecmp(v,"true")==0 || strcasecmp(v,"yes")==0 || strcasecmp(v,"on")==0);
}

int video_init(Video *V, const char *title){
  if (!V) return -1;
  memset(V, 0, sizeof(*V));

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "ERROR: SDL_Init: %s\n", SDL_GetError());
    return -1;
  }
  if (TTF_Init() != 0) {
    fprintf(stderr, "ERROR: TTF_Init: %s\n", TTF_GetError());
    return -1;
  }

  SDL_DisplayMode dm;
  if (SDL_GetCurrentDisplayMode(0, &dm) != 0) {
    dm.w = 1280; dm.h = 720;
  }

  // Create a window sized to the display (helps avoid the "small grey window" regression).
  Uint32 wflags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI;
  V->win = SDL_CreateWindow(title ? title : "arrival_board",
                            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            dm.w, dm.h, wflags);
  if (!V->win) {
    fprintf(stderr, "ERROR: SDL_CreateWindow: %s\n", SDL_GetError());
    return -1;
  }

  // Force fullscreen-desktop after creation.
  SDL_SetWindowFullscreen(V->win, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_SetWindowBordered(V->win, SDL_FALSE);
  SDL_SetWindowPosition(V->win, 0, 0);

  int want_software = env_is_true("SDL_RENDER_SOFTWARE") ||
                      (getenv("SDL_RENDER_DRIVER") && strcmp(getenv("SDL_RENDER_DRIVER"), "software") == 0);

  Uint32 rflags = want_software ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  V->ren = SDL_CreateRenderer(V->win, -1, rflags);
  if (!V->ren) {
    // Fallback to software.
    V->ren = SDL_CreateRenderer(V->win, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!V->ren) {
    fprintf(stderr, "ERROR: SDL_CreateRenderer: %s\n", SDL_GetError());
    return -1;
  }

  SDL_GetRendererOutputSize(V->ren, &V->w, &V->h);
  if (V->w <= 0 || V->h <= 0) { V->w = dm.w; V->h = dm.h; }

  fprintf(stderr, "Renderer size %dx%d\n", V->w, V->h);
  return 0;
}

void video_shutdown(Video *V){
  if (!V) return;
  if (V->ren) SDL_DestroyRenderer(V->ren);
  if (V->win) SDL_DestroyWindow(V->win);
  V->ren = NULL;
  V->win = NULL;
  TTF_Quit();
  SDL_Quit();
}

TTF_Font *load_font_or_die(const char *path, int px){
  if (!path || !*path) {
    fprintf(stderr, "ERROR: FONT_PATH is not set.\n");
    return NULL;
  }
  px = clampi(px, 10, 300);
  TTF_Font *f = TTF_OpenFont(path, px);
  if (!f) {
    fprintf(stderr, "ERROR: TTF_OpenFont('%s',%d): %s\n", path, px, TTF_GetError());
    return NULL;
  }
  TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
  return f;
}
