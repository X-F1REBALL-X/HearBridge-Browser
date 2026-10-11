/* diag.c - see diag.h. */
#include "diag.h"
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define N_KEYS  16
#define KEY_MAX 32
#define VAL_MAX 200

static struct { char k[KEY_MAX]; char v[VAL_MAX]; } g_d[N_KEYS];
static int g_n;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void diag_set(const char *key, const char *fmt, ...)
{
    char val[VAL_MAX];
    va_list ap;
    int i;

    va_start(ap, fmt);
    vsnprintf(val, sizeof val, fmt, ap);
    va_end(ap);
    log_line("diag: %s: %s", key, val);
    pthread_mutex_lock(&g_mu);
    for (i = 0; i < g_n; i++)
        if (strcmp(g_d[i].k, key) == 0) break;
    if (i == g_n && g_n < N_KEYS) {
        snprintf(g_d[i].k, KEY_MAX, "%s", key);
        g_n++;
    }
    if (i < N_KEYS) snprintf(g_d[i].v, VAL_MAX, "%s", val);
    pthread_mutex_unlock(&g_mu);
}

int diag_save(void) { return 0; }

static size_t esc(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 2 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c >= 0x20) o[n++] = (char)c;
    }
    o[n] = 0;
    return n;
}

int diag_json(char *out, size_t cap)
{
    size_t n = 0;
    int i;
    char k[KEY_MAX * 2], v[VAL_MAX * 2];

    if (cap) out[0] = 0;
    pthread_mutex_lock(&g_mu);
    for (i = 0; i < g_n && n < cap; i++) {
        esc(k, sizeof k, g_d[i].k);
        esc(v, sizeof v, g_d[i].v);
        n += (size_t)snprintf(out + n, cap - n, "%s\"%s\":\"%s\"", i ? "," : "", k, v);
    }
    pthread_mutex_unlock(&g_mu);
    if (n >= cap) { n = cap ? cap - 1 : 0; if (cap) out[n] = 0; }
    return (int)n;
}
