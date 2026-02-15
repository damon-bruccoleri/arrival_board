#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Passenger extraction (safe, optional) ---
   Anchor on "VehicleRef":"<vehicle>" then search forward in that stop-visit window
   for "EstimatedPassengerCount": N
*/
static const char* ab_strnstr(const char* hay, size_t haylen, const char* needle) {
  if (!hay || !needle) return NULL;
  size_t nlen = 0; for (const char* p = needle; *p; ++p) nlen++;
  if (nlen == 0 || haylen < nlen) return NULL;
  for (size_t i = 0; i + nlen <= haylen; ++i) {
    size_t j = 0;
    for (; j < nlen; ++j) if (hay[i + j] != needle[j]) break;
    if (j == nlen) return hay + i;
  }
  return NULL;
}

static int ab_parse_int(const char* p, const char* end, int* out) {
  if (!p || !end || p >= end || !out) return 0;
  while (p < end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p==':')) p++;
  int sign = 1;
  if (p < end && *p == '-') { sign = -1; p++; }
  if (p >= end || *p < '0' || *p > '9') return 0;
  long v = 0;
  while (p < end && *p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); p++; }
  *out = (int)(v * sign);
  return 1;
}

static void ab_fill_pax_text_for_vehicle(const char* json, const char* vehicle_ref,
                                        char* out, size_t outsz) {
  if (!out || outsz == 0) return;
  out[0] = '\0';
  if (!json || !vehicle_ref || !*vehicle_ref) return;

  char needle[256];
  int nw = snprintf(needle, sizeof(needle), "\"VehicleRef\":\"%s\"", vehicle_ref);
  if (nw <= 0 || (size_t)nw >= sizeof(needle)) return;

  const char* hit = strstr(json, needle);
  if (!hit) return;

  const char* p = hit;
  const char* next1 = strstr(p + 1, "\"MonitoredStopVisit\"");
  const char* next2 = strstr(p + 1, "\"VehicleRef\"");
  const char* end = NULL;
  if (next1 && next2) end = (next1 < next2) ? next1 : next2;
  else end = next1 ? next1 : next2;
  if (!end) end = p + 8000;
  if (end < p) return;

  size_t win = (size_t)(end - p);
  const char* key = "\"EstimatedPassengerCount\"";
  const char* k = ab_strnstr(p, win, key);
  if (!k) return;

  const char* colon = k;
  while (colon < end && *colon != ':') colon++;
  if (colon >= end) return;

  int count = -1;
  if (!ab_parse_int(colon + 1, end, &count)) return;
  if (count < 0) return;

  snprintf(out, outsz, "%d pax", count);
}

typedef struct {
  SDL_Window   *win;
  SDL_Renderer *ren;
  int w, h;
} Video;

/* from video.c */
int video_init(Video *V, const char *title);
void video_shutdown(Video *V);
TTF_Font *load_font_or_die(const char *path, int px);

/* from header.c */
void render_header(SDL_Renderer *ren,
                   TTF_Font *font_title,
                   TTF_Font *font_sub,
                   SDL_Rect r,
                   const char *stop_name,
                   const char *stop_id,
                   const char *datetime_line,
                   const char *weather_line);

/* from tile.c */
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
                 const char *info_line,
                 bool realtime);

#define MAX_ARR 60

typedef struct {
  char route_short[32];
  char dest[192];
  char vehicle[64];
  
  char pax_text[24];  /* "13 pax" or empty */
int  stops_away;
  int  dist_meters;
  time_t arrival_epoch;
  bool realtime;
  int pax;      // optional; 0 means unknown
  int pax_cap;  // optional; 0 means unknown
} Arrival;


typedef struct {
  char *p;
  size_t n;
} Buf;

static void safe_snprintf(char *dst, size_t dstsz, const char *fmt, ...){
  if (!dst || dstsz == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, dstsz, fmt, ap);
  va_end(ap);
  dst[dstsz-1] = '\0';
}

static size_t curl_write_cb(void *ptr, size_t sz, size_t nm, void *ud){
  size_t n = sz * nm;
  Buf *b = (Buf*)ud;
  if (!b || !ptr || n == 0) return 0;
  char *np = realloc(b->p, b->n + n + 1);
  if (!np) return 0;
  b->p = np;
  memcpy(b->p + b->n, ptr, n);
  b->n += n;
  b->p[b->n] = '\0';
  return n;
}

static char *http_get(const char *url, long *http_code_out){
  if (http_code_out) *http_code_out = 0;
  CURL *c = curl_easy_init();
  if (!c) return NULL;

  Buf b = {0};
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "arrival_board/3.0");
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 4L);

  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);

  if (http_code_out) *http_code_out = code;

  if (rc != CURLE_OK || code < 200 || code >= 300) {
    free(b.p);
    return NULL;
  }
  return b.p; // caller frees
}

static const char *jstr(const cJSON *obj, const char *key){
  if (!obj || !key) return NULL;
  const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON*)obj, key);
  if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
  return NULL;
}

static const cJSON *jobj(const cJSON *obj, const char *key){
  if (!obj || !key) return NULL;
  const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON*)obj, key);
  return cJSON_IsObject(v) ? v : NULL;
}

static const cJSON *jarr(const cJSON *obj, const char *key){
  if (!obj || !key) return NULL;
  const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON*)obj, key);
  return cJSON_IsArray(v) ? v : NULL;
}

static const char *jarr0str(const cJSON *arr){
  if (!cJSON_IsArray(arr)) return NULL;
  const cJSON *v = cJSON_GetArrayItem((cJSON*)arr, 0);
  if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
  return NULL;
}

static int jint(const cJSON *obj, const char *key, int defv){
  const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON*)obj, key);
  if (cJSON_IsNumber(v)) return (int)(v->valuedouble);
  return defv;
}

/* Parse ISO8601 like 2026-01-16T23:25:00.417-05:00 */
static time_t parse_iso8601(const char *s){
  if (!s || strlen(s) < 19) return (time_t)0;
  int Y=0,M=0,D=0,h=0,m=0,sec=0;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y,&M,&D,&h,&m,&sec) != 6) return (time_t)0;

  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = Y - 1900;
  t.tm_mon  = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min  = m;
  t.tm_sec  = sec;

  // timegm gives UTC; we then subtract the offset to get true UTC epoch
  time_t utc = timegm(&t);

  const char *p = s + 19;
  while (*p && *p != 'Z' && *p != '+' && *p != '-') p++;
  if (*p == 'Z') return utc;

  if (*p == '+' || *p == '-') {
    int sign = (*p == '-') ? -1 : 1;
    int oh=0, om=0;
    if (sscanf(p+1, "%2d:%2d", &oh, &om) == 2) {
      int off = sign * (oh*3600 + om*60);
      return utc - off;
    }
  }
  return utc;
}

static void sanitize_stop_id(const char *in, char *out, size_t outsz){
  if (!out || outsz == 0) return;
  out[0] = '\0';
  if (!in || !*in) return;

  // Trim spaces
  while (*in && isspace((unsigned char)*in)) in++;

  if (strncmp(in, "MTA_", 4) == 0) in += 4;

  // Keep digits only (MonitoringRef is numeric)
  size_t k = 0;
  for (; *in && k+1 < outsz; in++) {
    if (isdigit((unsigned char)*in)) out[k++] = *in;
  }
  out[k] = '\0';
}

static void route_short_from(const char *lineRef, const char *published, char *out, size_t outsz){
  if (!out || outsz == 0) return;
  out[0] = '\0';

  const char *src = NULL;
  if (published && *published) src = published;
  else if (lineRef && *lineRef) src = lineRef;
  if (!src) { snprintf(out, outsz, "?"); return; }

  // Prefer PublishedLineName like "Q27"
  if (published && *published) {
    safe_snprintf(out, outsz, "%s", published);
    return;
  }

  // LineRef like "MTA NYCT_Q27" or "MTA NYCT_Q27+"
  const char *u = strrchr(src, '_');
  const char *p = u ? (u+1) : src;
  char tmp[64];
  safe_snprintf(tmp, sizeof(tmp), "%s", p);
  // strip non-alnum tail
  for (size_t i=0; tmp[i]; i++){
    if (!isalnum((unsigned char)tmp[i])) { tmp[i] = '\0'; break; }
  }
  safe_snprintf(out, outsz, "%s", tmp[0] ? tmp : "?");
}

static SDL_Color route_color(const char *route){
  // Nice, distinct-ish palette
  static SDL_Color pal[] = {
    {  80, 170, 255, 255 }, // blue
    { 255, 140,  90, 255 }, // orange
    { 130, 220, 140, 255 }, // green
    { 240, 100, 200, 255 }, // pink
    { 250, 210,  90, 255 }, // yellow
    { 180, 140, 255, 255 }, // purple
  };
  unsigned h = 2166136261u;
  if (route) {
    for (const unsigned char *p=(const unsigned char*)route; *p; p++){
      h ^= *p;
      h *= 16777619u;
    }
  }
  return pal[h % (sizeof(pal)/sizeof(pal[0]))];
}

static int parse_mta_arrivals(const char *json_txt, Arrival *out, int outcap,
                              char *stop_name_out, size_t stop_name_sz)
{
  if (stop_name_out && stop_name_sz) stop_name_out[0] = '\0';
  if (!json_txt || !out || outcap <= 0) return 0;

  cJSON *root = cJSON_Parse(json_txt);
  if (!root) return 0;

  const cJSON *Siri = jobj(root, "Siri");
  const cJSON *SD   = Siri ? jobj(Siri, "ServiceDelivery") : NULL;
  const cJSON *SMD  = SD ? jarr(SD, "StopMonitoringDelivery") : NULL;
  const cJSON *SMD0 = (SMD && cJSON_GetArraySize((cJSON*)SMD) > 0) ? cJSON_GetArrayItem((cJSON*)SMD, 0) : NULL;
  const cJSON *MSV  = SMD0 ? jarr(SMD0, "MonitoredStopVisit") : NULL;

  int n = 0;
  if (MSV && cJSON_IsArray(MSV)) {
    int cnt = cJSON_GetArraySize((cJSON*)MSV);
    for (int i=0; i<cnt && n<outcap; i++) {
      const cJSON *v = cJSON_GetArrayItem((cJSON*)MSV, i);
      const cJSON *mj = v ? jobj(v, "MonitoredVehicleJourney") : NULL;
      if (!mj) continue;

      const char *lineRef = jstr(mj, "LineRef");
      const cJSON *pubArr = jarr(mj, "PublishedLineName");
      const char *pub0 = jarr0str(pubArr);

      const cJSON *destArr = jarr(mj, "DestinationName");
      const char *dest0 = jarr0str(destArr);

      const cJSON *mc = jobj(mj, "MonitoredCall");
      const char *stopPointName = NULL;
      if (mc) {
        const cJSON *spnA = jarr(mc, "StopPointName");
        stopPointName = jarr0str(spnA);
        if (!stopPointName) stopPointName = jstr(mc, "StopPointName");
      }

      if (stop_name_out && stop_name_sz && stop_name_out[0] == '\0' && stopPointName && *stopPointName) {
        safe_snprintf(stop_name_out, stop_name_sz, "%s", stopPointName);
      }

      const char *veh = jstr(mj, "VehicleRef");

      const char *t_exp = mc ? jstr(mc, "ExpectedArrivalTime") : NULL;
      const char *t_aim = mc ? jstr(mc, "AimedArrivalTime") : NULL;
      const char *t_use = (t_exp && *t_exp) ? t_exp : t_aim;
      if (!t_use) continue;

      Arrival *A = &out[n];
      memset(A, 0, sizeof(*A));

      route_short_from(lineRef, pub0, A->route_short, sizeof(A->route_short));
      safe_snprintf(A->dest, sizeof(A->dest), "%s", (dest0 && *dest0) ? dest0 : "(no destination)");
      safe_snprintf(A->vehicle, sizeof(A->vehicle), "%s", (veh && *veh) ? veh : "");

// --- optional passenger load fields (safe; may be missing) ---
A->pax = 0;
A->pax_cap = 0;
cJSON *call = cJSON_GetObjectItemCaseSensitive(mj, "MonitoredCall");
cJSON *ext  = call ? cJSON_GetObjectItemCaseSensitive(call, "Extensions") : NULL;
cJSON *caps = ext  ? cJSON_GetObjectItemCaseSensitive(ext,  "Capacities") : NULL;
cJSON *pc   = caps ? cJSON_GetObjectItemCaseSensitive(caps, "EstimatedPassengerCount") : NULL;
cJSON *cap  = caps ? cJSON_GetObjectItemCaseSensitive(caps, "EstimatedPassengerCapacity") : NULL;
if (pc && cJSON_IsNumber(pc))  A->pax = pc->valueint;
if (cap && cJSON_IsNumber(cap)) A->pax_cap = cap->valueint;

      A->stops_away = mc ? jint(mc, "NumberOfStopsAway", -1) : -1;
      A->dist_meters = mc ? jint(mc, "DistanceFromStop", -1) : -1;
      A->arrival_epoch = parse_iso8601(t_use);
      A->realtime = (t_exp && *t_exp);

      if (A->arrival_epoch <= 0) continue;
      n++;
    }
  }

  cJSON_Delete(root);
  return n;
}

static void vehicle_short(const char *veh, char *out, size_t outsz){
  if (!out || outsz==0) return;
  out[0] = '\0';
  if (!veh || !*veh) return;
  // Example: "MTA NYCT_8031" -> "8031"
  const char *u = strrchr(veh, '_');
  const char *p = u ? (u+1) : veh;
  while (*p && !isdigit((unsigned char)*p)) p++;
  safe_snprintf(out, outsz, "%s", (*p) ? p : veh);
}

static void fmt_info_line(const Arrival *A, char *out, size_t outsz){
  if (!out || outsz==0) return;
  out[0] = '\0';
  if (!A) return;

  char bus[64]; vehicle_short(A->vehicle, bus, sizeof(bus));

  char stops[64] = "";
  if (A->stops_away >= 0) safe_snprintf(stops, sizeof(stops), "%d stops", A->stops_away);

  char dist[64] = "";
  if (A->dist_meters >= 0) {
    double miles = (double)A->dist_meters / 1609.344;
    safe_snprintf(dist, sizeof(dist), "%.1f mi", miles);
  }

  // Build: "2 stops • 0.6 mi • Bus 8031"
  if (stops[0] && dist[0] && bus[0]) {
    safe_snprintf(out, outsz, "%s  •  %s  •  Bus %s", stops, dist, bus);
  } else if (stops[0] && dist[0]) {
    safe_snprintf(out, outsz, "%s  •  %s", stops, dist);
  } else if (dist[0] && bus[0]) {
    safe_snprintf(out, outsz, "%s  •  Bus %s", dist, bus);
  } else if (stops[0] && bus[0]) {
    safe_snprintf(out, outsz, "%s  •  Bus %s", stops, bus);
  } else if (stops[0]) {
    safe_snprintf(out, outsz, "%s", stops);
  } else if (dist[0]) {
    safe_snprintf(out, outsz, "%s", dist);
  } else if (bus[0]) {
    safe_snprintf(out, outsz, "Bus %s", bus);
  }
}

static void fmt_eta(time_t now, time_t arr, char *big, size_t bigsz, char *suf, size_t sufsz){
  if (big && bigsz) big[0] = '\0';
  if (suf && sufsz) suf[0] = '\0';
  if (!big || bigsz==0 || !suf || sufsz==0) return;

  int sec = (int)difftime(arr, now);
  if (sec <= 30) { safe_snprintf(big, bigsz, "DUE"); return; }
  int min = (sec + 30) / 60;
  safe_snprintf(big, bigsz, "%d", min);
  safe_snprintf(suf, sufsz, "min");
}

/* --- Weather via Open-Meteo (no key) --- */
static const char *wx_icon_from_code(int code){
  // WMO weather codes (simple buckets)
  if (code == 0) return "☀";                 // clear
  if (code == 1 || code == 2) return "⛅";   // partly cloudy
  if (code == 3) return "☁";                // overcast
  if ((code >= 45 && code <= 48)) return "☁"; // fog
  if ((code >= 51 && code <= 67)) return "☂"; // drizzle/rain
  if ((code >= 71 && code <= 77)) return "❄"; // snow
  if ((code >= 80 && code <= 99)) return "☂"; // showers/thunder
  return "☁";
}

static void fetch_weather_line(char *out, size_t outsz){
  if (!out || outsz==0) return;
  out[0] = '\0';

  // Oakland Gardens (11364 centroid)
  const double lat = 40.747;
  const double lon = -73.762;

  char url[512];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=%.3f&longitude=%.3f"
    "&current=temperature_2m,weather_code"
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code"
    "&temperature_unit=fahrenheit"
    "&timezone=America%%2FNew_York"
    "&forecast_days=1",
    lat, lon);

  long code = 0;
  char *body = http_get(url, &code);
  if (!body) return;

  cJSON *root = cJSON_Parse(body);
  free(body);
  if (!root) return;

  int cur_temp = 0, cur_code = -1;
  int day_hi = 0, day_lo = 0, pop = -1, day_code = -1;

  const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
  if (cJSON_IsObject(cur)) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive((cJSON*)cur, "temperature_2m");
    const cJSON *w = cJSON_GetObjectItemCaseSensitive((cJSON*)cur, "weather_code");
    if (cJSON_IsNumber(t)) cur_temp = (int)lround(t->valuedouble);
    if (cJSON_IsNumber(w)) cur_code = (int)lround(w->valuedouble);
  }

  const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
  if (cJSON_IsObject(daily)) {
    const cJSON *hiA = cJSON_GetObjectItemCaseSensitive((cJSON*)daily, "temperature_2m_max");
    const cJSON *loA = cJSON_GetObjectItemCaseSensitive((cJSON*)daily, "temperature_2m_min");
    const cJSON *ppA = cJSON_GetObjectItemCaseSensitive((cJSON*)daily, "precipitation_probability_max");
    const cJSON *wcA = cJSON_GetObjectItemCaseSensitive((cJSON*)daily, "weather_code");

    const cJSON *hi0 = cJSON_IsArray(hiA) ? cJSON_GetArrayItem((cJSON*)hiA, 0) : NULL;
    const cJSON *lo0 = cJSON_IsArray(loA) ? cJSON_GetArrayItem((cJSON*)loA, 0) : NULL;
    const cJSON *pp0 = cJSON_IsArray(ppA) ? cJSON_GetArrayItem((cJSON*)ppA, 0) : NULL;
    const cJSON *wc0 = cJSON_IsArray(wcA) ? cJSON_GetArrayItem((cJSON*)wcA, 0) : NULL;

    if (cJSON_IsNumber(hi0)) day_hi = (int)lround(hi0->valuedouble);
    if (cJSON_IsNumber(lo0)) day_lo = (int)lround(lo0->valuedouble);
    if (cJSON_IsNumber(pp0)) pop = (int)lround(pp0->valuedouble);
    if (cJSON_IsNumber(wc0)) day_code = (int)lround(wc0->valuedouble);
  }

  cJSON_Delete(root);

  const char *icon = wx_icon_from_code((day_code >= 0) ? day_code : cur_code);
  if (pop >= 30) {
    safe_snprintf(out, outsz, "%s  Oakland Gardens: %d°F now  •  %d/%d°F today  •  Precipitation %d%%",
                  icon, cur_temp, day_hi, day_lo, pop);
  } else if (pop >= 0) {
    safe_snprintf(out, outsz, "%s  Oakland Gardens: %d°F now  •  %d/%d°F today  •  Precipitation unlikely (%d%%)",
                  icon, cur_temp, day_hi, day_lo, pop);
  } else {
    safe_snprintf(out, outsz, "%s  Oakland Gardens: %d°F now  •  %d/%d°F today",
                  icon, cur_temp, day_hi, day_lo);
  }
}

static int clampi(int v, int lo, int hi){
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void now_datetime_line(char *out, size_t outsz){
  if (!out || outsz==0) return;
  time_t t = time(NULL);
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(out, outsz, "%a %b %d  %I:%M %p", &lt);
}

int main(int argc, char **argv){
  (void)argc; (void)argv;
  fprintf(stderr, "BUILD_TAG=AB_SPLIT_STABLE_2026-01-17\n");

  const char *key = getenv("MTA_KEY");
  const char *stop_in = getenv("STOP_ID");
  if (!key || !*key) { fprintf(stderr, "ERROR: MTA_KEY is not set in environment.\n"); return 1; }
  if (!stop_in || !*stop_in) { fprintf(stderr, "ERROR: STOP_ID is not set in environment.\n"); return 1; }

  char stop_id[32];
  sanitize_stop_id(stop_in, stop_id, sizeof(stop_id));
  if (!stop_id[0]) { fprintf(stderr, "ERROR: STOP_ID invalid (got '%s'). Use digits like 501627.\n", stop_in); return 1; }

  int poll_seconds = 10;
  const char *ps = getenv("POLL_SECONDS");
  if (ps && *ps) poll_seconds = clampi(atoi(ps), 3, 120);

  const char *font_path = getenv("FONT_PATH");
  if (!font_path || !*font_path) font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

  Video V;
  if (video_init(&V, "arrival_board") != 0) return 1;

  // Layout constants: 2 columns x 6 rows always
  const int COLS = 2;
  const int ROWS = 6;

  int outer = (V.w >= 3000) ? 26 : 14;
  int gap = (V.w >= 3000) ? 18 : 10;
  int header_h = (V.h >= 2000) ? 310 : 180;

  int grid_x = outer;
  int grid_y = outer + header_h + gap;
  int grid_w = V.w - 2*outer;
  int grid_h = V.h - grid_y - outer;

  int tile_w = (grid_w - gap*(COLS-1)) / COLS;
  int tile_h = (grid_h - gap*(ROWS-1)) / ROWS;

  // Font sizing based on tile height (prevents overlap even when we increase sizes)
  int header_title_px = clampi((int)lround(V.h * 0.040), 28, 96);
  int header_sub_px   = clampi((int)lround(V.h * 0.032), 20, 80);

  int route_px = clampi((int)lround(tile_h * 0.26), 26, 92);
  int dest_px  = clampi((int)lround(tile_h * 0.22), 22, 80);
  int eta_px   = clampi((int)lround(tile_h * 0.52), 34, 160);
  int small_px = clampi((int)lround(tile_h * 0.18), 18, 68);

  fprintf(stderr, "Font px: header_title=%d header_sub=%d route=%d dest=%d eta=%d small=%d\n",
          header_title_px, header_sub_px, route_px, dest_px, eta_px, small_px);

  TTF_Font *f_header_title = load_font_or_die(font_path, header_title_px);
  TTF_Font *f_header_sub   = load_font_or_die(font_path, header_sub_px);
  TTF_Font *f_route        = load_font_or_die(font_path, route_px);
  TTF_Font *f_dest         = load_font_or_die(font_path, dest_px);
  TTF_Font *f_eta          = load_font_or_die(font_path, eta_px);
  TTF_Font *f_small        = load_font_or_die(font_path, small_px);

  if (!f_header_title || !f_header_sub || !f_route || !f_dest || !f_eta || !f_small) {
    fprintf(stderr, "ERROR: font load failed.\n");
    video_shutdown(&V);
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  Arrival arr[MAX_ARR];
  int arr_n = 0;
  char stop_name[256] = "";
  char weather_line[256] = "";
  char datetime_line[128] = "";

  time_t last_fetch = 0;
  time_t last_weather = 0;

  // Fetch weather once at startup so it appears quickly.
  fetch_weather_line(weather_line, sizeof(weather_line));
  last_weather = time(NULL);

  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
    }

    time_t now = time(NULL);

    if (now - last_weather >= 15*60) {
      fetch_weather_line(weather_line, sizeof(weather_line));
      last_weather = now;
    }

    if (now - last_fetch >= poll_seconds) {
      char url[1024];
      snprintf(url, sizeof(url),
        "https://bustime.mta.info/api/siri/stop-monitoring.json"
        "?key=%s&version=2&OperatorRef=MTA&MonitoringRef=%s"
        "&MaximumStopVisits=60&StopMonitoringDetailLevel=normal",
        key, stop_id);

      long code = 0;
      char *body = http_get(url, &code);
      if (body) {
        memset(arr, 0, sizeof(arr));
        arr_n = parse_mta_arrivals(body, arr, MAX_ARR, stop_name, sizeof(stop_name));
        fprintf(stderr, "FETCH stop=%s HTTP %ld bytes=%zu\n", stop_id, code, strlen(body));
        fprintf(stderr, "PARSE: kept %d arrivals\n", arr_n);
        free(body);
      } else {
        fprintf(stderr, "ERROR: fetch failed stop=%s (HTTP %ld)\n", stop_id, code);
        arr_n = 0;
      }
      last_fetch = now;
    }

    now_datetime_line(datetime_line, sizeof(datetime_line));

    SDL_SetRenderDrawColor(V.ren, 0, 0, 0, 255);
    SDL_RenderClear(V.ren);

    SDL_Rect hdr = { outer, outer, V.w - 2*outer, header_h };
    render_header(V.ren, f_header_title, f_header_sub, hdr,
                  stop_name, stop_id, datetime_line, weather_line);

    // Draw tiles (2 x 6)
    int idx = 0;
    for (int r=0; r<ROWS; r++) {
      for (int c=0; c<COLS; c++) {
        SDL_Rect tr = {
          grid_x + c*(tile_w + gap),
          grid_y + r*(tile_h + gap),
          tile_w, tile_h
        };

        if (idx < arr_n) {
          Arrival *A = &arr[idx];

          char eta_big[32], eta_suf[16];
          fmt_eta(now, A->arrival_epoch, eta_big, sizeof(eta_big), eta_suf, sizeof(eta_suf));

          char info[256];
          fmt_info_line(A, info, sizeof(info));

          SDL_Color rc = route_color(A->route_short);
          render_tile(V.ren, f_route, f_dest, f_eta, f_small, tr,
                      A->route_short, rc, A->dest, eta_big, eta_suf, info, A->realtime);
        } else {
          // empty placeholder tile
          SDL_Color rc = (SDL_Color){120,120,120,255};
          render_tile(V.ren, f_route, f_dest, f_eta, f_small, tr,
                      "", rc, "", "", "", "", true);
        }
        idx++;
      }
    }

    SDL_RenderPresent(V.ren);
    SDL_Delay(80);
  }

  curl_global_cleanup();
  video_shutdown(&V);
  return 0;
}
