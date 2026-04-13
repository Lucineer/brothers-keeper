/**
 * jetson-edge-net.c — Edge network resilience for Jetson
 *
 * Scans real interfaces, DNS retry with backoff, raw HTTP GET.
 */

#include "jetson-edge-net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int read_sysfs_str(const char *path, char *buf, int len) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, len, f)) { buf[0] = 0; fclose(f); return -1; }
    int l = strlen(buf);
    while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = 0;
    fclose(f);
    return l;
}

static int read_sysfs_uint(const char *path) {
    char buf[64];
    return (read_sysfs_str(path, buf, sizeof(buf)) > 0) ? (uint32_t)strtoul(buf, NULL, 10) : 0;
}

int jt_net_probe(JTNetState *state) {
    if (!state) return -1;
    memset(state, 0, sizeof(*state));

    DIR *nd = opendir("/sys/class/net");
    if (!nd) return -2;

    struct dirent *ent;
    while ((ent = readdir(nd)) && state->iface_count < JT_NET_MAX_IFACES) {
        if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) continue;

        char path[128];
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ent->d_name);
        if (access(path, F_OK) != 0) continue;

        JTNetIface *iface = &state->ifaces[state->iface_count++];
        strncpy(iface->name, ent->d_name, sizeof(iface->name) - 1);
        snprintf(iface->path, sizeof(iface->path), "/sys/class/net/%s", ent->d_name);

        char opstate[16];
        iface->up = (read_sysfs_str(path, opstate, sizeof(opstate)) > 0 &&
                     strcmp(opstate, "up") == 0);

        snprintf(path, sizeof(path), "%s/statistics/rx_bytes", iface->path);
        iface->rx_bytes = read_sysfs_uint(path);
        snprintf(path, sizeof(path), "%s/statistics/tx_bytes", iface->path);
        iface->tx_bytes = read_sysfs_uint(path);
    }
    closedir(nd);

    state->active_iface = -1;
    for (int i = 0; i < state->iface_count; i++) {
        if (state->ifaces[i].up) { state->active_iface = i; break; }
    }

    return jt_net_refresh(state);
}

int jt_net_refresh(JTNetState *state) {
    if (!state) return -1;
    for (int i = 0; i < state->iface_count; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%s/operstate", state->ifaces[i].path);
        char opstate[16];
        state->ifaces[i].up = (read_sysfs_str(path, opstate, sizeof(opstate)) > 0 &&
                               strcmp(opstate, "up") == 0);
    }
    if (state->active_iface >= 0 && !state->ifaces[state->active_iface].up) {
        state->active_iface = -1;
        for (int i = 0; i < state->iface_count; i++)
            if (state->ifaces[i].up) { state->active_iface = i; break; }
    }
    return 0;
}

int jt_net_dns_resolve(const char *domain, char *ip_out, int max_len,
                       uint32_t *latency_ms) {
    if (!domain || !ip_out) return -1;
    if (latency_ms) *latency_ms = 0;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    for (int attempt = 0; attempt < JT_NET_DNS_RETRIES; attempt++) {
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        int rc = getaddrinfo(domain, NULL, &hints, &res);

        if (rc == 0 && res) {
            struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
            inet_ntop(AF_INET, &addr->sin_addr, ip_out, max_len);
            freeaddrinfo(res);
            gettimeofday(&t1, NULL);
            if (latency_ms) *latency_ms = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000 +
                                                      (t1.tv_usec - t0.tv_usec) / 1000);
            return 0;
        }
        if (res) freeaddrinfo(res);
        if (attempt < JT_NET_DNS_RETRIES - 1) sleep(JT_NET_DNS_BACKOFF_S);
    }
    return -2;
}

bool jt_net_any_up(const JTNetState *state) {
    if (!state) return false;
    for (int i = 0; i < state->iface_count; i++)
        if (state->ifaces[i].up) return true;
    return false;
}

int jt_net_best_iface(const JTNetState *state) {
    if (!state) return -1;
    if (state->active_iface >= 0 && state->active_iface < state->iface_count) return state->active_iface;
    for (int i = 0; i < state->iface_count; i++)
        if (state->ifaces[i].up) return i;
    return -1;
}

static int http_get_raw(const char *host, int port, const char *path,
                        char *response, int max_len, uint32_t timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) { close(sock); return -2; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return -3; }

    char req[2048];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    send(sock, req, strlen(req), 0);

    int total = 0;
    while (total < max_len - 1) {
        int n = recv(sock, response + total, max_len - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    response[total] = 0;
    close(sock);

    char *body = strstr(response, "\r\n\r\n");
    if (!body) return -4;
    body += 4;
    memmove(response, body, strlen(body) + 1);
    return 0;
}

int jt_net_http_get(const char *url, char *response, int max_len,
                    uint32_t timeout_ms, int retries) {
    if (!url || !response) return -1;

    char host[256] = "127.0.0.1";
    char path[1024] = "/";
    int port = 80, use_ssl = 0;
    const char *start = url;

    if (strncmp(url, "https://", 8) == 0) { start = url + 8; use_ssl = 1; port = 443; }
    else if (strncmp(url, "http://", 7) == 0) { start = url + 7; }

    const char *slash = strchr(start, '/');
    const char *host_end = slash ? slash : start + strlen(start);
    int hlen = host_end - start;
    if (hlen > 0 && hlen < (int)sizeof(host)) {
        memcpy(host, start, hlen);
        host[hlen] = 0;
        char *colon = strchr(host, ':');
        if (colon) { port = atoi(colon + 1); *colon = 0; }
    }
    if (slash) strncpy(path, slash, sizeof(path) - 1);
    if (use_ssl) return -5;

    for (int attempt = 0; attempt <= retries; attempt++) {
        int rc = http_get_raw(host, port, path, response, max_len, timeout_ms);
        if (rc == 0) return 0;
        if (attempt < retries) sleep(1);
    }
    return -3;
}

int jt_net_status(const JTNetState *state) {
    if (!state) return 2;
    if (!jt_net_any_up(state)) return 2;
    if (state->consecutive_dns_fails > 3) return 1;
    return 0;
}

int jt_net_report(const JTNetState *state, char *out, int max_len) {
    if (!state || !out) return -1;
    const char *sl[] = {"CONNECTED", "DEGRADED", "OFFLINE"};
    int pos = snprintf(out, max_len, "# Jetson Network Report\n\nStatus: %s\n\n",
                       sl[jt_net_status(state)]);
    for (int i = 0; i < state->iface_count && pos < max_len - 64; i++) {
        pos += snprintf(out + pos, max_len - pos,
            "- %s: %s (RX:%uKB TX:%uKB)\n",
            state->ifaces[i].name, state->ifaces[i].up ? "UP" : "DOWN",
            state->ifaces[i].rx_bytes / 1024, state->ifaces[i].tx_bytes / 1024);
    }
    pos += snprintf(out + pos, max_len - pos,
        "\nDNS: %u/%u ok\nRequests: %u/%u ok\n",
        state->dns_successes, state->dns_attempts,
        state->requests_ok, state->requests_sent);
    return pos;
}

int jt_edge_net_test(void) {
    int failures = 0;

    /* Test 1: Probe */
    JTNetState state;
    int rc = jt_net_probe(&state);
    if (rc != 0) { failures++; printf("FAIL probe: %d\n", rc); }
    else printf("  Probed: %d interfaces\n", state.iface_count);

    /* Test 2: Show interfaces */
    for (int i = 0; i < state.iface_count; i++) {
        printf("  %s: %s (RX:%uKB TX:%uKB)\n",
               state.ifaces[i].name, state.ifaces[i].up ? "UP" : "DOWN",
               state.ifaces[i].rx_bytes / 1024, state.ifaces[i].tx_bytes / 1024);
    }

    /* Test 3: Any up */
    bool up = jt_net_any_up(&state);
    printf("  Any up: %s\n", up ? "yes" : "no");

    /* Test 4: Best iface */
    int best = jt_net_best_iface(&state);
    printf("  Best: %s\n", best >= 0 ? state.ifaces[best].name : "none");

    /* Test 5: DNS resolve (1 retry for fast test) */
    char ip[64];
    uint32_t dns_ms = 0;
    rc = jt_net_dns_resolve("api.deepseek.com", ip, sizeof(ip), &dns_ms);
    if (rc == 0) printf("  DNS ok: %s (%u ms)\n", ip, dns_ms);
    else printf("  DNS: failed (%d, spotty net ok)\n", rc);

    /* Test 6: Status */
    const char *labels[] = {"CONNECTED","DEGRADED","OFFLINE"};
    printf("  Status: %s\n", labels[jt_net_status(&state)]);

    /* Test 7: Refresh */
    rc = jt_net_refresh(&state);
    if (rc != 0) { failures++; printf("FAIL refresh\n"); }
    else printf("  Refresh: OK\n");

    /* Test 8: Report */
    char report[1024];
    rc = jt_net_report(&state, report, sizeof(report));
    if (rc <= 0) { failures++; printf("FAIL report\n"); }
    else printf("  Report: %d bytes\n", rc);

    /* Test 9: Null safety */
    if (jt_net_probe(NULL) != -1) { failures++; printf("FAIL null probe\n"); }
    if (jt_net_any_up(NULL)) { failures++; printf("FAIL null any_up\n"); }
    printf("  Null safety: OK\n");

    /* Test 10: Zero interfaces */
    JTNetState empty;
    memset(&empty, 0, sizeof(empty));
    if (jt_net_any_up(&empty)) { failures++; printf("FAIL empty any_up\n"); }
    if (jt_net_best_iface(&empty) != -1) { failures++; printf("FAIL empty best\n"); }
    if (jt_net_status(&empty) != 2) { failures++; printf("FAIL empty status\n"); }
    printf("  Empty state: OK\n");

    printf("jt_edge_net_test: %d failures\n", failures);
    return failures;
}
