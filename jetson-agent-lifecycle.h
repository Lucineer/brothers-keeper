/**
 * jetson-agent-lifecycle.h — Agent process lifecycle management
 *
 * Spawns, monitors, and recovers agent processes on Jetson.
 * Uses hardware watchdog (/dev/watchdog0) when available.
 * Checkpoints agent state to local disk for crash recovery.
 *
 * Cloud agents have Kubernetes. Jetson agents have this.
 */

#ifndef JETSON_AGENT_LIFECYCLE_H
#define JETSON_AGENT_LIFECYCLE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define JT_LC_MAX_AGENTS    16
#define JT_LC_MAX_CHECKPOINTS 8
#define JT_LC_CHECKPOINT_DIR "/home/lucineer/.local/share/brothers-keeper/checkpoints"
#define JT_LC_WATCHDOG_DEV   "/dev/watchdog0"
#define JT_LC_MAX_CMD_LEN    256
#define JT_LC_MAX_NAME_LEN   32
#define JT_LC_DEFAULT_TIMEOUT 60   /* seconds before agent is considered stuck */
#define JT_LC_MAX_RESTARTS   5     /* max restarts before giving up */

typedef enum {
    JT_AGENT_STOPPED,
    JT_AGENT_STARTING,
    JT_AGENT_RUNNING,
    JT_AGENT_STUCK,
    JT_AGENT_CRASHED,
    JT_AGENT_KILLED
} JTAgentState;

typedef struct {
    char        name[JT_LC_MAX_NAME_LEN];
    char        command[JT_LC_MAX_CMD_LEN];
    pid_t       pid;
    JTAgentState state;
    uint32_t    restarts;
    uint64_t    start_time;      /* epoch seconds */
    uint64_t    last_heartbeat;  /* epoch seconds */
    uint32_t    timeout_sec;
    uint32_t    cpu_time_ms;     /* cumulative CPU time */
    uint64_t    rss_kb;          /* peak RSS */
    char        checkpoint_file[256];
} JTAgent;

typedef struct {
    JTAgent agents[JT_LC_MAX_AGENTS];
    int     agent_count;

    /* Hardware watchdog */
    int     wdt_fd;
    bool    wdt_available;
    uint32_t wdt_timeout_sec;

    /* Stats */
    uint32_t total_restarts;
    uint32_t total_kills;
    uint32_t total_checkpoints;
    uint64_t uptime_start;
} JTAgentManager;

/* ═══ API ═══ */

/** Init manager, try to open hardware watchdog */
int jt_lc_init(JTAgentManager *mgr);

/** Register an agent (doesn't start it) */
int jt_lc_register(JTAgentManager *mgr, const char *name,
                   const char *command, uint32_t timeout_sec);

/** Start a registered agent */
int jt_lc_start(JTAgentManager *mgr, const char *name);

/** Stop an agent gracefully, then SIGKILL */
int jt_lc_stop(JTAgentManager *mgr, const char *name);

/** Stop all agents */
int jt_lc_stop_all(JTAgentManager *mgr);

/** Check all agents — detect crashes, stuck processes */
int jt_lc_check(JTAgentManager *mgr);

/** Restart crashed/stuck agents */
int jt_lc_recover(JTAgentManager *mgr);

/** Ping hardware watchdog */
int jt_lc_watchdog_ping(JTAgentManager *mgr);

/** Save agent state checkpoint */
int jt_lc_checkpoint(JTAgentManager *mgr, const char *name);

/** Get agent by name */
JTAgent *jt_lc_find(JTAgentManager *mgr, const char *name);

/** Report */
int jt_lc_report(JTAgentManager *mgr, char *out, int max_len);

/** Cleanup */
int jt_lc_cleanup(JTAgentManager *mgr);

/** Test */
int jt_agent_lifecycle_test(void);

#endif
