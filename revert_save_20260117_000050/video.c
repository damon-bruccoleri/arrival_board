#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *k, const char *defv){
  const char *v = getenv(k);
  return (v && *v) ? v : defv;
}

static TTF_Font *open_font_or_die(const char *path, int px, const char *tag){
  TTF_Font *f = TTF_OpenFont(path, px);
  if (!f){
    fprintf(stderr, "ERROR: TTF_OpenFont(%s, %d) for %s failed: %s\n", path, px, tag, TTF_GetError());
    return NULL;
  }
  TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
  return f;
}

/*
  Initializes SDL + creates a FULLSCREEN_DESKTOP borderless window.
  Returns 0 on success, nonzero on failure.
*/
int video_init(SDL_Window **out_win, SDL_Renderer **out_ren,
               TTF_Font **font_title,
               TTF_Font **font_header,
               TTF_Font **font_route,
               TTF_Font **font_dest,
               TTF_Font **font_eta,
               TTF_Font **font_small,
               int *out_w, int *out_h)
{
  if (SDL_Init(SDL_INIT_VIDEO) != 0){
    fprintf(stderr, "ERROR: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() != 0){
    fprintf(stderr, "ERROR: TTF_Init: %s\n", TTF_GetError());
    return 1;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");

  SDL_DisplayMode dm;
  if (SDL_GetCurrentDisplayMode(0, &dm) != 0){
    dm.w = 1920; dm.h = 1080;
  }
  if (out_w) *out_w = dm.w;
  if (out_h) *out_h = dm.h;

  Uint32 wflags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
  SDL_Window *win = SDL_CreateWindow("arrival_board",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     dm.w, dm.h, wflags);
  if (!win){
    fprintf(stderr, "ERROR: SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren){
    fprintf(stderr, "ERROR: SDL_CreateRenderer: %s\n", SDL_GetError());
    SDL_DestroyWindow(win);
    return 1;
  }

  // Force fullscreen again (some X setups need the explicit call)
  SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(win);
  SDL_RaiseWindow(win);

  int rw=0, rh=0;
  SDL_GetRendererOutputSize(ren, &rw, &rh);
  fprintf(stderr, "Renderer size %dx%d\n", rw, rh);

  const char *font_path = env_or("FONT_PATH", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
  // Scale fonts for 4K
  int scale = (rw >= 3000) ? 3 : (rw >= 1900 ? 2 : 1);

  // Tuned to be readable on 4K TVs from across a room
  int px_title  = 24 * scale; // stop name
  int px_header = 16 * scale; // date/time + weather
  int px_route  = 20 * scale; // Q27
  int px_dest   = 16 * scale; // destination text
  int px_eta    = 34 * scale; // big ETA digits
  int px_small  = 14 * scale; // small lines / suffix

  TTF_Font *f_title  = open_font_or_die(font_path, px_title,  "title");
  TTF_Font *f_header = open_font_or_die(font_path, px_header, "header");
  TTF_Font *f_route  = open_font_or_die(font_path, px_route,  "route");
  TTF_Font *f_dest   = open_font_or_die(font_path, px_dest,   "dest");
  TTF_Font *f_eta    = open_font_or_die(font_path, px_eta,    "eta");
  TTF_Font *f_small  = open_font_or_die(font_path, px_small,  "small");

  if (!f_title || !f_header || !f_route || !f_dest || !f_eta || !f_small){
    if (f_title)  TTF_CloseFont(f_title);
    if (f_header) TTF_CloseFont(f_header);
    if (f_route)  TTF_CloseFont(f_route);
    if (f_dest)   TTF_CloseFont(f_dest);
    if (f_eta)    TTF_CloseFont(f_eta);
    if (f_small)  TTF_CloseFont(f_small);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return 1;
  }

  *out_win = win;
  *out_ren = ren;
  *font_title  = f_title;
  *font_header = f_header;
  *font_route  = f_route;
  *font_dest   = f_dest;
  *font_eta    = f_eta;
  *font_small  = f_small;

  return 0;
}

void video_shutdown(SDL_Window *win, SDL_Renderer *ren,
                    TTF_Font *font_title,
                    TTF_Font *font_header,
                    TTF_Font *font_route,
                    TTF_Font *font_dest,
                    TTF_Font *font_eta,
                    TTF_Font *font_small)
{
  if (font_title)  TTF_CloseFont(font_title);
  if (font_header) TTF_CloseFont(font_header);
  if (font_route)  TTF_CloseFont(font_route);
  if (font_dest)   TTF_CloseFont(font_dest);
  if (font_eta)    TTF_CloseFont(font_eta);
  if (font_small)  TTF_CloseFont(font_small);

  if (ren) SDL_DestroyRenderer(ren);
  if (win) SDL_DestroyWindow(win);

  TTF_Quit();
  SDL_Quit();
}
