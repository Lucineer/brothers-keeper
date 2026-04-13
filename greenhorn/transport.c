/*
 * Greenhorn Transport — HTTP (raw sockets) and Serial (POSIX termios)
 */
#include "transport.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

/* === HTTP Transport === */

int gh_http_init(GHTransportHTTP *t, const char *host, uint16_t port) {
    memset(t, 0, sizeof(*t));
    t->host = host;
    t->port = port;
    t->connected = 0;
    return 0;
}

static int http_connect(GHTransportHTTP *t) {
    struct hostent *he = gethostbyname(t->host);
    if (!he) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(t->port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    t->connected = 1;
    return fd;
}

static int http_request(GHTransportHTTP *t, const char *method, const char *path,
                        const char *body, char *buf, uint32_t bufsize) {
    int fd = http_connect(t);
    if (fd < 0) return -1;

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n",
        method, path, t->host);
    if (body) {
        hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen,
            "Content-Type: application/octet-stream\r\nContent-Length: %zu\r\n",
            strlen(body));
    }
    hlen += snprintf(hdr + hlen, sizeof(hdr) - hlen, "\r\n");
    write(fd, hdr, hlen);
    if (body) write(fd, body, strlen(body));

    int total = 0;
    while (total < (int)bufsize - 1) {
        int n = read(fd, buf + total, bufsize - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(fd);

    /* Skip headers, find \r\n\r\n */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        uint32_t body_len = total - (uint32_t)(body_start - buf);
        memmove(buf, body_start, body_len);
        buf[body_len] = '\0';
        return (int)body_len;
    }
    return total;
}

int gh_http_get(GHTransportHTTP *t, const char *path, char *buf, uint32_t bufsize) {
    return http_request(t, "GET", path, NULL, buf, bufsize);
}

int gh_http_post(GHTransportHTTP *t, const char *path, const char *body, char *buf, uint32_t bufsize) {
    return http_request(t, "POST", path, body, buf, bufsize);
}

void gh_http_close(GHTransportHTTP *t) {
    t->connected = 0;
}

/* === Serial Transport === */

int gh_serial_init(GHTransportSerial *t, const char *device, uint32_t baud) {
    memset(t, 0, sizeof(*t));
    t->device = device;
    t->baud = baud;
    t->fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (t->fd < 0) return -1;

    struct termios opts;
    tcgetattr(t->fd, &opts);
    cfmakeraw(&opts);
    cfsetispeed(&opts, baud);
    cfsetospeed(&opts, baud);
    opts.c_cflag |= (CLOCAL | CREAD);
    opts.c_cc[VMIN] = 1;
    opts.c_cc[VTIME] = 0;
    tcsetattr(t->fd, TCSANOW, &opts);
    return 0;
}

int gh_serial_send(GHTransportSerial *t, const uint8_t *data, uint32_t len) {
    if (t->fd < 0) return -1;
    return (int)write(t->fd, data, len);
}

int gh_serial_recv(GHTransportSerial *t, uint8_t *buf, uint32_t bufsize) {
    if (t->fd < 0) return -1;
    return (int)read(t->fd, buf, bufsize);
}

void gh_serial_close(GHTransportSerial *t) {
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
}
