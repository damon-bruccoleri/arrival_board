/*
 * MTA Bus Time API: fetch arrivals for a stop and optional stop name.
 * Uses SIRI stop-monitoring JSON.
 */
#pragma once

#include "types.h"

/*
 * Fetch up to max_arr arrivals for the given stop.
 * Fills arr[] and returns the count. If stop_name/stop_name_sz are non-null,
 * the stop's display name from the API is written to stop_name.
 * route_filter: comma-separated route IDs to allow, or NULL for all.
 */
int fetch_mta_arrivals(Arrival *arr, int max_arr,
                      char *stop_name, size_t stop_name_sz,
                      const char *mta_key, const char *stop_id,
                      const char *route_filter);
