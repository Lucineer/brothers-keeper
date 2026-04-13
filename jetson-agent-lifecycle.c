/**
 * jetson-agent-lifecycle.c — Agent process lifecycle on Jetson
 */

#include "jetson-agent-lifecycle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>

static uint64_t now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

static int mkdirp(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755);
}

static int read_proc_stat(pid_t pid, uint64_t *rss_kb, uint32_t *cpu_ms) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);

    /* Parse: pid (comm) state ppid pgrp session tty_nr tpgid flags ...
       We need utime (14th), stime (15th), rss (24th) — 1-indexed */
    long utime = 0, stime = 0, rss = 0;
    int field = 0;
    char *p = line;
    while (*p && field < 24) {
        if (field == 13) utime = strtol(p, &p, 10);
        else if (field == 14) stime = strtol(p, &p, 10);
        else if (field == 23) rss = strtol(p, &p, 10);
        else { while (*p && *p != ' ') p++; while (*p == ' ') p++; }
        field++;
    }

    if (rss_kb) *rss_kb = (uint64_t)rss * 4; /* pages to KB on 4K pages */
    if (cpu_ms) *cpu_ms = (uint32_t)((utime + stime) * 1000 / sysconf(_SC_CLK_TCK));
    return 0;
}

int jt_lc_init(JTAgentManager *mgr) {
    if (!mgr) return -1;
    memset(mgr, 0, sizeof(*mgr));
    mgr->wdt_fd = -1;
    mgr->uptime_start = now_sec();

    /* Try hardware watchdog */
    mgr->wdt_fd = open(JT_LC_WATCHDOG_DEV, O_WRONLY);
    if (mgr->wdt_fd >= 0) {
        mgr->wdt_available = true;
        mgr->wdt_timeout_sec = 30;
        ioctl(mgr->wdt_fd, WDIOC_SETTIMEOUT, &mgr->wdt_timeout_sec);
    }

    /* Ensure checkpoint dir exists */
    mkdirp(JT_LC_CHECKPOINT_DIR);

    return 0;
}

int jt_lc_register(JTAgentManager *mgr, const char *name,
                   const char *command, uint32_t timeout_sec) {
    if (!mgr || !name || !command) return -1;
    if (mgr->agent_count >= JT_LC_MAX_AGENTS) return -2;

    /* Check duplicate */
    for (int i = 0; i < mgr->agent_count; i++) {
        if (strcmp(mgr->agents[i].name, name) == 0) return -3;
    }

    JTAgent *a = &mgr->agents[mgr->agent_count++];
    memset(a, 0, sizeof(*a));
    strncpy(a->name, name, sizeof(a->name) - 1);
    strncpy(a->command, command, sizeof(a->command) - 1);
    a->state = JT_AGENT_STOPPED;
    a->pid = 0;
    a->timeout_sec = timeout_sec ? timeout_sec : JT_LC_DEFAULT_TIMEOUT;
    snprintf(a->checkpoint_file, sizeof(a->checkpoint_file),
        "%s/%s.checkpoint", JT_LC_CHECKPOINT_DIR, name);

    return 0;
}

int jt_lc_start(JTAgentManager *mgr, const char *name) {
    if (!mgr) return -1;
    JTAgent *a = jt_lc_find(mgr, name);
    if (!a) return -2;
    if (a->state == JT_AGENT_RUNNING || a->state == JT_AGENT_STARTING) return -3;

    pid_t pid = fork();
    if (pid < 0) return -4;

    if (pid == 0) {
        /* Child */
        setsid();
        execl("/bin/sh", "sh", "-c", a->command, NULL);
        _exit(127);
    }

    /* Parent */
    a->pid = pid;
    a->state = JT_AGENT_STARTING;
    a->start_time = now_sec();
    a->last_heartbeat = now_sec();
    return 0;
}

int jt_lc_stop(JTAgentManager *mgr, const char *name) {
    if (!mgr) return -1;
    JTAgent *a = jt_lc_find(mgr, name);
    if (!a) return -2;
    if (a->pid <= 0) { a->state = JT_AGENT_STOPPED; return 0; }

    /* Try SIGTERM first */
    kill(a->pid, SIGTERM);
    usleep(200000); /* 200ms grace */

    /* Check if still alive */
    int status;
    pid_t wp = waitpid(a->pid, &status, WNOHANG);
    if (wp == 0) {
        /* Still alive — SIGKILL */
        kill(a->pid, SIGKILL);
        waitpid(a->pid, &status, 0);
    }

    a->state = JT_AGENT_KILLED;
    a->pid = 0;
    mgr->total_kills++;
    return 0;
}

int jt_lc_stop_all(JTAgentManager *mgr) {
    if (!mgr) return -1;
    for (int i = 0; i < mgr->agent_count; i++) {
        if (mgr->agents[i].pid > 0) jt_lc_stop(mgr, mgr->agents[i].name);
    }
    return 0;
}

int jt_lc_check(JTAgentManager *mgr) {
    if (!mgr) return -1;

    for (int i = 0; i < mgr->agent_count; i++) {
        JTAgent *a = &mgr->agents[i];
        if (a->state != JT_AGENT_RUNNING && a->state != JT_AGENT_STARTING) continue;

        /* Check if process exists */
        int status;
        pid_t wp = waitpid(a->pid, &status, WNOHANG);

        if (wp == a->pid) {
            /* Process exited */
            a->state = JT_AGENT_CRASHED;
            a->pid = 0;
            continue;
        }

        if (wp < 0 && errno == ECHILD) {
            a->state = JT_AGENT_CRASHED;
            a->pid = 0;
            continue;
        }

        /* Check for stuck (no heartbeat for timeout) */
        uint64_t elapsed = now_sec() - a->last_heartbeat;
        if (elapsed > a->timeout_sec) {
            a->state = JT_AGENT_STUCK;
        } else if (a->state == JT_AGENT_STARTING) {
            a->state = JT_AGENT_RUNNING;
        }

        /* Update stats */
        uint64_t rss = 0;
        uint32_t cpu = 0;
        if (read_proc_stat(a->pid, &rss, &cpu) == 0) {
            a->rss_kb = rss;
            a->cpu_time_ms = cpu;
        }
    }

    return 0;
}

int jt_lc_recover(JTAgentManager *mgr) {
    if (!mgr) return -1;
    int recovered = 0;

    for (int i = 0; i < mgr->agent_count; i++) {
        JTAgent *a = &mgr->agents[i];
        if ((a->state == JT_AGENT_CRASHED || a->state == JT_AGENT_STUCK) &&
            a->restarts < JT_LC_MAX_RESTARTS) {
            printf("  Recovering %s (state=%d, restarts=%d)\n",
                   a->name, a->state, a->restarts);
            jt_lc_start(mgr, a->name);
            a->restarts++;
            mgr->total_restarts++;
            recovered++;
        }
    }

    return recovered;
}

int jt_lc_watchdog_ping(JTAgentManager *mgr) {
    if (!mgr || mgr->wdt_fd < 0) return -1;
    return write(mgr->wdt_fd, "1", 1);
}

int jt_lc_checkpoint(JTAgentManager *mgr, const char *name) {
    if (!mgr || !name) return -1;
    JTAgent *a = jt_lc_find(mgr, name);
    if (!a) return -2;

    FILE *f = fopen(a->checkpoint_file, "w");
    if (!f) return -3;

    fprintf(f, "name=%s\ncommand=%s\nstate=%d\npid=%d\nrestarts=%u\n"
               "start_time=%lu\nrss_kb=%lu\ncpu_ms=%u\n",
            a->name, a->command, a->state, a->pid, a->restarts,
            a->start_time, a->rss_kb, a->cpu_time_ms);
    fclose(f);
    fsync(fileno(f));

    mgr->total_checkpoints++;
    return 0;
}

JTAgent *jt_lc_find(JTAgentManager *mgr, const char *name) {
    if (!mgr || !name) return NULL;
    for (int i = 0; i < mgr->agent_count; i++) {
        if (strcmp(mgr->agents[i].name, name) == 0) return &mgr->agents[i];
    }
    return NULL;
}

int jt_lc_report(JTAgentManager *mgr, char *out, int max_len) {
    if (!mgr || !out) return -1;
    const char *state_labels[] = {"STOPPED","STARTING","RUNNING","STUCK","CRASHED","KILLED"};
    uint64_t uptime = now_sec() - mgr->uptime_start;
    int pos = 0;

    pos += snprintf(out + pos, max_len - pos,
        "# Agent Lifecycle Report\n\n"
        "Uptime: %lus, Watchdog: %s\n"
        "Total: %u restarts, %u kills, %u checkpoints\n\n"
        "## Agents (%d)\n\n",
        uptime, mgr->wdt_available ? "active" : "unavailable",
        mgr->total_restarts, mgr->total_kills, mgr->total_checkpoints,
        mgr->agent_count);

    for (int i = 0; i < mgr->agent_count && pos < max_len - 64; i++) {
        JTAgent *a = &mgr->agents[i];
        pos += snprintf(out + pos, max_len - pos,
            "- **%s**: %s (PID:%d, RSS:%luMB, CPU:%ums, restarts:%u)\n"
            "  cmd: `%s`\n",
            a->name, state_labels[a->state], a->pid,
            a->rss_kb / 1024, a->cpu_time_ms, a->restarts, a->command);
    }

    return pos;
}

int jt_lc_cleanup(JTAgentManager *mgr) {
    if (!mgr) return -1;
    jt_lc_stop_all(mgr);
    if (mgr->wdt_fd >= 0) {
        /* Write magic close to disable watchdog */
        write(mgr->wdt_fd, "V", 1);
        close(mgr->wdt_fd);
        mgr->wdt_fd = -1;
    }
    return 0;
}

int jt_agent_lifecycle_test(void) {
    int failures = 0;

    /* Test 1: Init */
    JTAgentManager mgr;
    int rc = jt_lc_init(&mgr);
    if (rc != 0) { failures++; printf("FAIL init\n"); }
    else printf("  Init: OK (watchdog: %s)\n", mgr.wdt_available ? "yes" : "no");

    /* Test 2: Register */
    rc = jt_lc_register(&mgr, "sleep-agent", "sleep 300", 60);
    if (rc != 0) { failures++; printf("FAIL register\n"); }
    rc = jt_lc_register(&mgr, "echo-agent", "echo hello && sleep 300", 60);
    if (rc != 0) { failures++; printf("FAIL register2\n"); }
    printf("  Registered: %d agents\n", mgr.agent_count);

    /* Test 3: Duplicate */
    rc = jt_lc_register(&mgr, "sleep-agent", "sleep 300", 60);
    if (rc != -3) { failures++; printf("FAIL dup register: %d\n", rc); }
    else printf("  Duplicate: rejected\n");

    /* Test 4: Find */
    JTAgent *a = jt_lc_find(&mgr, "sleep-agent");
    if (!a) { failures++; printf("FAIL find\n"); }
    else printf("  Find: %s, state=%d\n", a->name, a->state);

    /* Test 5: Start */
    rc = jt_lc_start(&mgr, "sleep-agent");
    if (rc != 0) { failures++; printf("FAIL start\n"); }
    else printf("  Started: PID %d\n", a->pid);

    /* Test 6: Check */
    usleep(100000); /* 100ms */
    rc = jt_lc_check(&mgr);
    if (rc != 0) { failures++; printf("FAIL check\n"); }
    else printf("  Check: state=%d (1=starting, 2=running)\n", a->state);

    /* Test 7: Read stats */
    if (a->pid > 0 && a->rss_kb > 0) {
        printf("  Stats: RSS=%lu KB, CPU=%u ms\n", a->rss_kb, a->cpu_time_ms);
    }

    /* Test 8: Checkpoint */
    rc = jt_lc_checkpoint(&mgr, "sleep-agent");
    if (rc != 0) { failures++; printf("FAIL checkpoint\n"); }
    else printf("  Checkpoint: OK\n");

    /* Test 9: Stop */
    rc = jt_lc_stop(&mgr, "sleep-agent");
    if (rc != 0) { failures++; printf("FAIL stop\n"); }
    else printf("  Stopped: state=%d (5=killed)\n", a->state);

    /* Test 10: Recover (should not restart killed agent — restarts=0, state=killed not crashed) */
    rc = jt_lc_recover(&mgr);
    printf("  Recover: %d agents recovered\n", rc);

    /* Test 11: Stop all */
    rc = jt_lc_stop_all(&mgr);
    if (rc != 0) { failures++; printf("FAIL stop_all\n"); }

    /* Test 12: Report */
    char report[1024];
    rc = jt_lc_report(&mgr, report, sizeof(report));
    if (rc <= 0) { failures++; printf("FAIL report\n"); }
    else printf("  Report: %d bytes\n", rc);

    /* Test 13: Null safety */
    if (jt_lc_init(NULL) != -1) { failures++; printf("FAIL null init\n"); }
    if (jt_lc_register(NULL, "a", "b", 10) != -1) { failures++; printf("FAIL null register\n"); }
    if (jt_lc_find(NULL, "a") != NULL) { failures++; printf("FAIL null find\n"); }
    printf("  Null safety: OK\n");

    /* Test 14: Max agents */
    JTAgentManager big;
    jt_lc_init(&big);
    int ok = 0;
    for (int i = 0; i < JT_LC_MAX_AGENTS + 2; i++) {
        char name[32], cmd[64];
        snprintf(name, sizeof(name), "agent-%d", i);
        snprintf(cmd, sizeof(cmd), "sleep %d", 10);
        rc = jt_lc_register(&big, name, cmd, 10);
        if (rc == -2) { ok = 1; break; }
    }
    if (!ok) { failures++; printf("FAIL max agents\n"); }
    else printf("  Max agents: enforced\n");

    /* Cleanup */
    jt_lc_cleanup(&mgr);
    jt_lc_cleanup(&big);

    printf("jt_agent_lifecycle_test: %d failures\n", failures);
    return failures;
}
