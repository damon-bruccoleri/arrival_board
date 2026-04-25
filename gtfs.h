/*
 * GTFS static schedule: load MTA bus GTFS, find next scheduled departures.
 * Uses cache file; updates daily. Timezone America/New_York.
 */
#pragma once

#include "types.h"

/* Load or refresh GTFS from URL. Uses cache if download fails. Call periodically (e.g. daily). */
void gtfs_load(const char *gtfs_url, const char *cache_path);

/* Get next scheduled departures at stop_id for routes NOT in realtime_routes.
 * stop_id: same as STOP_ID (try as GTFS stop_id, then stop_code).
 * realtime_routes: comma-separated or NULL = all routes.
 * out: filled with up to SCHEDULED_MAX entries, 1 per route. Returns count. */
int gtfs_next_departures(const char *stop_id, const char *realtime_routes,
                         ScheduledDeparture *out, int max_out);

/* Returns 1 if the loaded GTFS feed contains stop_id as a stop_id or stop_code.
 * Returns 0 if the feed is loaded and the stop is unknown, or -1 if unavailable. */
int gtfs_stop_known(const char *stop_id);

/* Last GTFS load status for debug instrumentation. */
int gtfs_last_status(void);
const char *gtfs_last_status_str(void);
