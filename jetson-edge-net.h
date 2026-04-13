/**
 * jetson-edge-net.h — Edge network resilience for Jetson
 *
 * Jetsons on cellular/Ethernet with spotty connectivity need:
 * - DNS retry with backoff (DNS fails ~5x/day on this Jetson)
 * - Interface failover (eth0 → wlan0 → wwan0)
 * - Request timeout with automatic retry
 *
 * Cloud agents have redundant datacenter networking.
 * Edge agents do not. This is the difference.
 */

#ifndef JETSON_EDGE_NET_H
#define JETSON_EDGE_NET_H

#include <stdint.h>
#include <stdbool.h>

#define JT_NET_MAX_IFACES    8
#define JT_NET_MAX_HISTORY   32
#define JT_NET_DNS_RETRIES   2
#define JT_NET_DNS_BACKOFF_S 5
#define JT_NET_DEFAULT_TIMEOUT_MS 30000

typedef struct {
    char     name[32];       /* eth0, wlan0, wwan0 */
    char     path[64];       /* /sys/class/net/eth0 */
    bool     up;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} JTNetIface;

typedef struct {
    JTNetIface ifaces[JT_NET_MAX_IFACES];
    int        iface_count;
    int        active_iface;       /* index of current primary */

    /* DNS tracking */
    uint32_t dns_attempts;
    uint32_t dns_successes;
    uint32_t dns_failures;
    uint64_t last_dns_success;     /* epoch seconds */
    uint64_t last_dns_failure;
    uint32_t consecutive_dns_fails;

    /* Request tracking */
    uint32_t requests_sent;
    uint32_t requests_ok;
    uint32_t requests_timeout;
    uint32_t requests_error;

    /* History */
    struct {
        uint32_t timestamp;
        bool     success;
        uint32_t latency_ms;
        char     host[128];
    } history[JT_NET_MAX_HISTORY];
    int      history_count;
} JTNetState;

/* ═══ API ═══ */

/** Probe network interfaces */
int jt_net_probe(JTNetState *state);

/** Refresh interface status */
int jt_net_refresh(JTNetState *state);

/** Test DNS resolution with retries */
int jt_net_dns_resolve(const char *domain, char *ip_out, int max_len,
                       uint32_t *latency_ms);

/** Test if any interface is up */
bool jt_net_any_up(const JTNetState *state);

/** Get best available interface */
int jt_net_best_iface(const JTNetState *state);

/** Simple HTTP GET with timeout and retry */
int jt_net_http_get(const char *url, char *response, int max_len,
                    uint32_t timeout_ms, int retries);

/** Get connectivity status: 0=connected, 1=degraded, 2=offline */
int jt_net_status(const JTNetState *state);

/** Report */
int jt_net_report(const JTNetState *state, char *out, int max_len);

/** Test */
int jt_edge_net_test(void);

#endif
