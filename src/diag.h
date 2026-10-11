/* diag.h - minimal stand-in for HearBridge's diag.{c,h} so avcap2.c compiles
 * unchanged. Keeps the last value per key (shown at /status) and mirrors
 * every set to the log. Thread-safe. */
#ifndef PSS_DIAG_H
#define PSS_DIAG_H

#include <stddef.h>

void diag_set(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  diag_save(void);
/* JSON object body ("k":"v",...) without braces. Returns length. */
int  diag_json(char *out, size_t cap);

#endif
