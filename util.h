/*
 * Utility helpers: logging, HTTP, JSON accessors.
 * Used by main.c, mta.c, and weather.c.
 */
#pragma once

#include <cjson/cJSON.h>
#include <stddef.h>

/* Clamp integer v to [lo, hi]. */
int clampi(int v, int lo, int hi);

/* Log a line to stderr (printf-style). */
void logf_(const char *fmt, ...);

/* URL-encode string 'in' into 'out', at most outsz bytes. Stops at first NUL. */
void urlencode(char *out, size_t outsz, const char *in);

/* Fetch URL via curl; returns malloc'd string or NULL. Caller must free. */
char *http_get(const char *url);

/* --- JSON helpers (return NULL or default; do not free the returned pointers) --- */
const cJSON *jgeto(const cJSON *o, const char *k);   /* get object member */
const cJSON *jgeti(const cJSON *a, int idx);         /* get array element */
const char *jgets(const cJSON *v);                   /* get string value */
int jint(const cJSON *v, int defv);                  /* get int with default */
double jdouble(const cJSON *v, double defv);         /* get double with default */
