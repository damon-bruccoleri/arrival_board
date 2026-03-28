/*
 * Weather: Open-Meteo forecast API and moon phase.
 */
#include "util.h"
#include "weather.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    WEATHER_STATUS_OK = 0,
    WEATHER_STATUS_THROTTLED = 1,
    WEATHER_STATUS_HTTP_FAIL = 2,
    WEATHER_STATUS_JSON_FAIL = 3,
    WEATHER_STATUS_SCHEMA_FAIL = 4,
};

static int g_weather_last_status = WEATHER_STATUS_OK;

/* Map Open-Meteo weather code to a single Unicode symbol (UTF-8).
 * `is_day`: 1=day, 0=night, -1 unknown. */
static void icon_for_code(int code, int is_day, char *out, size_t outsz) {
    const char *sym;
    switch (code) {
        case 0:
            if (is_day == 0) sym = "\xF0\x9F\x8C\x9C"; /* 🌜 (U+1F31C) */
            else             sym = "\xE2\x98\x80";      /* ☀ (U+2600) */
            break;
        case 1:
        case 2:
            /* Prefer day-specific sun+cloud glyphs when possible. */
            if (is_day == 1) sym = "\xF0\x9F\x8C\xA4"; /* 🌤 (U+1F324) */
            else             sym = "\xE2\x9B\x85";      /* ⛅ (U+26C5) */
            break;
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
            /* Log any weather codes that fall into the default bucket so we can see
             * which Open-Meteo codes might not have a good glyph mapping. This goes
             * to stderr and ends up in boot.log via run_arrival_board.sh. */
            logf_("Weather: unmapped weather_code=%d, using icon '%s'", code, sym);
            break;
    }
    size_t len = strlen(sym);
    if (len + 1 <= outsz) memcpy(out, sym, len + 1);
    else out[0] = '\0';
}

/* Moon phase 0..1 from current UTC date (lunation ~29.53 days). */
static float moon_phase_from_date(void) {
    time_t t = time(NULL);
    struct tm tm_now;
    if (!gmtime_r(&t, &tm_now)) return -1.f;
    double day = tm_now.tm_mday + (tm_now.tm_hour + tm_now.tm_min / 60.0) / 24.0;
    int month = tm_now.tm_mon + 1, year = tm_now.tm_year + 1900;
    if (month <= 2) { year--; month += 12; }
    int a = year / 100, b = 2 - a + (a / 4);
    double jd = (int)(365.25 * (year + 4716)) + (int)(30.6001 * (month + 1)) + day + b - 1524.5;
    double lunation = fmod(jd - 2451550.1, 29.530588853) / 29.530588853;
    return (float)(lunation < 0 ? lunation + 1.0 : lunation);
}

void fetch_weather(Weather *w, const char *stop_name) {
    (void)stop_name;
    if (!w) return;

    time_t now = time(NULL);
    if (w->last_fetch && difftime(now, w->last_fetch) < 600) {
        g_weather_last_status = WEATHER_STATUS_THROTTLED;
        return;
    }

    /* Location: STOP_LAT/STOP_LON only, else NYC default. */
    if (fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001) {
        const char *slat = getenv("STOP_LAT");
        const char *slon = getenv("STOP_LON");
        if (slat && slon) {
            w->lat = atof(slat);
            w->lon = atof(slon);
        }
    }
    if (fabs(w->lat) < 0.001 || fabs(w->lon) < 0.001) {
        w->lat = 40.7128;
        w->lon = -74.0060;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&timezone=America%%2FNew_York&temperature_unit=fahrenheit&precipitation_unit=inch&current=temperature_2m,precipitation,weather_code,is_day&hourly=precipitation_probability",
             w->lat, w->lon);

    char *json = http_get(url);
    if (!json) {
        logf_("Weather: fetch failed (no response from API)");
        g_weather_last_status = WEATHER_STATUS_HTTP_FAIL;
        return;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        logf_("Weather: invalid JSON from API");
        g_weather_last_status = WEATHER_STATUS_JSON_FAIL;
        return;
    }

    const cJSON *cur = jgeto(root, "current");
    if (!cur) {
        logf_("Weather: API response missing 'current'");
        cJSON_Delete(root);
        g_weather_last_status = WEATHER_STATUS_SCHEMA_FAIL;
        return;
    }
    int temp = jint(jgeto(cur, "temperature_2m"), -999);
    double precip = jdouble(jgeto(cur, "precipitation"), -1.0);
    int code = jint(jgeto(cur, "weather_code"), -1);
    int is_day = jint(jgeto(cur, "is_day"), -1);

    char icon[8];
    icon_for_code(code, is_day, icon, sizeof(icon));

    int prob = -1;
    const cJSON *hourly = jgeto(root, "hourly");
    const cJSON *pp = hourly ? jgeto(hourly, "precipitation_probability") : NULL;
    /* Use the maximum precip probability over the next several hours instead of only [0]. */
    if (pp && cJSON_IsArray((cJSON *)pp)) {
        int count = cJSON_GetArraySize((cJSON *)pp);
        int horizon = count < 6 ? count : 6;
        for (int i = 0; i < horizon; i++) {
            const cJSON *ppi = jgeti(pp, i);
            int v = jint(ppi, -1);
            if (v > prob) prob = v;
        }
    }

    w->moon_phase = moon_phase_from_date();

    cJSON_Delete(root);

    w->have = 1;
    snprintf(w->icon, sizeof(w->icon), "%s", icon);
    w->is_day = is_day;
    w->temp_f = temp;
    w->precip_prob = prob;
    w->precip_in = precip;
    w->last_fetch = now;

    /* Log each successful weather fetch so we can see when codes/icons
     * are updated and what values Open-Meteo returned. */
    logf_("Weather: fetched code=%d icon='%s' temp=%dF precip_prob=%d precip_in=%.2f moon_phase=%.2f",
          code, w->icon, w->temp_f, w->precip_prob, w->precip_in, w->moon_phase);
    g_weather_last_status = WEATHER_STATUS_OK;
}

int weather_last_status(void) {
    return g_weather_last_status;
}

const char *weather_last_status_str(void) {
    switch (g_weather_last_status) {
        case WEATHER_STATUS_OK: return "OK";
        case WEATHER_STATUS_THROTTLED: return "THROTTLED";
        case WEATHER_STATUS_HTTP_FAIL: return "HTTP_FAIL";
        case WEATHER_STATUS_JSON_FAIL: return "JSON_FAIL";
        case WEATHER_STATUS_SCHEMA_FAIL: return "SCHEMA_FAIL";
        default: return "UNKNOWN";
    }
}
