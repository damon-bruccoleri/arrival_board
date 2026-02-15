#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
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
static void draw_text(SDL_Renderer *ren, TTF_Font *font, SDL_Color col, int x, int y, const char *s){
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
static void draw_text_right(SDL_Renderer *ren, TTF_Font *font, SDL_Color col, int right_x, int y, const char *s){
  if (!ren || !font || !s || !*s) return;
  int w=0,h=0;
  if (TTF_SizeUTF8(font, s, &w, &h) != 0) return;
  draw_text(ren, font, col, right_x - w, y, s);
}

void render_header(SDL_Renderer *ren,
                   TTF_Font *font_title,
                   TTF_Font *font_header,
                   SDL_Rect r,
                   const char *stop_name,
                   const char *stop_id,
                   const char *datetime_line,
                   const char *weather_line,
                   const char *cec_line)
{
  SDL_Color bg     = { 18, 20, 26, 255 };
  SDL_Color border = { 60, 70, 90, 255 };
  SDL_Color title  = { 245, 245, 250, 255 };
  SDL_Color sub    = { 205, 215, 230, 255 };

  fill_rect(ren, r, bg);
  draw_rect(ren, r, border);

  int pad = (r.w > 2000) ? 36 : 18;
  int xL = r.x + pad;
  int xR = r.x + r.w - pad;

  // Line 1: Stop name
  char line1[256];
  if (stop_name && *stop_name) snprintf(line1, sizeof(line1), "%s", stop_name);
  else snprintf(line1, sizeof(line1), "Stop %s", (stop_id && *stop_id) ? stop_id : "?");

  int y = r.y + pad;
  draw_text(ren, font_title, title, xL, y, line1);

  // Move date/time + stop id DOWN and use header font (bigger)
  y += TTF_FontHeight(font_title) + 10;

  char line2[256];
  if (datetime_line && *datetime_line && stop_id && *stop_id)
    snprintf(line2, sizeof(line2), "%s   •   Stop %s", datetime_line, stop_id);
  else if (datetime_line && *datetime_line)
    snprintf(line2, sizeof(line2), "%s", datetime_line);
  else
    snprintf(line2, sizeof(line2), "Stop %s", (stop_id && *stop_id) ? stop_id : "?");

  draw_text(ren, font_header, sub, xL, y, line2);

  // Weather line under it, also header font
  y += TTF_FontHeight(font_header) + 8;
  if (weather_line && *weather_line) draw_text(ren, font_header, sub, xL, y, weather_line);
  else draw_text(ren, font_header, sub, xL, y, "Weather: (unavailable)");

  // Right top: label + last CEC key
  int ry = r.y + pad;
  draw_text_right(ren, font_header, sub, xR, ry, "MTA BusTime • SIRI");

  if (cec_line && *cec_line){
    ry += TTF_FontHeight(font_header) + 6;
    draw_text_right(ren, font_header, sub, xR, ry, cec_line);
  }
}
