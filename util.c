/*
 * Utility implementation: logging, HTTP, JSON accessors.
 */
#include "util.h"
#include "types.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

void tz_set_ny(void) {
    setenv("TZ", "America/New_York", 1);
    tzset();
}

int arrivals_refresh_eta(Arrival *arr, int n, time_t now) {
    if (!arr || n <= 0) return 0;
    int out = 0;
    for (int i = 0; i < n; i++) {
        if (arr[i].expected > 0) {
            double d = difftime(arr[i].expected, now);
            if (d < -90.0) continue;
            int mins = (int)lrint(d / 60.0);
            if (mins < 0) mins = 0;
            arr[i].mins = mins;
        }
        if (out != i) arr[out] = arr[i];
        out++;
    }
    return out;
}

float layout_scale(int screen_height) {
    return (screen_height > 0) ? ((float)screen_height / (float)LAYOUT_REF_HEIGHT) : 1.0f;
}

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void log_to_boot_log(const char *fmt, ...) {
    const char *path = getenv("BOOT_LOG_PATH");
    char buf[1024];
    if (!path || !path[0]) {
        const char *home = getenv("HOME");
        if (home && *home)
            snprintf(buf, sizeof(buf), "%s/arrival_board/boot.log", home);
        else
            return;
        path = buf;
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%ld ", (long)time(NULL));
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

void urlencode(char *out, size_t outsz, const char *in) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 4 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* Single attempt: run curl (new process = fresh connection). Returns malloc'd string or NULL. */
static char *http_get_one(const char *url) {
    if (!url) return NULL;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -fsSL --connect-timeout 10 --max-time 20 --retry 1 --retry-delay 1 --retry-connrefused '%s'",
             url);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 64 * 1024;
    const size_t max_cap = 4 * 1024 * 1024;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (len + 2 > cap) {
            if (cap >= max_cap) {
                logf_("HTTP_GET_FAIL response exceeds %zu bytes, aborting", max_cap);
                free(buf); pclose(fp); return NULL;
            }
            cap *= 2;
            if (cap > max_cap) cap = max_cap;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    int rc = pclose(fp);
    if (rc != 0) {
        if (WIFEXITED(rc))
            logf_("HTTP_GET_FAIL url=%s exit=%d", url, WEXITSTATUS(rc));
        else if (WIFSIGNALED(rc))
            logf_("HTTP_GET_FAIL url=%s signal=%d", url, WTERMSIG(rc));
        else
            logf_("HTTP_GET_FAIL url=%s rc=%d", url, rc);
        free(buf);
        return NULL;
    }
    return buf;
}

char *http_get(const char *url) {
    if (!url) return NULL;
    char *buf = http_get_one(url);
    if (!buf) {
        sleep(2);
        buf = http_get_one(url);
    }
    return buf;
}

int is_express_route(const char *route) {
    if (!route || !route[0]) return 0;
    return (strncmp(route, "QM", 2) == 0 || strncmp(route, "BM", 2) == 0 ||
            strncmp(route, "BxM", 3) == 0 || route[0] == 'X');
}

const cJSON *jgeto(const cJSON *o, const char *k) {
    if (!o || !cJSON_IsObject(o)) return NULL;
    return cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
}

const cJSON *jgeti(const cJSON *a, int idx) {
    if (!a || !cJSON_IsArray(a)) return NULL;
    return cJSON_GetArrayItem((cJSON *)a, idx);
}

const char *jgets(const cJSON *v) {
    if (!v) return NULL;
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    return NULL;
}

int jint(const cJSON *v, int defv) {
    if (!v) return defv;
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring) return atoi(v->valuestring);
    return defv;
}

double jdouble(const cJSON *v, double defv) {
    if (!v) return defv;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return atof(v->valuestring);
    return defv;
}
