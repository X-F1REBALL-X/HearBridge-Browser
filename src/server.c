/* server.c - HTTP + WebSocket server for HearBridge Browser (see server.h).
 * Per-listener quality over the same WebSocket (pcm48 / pcm24 decimated on
 * the console / 8-bit mu-law 48k / mu-law 24k), per-listener low-latency
 * catch-up, /status with app + version, and /lame.min.js for MP3 export.
 * The PCM ring and the capture side are unchanged from the PS-Stream audio MVP. */
#include "server.h"
#include "diag.h"
#include "log.h"
#include "page_html.h"
#include "lame_js.h"
#include "sha1.h"
#include "util.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define BYTES_PER_FRAME (PSS_CHANNELS * 2)
#define RING_BYTES  (PSS_RATE * BYTES_PER_FRAME * 2)   /* 2 s */
#define MAX_LAG     (PSS_RATE * BYTES_PER_FRAME)       /* 1 s: then skip to live */
#define MAX_CHUNK   16384                              /* ~85 ms per WS frame max */
#define MAX_CLIENTS 12
#define IN_MAX      4096

enum { K_FREE = 0, K_HTTP, K_WS, K_WAV };

/* Per-listener stream quality (WebSocket only; /live.wav stays pcm48). */
enum { Q_PCM48 = 0, Q_PCM24, Q_ULAW48, Q_ULAW24, Q_COUNT };
static const char *const Q_NAME[Q_COUNT] = { "pcm48", "pcm24", "ulaw48", "ulaw24" };

/* 47-tap Blackman half-band low-pass (-0.1 dB @10 kHz, -6 dB @12 kHz,
 * -77 dB @15 kHz at 48 kHz) used for the 2:1 decimation to 24 kHz. */
#define HB_TAPS 47
static const float HB_COEF[HB_TAPS] = { 0.000000000f, 0.000000000f, 0.000104511f, -0.000000000f, -0.000495819f, 0.000000000f, 0.001371807f, -0.000000000f, -0.003052238f, 0.000000000f, 0.005996360f, -0.000000000f, -0.010847626f, 0.000000000f, 0.018581654f, -0.000000000f, -0.030992063f, 0.000000000f, 0.052465979f, -0.000000000f, -0.099016832f, 0.000000000f, 0.315883798f, 0.500000939f, 0.315883798f, 0.000000000f, -0.099016832f, -0.000000000f, 0.052465979f, 0.000000000f, -0.030992063f, -0.000000000f, 0.018581654f, 0.000000000f, -0.010847626f, -0.000000000f, 0.005996360f, 0.000000000f, -0.003052238f, -0.000000000f, 0.001371807f, 0.000000000f, -0.000495819f, -0.000000000f, 0.000104511f, 0.000000000f, 0.000000000f };

typedef struct {
    int fd;
    int kind;
    char in[IN_MAX];
    int inlen;
    char *out;          /* pending bytes (headers, one frame) */
    int outlen, outoff, outcap;
    int close_after;    /* close once out is flushed */
    uint64_t rpos;      /* ring read position (bytes) */
    char peer[48];
    long t_open;
    int q;              /* Q_* */
    int low;            /* 1 = low-latency catch-up */
    float hist[PSS_CHANNELS][2 * HB_TAPS];  /* decimator history (doubled) */
    int hpos, phase;
} client_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t  g_ring[RING_BYTES];
static uint64_t g_wpos;            /* total bytes ever written */
static int      g_listeners;
static volatile int g_quit, g_stopreq;
static char     g_cap_state[96] = "idle (waiting for a listener)";
static float    g_peak;
static uint64_t g_skips;

static int g_srv = -1, g_wake[2] = { -1, -1 }, g_port;
static pthread_t g_thr;
static client_t g_cl[MAX_CLIENTS];
static long g_t0;

/* ---- ring -------------------------------------------------------------- */

void server_push_pcm(const int16_t *pcm, int frames)
{
    const uint8_t *src = (const uint8_t *)pcm;
    size_t n = (size_t)frames * BYTES_PER_FRAME, off;
    char b = 1;

    if (frames <= 0) return;
    if (n > RING_BYTES) { src += n - RING_BYTES; n = RING_BYTES; }
    pthread_mutex_lock(&g_mu);
    off = (size_t)(g_wpos % RING_BYTES);
    if (off + n <= RING_BYTES) memcpy(g_ring + off, src, n);
    else {
        memcpy(g_ring + off, src, RING_BYTES - off);
        memcpy(g_ring, src + (RING_BYTES - off), n - (RING_BYTES - off));
    }
    g_wpos += n;
    pthread_mutex_unlock(&g_mu);
    if (g_wake[1] >= 0) (void)!write(g_wake[1], &b, 1);
}

/* Copy up to max bytes from rpos; adjusts *rpos for lag. Returns bytes. */
static int ring_read(uint64_t *rpos, uint8_t *dst, int max, int low)
{
    const uint64_t max_lag = low ? (uint64_t)(PSS_RATE / 4) * BYTES_PER_FRAME  /* 250 ms */
                                 : (uint64_t)MAX_LAG;
    const uint64_t back = (uint64_t)BYTES_PER_FRAME * (low ? PSS_RATE / 33 : PSS_RATE / 20); /* 30 / 50 ms */
    uint64_t w;
    size_t n, off;

    pthread_mutex_lock(&g_mu);
    w = g_wpos;
    if (w - *rpos > max_lag) {
        *rpos = w - back;
        g_skips++;
    }
    n = (size_t)(w - *rpos);
    if (n > (size_t)max) n = (size_t)max;
    n -= n % BYTES_PER_FRAME;
    off = (size_t)(*rpos % RING_BYTES);
    if (off + n <= RING_BYTES) memcpy(dst, g_ring + off, n);
    else {
        memcpy(dst, g_ring + off, RING_BYTES - off);
        memcpy(dst + (RING_BYTES - off), g_ring, n - (RING_BYTES - off));
    }
    *rpos += n;
    pthread_mutex_unlock(&g_mu);
    return (int)n;
}

static uint64_t ring_live(void)
{
    uint64_t w;
    pthread_mutex_lock(&g_mu);
    w = g_wpos;
    pthread_mutex_unlock(&g_mu);
    return w;
}

int server_listeners(void)
{
    int n;
    pthread_mutex_lock(&g_mu);
    n = g_listeners;
    pthread_mutex_unlock(&g_mu);
    return n;
}

int server_stop_requested(void) { return g_stopreq; }

void server_set_capture(const char *state, float peak)
{
    pthread_mutex_lock(&g_mu);
    if (state) snprintf(g_cap_state, sizeof g_cap_state, "%s", state);
    g_peak = peak;
    pthread_mutex_unlock(&g_mu);
}

static void console_ip(char *ip, size_t n)
{
    struct ifaddrs *ifa, *p;
    snprintf(ip, n, "<console-ip>");
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        struct sockaddr_in *sa;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        sa = (struct sockaddr_in *)(void *)p->ifa_addr;
        if ((ntohl(sa->sin_addr.s_addr) >> 24) == 127) continue;
        inet_ntop(AF_INET, &sa->sin_addr, ip, (socklen_t)n);
        break;
    }
    freeifaddrs(ifa);
}

void server_url(char *out, int cap)
{
    char ip[48];
    console_ip(ip, sizeof ip);
    snprintf(out, (size_t)cap, "http://%s:%d", ip, g_port);
}

/* ---- client output ----------------------------------------------------- */

static int out_append(client_t *c, const void *p, int n)
{
    if (c->outoff && c->outoff == c->outlen) c->outoff = c->outlen = 0;
    if (c->outlen + n > c->outcap) {
        int cap = c->outcap ? c->outcap : 4096;
        char *nb;
        while (cap < c->outlen + n) cap *= 2;
        nb = realloc(c->out, (size_t)cap);
        if (!nb) return -1;
        c->out = nb;
        c->outcap = cap;
    }
    memcpy(c->out + c->outlen, p, (size_t)n);
    c->outlen += n;
    return 0;
}

static void ws_frame_hdr(client_t *c, int opcode, size_t len)
{
    uint8_t h[10];
    int hl;
    h[0] = (uint8_t)(0x80 | opcode);
    if (len < 126) { h[1] = (uint8_t)len; hl = 2; }
    else if (len < 65536) { h[1] = 126; h[2] = (uint8_t)(len >> 8); h[3] = (uint8_t)len; hl = 4; }
    else {
        int i;
        h[1] = 127;
        for (i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hl = 10;
    }
    out_append(c, h, hl);
}

static void ws_send(client_t *c, int opcode, const void *p, size_t n)
{
    ws_frame_hdr(c, opcode, n);
    if (n) out_append(c, p, (int)n);
}

static void reply(client_t *c, const char *status, const char *ctype,
                  const void *body, int blen, const char *extra)
{
    char h[512];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                     "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n%s\r\n",
                     status, ctype, blen, extra ? extra : "");
    out_append(c, h, n);
    if (blen) out_append(c, body, blen);
    c->close_after = 1;
}

static void client_close(client_t *c)
{
    if (c->kind == K_WS || c->kind == K_WAV) {
        pthread_mutex_lock(&g_mu);
        g_listeners--;
        pthread_mutex_unlock(&g_mu);
        log_line("http: listener %s left", c->peer);
    }
    close(c->fd);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void become_listener(client_t *c, int kind)
{
    c->kind = kind;
    c->rpos = ring_live();
    c->rpos -= c->rpos % BYTES_PER_FRAME;
    pthread_mutex_lock(&g_mu);
    g_listeners++;
    pthread_mutex_unlock(&g_mu);
    log_line("http: %s listener %s", kind == K_WS ? "websocket" : "wav", c->peer);
}

/* ---- request handling -------------------------------------------------- */

static int header(const char *req, const char *name, char *out, int cap)
{
    size_t nl = strlen(name);
    const char *p = strstr(req, "\r\n");
    while (p && p[2] && !(p[2] == '\r' && p[3] == '\n')) {
        p += 2;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1, *e;
            int n;
            while (*v == ' ' || *v == '\t') v++;
            e = strstr(v, "\r\n");
            if (!e) e = v + strlen(v);
            n = (int)(e - v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, (size_t)n);
            out[n] = 0;
            return 1;
        }
        p = strstr(p, "\r\n");
    }
    return 0;
}

static void status_json(client_t *c)
{
    char body[8192], dj[6144], url[64], st[96];
    uint64_t w, sk;
    int lis;
    float pk;
    pthread_mutex_lock(&g_mu);
    w = g_wpos; sk = g_skips; lis = g_listeners; pk = g_peak;
    snprintf(st, sizeof st, "%s", g_cap_state);
    pthread_mutex_unlock(&g_mu);
    diag_json(dj, sizeof dj);
    server_url(url, sizeof url);
    snprintf(body, sizeof body,
             "{\"app\":\"" HB_APP_NAME "\",\"id\":\"" HB_APP_ID "\",\"version\":\"" PSS_VERSION "\",\"url\":\"%s\",\"uptime_s\":%ld,"
             "\"format\":\"s16le\",\"rate\":%d,\"channels\":%d,"
             "\"capture\":\"%s\",\"peak\":%.4f,\"seconds_captured\":%.1f,"
             "\"listeners\":%d,\"lag_skips\":%llu,\"diag\":{%s}}",
             url, (now_ms() - g_t0) / 1000, PSS_RATE, PSS_CHANNELS, st, (double)pk,
             (double)w / (PSS_RATE * BYTES_PER_FRAME), lis, (unsigned long long)sk, dj);
    reply(c, "200 OK", "application/json", body, (int)strlen(body), NULL);
}

static void wav_header(uint8_t *h)
{
    const uint32_t rate = PSS_RATE, ch = PSS_CHANNELS, bps = 16;
    const uint32_t br = rate * ch * bps / 8;
    memcpy(h, "RIFF", 4); put32(h + 4, 0xFFFFFFFFu);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, ch); put32(h + 24, rate); put32(h + 28, br);
    put16(h + 32, ch * bps / 8); put16(h + 34, bps);
    memcpy(h + 36, "data", 4); put32(h + 40, 0xFFFFFFFFu - 36);
}

static void handle_request(client_t *c)
{
    char method[8] = "", path[256] = "", up[32], key[64];
    char *q;

    if (sscanf(c->in, "%7s %255s", method, path) != 2) {
        reply(c, "400 Bad Request", "text/plain", "bad request\n", 12, NULL);
        return;
    }
    if ((q = strchr(path, '?'))) *q = 0;

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ws") == 0 &&
        header(c->in, "Upgrade", up, sizeof up) && strcasecmp(up, "websocket") == 0 &&
        header(c->in, "Sec-WebSocket-Key", key, sizeof key)) {
        static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        char cat[128], acc[40], h[256], hello[256];
        uint8_t dg[20];
        int n;
        snprintf(cat, sizeof cat, "%s%s", key, guid);
        sha1(cat, strlen(cat), dg);
        base64_enc(dg, 20, acc);
        n = snprintf(h, sizeof h,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        out_append(c, h, n);
        n = snprintf(hello, sizeof hello,
                     "{\"type\":\"hello\",\"format\":\"s16le\",\"rate\":%d,\"channels\":%d,"
                     "\"q\":\"pcm48\",\"qualities\":[\"pcm48\",\"pcm24\",\"ulaw48\",\"ulaw24\"],\"low\":true}",
                     PSS_RATE, PSS_CHANNELS);
        ws_send(c, 1, hello, (size_t)n);
        c->inlen = 0;
        become_listener(c, K_WS);
        return;
    }
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/live.wav") == 0 || strcmp(path, "/live") == 0)) {
        static const char h[] =
            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        uint8_t wh[44];
        wav_header(wh);
        out_append(c, h, (int)sizeof h - 1);
        out_append(c, wh, 44);
        c->inlen = 0;
        become_listener(c, K_WAV);
        return;
    }
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        reply(c, "200 OK", "text/html; charset=utf-8", PAGE_HTML, (int)PAGE_HTML_LEN, NULL);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) { status_json(c); return; }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/lame.min.js") == 0) {
        reply(c, "200 OK", "application/javascript; charset=utf-8", LAME_JS, (int)LAME_JS_LEN, NULL);
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/stop") == 0) {
        log_line("http: stop requested by %s", c->peer);
        g_stopreq = 1;
        reply(c, "200 OK", "application/json", "{\"ok\":true}", 11, NULL);
        return;
    }
    if (strcmp(path, "/favicon.ico") == 0) { reply(c, "204 No Content", "text/plain", "", 0, NULL); return; }
    reply(c, "404 Not Found", "text/plain", "not found\n", 10, NULL);
}

/* Tell the listener how the next binary frames are encoded. */
static void send_format(client_t *c)
{
    char m[192];
    int dec = c->q == Q_PCM24 || c->q == Q_ULAW24, ul = c->q >= Q_ULAW48;
    int n = snprintf(m, sizeof m,
                     "{\"type\":\"format\",\"q\":\"%s\",\"codec\":\"%s\",\"rate\":%d,"
                     "\"channels\":%d,\"bits\":%d,\"low\":%s}",
                     Q_NAME[c->q], ul ? "mulaw" : "s16le", dec ? PSS_RATE / 2 : PSS_RATE,
                     PSS_CHANNELS, ul ? 8 : 16, c->low ? "true" : "false");
    ws_send(c, 1, m, (size_t)n);
}

/* Text control from the page: {"q":"pcm24","low":1}. Either key optional. */
static void ws_control(client_t *c, const char *p, int n)
{
    char t[256];
    const char *v;
    int i, changed = 0;
    if (n <= 0) return;
    if (n >= (int)sizeof t) n = (int)sizeof t - 1;
    memcpy(t, p, (size_t)n);
    t[n] = 0;
    if ((v = strstr(t, "\"q\""))) {
        for (i = 0; i < Q_COUNT; i++) {
            char want[24];
            snprintf(want, sizeof want, "\"%s\"", Q_NAME[i]);
            if (strstr(v, want)) {
                if (c->q != i) {
                    c->q = i;
                    memset(c->hist, 0, sizeof c->hist);
                    c->hpos = c->phase = 0;
                }
                changed = 1;
                break;
            }
        }
    }
    if ((v = strstr(t, "\"low\""))) {
        v += 5;
        while (*v == ' ' || *v == ':') v++;
        c->low = (*v == '1' || *v == 't');
        changed = 1;
    }
    if (changed) {
        log_line("http: %s quality %s%s", c->peer, Q_NAME[c->q], c->low ? " low-latency" : "");
        send_format(c);
    }
}

/* Parse client->server WebSocket frames (masked). Returns -1 to close. */
static int ws_input(client_t *c)
{
    while (c->inlen >= 2) {
        uint8_t *b = (uint8_t *)c->in;
        int op = b[0] & 0x0F, masked = b[1] & 0x80, hl = 2, i;
        uint64_t len = b[1] & 0x7F;
        if (len == 126) { if (c->inlen < 4) return 0; len = (uint64_t)b[2] << 8 | b[3]; hl = 4; }
        else if (len == 127) return -1;           /* we never expect huge client frames */
        if (masked) hl += 4;
        if (len > IN_MAX - 14) return -1;
        if ((uint64_t)c->inlen < hl + len) return 0;
        if (masked) for (i = 0; i < (int)len; i++) b[hl + i] ^= b[hl - 4 + (i & 3)];
        if (op == 8) { ws_send(c, 8, b + hl, len >= 2 ? 2 : 0); c->close_after = 1; }
        else if (op == 1) ws_control(c, (const char *)b + hl, (int)len);
        else if (op == 9) ws_send(c, 10, b + hl, (size_t)len);
        memmove(c->in, c->in + hl + len, (size_t)(c->inlen - hl - (int)len));
        c->inlen -= hl + (int)len;
    }
    return 0;
}

static int on_readable(client_t *c)
{
    int n = (int)recv(c->fd, c->in + c->inlen, (size_t)(IN_MAX - 1 - c->inlen), 0);
    if (n == 0) return -1;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    c->inlen += n;
    c->in[c->inlen] = 0;
    if (c->kind == K_HTTP) {
        if (c->close_after) { c->inlen = 0; return 0; }
        if (strstr(c->in, "\r\n\r\n")) handle_request(c);
        else if (c->inlen >= IN_MAX - 1) return -1;
    } else if (c->kind == K_WS) {
        return ws_input(c);
    } else {
        c->inlen = 0;                              /* WAV: ignore */
    }
    return 0;
}

/* Queue the next audio chunk for a listener if nothing is pending. */
static uint8_t ulaw_enc(int s)
{
    int sign = 0, exp = 7, mask, man;
    if (s < 0) { sign = 0x80; s = -s; }
    if (s > 32635) s = 32635;
    s += 0x84;
    for (mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1) exp--;
    man = (s >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | man);
}

/* 2:1 half-band decimation of interleaved s16 stereo, in place-safe
 * (dst may equal src: we only ever write behind the read position). */
static int decimate(client_t *c, const int16_t *src, int frames, int16_t *dst)
{
    int i, ch, k, o = 0;
    for (i = 0; i < frames; i++) {
        for (ch = 0; ch < PSS_CHANNELS; ch++) {
            float x = (float)src[i * PSS_CHANNELS + ch];
            c->hist[ch][c->hpos] = x;
            c->hist[ch][c->hpos + HB_TAPS] = x;
        }
        c->hpos = (c->hpos + 1) % HB_TAPS;
        c->phase ^= 1;
        if (c->phase) continue;
        for (ch = 0; ch < PSS_CHANNELS; ch++) {
            const float *h = &c->hist[ch][c->hpos];   /* oldest .. newest */
            float acc = 0.f, v;
            for (k = 0; k < HB_TAPS; k++) acc += HB_COEF[k] * h[k];
            v = acc > 32767.f ? 32767.f : acc < -32768.f ? -32768.f : acc;
            dst[o * PSS_CHANNELS + ch] = (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
        }
        o++;
    }
    return o;
}

static void feed(client_t *c)
{
    static uint8_t buf[MAX_CHUNK];
    int16_t *s = (int16_t *)(void *)buf;
    int n, frames, i;
    if (c->outoff < c->outlen || c->close_after) return;
    n = ring_read(&c->rpos, buf, MAX_CHUNK, c->kind == K_WS && c->low);
    if (n <= 0) return;
    if (c->kind != K_WS) { out_append(c, buf, n); return; }
    frames = n / BYTES_PER_FRAME;
    if (c->q == Q_PCM24 || c->q == Q_ULAW24) {
        frames = decimate(c, s, frames, s);
        n = frames * BYTES_PER_FRAME;
        if (n <= 0) return;
    }
    if (c->q >= Q_ULAW48) {
        for (i = 0; i < frames * PSS_CHANNELS; i++) buf[i] = ulaw_enc(s[i]);  /* writes behind reads */
        n = frames * PSS_CHANNELS;
    }
    ws_send(c, 2, buf, (size_t)n);
}

static int on_writable(client_t *c)
{
    while (c->outoff < c->outlen) {
        int w = (int)send(c->fd, c->out + c->outoff, (size_t)(c->outlen - c->outoff), MSG_NOSIGNAL);
        if (w < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        if (w == 0) return -1;
        c->outoff += w;
    }
    c->outoff = c->outlen = 0;
    if (c->close_after) return -1;
    return 0;
}

static void accept_one(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(g_srv, (struct sockaddr *)&a, &al), i, one = 1;
    if (fd < 0) return;
    for (i = 0; i < MAX_CLIENTS; i++) if (g_cl[i].kind == K_FREE) break;
    if (i == MAX_CLIENTS) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    memset(&g_cl[i], 0, sizeof g_cl[i]);
    g_cl[i].fd = fd;
    g_cl[i].kind = K_HTTP;
    g_cl[i].t_open = now_ms();
    inet_ntop(AF_INET, &a.sin_addr, g_cl[i].peer, sizeof g_cl[i].peer);
}

static void *srv_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        fd_set rs, ws;
        struct timeval tv = { 0, 200000 };
        int i, mx = g_srv > g_wake[0] ? g_srv : g_wake[0];
        FD_ZERO(&rs); FD_ZERO(&ws);
        FD_SET(g_srv, &rs);
        FD_SET(g_wake[0], &rs);
        for (i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_cl[i];
            if (c->kind == K_FREE) continue;
            if (c->kind == K_HTTP && now_ms() - c->t_open > 10000) { client_close(c); continue; }
            if (c->kind == K_WS || c->kind == K_WAV) feed(c);
            FD_SET(c->fd, &rs);
            if (c->outoff < c->outlen) FD_SET(c->fd, &ws);
            if (c->fd > mx) mx = c->fd;
        }
        if (select(mx + 1, &rs, &ws, NULL, &tv) < 0) { usleep(1000); continue; }
        if (FD_ISSET(g_wake[0], &rs)) { char d[256]; while (read(g_wake[0], d, sizeof d) > 0) {} }
        if (FD_ISSET(g_srv, &rs)) accept_one();
        for (i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &g_cl[i];
            if (c->kind == K_FREE) continue;
            if (FD_ISSET(c->fd, &rs) && on_readable(c) < 0) { client_close(c); continue; }
            if (c->outoff < c->outlen || c->close_after) {
                if (on_writable(c) < 0) { client_close(c); continue; }
            }
        }
    }
    return NULL;
}

int server_start(int port, int span)
{
    int p, one = 1;
    g_t0 = now_ms();
    if (pipe(g_wake) != 0) return 0;
    fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(g_wake[1], F_SETFL, O_NONBLOCK);
    for (p = port; p < port + span; p++) {
        struct sockaddr_in a;
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return 0;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)p);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0) {
            g_srv = s;
            g_port = p;
            break;
        }
        log_line("http: port %d unavailable (errno %d)", p, errno);
        close(s);
    }
    if (g_srv < 0) return 0;
    fcntl(g_srv, F_SETFL, fcntl(g_srv, F_GETFL, 0) | O_NONBLOCK);
    g_quit = 0;
    if (pthread_create(&g_thr, NULL, srv_main, NULL) != 0) {
        close(g_srv);
        g_srv = -1;
        return 0;
    }
    return g_port;
}

void server_stop(void)
{
    int i;
    if (g_srv < 0) return;
    g_quit = 1;
    pthread_join(g_thr, NULL);
    for (i = 0; i < MAX_CLIENTS; i++) if (g_cl[i].kind != K_FREE) client_close(&g_cl[i]);
    close(g_srv);
    g_srv = -1;
}
