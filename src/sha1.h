/* sha1.h - SHA-1 (FIPS 180-4), only for the WebSocket handshake (RFC 6455). */
#ifndef PSS_SHA1_H
#define PSS_SHA1_H
#include <stddef.h>
#include <stdint.h>
void sha1(const void *data, size_t len, uint8_t out[20]);
/* Standard base64 with padding; returns length written (out needs 4*ceil(n/3)+1). */
size_t base64_enc(const uint8_t *in, size_t n, char *out);
#endif
