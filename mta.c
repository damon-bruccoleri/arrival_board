/*
 * MTA Bus Time API implementation: SIRI stop-monitoring parsing and arrival list.
 */
#include "mta.h"
#include "util.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Normalize route string: use last segment after '_', ':', or '/'. */
static void normalize_route(char *dst, size_t dstsz, const char *src) {
    if (!src) { snprintf(dst, dstsz, "?"); return; }
    const char *p = src;
    const char *last = src;
    for (; *p; p++) {
        if (*p == '_' || *p == ':' || *p == '/') last = p + 1;
    }
    snprintf(dst, dstsz, "%s", last);
}

/* Parse ISO8601 with optional fractional seconds and timezone offset. */
static time_t parse_iso8601(const char *s) {
    if (!s) return (time_t)0;

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", s);

    char *dot = strchr(tmp, '.');
    if (dot) {
        char *tz = strchr(dot, 'Z');
        if (!tz) tz = strchr(dot, '+');
        if (!tz) {
            char *tpos = strchr(tmp, 'T');
            for (char *p = dot; p && *p; p++) {
                if (*p == '-' && tpos && p > tpos) { tz = p; break; }
            }
        }
        if (tz) memmove(dot, tz, strlen(tz) + 1);
        else *dot = '\0';
    }

    int tzsign = 0, tzh = 0, tzm = 0;
    char *z = strchr(tmp, 'Z');
    char *plus = strchr(tmp, '+');
    char *minus = NULL;

    char *tpos = strchr(tmp, 'T');
    if (tpos) {
        for (char *p = tpos; *p; p++) {
            if (*p == '-') minus = p;
        }
    }

    char *tzp = NULL;
    if (plus) tzp = plus;
    else if (minus) tzp = minus;

    if (z) {
        *z = '\0';
        tzsign = 0;
    } else if (tzp) {
        tzsign = (*tzp == '-') ? -1 : 1;
        char off[8];
        snprintf(off, sizeof(off), "%s", tzp);
        *tzp = '\0';
        if (strlen(off) >= 6) {
            tzh = atoi(off + 1);
            tzm = atoi(off + 4);
        }
    }

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    char *ok = strptime(tmp, "%Y-%m-%dT%H:%M:%S", &tmv);
    if (!ok) return (time_t)0;

    time_t t = timegm(&tmv);
    if (z) return t;
    if (tzp) {
        int offset = tzsign * (tzh * 3600 + tzm * 60);
        return t - offset;
    }
    return t;
}

/* Heuristic people-on-bus estimate (BusTime does not provide real occupancy). */
static int estimate_people(int mins, int stops_away) {
    static int inited = 0;
    static double base = 1.0;
    static double per_min = 0.22;
    static double per_stop = 0.60;
    static int cap = 45;

    if (!inited) {
        const char *s;
        if ((s = getenv("PPL_BASE")) && *s) base = atof(s);
        if ((s = getenv("PPL_PER_MIN")) && *s) per_min = atof(s);
        if ((s = getenv("PPL_PER_STOP")) && *s) per_stop = atof(s);
        if ((s = getenv("PPL_CAP")) && *s) cap = atoi(s);
        inited = 1;
    }

    if (cap < 5) cap = 5;
    if (cap > 200) cap = 200;

    int m = (mins >= 0) ? mins : 0;
    int st = (stops_away >= 0) ? stops_away : 0;

    double ppl = base + per_min * (double)m + per_stop * (double)st;
    if (mins >= 0 && mins <= 2) ppl *= 0.70;

    int ip = (int)lrint(ppl);
    return clampi(ip, 0, cap);
}

/* Return 1 if route is in the comma-separated filter list, or if filter is empty. */
static int route_allowed(const char *route, const char *filter_csv) {
    if (!filter_csv || !*filter_csv) return 1;
    if (!route || !*route) return 0;

    char copy[256];
    snprintf(copy, sizeof(copy), "%s", filter_csv);

    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        size_t n = strlen(tok);
        while (n > 0 && tok[n - 1] == ' ') tok[--n] = '\0';
        if (n == 0) continue;
        if (strcasecmp(tok, route) == 0) return 1;
    }
    return 0;
}

/* Parse stops-away from SIRI journey / MonitoredCall / Extensions / Distances. */
static int parse_stops_away(const cJSON *journey) {
    const cJSON *mc = jgeto(journey, "MonitoredCall");
    const cJSON *ext = mc ? jgeto(mc, "Extensions") : NULL;
    const cJSON *dist = ext ? jgeto(ext, "Distances") : NULL;

    int v = jint(jgeto(dist, "StopsFromCall"), -1);
    if (v >= 0) return v;
    v = jint(jgeto(dist, "StopsAway"), -1);
    if (v >= 0) return v;

    const cJSON *ext2 = jgeto(journey, "Extensions");
    const cJSON *dist2 = ext2 ? jgeto(ext2, "Distances") : NULL;
    v = jint(jgeto(dist2, "StopsFromCall"), -1);
    if (v >= 0) return v;

    return -1;
}

/* Parse distance to stop (meters) from SIRI and convert to miles. Returns <0 if unknown. */
static double parse_miles_away(const cJSON *journey) {
    const cJSON *mc = jgeto(journey, "MonitoredCall");
    const cJSON *ext = mc ? jgeto(mc, "Extensions") : NULL;
    const cJSON *dist = ext ? jgeto(ext, "Distances") : NULL;

    double meters = jdouble(jgeto(dist, "DistanceFromCall"), -1.0);
    if (meters < 0.0) meters = jdouble(jgeto(dist, "DistanceFromStop"), -1.0);

    if (meters < 0.0) {
        const cJSON *ext2 = jgeto(journey, "Extensions");
        const cJSON *dist2 = ext2 ? jgeto(ext2, "Distances") : NULL;
        meters = jdouble(jgeto(dist2, "DistanceFromCall"), -1.0);
        if (meters < 0.0) meters = jdouble(jgeto(dist2, "DistanceFromStop"), -1.0);
    }

    if (meters < 0.0) return -1.0;
    return meters / 1609.344;
}

/* Extract stop display name from the first delivery. */
static void parse_stop_name(char *out, size_t outsz, const cJSON *delivery) {
    if (!out || outsz == 0) return;
    out[0] = '\0';

    const cJSON *visits = jgeto(delivery, "MonitoredStopVisit");
    const cJSON *v0 = jgeti(visits, 0);
    if (!v0) return;

    const cJSON *mvj = jgeto(v0, "MonitoredVehicleJourney");
    const cJSON *mc = mvj ? jgeto(mvj, "MonitoredCall") : NULL;

    const cJSON *spn = mc ? jgeto(mc, "StopPointName") : NULL;
    if (jgets(spn)) {
        snprintf(out, outsz, "%s", jgets(spn));
        return;
    }
    const cJSON *spn0 = jgeti(spn, 0);
    if (jgets(spn0)) snprintf(out, outsz, "%s", jgets(spn0));
}

int fetch_mta_arrivals(Arrival *arr, int max_arr,
                       char *stop_name, size_t stop_name_sz,
                       const char *mta_key, const char *stop_id,
                       const char *route_filter) {
    if (!arr || max_arr <= 0) return 0;
    if (stop_name && stop_name_sz) stop_name[0] = '\0';

    if (!mta_key || !*mta_key || !stop_id || !*stop_id) return 0;

    char url[1024];
    snprintf(url, sizeof(url),
             "https://bustime.mta.info/api/siri/stop-monitoring.json?key=%s&MonitoringRef=%s&OperatorRef=MTA&MaximumStopVisits=%d",
             mta_key, stop_id, max_arr);

    char *json = http_get(url);
    if (!json) return 0;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;

    int count = 0;

    const cJSON *siri = jgeto(root, "Siri");
    const cJSON *sd = siri ? jgeto(siri, "ServiceDelivery") : NULL;
    const cJSON *smd = sd ? jgeto(sd, "StopMonitoringDelivery") : NULL;
    const cJSON *del = jgeti(smd, 0);

    if (del && stop_name && stop_name_sz)
        parse_stop_name(stop_name, stop_name_sz, del);

    const cJSON *visits = del ? jgeto(del, "MonitoredStopVisit") : NULL;
    int n = visits && cJSON_IsArray(visits) ? cJSON_GetArraySize((cJSON *)visits) : 0;

    time_t now = time(NULL);

    for (int i = 0; i < n && count < max_arr; i++) {
        const cJSON *v = jgeti(visits, i);
        const cJSON *mvj = v ? jgeto(v, "MonitoredVehicleJourney") : NULL;
        if (!mvj) continue;

        char route[32] = {0};
        normalize_route(route, sizeof(route), jgets(jgeto(mvj, "LineRef")));
        if (!route_allowed(route, route_filter)) continue;

        const char *veh = jgets(jgeto(mvj, "VehicleRef"));
        const cJSON *mc = jgeto(mvj, "MonitoredCall");

        char destbuf[128] = {0};
        const cJSON *destv = jgeto(mvj, "DestinationName");
        const char *dstr = jgets(destv);
        if (!dstr) {
            const cJSON *d0 = jgeti(destv, 0);
            dstr = jgets(d0);
        }
        if (dstr && *dstr)
            snprintf(destbuf, sizeof(destbuf), "%s", dstr);
        else
            snprintf(destbuf, sizeof(destbuf), "--");

        const char *tiso = NULL;
        if (mc) {
            tiso = jgets(jgeto(mc, "ExpectedArrivalTime"));
            if (!tiso) tiso = jgets(jgeto(mc, "AimedArrivalTime"));
        }
        time_t exp = parse_iso8601(tiso);
        int mins = -1;
        if (exp > 0) {
            double d = difftime(exp, now);
            mins = (int)lrint(d / 60.0);
            if (mins < 0) mins = 0;
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
