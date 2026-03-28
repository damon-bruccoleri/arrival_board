/*
 * Utility helpers: logging, HTTP, JSON accessors.
 * Used by main.c, mta.c, weather.c, ui.c.
 */
#pragma once

#include "types.h"
#include <cjson/cJSON.h>
#include <stddef.h>
#include <time.h>

/* Clamp integer v to [lo, hi]. */
int clampi(int v, int lo, int hi);

/* Layout scale factor from reference height (LAYOUT_REF_HEIGHT). */
float layout_scale(int screen_height);

/* Log a line to stderr (printf-style). */
void logf_(const char *fmt, ...);

/* Append one line to boot.log (path from BOOT_LOG_PATH or $HOME/arrival_board/boot.log). Used for resource/code failures only. */
void log_to_boot_log(const char *fmt, ...);

/* URL-encode string 'in' into 'out', at most outsz bytes. Stops at first NUL. */
void urlencode(char *out, size_t outsz, const char *in);

/* Fetch URL via curl; returns malloc'd string or NULL. Caller must free. */
char *http_get(const char *url);

/* Return 1 if route is an Express route (QM*, BM*, BxM*, X*). */
int is_express_route(const char *route);

/* Recompute each arrival's mins from expected vs now (call every frame or before render).
 * Keeps countdown accurate between MTA polls and when HTTP fails but last snapshot is kept.
 * Drops arrivals whose expected time is more than 90s in the past; returns new count. */
int arrivals_refresh_eta(Arrival *arr, int n, time_t now);

/* Set process TZ to America/New_York and call tzset(). Idempotent. */
void tz_set_ny(void);

/* --- JSON helpers (return NULL or default; do not free the returned pointers) --- */
const cJSON *jgeto(const cJSON *o, const char *k);   /* get object member */
const cJSON *jgeti(const cJSON *a, int idx);         /* get array element */
const char *jgets(const cJSON *v);                   /* get string value */
int jint(const cJSON *v, int defv);                  /* get int with default */
double jdouble(const cJSON *v, double defv);         /* get double with default */
