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

static void draw_text_right(SDL_Renderer *ren, TTF_Font *font, SDL_Color col,
                            int right_x, int y, const char *s){
  if (!ren || !font || !s || !*s) return;
  int w=0,h=0;
  if (TTF_SizeUTF8(font, s, &w, &h) != 0) return;
  draw_text(ren, font, col, right_x - w, y, s);
}

static void ellipsize_to_width(TTF_Font *font, const char *in, int max_w, char *out, size_t outsz){
  if (!out || outsz == 0) return;
  out[0] = '\0';
  if (!in || !*in) return;

  int w=0,h=0;
  if (TTF_SizeUTF8(font, in, &w, &h) == 0 && w <= max_w) {
    snprintf(out, outsz, "%s", in);
    return;
  }

  const char *ellipsis = "...";
  char tmp[512];
  tmp[0] = '\0';

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
    if (k == 0) {
      snprintf(out, outsz, "%s", ellipsis);
      return;
    }
    tmp[k] = '\0';
    snprintf(out, outsz, "%s%s", tmp, ellipsis);
    return;
  }
  snprintf(out, outsz, "%s", ellipsis);
}

void render_tile(SDL_Renderer *ren,
                 TTF_Font *font_route,
                 TTF_Font *font_dest,
                 TTF_Font *font_eta,
                 TTF_Font *font_small,
                 SDL_Rect r,
                 const char *route_short,
                 SDL_Color route_color,
                 const char *dest,
                 const char *eta_big,
                 const char *eta_suffix,
                 const char *dist_line,
                 const char *veh_line,
                 bool realtime)
{
  SDL_Color bg     = { 28, 30, 38, 255 };
  SDL_Color border = { 70, 80, 100, 255 };
  SDL_Color text   = { 235, 238, 245, 255 };
  SDL_Color muted  = { 190, 200, 215, 255 };

  if (!realtime) {
    bg.r = 26; bg.g = 28; bg.b = 34;
    muted.r = 175; muted.g = 185; muted.b = 200;
  }

  fill_rect(ren, r, bg);
  draw_rect(ren, r, border);

  int pad = (r.w > 1400) ? 28 : 14;
  int xL = r.x + pad;
  int xR = r.x + r.w - pad;
  int y  = r.y + pad;

  // --- Right side ETA block (digits + suffix UNDER digits) ---
  int eta_w=0, eta_h=0;
  int suf_w=0, suf_h=0;
  if (eta_big && *eta_big) (void)TTF_SizeUTF8(font_eta, eta_big, &eta_w, &eta_h);
  if (eta_suffix && *eta_suffix) (void)TTF_SizeUTF8(font_small, eta_suffix, &suf_w, &suf_h);

  if (eta_big && *eta_big){
    draw_text_right(ren, font_eta, text, xR, y, eta_big);

    if (eta_suffix && *eta_suffix){
      // Center suffix under the digits block
      int digits_left = xR - eta_w;
      int suf_x = digits_left + (eta_w - suf_w)/2;
      int suf_y = y + eta_h - 2; // just below digits
      draw_text(ren, font_small, muted, suf_x, suf_y, eta_suffix);
    }
  }

  // --- Left side: Route + Destination on SAME LINE: "Q27 - DEST" ---
  int left_max = xR - eta_w - 24;  // leave gap before ETA digits
  if (left_max < xL + 50) left_max = xL + 50;

  // Draw route
  int route_w=0, route_h=0;
  if (route_short && *route_short) (void)TTF_SizeUTF8(font_route, route_short, &route_w, &route_h);
  if (route_short && *route_short){
    draw_text(ren, font_route, route_color, xL, y, route_short);
  }

  // Draw " - " + destination (ellipsized to remaining width)
  const char *dash = " - ";
  int dash_w=0, dash_h=0;
  (void)TTF_SizeUTF8(font_dest, dash, &dash_w, &dash_h);

  int dest_x = xL + route_w;
  int baseline_adjust = (route_h > dash_h) ? (route_h - dash_h)/2 : 0;
  int dest_y = y + baseline_adjust;

  int rem_w = left_max - (dest_x + dash_w);
  if (rem_w > 40 && dest && *dest){
    char dest_fit[256];
    ellipsize_to_width(font_dest, dest, rem_w, dest_fit, sizeof(dest_fit));
    draw_text(ren, font_dest, muted, dest_x, dest_y, dash);
    draw_text(ren, font_dest, text,  dest_x + dash_w, dest_y, dest_fit);
  }

  // Next row start (account for suffix-under-digits height)
  int top_block_h = route_h;
  int eta_block_h = eta_h + ( (eta_suffix && *eta_suffix) ? (suf_h + 2) : 0 );
  if (eta_block_h > top_block_h) top_block_h = eta_block_h;

  y += top_block_h + 8;

  // Row 2: stops/distance (left) + vehicle (right)
  if (dist_line && *dist_line) draw_text(ren, font_small, muted, xL, y, dist_line);
  if (veh_line && *veh_line)   draw_text_right(ren, font_small, muted, xR, y, veh_line);
}
