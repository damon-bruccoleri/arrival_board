#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void fill_rect(SDL_Renderer *ren, SDL_Rect r, SDL_Color c){
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
  SDL_RenderFillRect(ren, &r);
}
static void draw_rect(SDL_Renderer *ren, SDL_Rect r, SDL_Color c){
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
  SDL_RenderDrawRect(ren, &r);
}

static void draw_text(SDL_Renderer *ren, TTF_Font *font, SDL_Color col,
                      int x, int y, const char *s){
  if (!ren || !font || !s || !*s) return;
  SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, col);
  if (!surf) return;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
  if (!tex) { SDL_FreeSurface(surf); return; }
  SDL_Rect r = { x, y, surf->w, surf->h };
  SDL_FreeSurface(surf);
  SDL_RenderCopy(ren, tex, NULL, &r);
  SDL_DestroyTexture(tex);
}

static void ellipsize_to_width(TTF_Font *font, const char *in, int max_w, char *out, size_t outsz){
  if (!out || outsz == 0) return;
  out[0] = '\0';
  if (!in || !*in || !font || max_w <= 0) return;

  int w=0,h=0;
  if (TTF_SizeUTF8(font, in, &w, &h) == 0 && w <= max_w) {
    snprintf(out, outsz, "%s", in);
    return;
  }

  const char *ellipsis = "…";
  char tmp[512]; tmp[0] = '\0';

  size_t n = strlen(in);
  size_t k = 0;
  while (k < n && k < sizeof(tmp)-8) {
    tmp[k] = in[k];
    tmp[k+1] = '\0';

    char t2[520];
    snprintf(t2, sizeof(t2), "%s%s", tmp, ellipsis);

    if (TTF_SizeUTF8(font, t2, &w, &h) == 0 && w <= max_w) {
      k++;
      continue;
    }

    if (k == 0) { snprintf(out, outsz, "%s", ellipsis); return; }
    tmp[k] = '\0';
    snprintf(out, outsz, "%s%s", tmp, ellipsis);
    return;
  }

  snprintf(out, outsz, "%s", ellipsis);
}

static void draw_text_right(SDL_Renderer *ren, TTF_Font *font, SDL_Color col,
                            int right_x, int y, const char *s){
  if (!ren || !font || !s || !*s) return;
  int w=0,h=0;
  if (TTF_SizeUTF8(font, s, &w, &h) != 0) return;
  draw_text(ren, font, col, right_x - w, y, s);
}

void render_tile(SDL_Renderer *ren,
                 TTF_Font *font_route,   // route code (bigger)
                 TTF_Font *font_dest,    // destination (bigger than before)
                 TTF_Font *font_eta,     // big ETA digits
                 TTF_Font *font_small,   // info line
                 SDL_Rect r,
                 const char *route_short,
                 SDL_Color route_color,
                 const char *dest,
                 const char *eta_big,
                 const char *eta_suffix, // "min" (now UNDER digits)
                 const char *info_line,
                 bool realtime)
{
  SDL_Color bg = { 28, 30, 38, 255 };
  SDL_Color border = { 70, 80, 100, 255 };
  SDL_Color text = { 235, 238, 245, 255 };
  SDL_Color muted = { 190, 200, 215, 255 };

  if (!realtime) {
    bg.r = 26; bg.g = 28; bg.b = 34;
    muted.r = 175; muted.g = 185; muted.b = 200;
  }

  fill_rect(ren, r, bg);
  draw_rect(ren, r, border);

  int pad = (r.w > 1400) ? 26 : 14;
  int xL = r.x + pad;
  int xR = r.x + r.w - pad;

  int top_y = r.y + pad;

  // Bottom info line: ALWAYS anchor to bottom inside the tile (fixes clipping).
  int small_h = TTF_FontHeight(font_small);
  int info_y = r.y + r.h - pad - small_h;

  // --- Right side ETA ---
  if (eta_big && *eta_big) {
    // Big digits at top-right
    draw_text_right(ren, font_eta, text, xR, top_y - 2, eta_big);

    // Suffix UNDER digits (right-aligned), clamped so it won't collide with bottom line.
    if (eta_suffix && *eta_suffix) {
      int eta_h = TTF_FontHeight(font_eta);
      int suf_h = TTF_FontHeight(font_small);

      int suf_y = top_y + eta_h - 2;               // under digits
      int max_y = info_y - suf_h - 4;              // keep above info line
      if (suf_y > max_y) suf_y = max_y;

      draw_text_right(ren, font_small, muted, xR, suf_y, eta_suffix);
    }
  }

  // --- Top-left: "Q27 - DESTINATION" ---
  int route_w=0, route_h=0;
  if (route_short && *route_short) {
    TTF_SizeUTF8(font_route, route_short, &route_w, &route_h);
    draw_text(ren, font_route, route_color, xL, top_y, route_short);
  }

  const char *dash = " - ";
  int dash_w=0, dash_h=0;
  TTF_SizeUTF8(font_dest, dash, &dash_w, &dash_h);

  int dest_x = xL + route_w + ((route_w > 0) ? 10 : 0);
  int dest_max_w = (xR - dest_x) - 8;
  if (dest_max_w < 40) dest_max_w = 40;

  char dest_fit[512];
  if (dest && *dest) {
    ellipsize_to_width(font_dest, dest, dest_max_w - dash_w, dest_fit, sizeof(dest_fit));
  } else {
    snprintf(dest_fit, sizeof(dest_fit), "%s", "(no destination)");
  }

  if (route_w > 0) {
    draw_text(ren, font_dest, text, dest_x, top_y + (route_h - TTF_FontHeight(font_dest))/2, dash);
    draw_text(ren, font_dest, text, dest_x + dash_w, top_y + (route_h - TTF_FontHeight(font_dest))/2, dest_fit);
  } else {
    draw_text(ren, font_dest, text, xL, top_y, dest_fit);
  }

  // --- Info line: moved UP (anchored) and made larger (font_small) ---
  if (info_line && *info_line) {
    // Ellipsize if needed to avoid overruns.
    char info_fit[512];
    ellipsize_to_width(font_small, info_line, (xR - xL), info_fit, sizeof(info_fit));
    draw_text(ren, font_small, muted, xL, info_y, info_fit);
  }
}
