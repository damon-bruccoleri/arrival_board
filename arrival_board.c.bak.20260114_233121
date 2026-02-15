#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUILD_TAG "AB_FIXFULL_2026-01-14_2359"

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s){ (void)s; g_quit = 1; }

/* ---------- logging ---------- */
static void ab_log(const char *fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  fflush(stderr);
}

/* ---------- small utils ---------- */
static int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

static void safe_copy(char *dst, size_t dstsz, const char *src){
  if(!dst || dstsz == 0) return;
  if(!src) { dst[0] = '\0'; return; }
  /* precision-bounded copy avoids GCC truncation warnings */
  snprintf(dst, dstsz, "%.*s", (int)dstsz - 1, src);
}

static const char* env_str(const char *k, const char *defv){
  const char *v = getenv(k);
  return (v && *v) ? v : defv;
}
static int env_int(const char *k, int defv){
  const char *v = getenv(k);
  if(!v || !*v) return defv;
  char *e = NULL;
  long n = strtol(v, &e, 10);
  if(e == v) return defv;
  if(n < -1000000) n = -1000000;
  if(n >  1000000) n =  1000000;
  return (int)n;
}

/* Parse ISO8601 like "2026-01-14T19:24:59.088-05:00" into local epoch.
   We intentionally ignore the offset part because the Pi is in the same local timezone.
*/
static time_t iso8601_to_epoch_local(const char *s){
  if(!s || !*s) return 0;
  int Y=0,M=0,D=0,h=0,m=0,sec=0;
  if(sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = Y - 1900;
  tmv.tm_mon  = M - 1;
  tmv.tm_mday = D;
  tmv.tm_hour = h;
  tmv.tm_min  = m;
  tmv.tm_sec  = sec;
  tmv.tm_isdst = -1;
  return mktime(&tmv);
}

/* ---------- curl fetch ---------- */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} Mem;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud){
  size_t n = size * nmemb;
  Mem *m = (Mem*)ud;
  if(n == 0) return 0;
  if(m->len + n + 1 > m->cap){
    size_t newcap = (m->cap ? m->cap : 4096);
    while(newcap < m->len + n + 1) newcap *= 2;
    char *nb = (char*)realloc(m->buf, newcap);
    if(!nb) return 0;
    m->buf = nb;
    m->cap = newcap;
  }
  memcpy(m->buf + m->len, ptr, n);
  m->len += n;
  m->buf[m->len] = '\0';
  return n;
}

static bool fetch_url(const char *url, long *http_code_out, char **body_out, size_t *bytes_out){
  *body_out = NULL;
  if(bytes_out) *bytes_out = 0;
  if(http_code_out) *http_code_out = 0;

  CURL *c = curl_easy_init();
  if(!c){ ab_log("ERROR: curl_easy_init failed"); return false; }

  Mem m = {0};
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "arrival_board/2.0");
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); /* allow gzip */

  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);

  if(http_code_out) *http_code_out = code;

  if(rc != CURLE_OK){
    ab_log("ERROR: curl failed: %s", curl_easy_strerror(rc));
    free(m.buf);
    return false;
  }
  if(!m.buf){
    ab_log("ERROR: empty body (HTTP %ld)", code);
    return false;
  }

  *body_out = m.buf;
  if(bytes_out) *bytes_out = m.len;
  return true;
}

/* ---------- JSON helpers ---------- */
static cJSON* jget(cJSON *obj, const char *key){
  if(!obj) return NULL;
  return cJSON_GetObjectItemCaseSensitive(obj, key);
}
static const char* jstr(cJSON *obj, const char *key){
  cJSON *it = jget(obj, key);
  return (it && cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}
static int jint(cJSON *obj, const char *key, int defv){
  cJSON *it = jget(obj, key);
  if(it && cJSON_IsNumber(it)) return it->valueint;
  return defv;
}
static const char* jstr0_from_array(cJSON *arr){
  if(!arr || !cJSON_IsArray(arr)) return NULL;
  cJSON *it = cJSON_GetArrayItem(arr, 0);
  return (it && cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

/* ---------- data model ---------- */
typedef struct {
  char route[16];        /* "Q27" */
  char dest[96];         /* "RUSH CAMBRIA HEIGHTS 120 AV" */
  char veh[32];          /* "MTA NYCT_8838" */
  char stopname[96];     /* stop name for title */
  int  dist_m;           /* meters */
  int  stops;            /* NumberOfStopsAway */
  time_t arr_epoch;      /* expected/aimed */
  bool rt;               /* true if ExpectedArrivalTime used */
} Arrival;

static int cmp_arrival(const void *a, const void *b){
  const Arrival *A = (const Arrival*)a;
  const Arrival *B = (const Arrival*)b;
  if(A->arr_epoch < B->arr_epoch) return -1;
  if(A->arr_epoch > B->arr_epoch) return  1;
  return strcmp(A->route, B->route);
}

/* Parse arrivals from SIRI StopMonitoring JSON. Returns count in out[] (up to cap). */
static int parse_arrivals(const char *json_txt, const char *stop_id_digits,
                          const char *route_filter, int lookahead_sec, time_t now,
                          Arrival *out, int cap, char *title_stopname, size_t title_sz)
{
  if(title_stopname && title_sz) title_stopname[0] = '\0';
  if(!json_txt || !*json_txt) return 0;

  cJSON *root = cJSON_Parse(json_txt);
  if(!root) return 0;

  cJSON *Siri = jget(root, "Siri");
  cJSON *SD   = Siri ? jget(Siri, "ServiceDelivery") : NULL;
  cJSON *SMD  = SD ? jget(SD, "StopMonitoringDelivery") : NULL;
  cJSON *SMD0 = (SMD && cJSON_IsArray(SMD)) ? cJSON_GetArrayItem(SMD, 0) : NULL;
  cJSON *MSV  = SMD0 ? jget(SMD0, "MonitoredStopVisit") : NULL;

  char stop_ref[32];
  snprintf(stop_ref, sizeof(stop_ref), "MTA_%.*s", (int)sizeof(stop_ref)-5, stop_id_digits ? stop_id_digits : "");

  int nout = 0;
  if(MSV && cJSON_IsArray(MSV)){
    int n = cJSON_GetArraySize(MSV);
    for(int i=0; i<n && nout < cap; i++){
      cJSON *visit = cJSON_GetArrayItem(MSV, i);
      if(!visit) continue;
      cJSON *MVJ = jget(visit, "MonitoredVehicleJourney");
      if(!MVJ) continue;

      cJSON *MC = jget(MVJ, "MonitoredCall");
      if(!MC) continue;

      const char *mc_stop = jstr(MC, "StopPointRef");
      if(!mc_stop || strcmp(mc_stop, stop_ref) != 0) continue;

      /* route */
      const char *route = NULL;
      const char *pln0 = jstr0_from_array(jget(MVJ, "PublishedLineName"));
      if(pln0 && *pln0) route = pln0;
      if(!route) route = jstr(MVJ, "LineRef");
      if(!route) route = "?";

      /* filter */
      if(route_filter && *route_filter){
        if(strcmp(route_filter, route) != 0){
          /* Some feeds use "MTA NYCT_Q27" in LineRef. If so, allow suffix match. */
          const char *p = strrchr(route, '_');
          if(!p || strcmp(route_filter, p+1) != 0) continue;
        }
      }

      /* dest */
      const char *dest = jstr0_from_array(jget(MVJ, "DestinationName"));
      if(!dest) dest = "?";

      /* vehicle */
      const char *veh = jstr(MVJ, "VehicleRef");
      if(!veh) veh = "?";

      /* stop name (for title) */
      const char *sn = jstr0_from_array(jget(MC, "StopPointName"));
      if(sn && *sn && title_stopname && title_sz && !title_stopname[0]){
        safe_copy(title_stopname, title_sz, sn);
      }

      /* time: prefer ExpectedArrivalTime */
      const char *exp = jstr(MC, "ExpectedArrivalTime");
      const char *aim = jstr(MC, "AimedArrivalTime");
      const char *tuse = (exp && *exp) ? exp : aim;
      if(!tuse || !*tuse) continue;
      time_t tepoch = iso8601_to_epoch_local(tuse);
      if(tepoch == 0) continue;

      int eta = (int)difftime(tepoch, now);
      if(eta < -120) continue;                 /* already long gone */
      if(eta > lookahead_sec) continue;        /* too far out */

      Arrival A;
      memset(&A, 0, sizeof(A));
      safe_copy(A.route, sizeof(A.route), route);
      /* If route is "MTA NYCT_Q27" keep only suffix */
      const char *u = strrchr(A.route, '_');
      if(u && *(u+1)){
        char tmp[16];
        safe_copy(tmp, sizeof(tmp), u+1);
        safe_copy(A.route, sizeof(A.route), tmp);
      }

      safe_copy(A.dest, sizeof(A.dest), dest);
      safe_copy(A.veh, sizeof(A.veh), veh);
      safe_copy(A.stopname, sizeof(A.stopname), sn ? sn : "");
      A.dist_m = jint(MC, "DistanceFromStop", -1);
      A.stops  = jint(MC, "NumberOfStopsAway", -1);
      A.arr_epoch = tepoch;
      A.rt = (exp && *exp);

      out[nout++] = A;
    }
  }

  cJSON_Delete(root);

  if(nout > 1) qsort(out, (size_t)nout, sizeof(out[0]), cmp_arrival);
  return nout;
}

/* ---------- simple text cache ---------- */
typedef struct {
  SDL_Texture *tex;
  int w, h;
  char last[192];
  SDL_Color last_col;
  int last_px;
} TextTex;

static bool same_color(SDL_Color a, SDL_Color b){
  return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a;
}

static void texttex_free(TextTex *t){
  if(t && t->tex){ SDL_DestroyTexture(t->tex); t->tex = NULL; }
  if(t){ t->w = t->h = 0; t->last[0] = '\0'; t->last_px = 0; t->last_col = (SDL_Color){0,0,0,0}; }
}

static void texttex_set(SDL_Renderer *r, TTF_Font *font, int font_px, SDL_Color col, TextTex *t, const char *s){
  if(!t) return;
  if(!s) s = "";
  if(t->tex && strcmp(t->last, s)==0 && same_color(t->last_col, col) && t->last_px==font_px){
    return; /* no change */
  }
  texttex_free(t);

  SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s, col);
  if(!surf) return;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
  if(!tex){ SDL_FreeSurface(surf); return; }

  t->tex = tex;
  t->w = surf->w;
  t->h = surf->h;
  safe_copy(t->last, sizeof(t->last), s);
  t->last_col = col;
  t->last_px = font_px;
  SDL_FreeSurface(surf);
}

static void draw_tex(SDL_Renderer *r, SDL_Texture *tex, int x, int y, int w, int h){
  if(!tex) return;
  SDL_Rect dst = { x, y, w, h };
  SDL_RenderCopy(r, tex, NULL, &dst);
}

/* ---------- colors ---------- */
static uint32_t fnv1a(const char *s){
  uint32_t h = 2166136261u;
  for(; s && *s; s++){
    h ^= (unsigned char)(*s);
    h *= 16777619u;
  }
  return h;
}
static SDL_Color route_color(const char *route){
  static SDL_Color pal[] = {
    {  0, 173, 239, 255}, /* cyan */
    {255,  99,  71, 255}, /* tomato */
    { 80, 200, 120, 255}, /* green */
    {255, 193,   7, 255}, /* amber */
    {186, 104, 200, 255}, /* purple */
    {255, 255, 255, 255}, /* white */
  };
  uint32_t h = fnv1a(route);
  return pal[h % (sizeof(pal)/sizeof(pal[0]))];
}

/* ---------- formatting ---------- */
static void fmt_eta(char *buf, size_t bufsz, int eta_sec){
  if(!buf || bufsz==0) return;
  if(eta_sec < 0) eta_sec = 0;
  int min = (eta_sec + 30) / 60;
  snprintf(buf, bufsz, "%d", min);
}
static void fmt_dist_stops(char *buf, size_t bufsz, int dist_m, int stops){
  if(!buf || bufsz==0) return;
  buf[0] = '\0';
  bool hasDist = (dist_m >= 0);
  bool hasStops = (stops >= 0);

  if(!hasDist && !hasStops) return;

  char tmp[128]; tmp[0] = '\0';
  if(hasDist){
    double miles = (double)dist_m / 1609.344;
    /* keep it readable */
    if(miles < 0.1) miles = 0.1;
    snprintf(tmp, sizeof(tmp), "%.1f miles", miles);
    safe_copy(buf, bufsz, tmp);
  }

  if(hasStops){
    char s2[64];
    if(stops == 1) snprintf(s2, sizeof(s2), "1 stop");
    else snprintf(s2, sizeof(s2), "%d stops", stops);
    if(buf[0]) snprintf(tmp, sizeof(tmp), "%s \xE2\x80\xA2 %s", buf, s2); /* "•" */
    else safe_copy(tmp, sizeof(tmp), s2);
    safe_copy(buf, bufsz, tmp);
  }
}

/* ---------- main ---------- */
int main(int argc, char **argv){
  (void)argc; (void)argv;
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  ab_log("BUILD_TAG=%s", BUILD_TAG);

  const char *key = getenv("MTA_KEY");
  if(!key || !*key){
    ab_log("ERROR: MTA_KEY is not set in environment.");
    return 1;
  }

  const char *stop_id = env_str("STOP_ID", "");
  if(!stop_id[0]){
    ab_log("ERROR: STOP_ID is not set in environment.");
    return 1;
  }

  const char *route_filter = env_str("ROUTE_FILTER", "");
  int poll_sec = env_int("POLL_SECONDS", 10);
  if(poll_sec < 3) poll_sec = 3;

  int lookahead_min = env_int("LOOKAHEAD_MIN", 90);
  if(lookahead_min < 15) lookahead_min = 15;
  int lookahead_sec = lookahead_min * 60;

  const char *font_path = env_str("FONT_PATH", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

  /* SDL init */
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0){
    ab_log("ERROR: SDL_Init: %s", SDL_GetError());
    return 1;
  }
  if(TTF_Init() != 0){
    ab_log("ERROR: TTF_Init: %s", TTF_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

  SDL_DisplayMode dm;
  if(SDL_GetDesktopDisplayMode(0, &dm) != 0){
    dm.w = 1280; dm.h = 720;
  }

  /* Create a borderless window and force FULLSCREEN_DESKTOP (prevents tiny grey box). */
  SDL_Window *win = SDL_CreateWindow(
    "arrival_board",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    dm.w, dm.h,
    SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI
  );
  if(!win){
    ab_log("ERROR: SDL_CreateWindow: %s", SDL_GetError());
    TTF_Quit(); SDL_Quit();
    return 1;
  }

  if(SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0){
    ab_log("WARN: SDL_SetWindowFullscreen(DESKTOP) failed: %s", SDL_GetError());
  }
  SDL_ShowCursor(SDL_DISABLE);
  SDL_RaiseWindow(win);

  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(!ren){
    ab_log("WARN: accelerated renderer failed: %s; falling back to software", SDL_GetError());
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  }
  if(!ren){
    ab_log("ERROR: SDL_CreateRenderer: %s", SDL_GetError());
    SDL_DestroyWindow(win);
    TTF_Quit(); SDL_Quit();
    return 1;
  }

  int RW=0,RH=0;
  SDL_GetRendererOutputSize(ren, &RW, &RH);
  ab_log("Renderer size %dx%d", RW, RH);

  /* Big fonts (about ~3x your old ones at 4K). */
  int title_px = clampi((int)(RH * 0.055), 48, 140);
  int sub_px   = clampi((int)(RH * 0.032), 28,  90);
  int route_px = clampi((int)(RH * 0.060), 54, 170);
  int eta_px   = clampi((int)(RH * 0.080), 64, 220);
  int body_px  = clampi((int)(RH * 0.028), 24,  80);
  int small_px = clampi((int)(RH * 0.024), 20,  70);

  TTF_Font *f_title = TTF_OpenFont(font_path, title_px);
  TTF_Font *f_sub   = TTF_OpenFont(font_path, sub_px);
  TTF_Font *f_route = TTF_OpenFont(font_path, route_px);
  TTF_Font *f_eta   = TTF_OpenFont(font_path, eta_px);
  TTF_Font *f_body  = TTF_OpenFont(font_path, body_px);
  TTF_Font *f_small = TTF_OpenFont(font_path, small_px);

  if(!f_title || !f_sub || !f_route || !f_eta || !f_body || !f_small){
    ab_log("ERROR: TTF_OpenFont failed (FONT_PATH=%s): %s", font_path, TTF_GetError());
    if(f_title) TTF_CloseFont(f_title);
    if(f_sub)   TTF_CloseFont(f_sub);
    if(f_route) TTF_CloseFont(f_route);
    if(f_eta)   TTF_CloseFont(f_eta);
    if(f_body)  TTF_CloseFont(f_body);
    if(f_small) TTF_CloseFont(f_small);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit(); SDL_Quit();
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  /* state */
  Arrival arr[64];
  int arr_n = 0;
  char stopname_for_title[128] = "";

  /* header caches */
  TextTex t_title = {0}, t_clock = {0};
  /* 12 tiles caches */
  enum { TILE_MAX = 12 };
  TextTex t_route[TILE_MAX] = {0};
  TextTex t_dest[TILE_MAX]  = {0};
  TextTex t_eta[TILE_MAX]   = {0};
  TextTex t_minlbl[TILE_MAX]= {0};
  TextTex t_dist[TILE_MAX]  = {0};
  TextTex t_veh[TILE_MAX]   = {0};

  uint32_t last_fetch_ms = 0;
  uint32_t last_draw_ms  = 0;

  /* Build URL */
  char url[512];
  snprintf(url, sizeof(url),
    "https://bustime.mta.info/api/siri/stop-monitoring.json?key=%s&version=2&OperatorRef=MTA&MonitoringRef=%s&MaximumStopVisits=60&StopMonitoringDetailLevel=normal",
    key, stop_id);

  /* initial fetch now */
  last_fetch_ms = 0;

  while(!g_quit){
    /* events */
    SDL_Event e;
    while(SDL_PollEvent(&e)){
      if(e.type == SDL_QUIT) g_quit = 1;
      if(e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) g_quit = 1;
    }

    uint32_t now_ms = SDL_GetTicks();

    /* fetch */
    if(last_fetch_ms == 0 || (now_ms - last_fetch_ms) >= (uint32_t)(poll_sec * 1000)){
      last_fetch_ms = now_ms;

      long code = 0;
      char *body = NULL;
      size_t bytes = 0;

      bool ok = fetch_url(url, &code, &body, &bytes);
      if(ok){
        ab_log("FETCH stop=%s HTTP %ld bytes=%zu", stop_id, code, bytes);
        time_t now = time(NULL);
        char tmp_stopname[128] = "";
        int n = parse_arrivals(body, stop_id, route_filter, lookahead_sec, now, arr, (int)(sizeof(arr)/sizeof(arr[0])), tmp_stopname, sizeof(tmp_stopname));
        arr_n = n;
        if(tmp_stopname[0]) safe_copy(stopname_for_title, sizeof(stopname_for_title), tmp_stopname);
        ab_log("PARSE: kept %d arrivals (lookahead=%d min, filter='%s')", arr_n, lookahead_min, route_filter);
      } else {
        ab_log("FETCH failed (HTTP %ld)", code);
      }
      free(body);
    }

    /* draw at ~20 FPS */
    if(now_ms - last_draw_ms < 50){
      SDL_Delay(5);
      continue;
    }
    last_draw_ms = now_ms;

    SDL_GetRendererOutputSize(ren, &RW, &RH);

    SDL_SetRenderDrawColor(ren, 10, 12, 16, 255);
    SDL_RenderClear(ren);

    int pad = clampi((int)(RH * 0.015), 12, 36);
    int header_h = clampi((int)(RH * 0.18), 160, 520);

    /* header background */
    SDL_Rect hb = { 0, 0, RW, header_h };
    SDL_SetRenderDrawColor(ren, 18, 22, 28, 255);
    SDL_RenderFillRect(ren, &hb);

    /* title */
    char title_line[256];
    if(stopname_for_title[0]){
      snprintf(title_line, sizeof(title_line), "%s (Stop %s)", stopname_for_title, stop_id);
    } else {
      snprintf(title_line, sizeof(title_line), "Stop %s", stop_id);
    }

    /* clock */
    time_t nowt = time(NULL);
    struct tm lt;
    localtime_r(&nowt, &lt);
    char clock_line[128];
    strftime(clock_line, sizeof(clock_line), "%a %b %d  %I:%M %p", &lt);

    SDL_Color white = {255,255,255,255};
    SDL_Color grey  = {180,190,205,255};

    texttex_set(ren, f_title, title_px, white, &t_title, title_line);
    texttex_set(ren, f_sub,   sub_px,   grey,  &t_clock, clock_line);

    int hx = pad;
    int hy = pad;
    draw_tex(ren, t_title.tex, hx, hy, t_title.w, t_title.h);
    draw_tex(ren, t_clock.tex, hx, hy + t_title.h + (pad/2), t_clock.w, t_clock.h);

    /* grid: always 2 columns x 6 rows */
    const int cols = 2;
    const int rows = 6;
    int grid_y = header_h;
    int grid_h = RH - grid_y;

    int tile_w = (RW - (cols + 1) * pad) / cols;
    int tile_h = (grid_h - (rows + 1) * pad) / rows;
    if(tile_h < 120) tile_h = 120;

    /* empty state */
    if(arr_n <= 0){
      const char *msg = "No upcoming arrivals to display";
      TextTex tmsg = {0};
      texttex_set(ren, f_body, body_px, grey, &tmsg, msg);
      int mx = (RW - tmsg.w)/2;
      int my = header_h + (grid_h - tmsg.h)/2;
      draw_tex(ren, tmsg.tex, mx, my, tmsg.w, tmsg.h);
      texttex_free(&tmsg);
      SDL_RenderPresent(ren);
      continue;
    }

    int show_n = arr_n;
    if(show_n > TILE_MAX) show_n = TILE_MAX;

    for(int i=0; i<show_n; i++){
      int c = i % cols;
      int r = i / cols;
      if(r >= rows) break;

      int x = pad + c * (tile_w + pad);
      int y = header_h + pad + r * (tile_h + pad);

      SDL_Rect tile = { x, y, tile_w, tile_h };

      /* tile bg */
      SDL_SetRenderDrawColor(ren, 26, 32, 40, 255);
      SDL_RenderFillRect(ren, &tile);

      /* border */
      SDL_SetRenderDrawColor(ren, 55, 65, 80, 255);
      SDL_RenderDrawRect(ren, &tile);

      Arrival *A = &arr[i];

      /* strings */
      int eta_sec = (int)difftime(A->arr_epoch, nowt);
      char eta_num[16]; fmt_eta(eta_num, sizeof(eta_num), eta_sec);

      char dist_line[96]; fmt_dist_stops(dist_line, sizeof(dist_line), A->dist_m, A->stops);

      char veh_line[64];
      snprintf(veh_line, sizeof(veh_line), "Bus %.*s", (int)sizeof(veh_line)-5, A->veh);

      SDL_Color rc = route_color(A->route);

      /* update cached textures */
      texttex_set(ren, f_route, route_px, rc,    &t_route[i], A->route);
      texttex_set(ren, f_eta,   eta_px,   white, &t_eta[i],   eta_num);
      texttex_set(ren, f_small, small_px, grey,  &t_minlbl[i],"min");
      texttex_set(ren, f_body,  body_px,  white, &t_dest[i],  A->dest);
      texttex_set(ren, f_small, small_px, grey,  &t_dist[i],  dist_line);
      texttex_set(ren, f_small, small_px, grey,  &t_veh[i],   veh_line);

      /* layout inside tile */
      int inner_pad = clampi((int)(tile_h * 0.08), 10, 22);

      int left_x = x + inner_pad;
      int top_y  = y + inner_pad;

      /* Route big (left) */
      draw_tex(ren, t_route[i].tex, left_x, top_y, t_route[i].w, t_route[i].h);

      /* ETA big (right) */
      int eta_x = x + tile_w - inner_pad - t_eta[i].w;
      int eta_y = top_y;
      draw_tex(ren, t_eta[i].tex, eta_x, eta_y, t_eta[i].w, t_eta[i].h);

      /* "min" label slightly higher so it never clips */
      int min_x = x + tile_w - inner_pad - t_minlbl[i].w;
      int min_y = eta_y + t_eta[i].h - (t_minlbl[i].h + (inner_pad/3));
      if(min_y < eta_y + (inner_pad/2)) min_y = eta_y + (inner_pad/2);
      draw_tex(ren, t_minlbl[i].tex, min_x, min_y, t_minlbl[i].w, t_minlbl[i].h);

      /* Destination line under route */
      int dest_y = top_y + t_route[i].h + (inner_pad/2);
      draw_tex(ren, t_dest[i].tex, left_x, dest_y, t_dest[i].w, t_dest[i].h);

      /* Dist/stops + vehicle near bottom */
      int bottom_y = y + tile_h - inner_pad;
      int dist_y = bottom_y - t_veh[i].h - (inner_pad/3) - t_dist[i].h;
      draw_tex(ren, t_dist[i].tex, left_x, dist_y, t_dist[i].w, t_dist[i].h);

      int veh_y = bottom_y - t_veh[i].h;
      draw_tex(ren, t_veh[i].tex, left_x, veh_y, t_veh[i].w, t_veh[i].h);
    }

    SDL_RenderPresent(ren);
  }

  /* cleanup */
  texttex_free(&t_title);
  texttex_free(&t_clock);
  for(int i=0;i<TILE_MAX;i++){
    texttex_free(&t_route[i]);
    texttex_free(&t_dest[i]);
    texttex_free(&t_eta[i]);
    texttex_free(&t_minlbl[i]);
    texttex_free(&t_dist[i]);
    texttex_free(&t_veh[i]);
  }

  curl_global_cleanup();

  TTF_CloseFont(f_title);
  TTF_CloseFont(f_sub);
  TTF_CloseFont(f_route);
  TTF_CloseFont(f_eta);
  TTF_CloseFont(f_body);
  TTF_CloseFont(f_small);

  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  TTF_Quit();
  SDL_Quit();
  return 0;
}
