#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

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

static void fill_rect(SDL_Renderer *ren, SDL_Rect r, SDL_Color c){
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
  SDL_RenderFillRect(ren, &r);
}

static void draw_rect(SDL_Renderer *ren, SDL_Rect r, SDL_Color c){
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
  SDL_RenderDrawRect(ren, &r);
}

void render_header(SDL_Renderer *ren,
                   TTF_Font *font_title,
                   TTF_Font *font_sub,      // date/time and weather (bigger now)
                   SDL_Rect r,
                   const char *stop_name,
                   const char *stop_id,
                   const char *datetime_line,
                   const char *weather_line)
{
  SDL_Color bg = { 18, 20, 26, 255 };
  SDL_Color border = { 60, 70, 90, 255 };
  SDL_Color title = { 245, 245, 250, 255 };
  SDL_Color sub = { 200, 210, 225, 255 };

  fill_rect(ren, r, bg);
  draw_rect(ren, r, border);

  int pad = (r.w > 2000) ? 42 : 18;
  int xL = r.x + pad;
  int xR = r.x + r.w - pad;

  char line1[256];
  if (stop_name && *stop_name) {
    snprintf(line1, sizeof(line1), "%s", stop_name);
  } else {
    snprintf(line1, sizeof(line1), "Stop %s", (stop_id && *stop_id) ? stop_id : "?");
  }

  // Line 1: Stop name
  int y = r.y + pad;
  draw_text(ren, font_title, title, xL, y, line1);

  // Move date/time & stop number DOWN and make larger (font_sub).
  // Slightly increase spacing between the first and second header lines.
  y += TTF_FontHeight(font_title) + 20;

  char line2[256];
  if (datetime_line && *datetime_line && stop_id && *stop_id) {
    snprintf(line2, sizeof(line2), "%s   •   Stop %s", datetime_line, stop_id);
  } else if (datetime_line && *datetime_line) {
    snprintf(line2, sizeof(line2), "%s", datetime_line);
  } else {
    snprintf(line2, sizeof(line2), "Stop %s", (stop_id && *stop_id) ? stop_id : "?");
  }
  draw_text(ren, font_sub, sub, xL, y, line2);

  // Weather line: one more step down, same larger font_sub.
  // Give a little extra breathing room here as well.
  y += TTF_FontHeight(font_sub) + 16;
  if (weather_line && *weather_line) {
    draw_text(ren, font_sub, sub, xL, y, weather_line);
  } else {
    draw_text(ren, font_sub, sub, xL, y, "Weather: (unavailable)");
  }

  draw_text_right(ren, font_sub, sub, xR, r.y + pad, "MTA BusTime • SIRI");
}
