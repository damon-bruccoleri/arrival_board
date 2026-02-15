#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/cec.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int video_init(SDL_Window **out_win, SDL_Renderer **out_ren,
               TTF_Font **font_title,
               TTF_Font **font_header,
               TTF_Font **font_route,
               TTF_Font **font_dest,
               TTF_Font **font_eta,
               TTF_Font **font_small,
               int *out_w, int *out_h);

void video_shutdown(SDL_Window *win, SDL_Renderer *ren,
                    TTF_Font *font_title,
                    TTF_Font *font_header,
                    TTF_Font *font_route,
                    TTF_Font *font_dest,
                    TTF_Font *font_eta,
                    TTF_Font *font_small);

void render_header(SDL_Renderer *ren,
                   TTF_Font *font_title,
                   TTF_Font *font_header,
                   SDL_Rect r,
                   const char *stop_name,
                   const char *stop_id,
                   const char *datetime_line,
                   const char *weather_line,
                   const char *cec_line);

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
                 bool realtime);

static uint64_t now_ms(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void str_copy(char *dst, size_t dstsz, const char *src){
  if (!dst || dstsz == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= dstsz) n = dstsz - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static uint32_t hash32(const char *s){
  uint32_t h = 2166136261u;
  for (; s && *s; s++){
    h ^= (unsigned char)(*s);
    h *= 16777619u;
  }
  return h;
}

static SDL_Color route_to_color(const char *route){
  uint32_t h = hash32(route ? route : "");
  // pleasant bright-ish palette from hash
  SDL_Color c;
  c.r = 80  + (h & 0x7F);
  c.g = 80  + ((h >> 8) & 0x7F);
  c.b = 90  + ((h >> 16) & 0x6F);
  c.a = 255;
  return c;
}

/* -----------------------
   CEC key monitoring
   ----------------------- */

typedef struct {
  int fd;
  char dev[32];
  char line[128];      // displayed string
} CECState;

static const char *ui_cmd_name(uint8_t code){
  switch (code){
    case 0x00: return "select";
    case 0x01: return "up";
    case 0x02: return "down";
    case 0x03: return "left";
    case 0x04: return "right";
    case 0x0D: return "back";
    case 0x44: return "play";
    case 0x45: return "stop";
    case 0x46: return "pause";
    case 0x48: return "rewind";
    case 0x49: return "fast-fwd";
    case 0x4B: return "skip-fwd";
    case 0x4C: return "skip-back";
    default:   return "";
  }
}

static void cec_init(CECState *cs, const char *devpath){
  if (!cs) return;
  memset(cs, 0, sizeof(*cs));
  cs->fd = -1;
  str_copy(cs->dev, sizeof(cs->dev), devpath ? devpath : "/dev/cec0");

  cs->fd = open(cs->dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (cs->fd < 0){
    snprintf(cs->line, sizeof(cs->line), "CEC: open %s failed", cs->dev);
    return;
  }

  // Put adapter in follower+passthrough mode so we can receive UI control presses
  __u32 mode = CEC_MODE_INITIATOR | CEC_MODE_EXCL_FOLLOWER_PASSTHRU;
  if (ioctl(cs->fd, CEC_S_MODE, &mode) < 0){
    mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
    (void)ioctl(cs->fd, CEC_S_MODE, &mode);
  }

  snprintf(cs->line, sizeof(cs->line), "CEC: %s ready (press remote)", cs->dev);
}

static void cec_poll(CECState *cs){
  if (!cs || cs->fd < 0) return;

  struct pollfd pfd;
  pfd.fd = cs->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  if (poll(&pfd, 1, 0) <= 0) return;
  if (!(pfd.revents & POLLIN)) return;

  // Drain a few messages quickly
  for (int i = 0; i < 6; i++){
    struct cec_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.timeout = 0; // non-blocking receive

    if (ioctl(cs->fd, CEC_RECEIVE, &msg) < 0){
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
      snprintf(cs->line, sizeof(cs->line), "CEC: recv error");
      return;
    }

    if (msg.len < 2) continue;

    uint8_t opcode = msg.msg[1];

    // USER_CONTROL_PRESSED (0x44): operand[0] is UI command
    if (opcode == 0x44 && msg.len >= 3){
      uint8_t ui = msg.msg[2];
      const char *nm = ui_cmd_name(ui);
      if (nm && *nm) snprintf(cs->line, sizeof(cs->line), "CEC: 0x%02X %s", ui, nm);
      else snprintf(cs->line, sizeof(cs->line), "CEC: 0x%02X", ui);
    }
    // USER_CONTROL_RELEASED (0x45)
    else if (opcode == 0x45){
      // keep last pressed visible; optionally show release
      // snprintf(cs->line, sizeof(cs->line), "CEC: released");
    }
  }
}

/* -----------------------
   Minimal MTA fetch/parse
   (kept intentionally simple here)
   ----------------------- */

typedef struct {
  char route[32];
  char dest[128];
  char veh[32];
  int  eta_min;
  int  stops_away;
  int  dist_m;
  bool realtime;
} Arrival;

typedef struct {
  char *data;
  size_t len;
} Mem;

static size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud){
  size_t n = sz * nm;
  Mem *m = (Mem*)ud;
  char *p = realloc(m->data, m->len + n + 1);
  if (!p) return 0;
  m->data = p;
  memcpy(m->data + m->len, ptr, n);
  m->len += n;
  m->data[m->len] = '\0';
  return n;
}

static char *http_get(const char *url, long *out_code){
  CURL *c = curl_easy_init();
  if (!c) return NULL;

  Mem m;
  m.data = NULL;
  m.len = 0;

  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "arrival_board/1.0");

  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK){
    curl_easy_cleanup(c);
    free(m.data);
    return NULL;
  }

  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  if (out_code) *out_code = code;
  return m.data;
}

static const char *jstr_loose(cJSON *obj){
  if (!obj) return NULL;
  if (cJSON_IsString(obj) && obj->valuestring) return obj->valuestring;
  if (cJSON_IsArray(obj) && cJSON_GetArraySize(obj) > 0){
    cJSON *a0 = cJSON_GetArrayItem(obj, 0);
    if (cJSON_IsString(a0) && a0->valuestring) return a0->valuestring;
    if (cJSON_IsObject(a0)){
      cJSON *v = cJSON_GetObjectItem(a0, "value");
      if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    }
  }
  if (cJSON_IsObject(obj)){
    cJSON *v = cJSON_GetObjectItem(obj, "value");
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
  }
  return NULL;
}

static int iso8601_to_epoch(const char *s, time_t *out){
  // Very small parser for: YYYY-MM-DDTHH:MM:SS[.fff]±HH:MM or Z
  if (!s || !*s || !out) return 0;

  int Y,M,D,h,m;
  int sec;
  char tzsign = 0;
  int tzh=0, tzm=0;

  // Try with timezone offset
  if (sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &Y,&M,&D,&h,&m,&sec,&tzsign,&tzh,&tzm) >= 7){
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon  = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = sec;
    time_t t = timegm(&tmv);

    if (tzsign == '+' || tzsign == '-'){
      int off = tzh*3600 + tzm*60;
      if (tzsign == '+') t -= off;
      else t += off;
    }
    *out = t;
    return 1;
  }

  // Try Zulu
  char z=0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d%c", &Y,&M,&D,&h,&m,&sec,&z) >= 6){
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon  = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = sec;
    *out = timegm(&tmv);
    return 1;
  }

  return 0;
}

static int parse_mta_arrivals(const char *json, const char *stop_ref,
                              Arrival *out, int out_cap,
                              char *out_stop_name, size_t out_stop_name_sz)
{
  if (out_stop_name && out_stop_name_sz) out_stop_name[0] = '\0';
  if (!json || !out || out_cap <= 0) return 0;

  cJSON *root = cJSON_Parse(json);
  if (!root) return 0;

  int nout = 0;

  cJSON *siri = cJSON_GetObjectItem(root, "Siri");
  cJSON *sd   = siri ? cJSON_GetObjectItem(siri, "ServiceDelivery") : NULL;
  cJSON *smd  = sd ? cJSON_GetObjectItem(sd, "StopMonitoringDelivery") : NULL;
  cJSON *d0   = (smd && cJSON_IsArray(smd) && cJSON_GetArraySize(smd)>0) ? cJSON_GetArrayItem(smd,0) : NULL;
  cJSON *msv  = d0 ? cJSON_GetObjectItem(d0, "MonitoredStopVisit") : NULL;

  if (!msv || !cJSON_IsArray(msv)){
    cJSON_Delete(root);
    return 0;
  }

  time_t now = time(NULL);

  int nvis = cJSON_GetArraySize(msv);
  for (int i=0; i<nvis && nout<out_cap; i++){
    cJSON *v = cJSON_GetArrayItem(msv, i);
    cJSON *mvj = v ? cJSON_GetObjectItem(v, "MonitoredVehicleJourney") : NULL;
    cJSON *mc  = mvj ? cJSON_GetObjectItem(mvj, "MonitoredCall") : NULL;
    if (!mc) continue;

    const char *spr = jstr_loose(cJSON_GetObjectItem(mc, "StopPointRef"));
    if (stop_ref && *stop_ref && spr && strcmp(spr, stop_ref) != 0) continue;

    // capture stop name once
    if (out_stop_name && out_stop_name_sz && out_stop_name[0] == '\0'){
      const char *sn = jstr_loose(cJSON_GetObjectItem(mc, "StopPointName"));
      if (sn && *sn) str_copy(out_stop_name, out_stop_name_sz, sn);
    }

    Arrival A;
    memset(&A, 0, sizeof(A));
    A.eta_min = 9999;
    A.stops_away = -1;
    A.dist_m = -1;
    A.realtime = true;

    // route short
    const char *pub = jstr_loose(cJSON_GetObjectItem(mvj, "PublishedLineName"));
    if (pub && *pub) str_copy(A.route, sizeof(A.route), pub);
    else {
      const char *lr = jstr_loose(cJSON_GetObjectItem(mvj, "LineRef"));
      if (lr && *lr) str_copy(A.route, sizeof(A.route), lr);
      else str_copy(A.route, sizeof(A.route), "?");
    }

    // dest
    const char *dn = jstr_loose(cJSON_GetObjectItem(mvj, "DestinationName"));
    if (dn && *dn) str_copy(A.dest, sizeof(A.dest), dn);
    else str_copy(A.dest, sizeof(A.dest), "");

    // vehicle
    const char *vr = jstr_loose(cJSON_GetObjectItem(mvj, "VehicleRef"));
    if (vr && *vr) str_copy(A.veh, sizeof(A.veh), vr);

    // eta
    const char *eat = jstr_loose(cJSON_GetObjectItem(mc, "ExpectedArrivalTime"));
    const char *aat = jstr_loose(cJSON_GetObjectItem(mc, "AimedArrivalTime"));
    time_t tarr=0;
    bool ok = iso8601_to_epoch(eat, &tarr) || iso8601_to_epoch(aat, &tarr);
    if (ok){
      int diff = (int)difftime(tarr, now);
      int min = diff / 60;
      if (min < 0) min = 0;
      A.eta_min = min;
    }

    // realtime?
    cJSON *mon = cJSON_GetObjectItem(mvj, "Monitored");
    if (cJSON_IsBool(mon)) A.realtime = cJSON_IsTrue(mon);

    // stops/dist
    cJSON *ns = cJSON_GetObjectItem(mc, "NumberOfStopsAway");
    if (cJSON_IsNumber(ns)) A.stops_away = ns->valueint;
    cJSON *df = cJSON_GetObjectItem(mc, "DistanceFromStop");
    if (cJSON_IsNumber(df)) A.dist_m = (int)(df->valuedouble + 0.5);

    out[nout++] = A;
  }

  cJSON_Delete(root);

  // simple sort by eta_min ascending
  for (int i=0;i<nout;i++){
    for (int j=i+1;j<nout;j++){
      if (out[j].eta_min < out[i].eta_min){
        Arrival tmp = out[i]; out[i]=out[j]; out[j]=tmp;
      }
    }
  }

  return nout;
}

/* -----------------------
   Main
   ----------------------- */
int main(void){
  const char *mta_key  = getenv("MTA_KEY");
  const char *stop_id  = getenv("STOP_ID");
  const char *stop_ref = stop_id ? stop_id : "";

  if (!mta_key || !*mta_key){
    fprintf(stderr, "ERROR: MTA_KEY is not set in environment.\n");
    return 2;
  }
  if (!stop_ref || !*stop_ref){
    fprintf(stderr, "ERROR: STOP_ID is not set in environment.\n");
    return 2;
  }

  SDL_Window *win = NULL;
  SDL_Renderer *ren = NULL;
  TTF_Font *font_title=NULL, *font_header=NULL, *font_route=NULL, *font_dest=NULL, *font_eta=NULL, *font_small=NULL;
  int W=0,H=0;

  if (video_init(&win, &ren, &font_title, &font_header, &font_route, &font_dest, &font_eta, &font_small, &W, &H) != 0){
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  CECState cec;
  cec_init(&cec, "/dev/cec0");

  uint64_t last_fetch = 0;
  char stop_name[256]; stop_name[0] = '\0';
  char weather_line[256]; str_copy(weather_line, sizeof(weather_line), "Weather: (unavailable)");

  Arrival arr[24];
  int arr_n = 0;

  bool running = true;

  while (running){
    SDL_Event e;
    while (SDL_PollEvent(&e)){
      if (e.type == SDL_QUIT) running = false;
    }

    cec_poll(&cec);

    uint64_t t = now_ms();
    if (t - last_fetch > 10000){
      last_fetch = t;

      char url[512];
      snprintf(url, sizeof(url),
               "https://bustime.mta.info/api/siri/stop-monitoring.json?key=%s&MonitoringRef=%s&StopMonitoringDetailLevel=normal",
               mta_key, stop_ref);

      long code = 0;
      char *body = http_get(url, &code);
      if (body && code == 200){
        arr_n = parse_mta_arrivals(body, stop_ref, arr, (int)(sizeof(arr)/sizeof(arr[0])),
                                   stop_name, sizeof(stop_name));
        free(body);
      } else {
        free(body);
      }
    }

    // Date/time line
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char dt[64];
    strftime(dt, sizeof(dt), "%a %b %d  %I:%M %p", &lt);

    // Layout: header + 2 cols x 6 rows
    int header_h = (H >= 2000) ? 300 : 180;
    SDL_Rect r_header = { 0, 0, W, header_h };

    SDL_SetRenderDrawColor(ren, 10, 12, 16, 255);
    SDL_RenderClear(ren);

    render_header(ren, font_title, font_header, r_header,
                  stop_name[0] ? stop_name : NULL,
                  stop_ref, dt, weather_line,
                  cec.line[0] ? cec.line : NULL);

    int grid_y = header_h + 16;
    int grid_h = H - grid_y - 16;
    int cols = 2;
    int rows = 6;
    int gap = (W >= 3000) ? 16 : 10;

    int tile_w = (W - gap*(cols+1)) / cols;
    int tile_h = (grid_h - gap*(rows+1)) / rows;

    int show = cols * rows;
    if (show > arr_n) show = arr_n;

    for (int i=0; i<cols*rows; i++){
      int c = i % cols;
      int r = i / cols;
      SDL_Rect tr = { gap + c*(tile_w+gap), grid_y + gap + r*(tile_h+gap), tile_w, tile_h };

      if (i < arr_n){
        Arrival *A = &arr[i];

        char eta_big[16], eta_suf[8];
        if (A->eta_min <= 0){
          str_copy(eta_big, sizeof(eta_big), "0");
          str_copy(eta_suf, sizeof(eta_suf), "min");
        } else {
          snprintf(eta_big, sizeof(eta_big), "%d", A->eta_min);
          str_copy(eta_suf, sizeof(eta_suf), "min");
        }

        char dist_line[128];
        char veh_line[64];

        // stops/dist on left, vehicle on right
        if (A->stops_away >= 0 && A->dist_m >= 0){
          snprintf(dist_line, sizeof(dist_line), "%d stops  •  %dm", A->stops_away, A->dist_m);
        } else if (A->stops_away >= 0){
          snprintf(dist_line, sizeof(dist_line), "%d stops", A->stops_away);
        } else if (A->dist_m >= 0){
          snprintf(dist_line, sizeof(dist_line), "%dm", A->dist_m);
        } else {
          dist_line[0] = '\0';
        }

        if (A->veh[0]) snprintf(veh_line, sizeof(veh_line), "%s", A->veh);
        else veh_line[0] = '\0';

        SDL_Color rc = route_to_color(A->route);

        render_tile(ren, font_route, font_dest, font_eta, font_small, tr,
                    A->route, rc,
                    A->dest,
                    eta_big, eta_suf,
                    dist_line, veh_line,
                    A->realtime);
      } else {
        SDL_Color emptyc = { 30, 32, 40, 255 };
        SDL_SetRenderDrawColor(ren, emptyc.r, emptyc.g, emptyc.b, emptyc.a);
        SDL_RenderFillRect(ren, &tr);
      }
    }

    SDL_RenderPresent(ren);
    SDL_Delay(16);
  }

  if (cec.fd >= 0) close(cec.fd);

  curl_global_cleanup();
  video_shutdown(win, ren, font_title, font_header, font_route, font_dest, font_eta, font_small);
  return 0;
}
