/*
 * MTA Bus Time API: fetch arrivals for a stop and optional stop name.
 * Uses SIRI stop-monitoring JSON.
 */
#pragma once

#include "types.h"

/*
 * Fetch up to max_arr arrivals for the given stop.
 * Fills arr[] and returns the count (>=0). On HTTP/JSON failure returns -1
 * without changing arr[] — caller may keep showing the previous snapshot.
 * If stop_name/stop_name_sz are non-null, the stop's display name from the API is written to stop_name.
 * route_filter: comma-separated route IDs to allow, or NULL for all.
 */
int fetch_mta_arrivals(Arrival *arr, int max_arr,
                      char *stop_name, size_t stop_name_sz,
                      const char *mta_key, const char *stop_id,
                      const char *route_filter);

/* Build comma-separated list of routes in arr[0..n-1]. Log to stderr if any are Express (QM, BM, BxM, X). */
void mta_log_realtime_express_routes(const Arrival *arr, int n);

/* Last fetch status for debug instrumentation. */
int mta_last_status(void);
const char *mta_last_status_str(void);
