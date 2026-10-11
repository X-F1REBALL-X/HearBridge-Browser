#ifndef PSS_NOTIFY_H
#define PSS_NOTIFY_H
/* On-screen toast (PS5); stderr on the host build. */
void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
