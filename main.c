#include "tile.h"
#include <cjson/cJSON.h>
#ifdef USE_SDL_IMAGE
#include <SDL2/SDL_image.h>
#endif

#define LAYOUT_VER "2"   /* ROUTE - DESTINATION line 1; bus # only on line 2 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Arrival {
    char route[32];
    char bus[32];
    char dest[128];
    int  stops_away;     // -1 if unknown
    int  mins;           // minutes until arrival, -1 if unknown
    time_t expected;
    double miles_away;   // miles until stop, <0 if unknown
    int ppl_est;         // estimated people count
} Arrival;

typedef struct Weather {
    int have;
    char icon[8];       // unicode symbol
    int temp_f;
    int precip_prob;    // percent, -1 if unknown
    double precip_in;   // inches, -1 if unknown
    time_t last_fetch;
    double lat, lon;
} Weather;

static int clampi(int v, int lo, int hi){ return (v<lo)?lo:(v>hi)?hi:v; }

static void logf_(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const cJSON* jgeto(const cJSON *o, const char *k){
    if(!o || !cJSON_IsObject(o)) return NULL;
    return cJSON_GetObjectItemCaseSensitive((cJSON*)o, k);
}
static const cJSON* jgeti(const cJSON *a, int idx){
    if(!a || !cJSON_IsArray(a)) return NULL;
    return cJSON_GetArrayItem((cJSON*)a, idx);
}
static const char* jgets(const cJSON *v){
    if(!v) return NULL;
    if(cJSON_IsString(v) && v->valuestring) return v->valuestring;
    return NULL;
}
static int jint(const cJSON *v, int defv){
    if(!v) return defv;
    if(cJSON_IsNumber(v)) return v->valueint;
    if(cJSON_IsString(v) && v->valuestring){
        return atoi(v->valuestring);
    }
    return defv;
}
static double jdouble(const cJSON *v, double defv){
    if(!v) return defv;
    if(cJSON_IsNumber(v)) return v->valuedouble;
    if(cJSON_IsString(v) && v->valuestring){
        return atof(v->valuestring);
    }
    return defv;
}

static void normalize_route(char *dst, size_t dstsz, const char *src){
    if(!src){ snprintf(dst, dstsz, "?"); return; }
    const char *p = src;
    const char *last = src;
    for(; *p; p++){
        if(*p=='_' || *p==':' || *p=='/') last = p+1;
    }
    snprintf(dst, dstsz, "%s", last);
}

/* Very small urlencode */
static void urlencode(char *out, size_t outsz, const char *in){
    static const char *hex = "0123456789ABCDEF";
    size_t o=0;
    for(size_t i=0; in && in[i] && o+4<outsz; i++){
        unsigned char c = (unsigned char)in[i];
        if( (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-'||c=='_'||c=='.'||c=='~'){
            out[o++] = (char)c;
        } else if(c==' '){
            out[o++] = '%'; out[o++]='2'; out[o++]='0';
        } else {
            out[o++] = '%';
            out[o++] = hex[(c>>4)&0xF];
            out[o++] = hex[c&0xF];
        }
    }
    out[o]='\0';
}

static char* http_get(const char *url){
    if(!url) return NULL;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -fsSL --connect-timeout 4 --max-time 8 '%s'", url);
    FILE *fp = popen(cmd, "r");
    if(!fp) return NULL;

    size_t cap = 64*1024;
    size_t len = 0;
    char *buf = (char*)malloc(cap);
    if(!buf){ pclose(fp); return NULL; }

    int ch;
    while((ch = fgetc(fp)) != EOF){
        if(len + 2 > cap){
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if(!nb){ free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

/* Parse ISO8601 with optional fractional seconds and timezone offset. */
static time_t parse_iso8601(const char *s){
    if(!s) return (time_t)0;

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", s);

    char *dot = strchr(tmp, '.');
    if(dot){
        char *tz = strchr(dot, 'Z');
        if(!tz) tz = strchr(dot, '+');
        if(!tz){
            char *tpos = strchr(tmp, 'T');
            for(char *p = dot; p && *p; p++){
                if(*p=='-' && tpos && p>tpos){ tz = p; break; }
            }
        }
        if(tz) memmove(dot, tz, strlen(tz)+1);
        else *dot = '\0';
    }

    int tzsign = 0, tzh=0, tzm=0;
    char *z = strchr(tmp, 'Z');
    char *plus = strchr(tmp, '+');
    char *minus = NULL;

    char *tpos = strchr(tmp, 'T');
    if(tpos){
        for(char *p = tpos; *p; p++){
            if(*p=='-') minus = p;
        }
    }

    char *tzp = NULL;
    if(plus) tzp = plus;
    else if(minus) tzp = minus;

    if(z){
        *z = '\0';
        tzsign = 0;
    } else if(tzp){
        tzsign = (*tzp == '-') ? -1 : +1;
        char off[8]; snprintf(off, sizeof(off), "%s", tzp);
        *tzp = '\0';
        if(strlen(off) >= 6){
            tzh = atoi(off+1);
            tzm = atoi(off+4);
        }
    }

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    char *ok = strptime(tmp, "%Y-%m-%dT%H:%M:%S", &tmv);
    if(!ok) return (time_t)0;

    time_t t = timegm(&tmv);
    if(z) return t;
    if(tzp){
        int offset = tzsign * (tzh*3600 + tzm*60);
        return t - offset;
    }
    return t;
}

/* Tunable estimate. NOTE: BusTime does not provide real occupancy, so this is a heuristic.
   We cache the env-derived parameters so they are only parsed once. */
static int estimate_people(int mins, int stops_away){
    static int inited = 0;
    static double base = 1.0;
    static double per_min = 0.22;
    static double per_stop = 0.60;
    static int cap = 45;

    if(!inited){
        const char *s;
        if((s=getenv("PPL_BASE")) && *s) base = atof(s);
        if((s=getenv("PPL_PER_MIN")) && *s) per_min = atof(s);
        if((s=getenv("PPL_PER_STOP")) && *s) per_stop = atof(s);
        if((s=getenv("PPL_CAP")) && *s) cap = atoi(s);
        inited = 1;
    }

    if(cap < 5) cap = 5;
    if(cap > 200) cap = 200;

    int m = (mins >= 0) ? mins : 0;
    int st = (stops_away >= 0) ? stops_away : 0;

    /* Use minutes as primary driver; stops-away only adds a small correction. */
    double ppl = base + per_min * (double)m + per_stop * (double)st;

    /* If bus is imminent, crowd should usually be smaller. */
    if(mins >= 0 && mins <= 2) ppl *= 0.70;

    int ip = (int)lrint(ppl);
    return clampi(ip, 0, cap);
}

static int route_allowed(const char *route, const char *filter_csv){
    if(!filter_csv || !*filter_csv) return 1;
    if(!route || !*route) return 0;

    char copy[256];
    snprintf(copy, sizeof(copy), "%s", filter_csv);

    char *save=NULL;
    for(char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)){
        while(*tok==' ') tok++;
        size_t n = strlen(tok);
        while(n>0 && tok[n-1]==' ') tok[--n]='\0';
        if(n==0) continue;
        if(strcasecmp(tok, route)==0) return 1;
    }
    return 0;
}

static int parse_stops_away(const cJSON *journey){
    const cJSON *mc = jgeto(journey, "MonitoredCall");
    const cJSON *ext = mc ? jgeto(mc, "Extensions") : NULL;
    const cJSON *dist = ext ? jgeto(ext, "Distances") : NULL;

    int v = jint(jgeto(dist, "StopsFromCall"), -1);
    if(v >= 0) return v;

    v = jint(jgeto(dist, "StopsAway"), -1);
    if(v >= 0) return v;

    const cJSON *ext2 = jgeto(journey, "Extensions");
    const cJSON *dist2 = ext2 ? jgeto(ext2, "Distances") : NULL;
    v = jint(jgeto(dist2, "StopsFromCall"), -1);
    if(v >= 0) return v;

    return -1;
}

/* Parse distance to stop in meters from the SIRI Distances block and convert to miles.
   Returns <0.0 if unknown. */
static double parse_miles_away(const cJSON *journey){
    const cJSON *mc  = jgeto(journey, "MonitoredCall");
    const cJSON *ext = mc ? jgeto(mc, "Extensions") : NULL;
    const cJSON *dist = ext ? jgeto(ext, "Distances") : NULL;

    double meters = jdouble(jgeto(dist, "DistanceFromCall"), -1.0);
    if(meters < 0.0) meters = jdouble(jgeto(dist, "DistanceFromStop"), -1.0);

    if(meters < 0.0){
        const cJSON *ext2  = jgeto(journey, "Extensions");
        const cJSON *dist2 = ext2 ? jgeto(ext2, "Distances") : NULL;
        meters = jdouble(jgeto(dist2, "DistanceFromCall"), -1.0);
        if(meters < 0.0) meters = jdouble(jgeto(dist2, "DistanceFromStop"), -1.0);
    }

    if(meters < 0.0) return -1.0;
    return meters / 1609.344; /* meters -> miles */
}

static void parse_stop_name(char *out, size_t outsz, const cJSON *delivery){
    if(!out || outsz==0){ return; }
    out[0]='\0';

    const cJSON *visits = jgeto(delivery, "MonitoredStopVisit");
    const cJSON *v0 = jgeti(visits, 0);
    if(!v0) return;

    const cJSON *mvj = jgeto(v0, "MonitoredVehicleJourney");
    const cJSON *mc  = mvj ? jgeto(mvj, "MonitoredCall") : NULL;

    const cJSON *spn = mc ? jgeto(mc, "StopPointName") : NULL;
    if(jgets(spn)){
        snprintf(out, outsz, "%s", jgets(spn));
        return;
    }
    const cJSON *spn0 = jgeti(spn, 0);
    if(jgets(spn0)){
        snprintf(out, outsz, "%s", jgets(spn0));
        return;
    }
}

static int fetch_mta_arrivals(Arrival *arr, int max_arr,
                             char *stop_name, size_t stop_name_sz,
                             const char *mta_key, const char *stop_id,
                             const char *route_filter) {
    if(!arr || max_arr<=0) return 0;
    if(stop_name && stop_name_sz) stop_name[0]='\0';

    if(!mta_key || !*mta_key || !stop_id || !*stop_id) return 0;

    char url[1024];
    snprintf(url, sizeof(url),
             "https://bustime.mta.info/api/siri/stop-monitoring.json?key=%s&MonitoringRef=%s&OperatorRef=MTA&MaximumStopVisits=%d",
             mta_key, stop_id, max_arr);

    char *json = http_get(url);
    if(!json) return 0;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if(!root) return 0;

    int count = 0;

    const cJSON *siri = jgeto(root, "Siri");
    const cJSON *sd = siri ? jgeto(siri, "ServiceDelivery") : NULL;
    const cJSON *smd = sd ? jgeto(sd, "StopMonitoringDelivery") : NULL;
    const cJSON *del = jgeti(smd, 0);

    if(del && stop_name && stop_name_sz){
        parse_stop_name(stop_name, stop_name_sz, del);
    }

    const cJSON *visits = del ? jgeto(del, "MonitoredStopVisit") : NULL;
    int n = visits && cJSON_IsArray(visits) ? cJSON_GetArraySize((cJSON*)visits) : 0;

    time_t now = time(NULL);

    for(int i=0; i<n && count<max_arr; i++){
        const cJSON *v = jgeti(visits, i);
        const cJSON *mvj = v ? jgeto(v, "MonitoredVehicleJourney") : NULL;
        if(!mvj) continue;

        char route[32] = {0};
        normalize_route(route, sizeof(route), jgets(jgeto(mvj, "LineRef")));
        if(!route_allowed(route, route_filter)) continue;

        const char *veh = jgets(jgeto(mvj, "VehicleRef"));
        const cJSON *mc = jgeto(mvj, "MonitoredCall");

        /* destination: DestinationName can be a string or an array; prefer first element if array */
        char destbuf[128] = {0};
        const cJSON *destv = jgeto(mvj, "DestinationName");
        const char *dstr = jgets(destv);
        if(!dstr){
            const cJSON *d0 = jgeti(destv, 0);
            dstr = jgets(d0);
        }
        if(dstr && *dstr){
            snprintf(destbuf, sizeof(destbuf), "%s", dstr);
        } else {
            snprintf(destbuf, sizeof(destbuf), "--");
        }

        const char *tiso = NULL;
        if(mc){
            tiso = jgets(jgeto(mc, "ExpectedArrivalTime"));
            if(!tiso) tiso = jgets(jgeto(mc, "AimedArrivalTime"));
        }
        time_t exp = parse_iso8601(tiso);
        int mins = -1;
        if(exp > 0){
            double d = difftime(exp, now);
            mins = (int)lrint(d / 60.0);
            if(mins < 0) mins = 0;
        }

        int stops = parse_stops_away(mvj);
        double miles = parse_miles_away(mvj);

        Arrival a;
        memset(&a, 0, sizeof(a));
        snprintf(a.route, sizeof(a.route), "%s", route);
        snprintf(a.bus, sizeof(a.bus), "%s", veh ? veh : "--");
        snprintf(a.dest, sizeof(a.dest), "%s", destbuf);
        a.stops_away = stops;
        a.mins = mins;
        a.expected = exp;
        a.miles_away = miles;
        a.ppl_est = estimate_people(mins, stops);

        arr[count++] = a;
    }

    cJSON_Delete(root);
    return count;
}

/* Open-Meteo weather code -> Unicode symbol. Use hex UTF-8 so encoding is independent of source file. */
static void icon_for_code(int code, char *out, size_t outsz){
    const char *sym;
    switch(code){
        case 0:   sym = "\xE2\x98\x80"; break;  /* ☀ U+2600 */
        case 1:
        case 2:   sym = "\xE2\x9B\x85"; break;  /* ⛅ U+26C5 */
        case 3:
        case 45:
        case 48:  sym = "\xE2\x98\x81"; break;  /* ☁ U+2601 */
        default:
            if(code >= 51 && code <= 57) sym = "\xE2\x98\x94";   /* ☔ U+2614 */
            else if(code >= 61 && code <= 67) sym = "\xE2\x98\x94";
            else if(code >= 71 && code <= 77) sym = "\xE2\x9D\x84"; /* ❄ U+2744 */
            else if(code >= 80 && code <= 82) sym = "\xE2\x98\x94";
            else if(code >= 95 || code == 96 || code == 99) sym = "\xE2\x9A\xA1"; /* ⚡ U+26A1 */
            else sym = "\xE2\x98\x81";  /* ☁ default */
            break;
    }
    size_t len = strlen(sym);
    if(len + 1 <= outsz){ memcpy(out, sym, len + 1); }
    else { out[0] = '\0'; }
}

/* Geocode a place string to lat/lon (e.g. "Stop Name" + suffix). Suffix from WEATHER_GEOCODE_SUFFIX or ", New York City". */
static int geocode_stop_to_latlon(const char *stop_name, double *olat, double *olon){
    if(!stop_name || !*stop_name) return 0;
    const char *suffix = getenv("WEATHER_GEOCODE_SUFFIX");
    if(!suffix || !*suffix) suffix = ", New York City";
    char q[512], enc[1024];
    snprintf(q, sizeof(q), "%s%s", stop_name, suffix);
    urlencode(enc, sizeof(enc), q);

    char url[1400];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
             enc);

    char *json = http_get(url);
    if(!json) return 0;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if(!root) return 0;

    const cJSON *results = jgeto(root, "results");
    const cJSON *r0 = jgeti(results, 0);
    if(!r0){ cJSON_Delete(root); return 0; }

    double lat = jdouble(jgeto(r0, "latitude"), 0.0);
    double lon = jdouble(jgeto(r0, "longitude"), 0.0);
    cJSON_Delete(root);

    if(fabs(lat) < 0.001 || fabs(lon) < 0.001) return 0;
    *olat = lat; *olon = lon;
    return 1;
}

/* Weather is tied to the stop when possible: STOP_LAT/STOP_LON (this stop) or geocode of stop name, else WEATHER_LAT/LON fallback. */
static void fetch_weather(Weather *w, const char *stop_name){
    if(!w) return;

    time_t now = time(NULL);
    if(w->last_fetch && difftime(now, w->last_fetch) < 600) return;

    if(fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001){
        const char *slat = getenv("STOP_LAT");
        const char *slon = getenv("STOP_LON");
        if(slat && slon){
            w->lat = atof(slat);
            w->lon = atof(slon);
        }
    }
    if(fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001){
        double lat=0, lon=0;
        if(geocode_stop_to_latlon(stop_name, &lat, &lon)){
            w->lat = lat; w->lon = lon;
        }
    }
    if(fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001){
        const char *elat = getenv("WEATHER_LAT");
        const char *elon = getenv("WEATHER_LON");
        if(elat && elon){
            w->lat = atof(elat);
            w->lon = atof(elon);
        }
    }
    if(fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001){
        w->lat = 40.7128;
        w->lon = -74.0060;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&timezone=America%%2FNew_York&temperature_unit=fahrenheit&precipitation_unit=inch&current=temperature_2m,precipitation,weather_code&hourly=precipitation_probability",
             w->lat, w->lon);

    char *json = http_get(url);
    if(!json){ w->have = 0; return; }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if(!root){ w->have = 0; return; }

    const cJSON *cur = jgeto(root, "current");
    int temp = jint(jgeto(cur, "temperature_2m"), -999);
    double precip = jdouble(jgeto(cur, "precipitation"), -1.0);
    int code = jint(jgeto(cur, "weather_code"), -1);

    char icon[8]; icon_for_code(code, icon, sizeof(icon));

    int prob = -1;
    const cJSON *hourly = jgeto(root, "hourly");
    const cJSON *pp = hourly ? jgeto(hourly, "precipitation_probability") : NULL;
    const cJSON *pp0 = jgeti(pp, 0);
    if(pp0) prob = jint(pp0, -1);

    cJSON_Delete(root);

    w->have = 1;
    snprintf(w->icon, sizeof(w->icon), "%s", icon);
    w->temp_f = temp;
    w->precip_prob = prob;
    w->precip_in = precip;
    w->last_fetch = now;
}

#define TILE_COLS_FIXED 2
#define TILE_ROWS_FIXED  6
#define TILE_SLOTS_MAX   (TILE_COLS_FIXED * TILE_ROWS_FIXED)

#define STEAM_PUFFS  2
#define STEAM_PUFF_SIZE  96

typedef struct SteamPuff {
    float x, y;
    float alpha;
    float scale;
    float rise;
} SteamPuff;

/* Filled circle (for robot eyes). cx,cy in screen coords; radius in pixels. */
static void draw_filled_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color c){
    if(radius <= 0) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    for(int y = -radius; y <= radius; y++){
        int dx = (int)(sqrtf((float)(radius*radius - y*y)) + 0.5f);
        if(dx < 0) continue;
        SDL_Rect line = { cx - dx, cy + y, 2*dx, 1 };
        SDL_RenderFillRect(r, &line);
    }
}

/* Draw one tile's content (background + text) into rect. Used for both normal draw and flip texture. */
static void draw_tile_content(SDL_Renderer *r, Fonts *f, const Arrival *a,
                              SDL_Rect rect, float scale,
                              SDL_Color white, SDL_Color dim, int radius){
    int inner = clampi((int)(32*scale), 12, 60);
    int x = rect.x + inner;
    int y = rect.y + clampi((int)(20*scale), 8, 40);  /* Move route text up */

    SDL_SetRenderDrawColor(r, 18, 20, 26, 255);
    fill_round_rect(r, rect, radius);

    char busnum[64];
    const char *busid = a->bus;
    if(!busid || !busid[0]) snprintf(busnum, sizeof(busnum), "--");
    else {
        const char *p = strrchr(busid, '_');
        if(p && p[1]) busid = p + 1;
        else {
            const char *sp = strrchr(busid, ' ');
            if(sp && sp[1]) busid = sp + 1;
        }
        snprintf(busnum, sizeof(busnum), "%s", busid);
    }
    const char *route = a->route[0] ? a->route : "--";
    const char *dest  = a->dest[0]  ? a->dest  : "--";
    char minsbuf[16];
    if(a->mins == 0) snprintf(minsbuf, sizeof(minsbuf), "NOW");
    else if(a->mins > 0) snprintf(minsbuf, sizeof(minsbuf), "%d", a->mins);
    else snprintf(minsbuf, sizeof(minsbuf), "--");

    int route_w = 0, eta_w = 0;
    text_size(f->tile_big, route, &route_w, NULL);
    text_size(f->tile_big, minsbuf, &eta_w, NULL);
    int line1_gap = clampi((int)(10*scale), 6, 20);
    int max_dest_w = rect.w - 2*inner - route_w - eta_w - line1_gap*2;
    if(max_dest_w < 40) max_dest_w = 40;

    draw_text(r, f->tile_big, route, x, y, white, 0);
    char dest_line[256];
    snprintf(dest_line, sizeof(dest_line), " - %s", dest);
    draw_text_trunc(r, f->tile_med, dest_line, x + route_w + line1_gap, y, max_dest_w, dim, 0);
    draw_text(r, f->tile_big, minsbuf, rect.x + rect.w - inner, y, white, 2);

    int y2 = y + clampi((int)(120*scale), 70, 190);
    char stopsbuf[32];
    if(a->stops_away >= 0) snprintf(stopsbuf, sizeof(stopsbuf), "%d", a->stops_away);
    else snprintf(stopsbuf, sizeof(stopsbuf), "--");
    char milesbuf[32];
    if(a->miles_away >= 0.0) snprintf(milesbuf, sizeof(milesbuf), "%.1f", (double)a->miles_away);
    else snprintf(milesbuf, sizeof(milesbuf), "--");
    char meta[256];
    snprintf(meta, sizeof(meta), "%s stops  •  %d ppl  •  BUS %s  •  %s mi",
             stopsbuf, a->ppl_est, busnum, milesbuf);
    draw_text(r, f->tile_small, meta, x, y2, dim, 0);
    if(a->mins != 0)
        draw_text(r, f->tile_small, "min", rect.x + rect.w - inner, y2, dim, 2);
}

static void render_ui(SDL_Renderer *r, Fonts *f,
                      int W, int H,
                      const char *stop_id,
                      const char *stop_name,
                      Weather *wx,
                      Arrival *arr, int n,
                      SDL_Texture *bg_tex,
                      SDL_Texture *steam_tex,
                      SDL_Texture *logo_tex,
                      TTF_Font *symbol_font) {
    SDL_Color white = {255,255,255,255};
    SDL_Color dim   = {210,210,210,255};

    SDL_SetRenderDrawColor(r, 10, 12, 16, 255);
    SDL_RenderClear(r);

    float scale = (H>0) ? ((float)H / 2160.0f) : 1.0f;
    int pad = clampi((int)(46*scale), 18, 90);
    int header_h = clampi((int)(260*scale), 140, 420);
    int body_y = pad + header_h + pad;
    int body_h = H - body_y - pad;
    if(body_h < 100) body_h = 100;

    /* Background image: stretch so top = header bottom, bottom = screen bottom, left/right = screen edges. */
    if(bg_tex){
        SDL_Rect dst = { 0, body_y, W, H - body_y };
        SDL_SetTextureAlphaMod(bg_tex, 76);  /* 30% opacity at draw time */
        SDL_RenderCopy(r, bg_tex, NULL, &dst);
        SDL_SetTextureAlphaMod(bg_tex, 255);
    }

    /* Steam/smoke drift: puffs from exhaust pipe openings, using image scaled/translated position */
    if(steam_tex){
        static SteamPuff puffs[STEAM_PUFFS];
        static int init;
        /* Exhaust positions in IMAGE space (fraction 0–1 of the drawn background rect).
         * Background is drawn at dst = (0, body_y, W, body_h), so image-space (fx, fy) maps to
         * screen: x = fx*W, y = body_y + fy*body_h. */
        const float exhaust_img_x[STEAM_PUFFS] = { 0.22f, 0.78f };   /* left, right only */
        const float exhaust_img_y[STEAM_PUFFS] = { 0.88f, 0.88f };
        const float rise_speed = 4.4f;    /* drift up (2x faster again) */
        const float fade_speed = 0.28f;   /* fade out as they drift */
        const float scale_grow = 0.0012f;
        const float puff_size_mult = 2.f;  /* puffs 2x larger */
        const float start_alpha = 64.f;    /* 25% opacity */

        /* Origin offsets: left -70x,-470y; right +290x,-660y */
        const float origin_dx[STEAM_PUFFS] = { -70.f, 290.f };   /* left: 10 -x; right: 10 +x, 20 -y */
        const float origin_dy[STEAM_PUFFS] = { -470.f, -660.f };
        const float drift_right_per_up = 1.0f;  /* 1 px right per 1 px up */

        if(!init){
            for(int i = 0; i < STEAM_PUFFS; i++){
                float ex_x = (float)W * exhaust_img_x[i] + origin_dx[i];
                float ex_y = (float)body_y + (float)body_h * exhaust_img_y[i] + origin_dy[i];
                puffs[i].x = ex_x + (float)((i * 17) % 21 - 10);
                puffs[i].y = ex_y + (float)((i * 11) % 12);
                puffs[i].alpha = start_alpha;
                puffs[i].scale = 0.7f + (float)(i % 3) * 0.1f;
                puffs[i].rise = rise_speed + (float)(i % 2) * 1.2f;
            }
            init = 1;
        }

        SDL_SetTextureBlendMode(steam_tex, SDL_BLENDMODE_BLEND);
        for(int i = 0; i < STEAM_PUFFS; i++){
            puffs[i].y -= puffs[i].rise;
            puffs[i].x += drift_right_per_up * puffs[i].rise;  /* 7/5 px right per px up */
            puffs[i].alpha -= fade_speed;
            puffs[i].scale += scale_grow;

            if(puffs[i].alpha <= 0.f || puffs[i].y < (float)(body_y - 120)){
                /* Respawn at this exhaust (image-space position → screen) + origin offset */
                float ex_x = (float)W * exhaust_img_x[i] + origin_dx[i];
                float ex_y = (float)body_y + (float)body_h * exhaust_img_y[i] + origin_dy[i];
                puffs[i].x = ex_x + (float)((i * 17) % 21 - 10);
                puffs[i].y = ex_y + (float)((i * 11) % 12);
                puffs[i].alpha = start_alpha;
                puffs[i].scale = 0.6f + (float)(i % 3) * 0.1f;
                puffs[i].rise = rise_speed + (float)(i % 2) * 1.0f;
            }

            int a = (int)puffs[i].alpha;
            if(a > 0){
                int sz = (int)(STEAM_PUFF_SIZE * puffs[i].scale * puff_size_mult);
                if(sz < 12) sz = 12;
                SDL_Rect dst = {
                    (int)puffs[i].x - sz/2,
                    (int)puffs[i].y - sz/2,
                    sz, sz
                };
                SDL_SetTextureAlphaMod(steam_tex, a > 255 ? 255 : a);
                SDL_RenderCopy(r, steam_tex, NULL, &dst);
            }
        }
    }

    /* Robot eyes: two pulsing cyan circles; offsets to align with face in image. */
    {
        const float eye_left_fx = 0.38f, eye_left_fy = 0.22f;
        const float eye_right_fx = 0.62f, eye_right_fy = 0.22f;
        const int eye_left_dy = 366;   /* eyes: 4px -x, 4px -y */
        const int eye_right_dy = 356;
        const int eye_left_dx = -49;
        const int eye_right_dx = -924;
        int eye_radius = clampi((int)(18*scale), 8, 36);
        int cx_left  = (int)((float)W * eye_left_fx + 0.5f) + eye_left_dx;
        int cy_left  = (int)(body_y + (float)(H - body_y) * eye_left_fy + 0.5f) + eye_left_dy;
        int cx_right = (int)((float)W * eye_right_fx + 0.5f) + eye_right_dx;
        int cy_right = (int)(body_y + (float)(H - body_y) * eye_right_fy + 0.5f) + eye_right_dy;
        float t = (float)SDL_GetTicks() * 0.001f;
        float pulse = 0.5f + 0.5f * sinf(t * 6.283185f * 2.2f);  /* smooth, ~2.2 Hz */
        int alpha = 140 + (int)(100.f * pulse);
        if(alpha > 255) alpha = 255;
        SDL_Color cyan = { 0, 200, 255, (Uint8)alpha };
        draw_filled_circle(r, cx_left,  cy_left,  eye_radius, cyan);
        draw_filled_circle(r, cx_right, cy_right, eye_radius, cyan);
    }

    SDL_Rect hdr = { pad, pad, W - 2*pad, header_h };
    SDL_SetRenderDrawColor(r, 22, 26, 34, 255);
    fill_round_rect(r, hdr, clampi((int)(24*scale), 10, 40));

    /* Application name centered at top of header */
    int title_y = hdr.y + clampi((int)(22*scale), 10, 36);
    draw_text(r, f->h1, "Arrival Board", hdr.x + hdr.w / 2, title_y, white, 1);
    char left1[256];
    if(stop_name && *stop_name) snprintf(left1, sizeof(left1), "%s", stop_name);
    else snprintf(left1, sizeof(left1), "Stop %s", stop_id ? stop_id : "--");

    int left_x = hdr.x + pad;
    int top_y  = hdr.y + clampi((int)(52*scale), 28, 80);

    /* Stop name: smaller font (h2) */
    draw_text_trunc(r, f->h2, left1, left_x, top_y, hdr.w - 2*pad - (int)(560*scale), white, 0);

    char left2[256];
    snprintf(left2, sizeof(left2), "Stop %s", stop_id ? stop_id : "--");
    /* add a bit more vertical space between the two header text lines */
    draw_text(r, f->h2, left2, left_x, top_y + clampi((int)(78*scale), 44, 120), dim, 0);

    /* Top right: date/time and weather+precip, same font size as stop number (h2) */
    int right_x = hdr.x + hdr.w - pad;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char ts[64];
    strftime(ts, sizeof(ts), "%a %b %-d  %-I:%M %p", &lt);
    int ts_h = 0;
    text_size(f->h2, ts, NULL, &ts_h);
    int right_line_gap = clampi((int)(12*scale), 6, 24);
    draw_text(r, f->h2, ts, right_x, hdr.y + pad, white, 2);

    if(wx && wx->have){
        /* Weather + temp + precipitation on one line; same font size as date/time (h2) */
        char wline[128];
        if(wx->precip_prob >= 0)
            snprintf(wline, sizeof(wline), "%s  %d°F   Precip %d%%", wx->icon, wx->temp_f, wx->precip_prob);
        else if(wx->precip_in >= 0)
            snprintf(wline, sizeof(wline), "%s  %d°F   Precip %.2f in", wx->icon, wx->temp_f, wx->precip_in);
        else
            snprintf(wline, sizeof(wline), "%s  %d°F   Precip --", wx->icon, wx->temp_f);
        TTF_Font *wf = symbol_font ? symbol_font : f->h2;
        draw_text(r, wf, wline, right_x, hdr.y + pad + ts_h + right_line_gap, white, 2);
    } else {
        draw_text(r, f->h2, "Weather --", right_x, hdr.y + pad + ts_h + right_line_gap, dim, 2);
    }

    body_y = hdr.y + hdr.h + pad;
    body_h = H - body_y - pad;
    if(body_h < 100) body_h = 100;

    /* Logo (bottom left) and copyright (bottom center): above background, below tiles */
    {
        int bottom_pad = pad;
        int logo_max_h = clampi((int)(280*scale), 120, 440);  /* 2x larger */
        if(logo_tex){
            int tw = 0, th = 0;
            SDL_QueryTexture(logo_tex, NULL, NULL, &tw, &th);
            if(tw > 0 && th > 0){
                int dw = (int)((long)logo_max_h * (long)tw / (long)th);
                if(dw > W - 2*bottom_pad) dw = W - 2*bottom_pad;
                int dh = (int)((long)dw * (long)th / (long)tw);
                SDL_Rect logo_dst = { bottom_pad, H - bottom_pad - dh, dw, dh };
                SDL_RenderCopy(r, logo_tex, NULL, &logo_dst);
            }
        }
        static const char copy_str[] = "\xC2\xA9 2026 Damon";  /* © 2026 Damon */
        int cw = 0, ch = 0;
        text_size(f->tile_small, copy_str, &cw, &ch);
        draw_text(r, f->tile_small, copy_str, W / 2, H - bottom_pad - ch / 2, dim, 1);
    }

    if(n <= 0){
        draw_text(r, f->h1, "No upcoming buses", W/2, body_y + body_h/2, white, 1);
        SDL_RenderPresent(r);
        return;
    }

    /* Fixed 6 columns, 2 rows; tile size does not change with number of buses. */
    const int cols = TILE_COLS_FIXED;
    const int rows = TILE_ROWS_FIXED;
    int gap = clampi((int)(38*scale), 14, 70);
    int tile_w = (W - 2*pad - gap*(cols-1)) / cols;
    int tile_h = (body_h - gap*(rows-1)) / rows;

    int radius = clampi((int)(26*scale), 10, 42);

    /* Only draw tiles for buses we have; no empty placeholder tiles. */
    int show_n = n < TILE_SLOTS_MAX ? n : TILE_SLOTS_MAX;
    for(int i=0; i<show_n; i++){
        int c = i % cols;
        int rr = i / cols;
        SDL_Rect trc = {
            pad + c*(tile_w + gap),
            body_y + rr*(tile_h + gap),
            tile_w,
            tile_h
        };

        draw_tile_content(r, f, &arr[i], trc, scale, white, dim, radius);
    }

    SDL_RenderPresent(r);
}

int main(int argc, char **argv){
    (void)argc; (void)argv;

    const char *font_path = getenv("FONT_PATH");
    if(!font_path || !*font_path) font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

    const char *mta_key = getenv("MTA_KEY");
    const char *stop_id = getenv("STOP_ID");
    const char *route_filter = getenv("ROUTE_FILTER");
    const char *poll_s = getenv("POLL_SECONDS");
    int poll = poll_s ? atoi(poll_s) : 10;
    if(poll < 5) poll = 5;

    const char *max_s = getenv("MAX_TILES");
    int max_tiles = max_s ? atoi(max_s) : 12;
    max_tiles = clampi(max_tiles, 1, 24);

    const char *stop_name_override = getenv("STOP_NAME");
    char stop_name[256]; stop_name[0] = '\0';
    if(stop_name_override && *stop_name_override){
        snprintf(stop_name, sizeof(stop_name), "%s", stop_name_override);
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0){
        logf_("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if(TTF_Init() != 0){
        logf_("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return 1;
    }
#ifdef USE_SDL_IMAGE
    if((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0){
        logf_("IMG_Init PNG failed: %s", IMG_GetError());
    }
#endif

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    SDL_Window *win = SDL_CreateWindow("Arrival Board",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 720,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP);
    if(!win){
        logf_("CreateWindow failed: %s", SDL_GetError());
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if(!r){
        logf_("CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    int W=0,H=0;
    SDL_GetRendererOutputSize(r, &W, &H);
    if(W<=0 || H<=0) SDL_GetWindowSize(win, &W, &H);

    /* Load background image (under the tiles). Path: env BACKGROUND_IMAGE or "Steampunk bus image.png" in cwd. */
    SDL_Texture *bg_tex = NULL;
    SDL_Texture *steam_tex = NULL;
    SDL_Texture *logo_tex = NULL;
#ifdef USE_SDL_IMAGE
    const char *bg_path = getenv("BACKGROUND_IMAGE");
    if(!bg_path || !*bg_path) bg_path = "Steampunk bus image.png";
    SDL_Surface *bg_surf = IMG_Load(bg_path);
    if(!bg_surf && getenv("HOME")){
        char alt[512];
        snprintf(alt, sizeof(alt), "%s/arrival_board/Steampunk bus image.png", getenv("HOME"));
        bg_surf = IMG_Load(alt);
        if(bg_surf) bg_path = alt;
    }
    if(bg_surf){
        bg_tex = SDL_CreateTextureFromSurface(r, bg_surf);
        SDL_FreeSurface(bg_surf);
        if(!bg_tex) logf_("Could not create texture from background image");
        else {
            SDL_SetTextureBlendMode(bg_tex, SDL_BLENDMODE_BLEND);
            /* Opacity is set at draw time in render_ui (30%) */
            logf_("Background image loaded: %s", bg_path);
        }
    } else {
        logf_("Could not load background image '%s': %s", bg_path, IMG_GetError());
    }
#endif

    /* Load steam puff texture (one small soft white PNG, reused for all puffs). Create if missing. */
#ifdef USE_SDL_IMAGE
    {
        const char *steam_path = "steam_puff.png";
        char steam_alt[512];
        SDL_Surface *steam_surf = IMG_Load(steam_path);
        if(!steam_surf && getenv("HOME")){
            snprintf(steam_alt, sizeof(steam_alt), "%s/arrival_board/steam_puff.png", getenv("HOME"));
            steam_surf = IMG_Load(steam_alt);
            if(steam_surf) steam_path = steam_alt;
        }
        if(!steam_surf){
            /* Pre-render one small soft white puff and save as PNG */
            const int sz = STEAM_PUFF_SIZE;
            steam_surf = SDL_CreateRGBSurfaceWithFormat(0, sz, sz, 32, SDL_PIXELFORMAT_RGBA8888);
            if(steam_surf){
                const float cen = (float)(sz / 2);
                const float rad = cen - 4.f;
                SDL_LockSurface(steam_surf);
                Uint32 *pix = (Uint32 *)steam_surf->pixels;
                for(int y = 0; y < sz; y++){
                    for(int x = 0; x < sz; x++){
                        float dx = (float)x - cen, dy = (float)y - cen;
                        float d = sqrtf(dx*dx + dy*dy);
                        Uint8 alpha = 0;
                        if(d < rad){
                            float t = 1.f - (d / rad) * (d / rad);
                            if(t > 0.f){ t = (float)pow((double)t, 1.5); alpha = (Uint8)(220.f * t); }
                        }
                        pix[y * steam_surf->pitch / 4 + x] = (0xFFU << 24) | (255u << 16) | (255u << 8) | (unsigned)alpha;
                    }
                }
                SDL_UnlockSurface(steam_surf);
                if(IMG_SavePNG(steam_surf, steam_path) == 0)
                    logf_("Created %s", steam_path);
                SDL_FreeSurface(steam_surf);
                steam_surf = IMG_Load(steam_path);
            }
        }
        if(steam_surf){
            steam_tex = SDL_CreateTextureFromSurface(r, steam_surf);
            SDL_FreeSurface(steam_surf);
            if(steam_tex){
                SDL_SetTextureBlendMode(steam_tex, SDL_BLENDMODE_BLEND);
                logf_("Steam puff texture loaded");
            }
        }
    }
    /* Logo at bottom left: Damon Logo Large.png */
    {
        const char *logo_path = "Damon Logo Large.png";
        SDL_Surface *logo_surf = IMG_Load(logo_path);
        if(!logo_surf && getenv("HOME")){
            char alt[512];
            snprintf(alt, sizeof(alt), "%s/arrival_board/Damon Logo Large.png", getenv("HOME"));
            logo_surf = IMG_Load(alt);
        }
        if(logo_surf){
            logo_tex = SDL_CreateTextureFromSurface(r, logo_surf);
            SDL_FreeSurface(logo_surf);
            if(logo_tex){
                SDL_SetTextureBlendMode(logo_tex, SDL_BLENDMODE_BLEND);
                logf_("Logo loaded: Damon Logo Large.png");
            }
        } else {
            logf_("Logo not found: %s", IMG_GetError());
        }
    }
#endif

    Fonts fonts;
    if(tile_load_fonts(&fonts, font_path, H) != 0){
        logf_("Failed to load font at %s", font_path);
        SDL_DestroyRenderer(r);
        SDL_DestroyWindow(win);
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    /* Symbol font for weather icon (Unicode ☀☁⛅ etc.); try several paths on Pi */
    const char *symbol_font_path = getenv("SYMBOL_FONT_PATH");
    if(!symbol_font_path || !*symbol_font_path) symbol_font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    float sym_scale = (H > 0) ? ((float)H / 2160.0f) : 1.0f;
    int sym_pt = clampi((int)(58.f * sym_scale), 26, 120);
    TTF_Font *symbol_font = TTF_OpenFont(symbol_font_path, sym_pt);
    const char *loaded_symbol_path = symbol_font ? symbol_font_path : NULL;
    if(!symbol_font){
        const char *fallbacks[] = {
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            NULL
        };
        for(int i = 0; fallbacks[i] && !symbol_font; i++){
            symbol_font = TTF_OpenFont(fallbacks[i], sym_pt);
            if(symbol_font) loaded_symbol_path = fallbacks[i];
        }
    }
    if(symbol_font){
        logf_("Symbol font loaded: %s (pt %d) for weather icon", loaded_symbol_path, sym_pt);
    } else {
        logf_("Symbol font failed (weather icon may show tofu): %s - %s", symbol_font_path, TTF_GetError());
    }

    Weather wx;
    memset(&wx, 0, sizeof(wx));
    wx.precip_prob = -1;
    wx.precip_in = -1.0;

    Arrival arrivals[32];
    int n = 0;

    time_t last_fetch = 0;

    for(;;){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type == SDL_QUIT) goto done;
            if(e.type == SDL_KEYDOWN){
                if(e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q) goto done;
            }
        }

        time_t now = time(NULL);
        if(!last_fetch || difftime(now, last_fetch) >= poll){
            char sn[256]; sn[0]='\0';
            n = fetch_mta_arrivals(arrivals, max_tiles, sn, sizeof(sn), mta_key, stop_id, route_filter);

            if((!stop_name[0]) && sn[0]){
                snprintf(stop_name, sizeof(stop_name), "%s", sn);
            }
            fetch_weather(&wx, stop_name[0] ? stop_name : NULL);

            last_fetch = now;
        }

        SDL_GetRendererOutputSize(r, &W, &H);
        render_ui(r, &fonts, W, H, stop_id ? stop_id : "--", stop_name, &wx, arrivals, n, bg_tex, steam_tex, logo_tex, symbol_font);

        SDL_Delay(80);
    }

done:
    if(bg_tex) SDL_DestroyTexture(bg_tex);
    if(steam_tex) SDL_DestroyTexture(steam_tex);
    if(logo_tex) SDL_DestroyTexture(logo_tex);
    if(symbol_font) TTF_CloseFont(symbol_font);
    tile_free_fonts(&fonts);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
#ifdef USE_SDL_IMAGE
    IMG_Quit();
#endif
    TTF_Quit();
    SDL_Quit();
    return 0;
}
