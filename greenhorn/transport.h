/*
 * Greenhorn Transport — HTTP and Serial communication with keeper
 * Raw sockets only, no libcurl.
 */
#ifndef GREENHORN_TRANSPORT_H
#define GREENHORN_TRANSPORT_H

#include "vm.h"
#include <stdint.h>

/* HTTP transport — raw TCP sockets */
typedef struct {
    const char *host;
    uint16_t port;
    int connected;
} GHTransportHTTP;

int  gh_http_init(GHTransportHTTP *t, const char *host, uint16_t port);
int  gh_http_get(GHTransportHTTP *t, const char *path, char *buf, uint32_t bufsize);
int  gh_http_post(GHTransportHTTP *t, const char *path, const char *body, char *buf, uint32_t bufsize);
void gh_http_close(GHTransportHTTP *t);

/* Serial transport — for ESP32 */
typedef struct {
    const char *device;
    int fd;
    uint32_t baud;
} GHTransportSerial;

int  gh_serial_init(GHTransportSerial *t, const char *device, uint32_t baud);
int  gh_serial_send(GHTransportSerial *t, const uint8_t *data, uint32_t len);
int  gh_serial_recv(GHTransportSerial *t, uint8_t *buf, uint32_t bufsize);
void gh_serial_close(GHTransportSerial *t);

#endif
