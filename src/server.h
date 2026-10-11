/* server.h - tiny HTTP + WebSocket server for the PS-Stream audio MVP.
 *
 *   GET  /           mobile player page (Web Audio playback + recording)
 *   GET  /ws         WebSocket: one text "hello" JSON, then binary frames of
 *                    raw PCM s16le, 48 kHz, 2 ch interleaved (live edge)
 *   GET  /live.wav   endless WAV over plain HTTP (VLC / ffplay / <audio>)
 *   GET  /status     JSON status
 *   POST /api/stop   stop the payload
 *
 * One thread, select(), non-blocking sockets. PCM goes through a 2 s ring;
 * a client that falls >1 s behind is skipped forward to the live edge. */
#ifndef PSS_SERVER_H
#define PSS_SERVER_H

#include <stdint.h>

#define PSS_RATE     48000
#define PSS_CHANNELS 2

/* Bind the first free port in [port, port+span). Returns the port or 0. */
int  server_start(int port, int span);
void server_stop(void);

/* Append interleaved s16 stereo frames to the ring (capture thread). */
void server_push_pcm(const int16_t *pcm, int frames);

/* Number of clients currently listening (WebSocket or WAV). */
int  server_listeners(void);

/* 1 once a client POSTed /api/stop. */
int  server_stop_requested(void);

/* Capture status shown at /status (set by the capture thread). */
void server_set_capture(const char *state, float peak);

/* http://<ip>:<port> of the console. */
void server_url(char *out, int cap);

#endif
