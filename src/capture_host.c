/* capture_host.c - host-only stand-in for avcap2.c (never built for the PS5):
 * a real-time paced test signal with the same API.
 *   default         : test tone 440 Hz left / 660 Hz right, -12 dBFS
 *   HB_TEST=program : 8 s loop of game-like material (quiet ambience, speech,
 *                     music + kick, explosion) for loudness/boost tests. */
#include "avcap2.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct avcap2_session { long t0; long long frames; int program; uint32_t seed; double b0, b1, lp; };

static double prg_rand(avcap2_session *s)
{
    s->seed = s->seed * 1664525u + 1013904223u;
    return (double)s->seed / 4294967296.0 * 2.0 - 1.0;
}

static double db(double d) { return pow(10.0, d / 20.0); }

/* Same material as the boost prototype: RMS about -22 dBFS, peaks about -1 dBFS. */
static void program_frame(avcap2_session *s, long long i, int16_t *lr)
{
    double t = fmod((double)i / 48000.0, 8.0), w = prg_rand(s), x = 0, pink, l, r;
    s->b0 = 0.99 * s->b0 + 0.1 * w; s->b1 = 0.9 * s->b1 + 0.3 * w;
    pink = (s->b0 + s->b1 + 0.2 * w) * 0.5;
    s->lp += 0.15 * (w - s->lp);
    if (t < 2) x = pink * db(-36) + sin(2 * M_PI * 220 * i / 48000.0) * db(-30);
    else if (t < 4) {
        double syl = sin(2 * M_PI * 4 * t); syl = syl > 0 ? syl * syl : 0;
        x = s->lp * 2 * db(-12) * syl + sin(2 * M_PI * 180 * i / 48000.0) * syl * db(-18);
    } else if (t < 6) {
        double tn = fmod(t - 4, 0.5), env = exp(-tn * 5), k = exp(-tn * 18);
        const double f[4] = { 220, 277.2, 329.6, 440 };
        int j;
        for (j = 0; j < 4; j++) x += sin(2 * M_PI * f[j] * i / 48000.0) * env * db(-16);
        x += sin(2 * M_PI * 55 * tn * (1 + 2 * exp(-tn * 30))) * k * db(-6);
    } else {
        double tt = t - 6, env = tt < 0.01 ? tt / 0.01 : exp(-(tt - 0.01) * 2.5);
        x = (w * 0.5 + s->lp * 1.5) * env * db(-7);
    }
    l = x; r = x * 0.9 + pink * db(-40);
    l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
    lr[0] = (int16_t)(l * 32767); lr[1] = (int16_t)(r * 32767);
}

avcap2_session *avcap2_session_open(void)
{
    avcap2_session *s = calloc(1, sizeof *s);
    if (s) {
        const char *e = getenv("HB_TEST");
        s->t0 = now_ms();
        s->program = e && strcmp(e, "program") == 0;
        s->seed = 1;
    }
    return s;
}

void avcap2_session_close(avcap2_session *s) { free(s); }

int avcap2_probe(void) { return 0; }

int avcap2_session_read_s16(avcap2_session *s, int16_t *out, int max_frames, float *peak)
{
    long long due = (long long)(now_ms() - s->t0) * 48;
    int n, i;
    if (peak) *peak = 0.f;
    if (due - s->frames < 1024) { usleep(3000); return 0; }  /* ~21 ms packets like Avcap2 */
    n = 1024 < max_frames ? 1024 : max_frames;
    if (s->program) {
        for (i = 0; i < n; i++) program_frame(s, s->frames + i, out + 2 * i);
        s->frames += n;
        if (peak) *peak = 0.5f;
        return n;
    }
    for (i = 0; i < n; i++) {
        double t = (double)(s->frames + i) / 48000.0;
        out[2 * i]     = (int16_t)(8192 * sin(2 * M_PI * 440 * t));
        out[2 * i + 1] = (int16_t)(8192 * sin(2 * M_PI * 660 * t));
    }
    s->frames += n;
    if (peak) *peak = 0.25f;
    return n;
}
