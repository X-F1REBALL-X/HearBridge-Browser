/* HearBridge Browser — PS5 payload (from the PS-Stream audio MVP).
 *
 * Captures the console's system audio with libSceAvcap2 (the HearBridge-PS5
 * path: OpenAudio source=2 "Remote Play mix", 48 kHz) and serves it on the
 * LAN at http://<console-ip>:8096/ as raw PCM over WebSocket (+ endless WAV
 * at /live.wav). No video. Runs entirely in its own process: no ShellCore /
 * ShellUI injection, no kernel writes besides the payload's own authid
 * (same elevation HearBridge uses, see creds.c / avcap2.c).
 *
 * Copyright 2026 X-F1REBALL-X. GPL-3.0-or-later (reuses HearBridge-PS5 code).
 */
#include "avcap2.h"
#include "log.h"
#include "notify.h"
#include "server.h"
#include "util.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef PSS_STATE_DIR
#define PSS_STATE_DIR "/data/hearbridge-browser"
#endif
#define LOG_PATH  PSS_STATE_DIR "/audio.log"
#define STOP_PATH PSS_STATE_DIR "/stop-audio"
#define PORT      8096

#define IDLE_CLOSE_MS 15000   /* close the capture this long after the last listener */
#define RETRY_MS      5000

static volatile sig_atomic_t g_sig;
static volatile int g_quit;

static void on_sig(int s) { (void)s; g_sig = 1; }

/* If our own older copy holds the port, ask it to stop (POST /api/stop).
 * Only does so when GET /status says HearBridge Browser or the older
 * "ps-stream-audio" / hb-browser step builds. */
static int http_local(const char *req, char *resp, int cap)
{
    struct sockaddr_in a;
    struct timeval tv = { 2, 0 };
    int s = socket(AF_INET, SOCK_STREAM, 0), n = 0, r;
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    if (send(s, req, strlen(req), 0) < 0) { close(s); return -1; }
    while (n < cap - 1 && (r = (int)recv(s, resp + n, (size_t)(cap - 1 - n), 0)) > 0) n += r;
    resp[n] = 0;
    close(s);
    return n;
}

static void stop_old_copy(void)
{
    char resp[2048];
    int i;
    if (http_local("GET /status HTTP/1.0\r\n\r\n", resp, sizeof resp) <= 0) return;
    if (!strstr(resp, "\"" HB_APP_NAME "\"") && !strstr(resp, "\"ps-stream-audio\"")) {
        log_line("main: port %d is used by something else; not touching it", PORT);
        return;
    }
    log_line("main: an older HearBridge Browser / ps-stream-audio holds port %d; asking it to stop", PORT);
    (void)http_local("POST /api/stop HTTP/1.0\r\nContent-Length: 0\r\n\r\n", resp, sizeof resp);
    for (i = 0; i < 30; i++) {           /* up to ~3 s for it to release the port */
        usleep(100000);
        if (http_local("GET /status HTTP/1.0\r\n\r\n", resp, sizeof resp) <= 0) break;
    }
    usleep(300000);
}

static void *capture_main(void *arg)
{
    static int16_t pcm[2048 * 2];
    avcap2_session *cap = NULL;
    long idle_since = 0, last_fail = -RETRY_MS, last_peak = 0, opens = 0;
    float peak_win = 0.f;
    char st[96];
    (void)arg;

    while (!g_quit) {
        long now = now_ms();
        int lis = server_listeners(), n;
        float peak = 0.f;

        if (!cap) {
            if (lis == 0) { usleep(50000); continue; }
            if (now - last_fail < RETRY_MS) { usleep(100000); continue; }
            server_set_capture("opening libSceAvcap2 audio", 0.f);
            log_line("capture: opening (listener connected)");
            cap = avcap2_session_open();
            if (!cap) {
                last_fail = now_ms();
                server_set_capture("open FAILED (see diag); retrying every 5 s", 0.f);
                log_line("capture: open failed");
                continue;
            }
            opens++;
            snprintf(st, sizeof st, "capturing (open #%ld)", opens);
            server_set_capture(st, 0.f);
            idle_since = 0;
        }
        if (lis == 0) {
            if (!idle_since) idle_since = now;
            else if (now - idle_since > IDLE_CLOSE_MS) {
                log_line("capture: no listeners for %d s, closing", IDLE_CLOSE_MS / 1000);
                avcap2_session_close(cap);
                cap = NULL;
                server_set_capture("idle (waiting for a listener)", 0.f);
                continue;
            }
        } else idle_since = 0;

        n = avcap2_session_read_s16(cap, pcm, 2048, &peak);
        if (n > 0) {
            server_push_pcm(pcm, n);
            if (peak > peak_win) peak_win = peak;
            if (now - last_peak >= 250) {
                server_set_capture(NULL, peak_win);
                peak_win = 0.f;
                last_peak = now;
            }
        } else if (n == 0) {
            usleep(2000);
        } else {
            log_line("capture: session ended; will reopen");
            avcap2_session_close(cap);
            cap = NULL;
            last_fail = now_ms();
            server_set_capture("capture ended; reopening in 5 s", 0.f);
        }
    }
    if (cap) avcap2_session_close(cap);
    return NULL;
}

int main(void)
{
    pthread_t thr;
    char url[64];
    int port;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);

    notify(HB_APP_NAME " %s: starting", PSS_VERSION);
    if (mkdir(PSS_STATE_DIR, 0755) != 0 && errno != EEXIST) { /* log goes to stderr */ }
    (void)log_open(LOG_PATH);
    unlink(STOP_PATH);
    log_line(HB_APP_NAME " %s (built %s)", PSS_VERSION, __DATE__);

    port = server_start(PORT, 1);
    if (!port) {
        stop_old_copy();
        port = server_start(PORT, 1);
    }
    if (!port) port = server_start(PORT + 1, 3);
    if (!port) {
        log_line("main: no free port in %d-%d", PORT, PORT + 3);
        notify(HB_APP_NAME ": could not open port %d", PORT);
        log_close();
        return 1;
    }
    server_url(url, sizeof url);
    log_line("main: player at %s/  (WebSocket %s/ws, WAV %s/live.wav)", url, url, url);
    notify(HB_APP_NAME ": open %s", url);

    if (pthread_create(&thr, NULL, capture_main, NULL) != 0) {
        log_line("main: capture thread failed");
        server_stop();
        return 1;
    }
    while (!g_sig && !server_stop_requested() && access(STOP_PATH, F_OK) != 0)
        usleep(200000);

    log_line("main: stopping (%s)", g_sig ? "signal" : server_stop_requested() ? "web stop" : "stop file");
    g_quit = 1;
    pthread_join(thr, NULL);
    server_stop();
    unlink(STOP_PATH);
    notify(HB_APP_NAME ": stopped");
    log_line("main: bye");
    log_close();
    return 0;
}
