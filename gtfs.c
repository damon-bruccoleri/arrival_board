/*
 * GTFS static: load MTA bus GTFS zip, parse, find next scheduled departures.
 * Timezone America/New_York. Cache zip; use previous if download fails.
 */
#include "gtfs.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ROUTES   512
#define MAX_TRIPS    120000
#define MAX_STOPTIMES_AT_STOP 8000
#define MAX_STOPS    16000
#define MAX_CALENDAR 128
#define MAX_CAL_DATES 4096
#define MAX_SERVICE_IDS 128

/* Parsed GTFS rows (minimal fields we need) */
typedef struct {
    char route_id[64];
    char short_name[32];
} GtfsRoute;

typedef struct {
    char trip_id[64];
    char route_id[64];
    char service_id[64];
    char headsign[128];
} GtfsTrip;

typedef struct {
    char trip_id[64];
    int arrival_mins;   /* minutes from midnight; can be 24*60+ for next day */
} GtfsStopTime;

typedef struct {
    char stop_id[32];
    char stop_code[32];
} GtfsStop;

typedef struct {
    char service_id[64];
    int start_ymd;   /* YYYYMMDD */
    int end_ymd;
    int dow[7];      /* 0 or 1 */
} GtfsCalendar;

typedef struct {
    char service_id[64];
    int date_ymd;
    int exception_type; /* 1=add, 2=remove */
} GtfsCalendarDate;

static GtfsRoute *routes;
static int n_routes;
static GtfsTrip *trips;
static int n_trips;
static GtfsStopTime *stop_times;
static int n_stop_times;
static GtfsStop *stops;
static int n_stops;
static GtfsCalendar *calendars;
static int n_calendars;
static GtfsCalendarDate *cal_dates;
static int n_cal_dates;

static char gtfs_stop_id_resolved[32];
#define MAX_STOP_FILTER_IDS 32
static char stop_filter_ids[MAX_STOP_FILTER_IDS][32];
static int n_stop_filter_ids;
static char gtfs_cache_path[512];
static int gtfs_loaded;

/* Parse CSV line into fields (handles quoted). Max 32 fields. */
static int parse_csv_line(char *line, char *fields[], int max_fields) {
    int n = 0;
    char *p = line;
    while (n < max_fields && *p) {
        fields[n++] = p;
        if (*p == '"') {
            p++;
            fields[n - 1] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
            while (*p && *p != ',') p++;
            if (*p) *p++ = '\0';
        } else {
            while (*p && *p != ',') p++;
            if (*p) *p++ = '\0';
        }
    }
    return n;
}

/* Read file from zip via unzip -p. Call fn for each line (skip header). */
static int read_zip_file(const char *zip_path, const char *file_name,
                         int (*fn)(char *line, void *ctx), void *ctx) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "unzip -p '%s' '%s' 2>/dev/null", zip_path, file_name);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[2048];
    if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return 0; } /* skip header */
    int count = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (fn(buf, ctx) != 0) break;
        count++;
    }
    pclose(fp);
    return count;
}

/* Parse arrival_time HH:MM:SS or H:MM:SS to minutes from midnight. 24:00:00 = 1440, 25:30:00 = 1530. */
static int parse_time_mins(const char *s) {
    if (!s || !*s) return -1;
    int h, m, sec;
    if (sscanf(s, "%d:%d:%d", &h, &m, &sec) != 3) return -1;
    return h * 60 + m;
}

static int routes_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 2) return 0;
    if (n_routes >= MAX_ROUTES) return -1;
    strncpy(routes[n_routes].route_id, f[0], sizeof(routes[n_routes].route_id) - 1);
    routes[n_routes].route_id[sizeof(routes[n_routes].route_id) - 1] = '\0';
    strncpy(routes[n_routes].short_name, n >= 3 ? f[2] : f[0], sizeof(routes[n_routes].short_name) - 1);
    routes[n_routes].short_name[sizeof(routes[n_routes].short_name) - 1] = '\0';
    n_routes++;
    return 0;
}

static int trips_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    /* GTFS: route_id, service_id, trip_id, trip_headsign */
    if (n < 3) return 0;
    if (n_trips >= MAX_TRIPS) return -1;
    strncpy(trips[n_trips].trip_id, f[2], sizeof(trips[n_trips].trip_id) - 1);
    trips[n_trips].trip_id[sizeof(trips[n_trips].trip_id) - 1] = '\0';
    strncpy(trips[n_trips].route_id, f[0], sizeof(trips[n_trips].route_id) - 1);
    trips[n_trips].route_id[sizeof(trips[n_trips].route_id) - 1] = '\0';
    strncpy(trips[n_trips].service_id, f[1], sizeof(trips[n_trips].service_id) - 1);
    trips[n_trips].service_id[sizeof(trips[n_trips].service_id) - 1] = '\0';
    strncpy(trips[n_trips].headsign, n >= 4 ? f[3] : "", sizeof(trips[n_trips].headsign) - 1);
    trips[n_trips].headsign[sizeof(trips[n_trips].headsign) - 1] = '\0';
    n_trips++;
    return 0;
}

static int stop_times_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    /* stop_times: trip_id, arrival_time, departure_time, stop_id, stop_sequence, ... */
    if (n < 4) return 0;
    int match = 0;
    for (int i = 0; i < n_stop_filter_ids; i++)
        if (strcmp(f[3], stop_filter_ids[i]) == 0) { match = 1; break; }
    if (!match) return 0;
    if (n_stop_times >= MAX_STOPTIMES_AT_STOP) return -1;
    strncpy(stop_times[n_stop_times].trip_id, f[0], sizeof(stop_times[n_stop_times].trip_id) - 1);
    stop_times[n_stop_times].trip_id[sizeof(stop_times[n_stop_times].trip_id) - 1] = '\0';
    stop_times[n_stop_times].arrival_mins = parse_time_mins(f[1]);
    if (stop_times[n_stop_times].arrival_mins < 0) return 0;
    n_stop_times++;
    return 0;
}

static int stops_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 1) return 0;
    if (n_stops >= MAX_STOPS) return -1;
    strncpy(stops[n_stops].stop_id, f[0], sizeof(stops[n_stops].stop_id) - 1);
    stops[n_stops].stop_id[sizeof(stops[n_stops].stop_id) - 1] = '\0';
    strncpy(stops[n_stops].stop_code, n >= 2 ? f[1] : "", sizeof(stops[n_stops].stop_code) - 1);
    stops[n_stops].stop_code[sizeof(stops[n_stops].stop_code) - 1] = '\0';
    n_stops++;
    return 0;
}

static int calendar_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 10) return 0;
    if (n_calendars >= MAX_CALENDAR) return -1;
    strncpy(calendars[n_calendars].service_id, f[0], sizeof(calendars[n_calendars].service_id) - 1);
    calendars[n_calendars].service_id[sizeof(calendars[n_calendars].service_id) - 1] = '\0';
    if (strlen(f[8]) >= 8) calendars[n_calendars].start_ymd = atoi(f[8]);
    if (strlen(f[9]) >= 8) calendars[n_calendars].end_ymd = atoi(f[9]);
    /* dow: 0=Sun..6=Sat; GTFS cols 1=monday..7=sunday */
    if (n >= 8) {
        calendars[n_calendars].dow[0] = (f[7][0] == '1') ? 1 : 0; /* sunday */
        for (int i = 1; i < 7; i++)
            calendars[n_calendars].dow[i] = (f[i][0] == '1') ? 1 : 0;
    }
    n_calendars++;
    return 0;
}

static int calendar_dates_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 3) return 0;
    if (n_cal_dates >= MAX_CAL_DATES) return -1;
    strncpy(cal_dates[n_cal_dates].service_id, f[0], sizeof(cal_dates[n_cal_dates].service_id) - 1);
    cal_dates[n_cal_dates].service_id[sizeof(cal_dates[n_cal_dates].service_id) - 1] = '\0';
    if (strlen(f[1]) >= 8) cal_dates[n_cal_dates].date_ymd = atoi(f[1]);
    cal_dates[n_cal_dates].exception_type = atoi(f[2]);
    n_cal_dates++;
    return 0;
}

/* Is service_id active on date_ymd (YYYYMMDD)? */
static int service_active_on(int service_ymd, const char *service_id) {
    int y = service_ymd / 10000, m = (service_ymd / 100) % 100, d = service_ymd % 100;
    int dow = -1; /* 0=Sun .. 6=Sat */
    struct tm tm = { 0 };
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_isdst = -1;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) return 0;
    struct tm *lt = gmtime(&t);
    if (lt) dow = lt->tm_wday;

    for (int i = 0; i < n_cal_dates; i++) {
        if (strcmp(cal_dates[i].service_id, service_id) != 0) continue;
        if (cal_dates[i].date_ymd != service_ymd) continue;
        if (cal_dates[i].exception_type == 1) return 1;
        if (cal_dates[i].exception_type == 2) return 0;
    }
    for (int i = 0; i < n_calendars; i++) {
        if (strcmp(calendars[i].service_id, service_id) != 0) continue;
        if (service_ymd < calendars[i].start_ymd || service_ymd > calendars[i].end_ymd) continue;
        if (dow >= 0 && dow < 7 && calendars[i].dow[dow]) return 1;
    }
    return 0;
}

/* Resolve stop_id: try as stop_id, then as stop_code. Build list of all stop_ids at same location (same stop_code) so we get all routes. Return 1 if found. */
static int resolve_stop(const char *stop_id) {
    const char *code_to_match = NULL;
    int found_idx = -1;
    for (int i = 0; i < n_stops; i++) {
        if (strcmp(stops[i].stop_id, stop_id) == 0) {
            strncpy(gtfs_stop_id_resolved, stops[i].stop_id, sizeof(gtfs_stop_id_resolved) - 1);
            gtfs_stop_id_resolved[sizeof(gtfs_stop_id_resolved) - 1] = '\0';
            code_to_match = stops[i].stop_code[0] ? stops[i].stop_code : stops[i].stop_id;
            found_idx = i;
            break;
        }
    }
    if (found_idx < 0) {
        for (int i = 0; i < n_stops; i++) {
            if (stops[i].stop_code[0] && strcmp(stops[i].stop_code, stop_id) == 0) {
                strncpy(gtfs_stop_id_resolved, stops[i].stop_id, sizeof(gtfs_stop_id_resolved) - 1);
                gtfs_stop_id_resolved[sizeof(gtfs_stop_id_resolved) - 1] = '\0';
                code_to_match = stops[i].stop_code;
                found_idx = i;
                break;
            }
        }
    }
    if (found_idx < 0) {
        logf_("GTFS: stop_id '%s' not found in feed (check GTFS_BUS_URL and that stop is in this feed)", stop_id);
        return 0;
    }
    n_stop_filter_ids = 0;
    for (int i = 0; i < n_stops && n_stop_filter_ids < MAX_STOP_FILTER_IDS; i++) {
        int include = 0;
        if (code_to_match && stops[i].stop_code[0] && strcmp(stops[i].stop_code, code_to_match) == 0)
            include = 1;
        else if (code_to_match && !stops[i].stop_code[0] && strcmp(stops[i].stop_id, code_to_match) == 0)
            include = 1;
        /* Also include any stop whose stop_code is the user's stop_id (e.g. sign 501627) so we get all routes at that sign */
        if (!include && stops[i].stop_code[0] && strcmp(stops[i].stop_code, stop_id) == 0)
            include = 1;
        if (include) {
            int already = 0;
            for (int j = 0; j < n_stop_filter_ids; j++)
                if (strcmp(stop_filter_ids[j], stops[i].stop_id) == 0) { already = 1; break; }
            if (!already) {
                strncpy(stop_filter_ids[n_stop_filter_ids], stops[i].stop_id, 31);
                stop_filter_ids[n_stop_filter_ids][31] = '\0';
                n_stop_filter_ids++;
            }
        }
    }
    if (n_stop_filter_ids == 0) {
        strncpy(stop_filter_ids[0], gtfs_stop_id_resolved, 31);
        stop_filter_ids[0][31] = '\0';
        n_stop_filter_ids = 1;
    }
    return 1;
}

/* Get route short_name from route_id */
static const char *route_short_name(const char *route_id) {
    for (int i = 0; i < n_routes; i++)
        if (strcmp(routes[i].route_id, route_id) == 0)
            return routes[i].short_name;
    return route_id;
}

/* Trip info by trip_id */
static GtfsTrip *trip_by_id(const char *trip_id) {
    for (int i = 0; i < n_trips; i++)
        if (strcmp(trips[i].trip_id, trip_id) == 0)
            return &trips[i];
    return NULL;
}

/* Is route in the comma-separated list (or NULL = not excluded)? */
static int route_excluded(const char *short_name, const char *realtime_routes) {
    if (!realtime_routes || !*realtime_routes) return 0;
    char list[1024];
    snprintf(list, sizeof(list), "%s", realtime_routes);
    char *p = list;
    while (*p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        while (*p == ' ') p++;
        if (strcmp(p, short_name) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* Is route an Express bus? (QM, BM, BxM, X prefixes) */
static int is_express_route(const char *route_id) {
    if (!route_id || !*route_id) return 0;
    return (strncmp(route_id, "QM", 2) == 0 ||
            strncmp(route_id, "BM", 2) == 0 ||
            strncmp(route_id, "BxM", 3) == 0 ||
            strncmp(route_id, "X", 1) == 0);
}

/* Now in America/New_York as (date_ymd, mins_since_midnight) */
static void now_ny(int *out_ymd, int *out_mins) {
    time_t t = time(NULL);
    struct tm tm;
    /* Use POSIX: set TZ, then localtime. */
    setenv("TZ", "America/New_York", 1);
    tzset();
    localtime_r(&t, &tm);
    *out_ymd = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    *out_mins = tm.tm_hour * 60 + tm.tm_min;
}

/* Build time_t from date_ymd and mins (minutes from midnight; can be >= 1440 for next day). */
static time_t ymd_mins_to_time(int date_ymd, int mins) {
    int day_offset = 0;
    while (mins >= 24 * 60) { mins -= 24 * 60; day_offset++; }
    while (mins < 0) { mins += 24 * 60; day_offset--; }
    int y = date_ymd / 10000, m = (date_ymd / 100) % 100, d = date_ymd % 100;
    struct tm tm = { 0 };
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d + day_offset;
    tm.tm_hour = mins / 60;
    tm.tm_min = mins % 60;
    tm.tm_isdst = -1;
    setenv("TZ", "America/New_York", 1);
    tzset();
    return mktime(&tm);
}

void gtfs_load(const char *gtfs_url, const char *cache_path) {
    if (!gtfs_url || !*gtfs_url || !cache_path || !*cache_path) return;

    n_routes = n_trips = n_stop_times = n_stops = n_calendars = n_cal_dates = 0;
    if (!routes) routes = calloc(MAX_ROUTES, sizeof(GtfsRoute));
    if (!trips) trips = calloc(MAX_TRIPS, sizeof(GtfsTrip));
    if (!stop_times) stop_times = calloc(MAX_STOPTIMES_AT_STOP, sizeof(GtfsStopTime));
    if (!stops) stops = calloc(MAX_STOPS, sizeof(GtfsStop));
    if (!calendars) calendars = calloc(MAX_CALENDAR, sizeof(GtfsCalendar));
    if (!cal_dates) cal_dates = calloc(MAX_CAL_DATES, sizeof(GtfsCalendarDate));
    if (!routes || !trips || !stop_times || !stops || !calendars || !cal_dates) return;

    /* Ensure cache dir exists; try download; if fail, use cache if exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", cache_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash && last_slash > dir) {
        *last_slash = '\0';
        char mkdir_cmd[1024];
        (void)snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s' 2>/dev/null", dir);
        (void)system(mkdir_cmd);
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -fsSL -o '%s' '%s' 2>/dev/null", cache_path, gtfs_url);
    int dl_ok = (system(cmd) == 0);
    if (!dl_ok) {
        FILE *f = fopen(cache_path, "rb");
        if (!f) {
            logf_("GTFS: download failed and no cache at %s", cache_path);
            return;
        }
        fclose(f);
    }

    read_zip_file(cache_path, "routes.txt", routes_fn, NULL);
    read_zip_file(cache_path, "trips.txt", trips_fn, NULL);
    read_zip_file(cache_path, "stops.txt", stops_fn, NULL);
    read_zip_file(cache_path, "calendar.txt", calendar_fn, NULL);
    read_zip_file(cache_path, "calendar_dates.txt", calendar_dates_fn, NULL);

    strncpy(gtfs_cache_path, cache_path, sizeof(gtfs_cache_path) - 1);
    gtfs_cache_path[sizeof(gtfs_cache_path) - 1] = '\0';
    gtfs_loaded = (n_routes > 0 && n_stops > 0);
    logf_("GTFS: routes=%d trips=%d stops=%d calendar=%d cal_dates=%d loaded=%d",
          n_routes, n_trips, n_stops, n_calendars, n_cal_dates, gtfs_loaded);
}

int gtfs_next_departures(const char *stop_id, const char *realtime_routes,
                         ScheduledDeparture *out, int max_out) {
    if (!gtfs_loaded || !out || max_out <= 0 || !stop_id || !*stop_id) return 0;

    if (!resolve_stop(stop_id)) return 0;
    n_stop_times = 0;
    read_zip_file(gtfs_cache_path[0] ? gtfs_cache_path : "/tmp/gtfs_bus_cache.zip",
                  "stop_times.txt", stop_times_fn, NULL);
    logf_("GTFS: stop_times at stop (%d ids): %d", n_stop_filter_ids, n_stop_times);

    int now_ymd, now_mins;
    now_ny(&now_ymd, &now_mins);

    typedef struct { char route_id[64]; time_t when; char headsign[128]; } Cand;
    Cand best[MAX_ROUTES];
    int route_seen[MAX_ROUTES];
    for (int i = 0; i < MAX_ROUTES; i++) {
        route_seen[i] = 0;
        best[i].when = (time_t)-1;
        best[i].route_id[0] = '\0';
    }

    time_t now_sec = time(NULL);
    int express_routes_found = 0;
    for (int i = 0; i < n_stop_times; i++) {
        GtfsTrip *tr = trip_by_id(stop_times[i].trip_id);
        if (!tr) continue;
        const char *short_name = route_short_name(tr->route_id);
        if (route_excluded(short_name, realtime_routes)) continue;
        if (is_express_route(tr->route_id)) express_routes_found++;

        int route_idx = -1;
        for (int r = 0; r < n_routes; r++)
            if (strcmp(routes[r].route_id, tr->route_id) == 0) { route_idx = r; break; }
        if (route_idx < 0) continue;

        int arr_mins = stop_times[i].arrival_mins;
        for (int day = 0; day < 14; day++) {
            int y = now_ymd / 10000, m = (now_ymd / 100) % 100, d = now_ymd % 100;
            struct tm tm = { 0 };
            tm.tm_year = y - 1900;
            tm.tm_mon = m - 1;
            tm.tm_mday = d + day;
            tm.tm_isdst = -1;
            setenv("TZ", "America/New_York", 1);
            tzset();
            time_t t = mktime(&tm);
            if (t == (time_t)-1) continue;
            struct tm *lt = localtime(&t);
            if (!lt) continue;
            int date_ymd = (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday;
            if (!service_active_on(date_ymd, tr->service_id)) continue;

            int day_off = 0, mins = arr_mins;
            while (mins >= 24 * 60) { mins -= 24 * 60; day_off++; }
            int actual_ymd = date_ymd;
            for (int k = 0; k < day_off; k++) {
                int yy = actual_ymd / 10000, mm = (actual_ymd / 100) % 100, dd = actual_ymd % 100;
                struct tm tm2 = { 0 };
                tm2.tm_year = yy - 1900;
                tm2.tm_mon = mm - 1;
                tm2.tm_mday = dd + 1;
                tm2.tm_isdst = -1;
                time_t t2 = mktime(&tm2);
                if (t2 == (time_t)-1) break;
                struct tm *lt2 = localtime(&t2);
                if (!lt2) break;
                actual_ymd = (lt2->tm_year + 1900) * 10000 + (lt2->tm_mon + 1) * 100 + lt2->tm_mday;
            }
            time_t when = ymd_mins_to_time(actual_ymd, mins);
            if (when >= now_sec && (best[route_idx].when == (time_t)-1 || when < best[route_idx].when)) {
                best[route_idx].when = when;
                (void)snprintf(best[route_idx].route_id, sizeof(best[route_idx].route_id), "%s", tr->route_id);
                (void)snprintf(best[route_idx].headsign, sizeof(best[route_idx].headsign), "%s", tr->headsign[0] ? tr->headsign : "--");
                route_seen[route_idx] = 1;
            }
        }
    }

    /* Collect unique routes with next departure, sort by when */
    /* For scheduled departures: filter to Express routes only, exclude Q27 */
    int n_out = 0;
    int q27_filtered = 0, non_express_filtered = 0;
    for (int r = 0; r < n_routes && n_out < max_out; r++) {
        if (!route_seen[r]) continue;
        const char *short_name = route_short_name(best[r].route_id);
        if (route_excluded(short_name, realtime_routes)) continue;
        /* Exclude Q27 from scheduled routes */
        if (strcmp(short_name, "Q27") == 0) {
            q27_filtered++;
            continue;
        }
        /* Only include Express routes in scheduled departures */
        if (!is_express_route(best[r].route_id)) {
            non_express_filtered++;
            continue;
        }
        out[n_out].when = best[r].when;
        strncpy(out[n_out].route, short_name, sizeof(out[n_out].route) - 1);
        out[n_out].route[sizeof(out[n_out].route) - 1] = '\0';
        strncpy(out[n_out].dest, best[r].headsign, sizeof(out[n_out].dest) - 1);
        out[n_out].dest[sizeof(out[n_out].dest) - 1] = '\0';
        n_out++;
    }
    /* Simple sort by when */
    for (int i = 0; i < n_out; i++)
        for (int j = i + 1; j < n_out; j++)
            if (out[j].when < out[i].when) {
                ScheduledDeparture tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
    logf_("GTFS: scheduled departures: %d (Express routes at stop: %d, Q27 filtered: %d, non-Express filtered: %d)", 
          n_out, express_routes_found, q27_filtered, non_express_filtered);
    return n_out;
}
