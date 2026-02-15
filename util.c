/*
 * Utility implementation: logging, HTTP, JSON accessors.
 */
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
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

char *http_get(const char *url) {
    if (!url) return NULL;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -fsSL --connect-timeout 4 --max-time 8 '%s'", url);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 64 * 1024;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (len + 2 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
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
