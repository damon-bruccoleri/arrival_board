/*
 * Weather: Open-Meteo forecast at stop location.
 * Location from STOP_LAT/STOP_LON or NYC default.
 */
#pragma once

#include "types.h"

void fetch_weather(Weather *w, const char *stop_name);

/* Last weather fetch status for debug instrumentation. */
int weather_last_status(void);
const char *weather_last_status_str(void);
