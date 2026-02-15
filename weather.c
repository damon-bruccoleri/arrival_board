/*
 * Weather implementation: Open-Meteo geocoding and forecast API.
 */
#include "util.h"
#include "weather.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Map Open-Meteo weather code to a single Unicode symbol (UTF-8). */
static void icon_for_code(int code, char *out, size_t outsz) {
    const char *sym;
    switch (code) {
        case 0:   sym = "\xE2\x98\x80"; break;  /* ☀ */
        case 1:
        case 2:   sym = "\xE2\x9B\x85"; break;  /* ⛅ */
        case 3:
        case 45:
        case 48:  sym = "\xE2\x98\x81"; break;  /* ☁ */
        default:
            if (code >= 51 && code <= 57) sym = "\xE2\x98\x94";   /* ☔ */
            else if (code >= 61 && code <= 67) sym = "\xE2\x98\x94";
            else if (code >= 71 && code <= 77) sym = "\xE2\x9D\x84"; /* ❄ */
            else if (code >= 80 && code <= 82) sym = "\xE2\x98\x94";
            else if (code >= 95 || code == 96 || code == 99) sym = "\xE2\x9A\xA1"; /* ⚡ */
            else sym = "\xE2\x98\x81";
            break;
    }
    size_t len = strlen(sym);
    if (len + 1 <= outsz) memcpy(out, sym, len + 1);
    else out[0] = '\0';
}

/* Geocode "stop_name + suffix" to lat/lon via Open-Meteo. Suffix from WEATHER_GEOCODE_SUFFIX or ", New York City". */
static int geocode_stop_to_latlon(const char *stop_name, double *olat, double *olon) {
    if (!stop_name || !*stop_name) return 0;
    const char *suffix = getenv("WEATHER_GEOCODE_SUFFIX");
    if (!suffix || !*suffix) suffix = ", New York City";

    char q[512], enc[1024];
    snprintf(q, sizeof(q), "%s%s", stop_name, suffix);
    urlencode(enc, sizeof(enc), q);

    char url[1400];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
             enc);

    char *json = http_get(url);
    if (!json) return 0;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;

    const cJSON *results = jgeto(root, "results");
    const cJSON *r0 = jgeti(results, 0);
    if (!r0) { cJSON_Delete(root); return 0; }

    double lat = jdouble(jgeto(r0, "latitude"), 0.0);
    double lon = jdouble(jgeto(r0, "longitude"), 0.0);
    cJSON_Delete(root);

    if (fabs(lat) < 0.001 || fabs(lon) < 0.001) return 0;
    *olat = lat;
    *olon = lon;
    return 1;
}

void fetch_weather(Weather *w, const char *stop_name) {
    if (!w) return;

    time_t now = time(NULL);
    if (w->last_fetch && difftime(now, w->last_fetch) < 600) return;

    /* Priority: STOP_LAT/STOP_LON (this stop) -> geocode stop name -> WEATHER_LAT/LON -> NYC default */
    if (fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001) {
        const char *slat = getenv("STOP_LAT");
        const char *slon = getenv("STOP_LON");
        if (slat && slon) {
            w->lat = atof(slat);
            w->lon = atof(slon);
        }
    }
    if (fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001) {
        double lat = 0, lon = 0;
        if (geocode_stop_to_latlon(stop_name, &lat, &lon)) {
            w->lat = lat;
            w->lon = lon;
        }
    }
    if (fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001) {
        const char *elat = getenv("WEATHER_LAT");
        const char *elon = getenv("WEATHER_LON");
        if (elat && elon) {
            w->lat = atof(elat);
            w->lon = atof(elon);
        }
    }
    if (fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001) {
        w->lat = 40.7128;
        w->lon = -74.0060;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&timezone=America%%2FNew_York&temperature_unit=fahrenheit&precipitation_unit=inch&current=temperature_2m,precipitation,weather_code&hourly=precipitation_probability",
             w->lat, w->lon);

    char *json = http_get(url);
    if (!json) { w->have = 0; return; }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) { w->have = 0; return; }

    const cJSON *cur = jgeto(root, "current");
    int temp = jint(jgeto(cur, "temperature_2m"), -999);
    double precip = jdouble(jgeto(cur, "precipitation"), -1.0);
    int code = jint(jgeto(cur, "weather_code"), -1);

    char icon[8];
    icon_for_code(code, icon, sizeof(icon));

    int prob = -1;
    const cJSON *hourly = jgeto(root, "hourly");
    const cJSON *pp = hourly ? jgeto(hourly, "precipitation_probability") : NULL;
    const cJSON *pp0 = jgeti(pp, 0);
    if (pp0) prob = jint(pp0, -1);

    cJSON_Delete(root);

    w->have = 1;
    snprintf(w->icon, sizeof(w->icon), "%s", icon);
    w->temp_f = temp;
    w->precip_prob = prob;
    w->precip_in = precip;
    w->last_fetch = now;
}
