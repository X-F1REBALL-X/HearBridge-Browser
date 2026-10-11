/* notify.c - on-screen toast via sceKernelSendNotificationRequest (same
 * 3120-byte request as HearBridge-PS5 src/notify.c, GPL-3.0-or-later). */
#include "notify.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef PSS_HOST
void notify(const char *fmt, ...)
{
    va_list va;
    fputs("[notify] ", stderr);
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    fputc('\n', stderr);
}
#else
#define NOTIF_TEXT_OFFSET 45
#define NOTIF_TEXT_BYTES  3075

struct sce_notification {
    unsigned char head[NOTIF_TEXT_OFFSET];
    char text[NOTIF_TEXT_BYTES];
};

int sceKernelSendNotificationRequest(int, struct sce_notification *, size_t, int);

static struct sce_notification toast;

void notify(const char *fmt, ...)
{
    va_list va;
    memset(&toast, 0, sizeof toast);
    va_start(va, fmt);
    (void)vsnprintf(toast.text, NOTIF_TEXT_BYTES, fmt, va);
    va_end(va);
    (void)sceKernelSendNotificationRequest(0, &toast, sizeof(struct sce_notification), 0);
}
#endif
