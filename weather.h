/*
 * Weather: Open-Meteo forecast at stop location.
 * Location is derived from STOP_LAT/STOP_LON, or geocoding the stop name, or WEATHER_LAT/LON fallback.
 */
#pragma once

#include "types.h"

/* Fetch current weather for the given stop (stop_name used for geocoding if no coords set). */
void fetch_weather(Weather *w, const char *stop_name);
