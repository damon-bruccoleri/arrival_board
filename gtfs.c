/*
 * GTFS static: load MTA bus GTFS zip, parse, find next scheduled departures.
 * Timezone America/New_York. Uses cache file; updates daily.
 */
#include "gtfs.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    GTFS_STATUS_OK = 0,
    GTFS_STATUS_BAD_INPUT = 1,
    GTFS_STATUS_ALLOC_FAIL = 2,
    GTFS_STATUS_DOWNLOAD_FAIL = 3,
    GTFS_STATUS_PARSE_FAIL = 4,
};

#define MAX_ROUTES   512
#define MAX_TRIPS    4000
#define MAX_STOPTIMES_AT_STOP 8000
#define MAX_STOPS    16000
#define MAX_CALENDAR 128
#define MAX_CAL_DATES 4096
#define MAX_SERVICE_IDS 128

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
    int arrival_mins;
} GtfsStopTime;

typedef struct {
    char stop_id[32];
    char stop_code[32];
} GtfsStop;

typedef struct {
    char service_id[64];
    int start_ymd;
    int end_ymd;
    int dow[7];
} GtfsCalendar;

typedef struct {
    char service_id[64];
    int date_ymd;
    int exception_type;
} GtfsCalendarDate;

typedef struct {
    GtfsRoute        *routes;
    int               n_routes;
    GtfsTrip         *trips;
    int               n_trips;
    GtfsStopTime     *stop_times;
    int               n_stop_times;
    GtfsStop         *stops;
    int               n_stops;
    GtfsCalendar     *calendars;
    int               n_calendars;
    GtfsCalendarDate *cal_dates;
    int               n_cal_dates;
    char              cache_path[512];
    int               loaded;
    int               stop_times_cached;
    char              cached_stop_id[64];
} GtfsFeed;

static GtfsFeed feed;

#define MAX_STOP_FILTER_IDS 32
static char stop_filter_ids[MAX_STOP_FILTER_IDS][32];
static int n_stop_filter_ids;
static char gtfs_stop_id_resolved[32];
static int g_gtfs_last_status = GTFS_STATUS_OK;

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

static int read_zip_file(const char *zip_path, const char *file_name,
                         int (*fn)(char *line, void *ctx), void *ctx) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "unzip -p '%s' '%s' 2>/dev/null", zip_path, file_name);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[2048];
    if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return 0; }
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
    if (n < 2 || feed.n_routes >= MAX_ROUTES) return (n < 2) ? 0 : -1;
    GtfsRoute *r = &feed.routes[feed.n_routes++];
    snprintf(r->route_id, sizeof(r->route_id), "%s", f[0]);
    snprintf(r->short_name, sizeof(r->short_name), "%s", n >= 3 ? f[2] : f[0]);
    return 0;
}

static int stop_times_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 4) return 0;
    int match = 0;
    for (int i = 0; i < n_stop_filter_ids; i++)
        if (strcmp(f[3], stop_filter_ids[i]) == 0) { match = 1; break; }
    if (!match) return 0;
    if (feed.n_stop_times >= MAX_STOPTIMES_AT_STOP) return -1;
    GtfsStopTime *st = &feed.stop_times[feed.n_stop_times];
    snprintf(st->trip_id, sizeof(st->trip_id), "%s", f[0]);
    st->arrival_mins = parse_time_mins(f[1]);
    if (st->arrival_mins < 0) return 0;
    feed.n_stop_times++;
    return 0;
}

static int stops_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 1 || feed.n_stops >= MAX_STOPS) return (n < 1) ? 0 : -1;
    GtfsStop *s = &feed.stops[feed.n_stops++];
    snprintf(s->stop_id, sizeof(s->stop_id), "%s", f[0]);
    snprintf(s->stop_code, sizeof(s->stop_code), "%s", n >= 2 ? f[1] : "");
    return 0;
}

static int calendar_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 10 || feed.n_calendars >= MAX_CALENDAR) return (n < 10) ? 0 : -1;
    GtfsCalendar *c = &feed.calendars[feed.n_calendars];
    snprintf(c->service_id, sizeof(c->service_id), "%s", f[0]);
    if (strlen(f[8]) >= 8) c->start_ymd = atoi(f[8]);
    if (strlen(f[9]) >= 8) c->end_ymd = atoi(f[9]);
    if (n >= 8) {
        c->dow[0] = (f[7][0] == '1') ? 1 : 0;
        for (int i = 1; i < 7; i++)
            c->dow[i] = (f[i][0] == '1') ? 1 : 0;
    }
    feed.n_calendars++;
    return 0;
}

static int calendar_dates_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 3 || feed.n_cal_dates >= MAX_CAL_DATES) return (n < 3) ? 0 : -1;
    GtfsCalendarDate *cd = &feed.cal_dates[feed.n_cal_dates++];
    snprintf(cd->service_id, sizeof(cd->service_id), "%s", f[0]);
    if (strlen(f[1]) >= 8) cd->date_ymd = atoi(f[1]);
    cd->exception_type = atoi(f[2]);
    return 0;
}

static int service_active_on(int service_ymd, const char *service_id) {
    int y = service_ymd / 10000, m = (service_ymd / 100) % 100, d = service_ymd % 100;
    struct tm tm = { 0 };
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_isdst = -1;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) return 0;
    struct tm *lt = gmtime(&t);
    int dow = lt ? lt->tm_wday : -1;

    for (int i = 0; i < feed.n_cal_dates; i++) {
        if (strcmp(feed.cal_dates[i].service_id, service_id) != 0) continue;
        if (feed.cal_dates[i].date_ymd != service_ymd) continue;
        if (feed.cal_dates[i].exception_type == 1) return 1;
        if (feed.cal_dates[i].exception_type == 2) return 0;
    }
    for (int i = 0; i < feed.n_calendars; i++) {
        if (strcmp(feed.calendars[i].service_id, service_id) != 0) continue;
        if (service_ymd < feed.calendars[i].start_ymd || service_ymd > feed.calendars[i].end_ymd) continue;
        if (dow >= 0 && dow < 7 && feed.calendars[i].dow[dow]) return 1;
    }
    return 0;
}

static int resolve_stop(const char *stop_id) {
    const char *code_to_match = NULL;
    int found_idx = -1;
    for (int i = 0; i < feed.n_stops; i++) {
        if (strcmp(feed.stops[i].stop_id, stop_id) == 0) {
            snprintf(gtfs_stop_id_resolved, sizeof(gtfs_stop_id_resolved), "%s", feed.stops[i].stop_id);
            code_to_match = feed.stops[i].stop_code[0] ? feed.stops[i].stop_code : feed.stops[i].stop_id;
            found_idx = i;
            break;
        }
    }
    if (found_idx < 0) {
        for (int i = 0; i < feed.n_stops; i++) {
            if (feed.stops[i].stop_code[0] && strcmp(feed.stops[i].stop_code, stop_id) == 0) {
                snprintf(gtfs_stop_id_resolved, sizeof(gtfs_stop_id_resolved), "%s", feed.stops[i].stop_id);
                code_to_match = feed.stops[i].stop_code;
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
    for (int i = 0; i < feed.n_stops && n_stop_filter_ids < MAX_STOP_FILTER_IDS; i++) {
        int include = 0;
        if (code_to_match && feed.stops[i].stop_code[0] && strcmp(feed.stops[i].stop_code, code_to_match) == 0)
            include = 1;
        else if (code_to_match && !feed.stops[i].stop_code[0] && strcmp(feed.stops[i].stop_id, code_to_match) == 0)
            include = 1;
        if (!include && feed.stops[i].stop_code[0] && strcmp(feed.stops[i].stop_code, stop_id) == 0)
            include = 1;
        if (include) {
            int already = 0;
            for (int j = 0; j < n_stop_filter_ids; j++)
                if (strcmp(stop_filter_ids[j], feed.stops[i].stop_id) == 0) { already = 1; break; }
            if (!already) {
                snprintf(stop_filter_ids[n_stop_filter_ids], sizeof(stop_filter_ids[0]), "%s", feed.stops[i].stop_id);
                n_stop_filter_ids++;
            }
        }
    }
    if (n_stop_filter_ids == 0) {
        snprintf(stop_filter_ids[0], sizeof(stop_filter_ids[0]), "%s", gtfs_stop_id_resolved);
        n_stop_filter_ids = 1;
    }
    return 1;
}

static const char *route_short_name(const char *route_id) {
    for (int i = 0; i < feed.n_routes; i++)
        if (strcmp(feed.routes[i].route_id, route_id) == 0)
            return feed.routes[i].short_name;
    return route_id;
}

static GtfsTrip *trip_by_id(const char *trip_id) {
    for (int i = 0; i < feed.n_trips; i++)
        if (strcmp(feed.trips[i].trip_id, trip_id) == 0)
            return &feed.trips[i];
    return NULL;
}

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

static void now_ny(int *out_ymd, int *out_mins) {
    time_t t = time(NULL);
    struct tm tm;
    tz_set_ny();
    localtime_r(&t, &tm);
    *out_ymd = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    *out_mins = tm.tm_hour * 60 + tm.tm_min;
}

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
    tz_set_ny();
    return mktime(&tm);
}

/* Only keep trips whose trip_id appears in the already-parsed stop_times. */
static int trips_filtered_fn(char *line, void *ctx) {
    (void)ctx;
    char *f[32];
    int n = parse_csv_line(line, f, 32);
    if (n < 3) return 0;
    int found = 0;
    for (int i = 0; i < feed.n_stop_times; i++)
        if (strcmp(feed.stop_times[i].trip_id, f[2]) == 0) { found = 1; break; }
    if (!found) return 0;
    if (feed.n_trips >= MAX_TRIPS) return -1;
    GtfsTrip *t = &feed.trips[feed.n_trips++];
    snprintf(t->trip_id, sizeof(t->trip_id), "%s", f[2]);
    snprintf(t->route_id, sizeof(t->route_id), "%s", f[0]);
    snprintf(t->service_id, sizeof(t->service_id), "%s", f[1]);
    snprintf(t->headsign, sizeof(t->headsign), "%s", n >= 4 ? f[3] : "");
    return 0;
}

static void gtfs_parse_zip(const char *zip_path) {
    feed.n_routes = feed.n_trips = feed.n_stop_times = 0;
    feed.n_stops = feed.n_calendars = feed.n_cal_dates = 0;
    feed.stop_times_cached = 0;
    feed.cached_stop_id[0] = '\0';
    read_zip_file(zip_path, "routes.txt", routes_fn, NULL);
    /* trips.txt is loaded lazily in gtfs_next_departures, filtered by stop */
    read_zip_file(zip_path, "stops.txt", stops_fn, NULL);
    read_zip_file(zip_path, "calendar.txt", calendar_fn, NULL);
    read_zip_file(zip_path, "calendar_dates.txt", calendar_dates_fn, NULL);
}

static int feed_alloc(void) {
    if (!feed.routes)    feed.routes    = calloc(MAX_ROUTES, sizeof(GtfsRoute));
    if (!feed.trips)     feed.trips     = calloc(MAX_TRIPS, sizeof(GtfsTrip));
    if (!feed.stop_times)feed.stop_times= calloc(MAX_STOPTIMES_AT_STOP, sizeof(GtfsStopTime));
    if (!feed.stops)     feed.stops     = calloc(MAX_STOPS, sizeof(GtfsStop));
    if (!feed.calendars) feed.calendars = calloc(MAX_CALENDAR, sizeof(GtfsCalendar));
    if (!feed.cal_dates) feed.cal_dates = calloc(MAX_CAL_DATES, sizeof(GtfsCalendarDate));
    return (feed.routes && feed.trips && feed.stop_times &&
            feed.stops && feed.calendars && feed.cal_dates);
}

void gtfs_load(const char *gtfs_url, const char *cache_path) {
    if (!gtfs_url || !*gtfs_url || !cache_path || !*cache_path) {
        g_gtfs_last_status = GTFS_STATUS_BAD_INPUT;
        return;
    }

    if (!feed_alloc()) {
        g_gtfs_last_status = GTFS_STATUS_ALLOC_FAIL;
        return;
    }

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", cache_path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash && last_slash > dir) {
        *last_slash = '\0';
        char mkdir_cmd[1024];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s' 2>/dev/null", dir);
        (void)system(mkdir_cmd);
    }
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --connect-timeout 15 --max-time 120 -o '%s' '%s' 2>/dev/null",
             cache_path, gtfs_url);
    int dl_ok = (system(cmd) == 0);
    if (!dl_ok) {
        logf_("GTFS: download failed, retrying in 2s");
        sleep(2);
        dl_ok = (system(cmd) == 0);
    }
    if (!dl_ok) {
        logf_("GTFS: download failed at %s", cache_path);
        g_gtfs_last_status = GTFS_STATUS_DOWNLOAD_FAIL;
        if (access(cache_path, R_OK) == 0) {
            gtfs_parse_zip(cache_path);
            snprintf(feed.cache_path, sizeof(feed.cache_path), "%s", cache_path);
            feed.loaded = (feed.n_routes > 0 && feed.n_stops > 0);
            g_gtfs_last_status = feed.loaded ? GTFS_STATUS_OK : GTFS_STATUS_PARSE_FAIL;
            logf_("GTFS: loaded from existing cache after download fail: routes=%d trips=%d stops=%d loaded=%d",
                  feed.n_routes, feed.n_trips, feed.n_stops, feed.loaded);
        } else {
            logf_("GTFS: no cache file; keeping previous in-memory feed if any");
        }
        return;
    }

    gtfs_parse_zip(cache_path);
    snprintf(feed.cache_path, sizeof(feed.cache_path), "%s", cache_path);
    feed.loaded = (feed.n_routes > 0 && feed.n_stops > 0);
    g_gtfs_last_status = feed.loaded ? GTFS_STATUS_OK : GTFS_STATUS_PARSE_FAIL;
    logf_("GTFS: routes=%d trips=%d stops=%d calendar=%d cal_dates=%d loaded=%d",
          feed.n_routes, feed.n_trips, feed.n_stops, feed.n_calendars, feed.n_cal_dates, feed.loaded);
}

int gtfs_last_status(void) {
    return g_gtfs_last_status;
}

const char *gtfs_last_status_str(void) {
    switch (g_gtfs_last_status) {
        case GTFS_STATUS_OK:            return "OK";
        case GTFS_STATUS_BAD_INPUT:     return "BAD_INPUT";
        case GTFS_STATUS_ALLOC_FAIL:    return "ALLOC_FAIL";
        case GTFS_STATUS_DOWNLOAD_FAIL: return "DOWNLOAD_FAIL";
        case GTFS_STATUS_PARSE_FAIL:    return "PARSE_FAIL";
        default:                        return "UNKNOWN";
    }
}

int gtfs_next_departures(const char *stop_id, const char *realtime_routes,
                         ScheduledDeparture *out, int max_out) {
    if (!feed.loaded || !out || max_out <= 0 || !stop_id || !*stop_id) return 0;

    if (!feed.stop_times_cached || strcmp(stop_id, feed.cached_stop_id) != 0) {
        if (!resolve_stop(stop_id)) return 0;
        const char *zip = feed.cache_path[0] ? feed.cache_path : "/tmp/gtfs_bus_cache.zip";

        feed.n_stop_times = 0;
        read_zip_file(zip, "stop_times.txt", stop_times_fn, NULL);
        logf_("GTFS: stop_times at stop (%d ids): %d", n_stop_filter_ids, feed.n_stop_times);

        feed.n_trips = 0;
        read_zip_file(zip, "trips.txt", trips_filtered_fn, NULL);
        logf_("GTFS: trips matching stop: %d", feed.n_trips);

        feed.stop_times_cached = 1;
        snprintf(feed.cached_stop_id, sizeof(feed.cached_stop_id), "%s", stop_id);
    }

    int now_ymd, now_mins;
    now_ny(&now_ymd, &now_mins);

    typedef struct { char route_id[64]; time_t when; char headsign[128]; } Cand;
    static Cand best[MAX_ROUTES];
    static int route_seen[MAX_ROUTES];
    for (int i = 0; i < MAX_ROUTES; i++) {
        route_seen[i] = 0;
        best[i].when = (time_t)-1;
        best[i].route_id[0] = '\0';
    }

    time_t now_sec = time(NULL);
    int express_routes_found = 0;
    for (int i = 0; i < feed.n_stop_times; i++) {
        GtfsTrip *tr = trip_by_id(feed.stop_times[i].trip_id);
        if (!tr) continue;
        const char *short_name = route_short_name(tr->route_id);
        if (route_excluded(short_name, realtime_routes)) continue;
        if (is_express_route(tr->route_id)) express_routes_found++;

        int route_idx = -1;
        for (int ri = 0; ri < feed.n_routes; ri++)
            if (strcmp(feed.routes[ri].route_id, tr->route_id) == 0) { route_idx = ri; break; }
        if (route_idx < 0) continue;

        int arr_mins = feed.stop_times[i].arrival_mins;
        for (int day = 0; day < 14; day++) {
            int y = now_ymd / 10000, m = (now_ymd / 100) % 100, d = now_ymd % 100;
            struct tm tm = { 0 };
            tm.tm_year = y - 1900;
            tm.tm_mon = m - 1;
            tm.tm_mday = d + day;
            tm.tm_isdst = -1;
            tz_set_ny();
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
                snprintf(best[route_idx].route_id, sizeof(best[route_idx].route_id), "%s", tr->route_id);
                snprintf(best[route_idx].headsign, sizeof(best[route_idx].headsign), "%s",
                         tr->headsign[0] ? tr->headsign : "--");
                route_seen[route_idx] = 1;
            }
        }
    }

    int n_out = 0;
    int q27_filtered = 0, non_express_filtered = 0;
    for (int ri = 0; ri < feed.n_routes && n_out < max_out; ri++) {
        if (!route_seen[ri]) continue;
        const char *short_name = route_short_name(best[ri].route_id);
        if (route_excluded(short_name, realtime_routes)) continue;
        if (strcmp(short_name, "Q27") == 0) { q27_filtered++; continue; }
        if (!is_express_route(best[ri].route_id)) { non_express_filtered++; continue; }
        out[n_out].when = best[ri].when;
        snprintf(out[n_out].route, sizeof(out[n_out].route), "%.31s", short_name);
        snprintf(out[n_out].dest, sizeof(out[n_out].dest), "%s", best[ri].headsign);
        n_out++;
    }
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
