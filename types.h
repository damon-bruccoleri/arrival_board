/*
 * Shared data types and layout constants for the arrival board.
 * Included by main.c, mta.c, and weather.c.
 */
#pragma once

#include <time.h>

/* One bus arrival from the MTA API. */
typedef struct Arrival {
    char route[32];
    char bus[32];
    char dest[128];
    int  stops_away;     /* -1 if unknown */
    int  mins;           /* minutes until arrival, -1 if unknown */
    time_t expected;
    double miles_away;   /* miles until stop, <0 if unknown */
    int ppl_est;         /* estimated people count */
} Arrival;

/* Weather data from Open-Meteo (tied to stop location when possible). */
typedef struct Weather {
    int have;
    char icon[8];       /* unicode symbol */
    int temp_f;
    int precip_prob;    /* percent, -1 if unknown */
    double precip_in;   /* inches, -1 if unknown */
    time_t last_fetch;
    double lat, lon;
} Weather;

/* Grid layout: fixed columns and rows; only this many tiles are drawn. */
#define TILE_COLS_FIXED  2
#define TILE_ROWS_FIXED  6
#define TILE_SLOTS_MAX  (TILE_COLS_FIXED * TILE_ROWS_FIXED)

/* Reference height for scaling (e.g. 2160p). Layout scales from this. */
#define LAYOUT_REF_HEIGHT  2160
