#include <time.h>
/**
 * jetson-stream-scheduler.c — Multi-agent GPU stream scheduler
 *
 * Weighted fair queueing without CUDA dependency.
 * Uses sysfs for GPU utilization, token buckets for rate limiting.
 */

#include "jetson-stream-scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_sysfs_int(const char *path) {
    char buf[64];
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    return atoi(buf);
}

/* ═══ Init ═══ */

int jt_sched_init(JTStreamScheduler *sched) {
    if (!sched) return -1;
    memset(sched, 0, sizeof(*sched));
    sched->cycle_ms = JT_SCHED_TIMESLICE_MS;
    sched->current_agent = -1;

    /* Find GPU utilization path */
    if (access(JT_SCHED_GPU_LOAD_PATH, F_OK) == 0) {
        strncpy(sched->gpu_load_path, JT_SCHED_GPU_LOAD_PATH,
                sizeof(sched->gpu_load_path) - 1);
        sched->gpu_load_available = true;
    } else if (access(JT_SCHED_ALT_GPU_LOAD, F_OK) == 0) {
        strncpy(sched->gpu_load_path, JT_SCHED_ALT_GPU_LOAD,
                sizeof(sched->gpu_load_path) - 1);
        sched->gpu_load_available = true;
    } else {
        sched->gpu_load_available = false;
    }

    sched->gpu_utilization = jt_sched_gpu_util(sched);
    return 0;
}

/* ═══ Register ═══ */

int jt_sched_register(JTStreamScheduler *sched, const char *name,
                      uint32_t priority, uint32_t tokens_per_sec) {
    if (!sched || !name) return -1;
    for (int i = 0; i < sched->agent_count; i++) {
        if (strcmp(sched->agents[i].name, name) == 0) {
            sched->agents[i].priority = priority;
            sched->agents[i].tokens_per_sec = tokens_per_sec ?: JT_SCHED_DEFAULT_TPS;
            sched->agents[i].max_tokens = tokens_per_sec * 10;
            return 0; /* update */
        }
    }
    if (sched->agent_count >= JT_SCHED_MAX_AGENTS) return -2;

    JTSchedAgent *a = &sched->agents[sched->agent_count++];
    memset(a, 0, sizeof(*a));
    strncpy(a->name, name, sizeof(a->name) - 1);
    a->priority = priority;
    a->tokens_per_sec = tokens_per_sec ?: JT_SCHED_DEFAULT_TPS;
    a->max_tokens = a->tokens_per_sec * 10;
    a->tokens = a->max_tokens; /* start full */
    a->last_refill_ms = now_ms();
    a->active = true;

    if (sched->current_agent < 0) sched->current_agent = 0;
    return 0;
}

int jt_sched_unregister(JTStreamScheduler *sched, const char *name) {
    if (!sched || !name) return -1;
    for (int i = 0; i < sched->agent_count; i++) {
        if (strcmp(sched->agents[i].name, name) == 0) {
            memmove(&sched->agents[i], &sched->agents[i+1],
                    (sched->agent_count - i - 1) * sizeof(JTSchedAgent));
            sched->agent_count--;
            if (sched->current_agent >= sched->agent_count)
                sched->current_agent = sched->agent_count - 1;
            return 0;
        }
    }
    return -2;
}

/* ═══ Launch ═══ */

bool jt_sched_can_launch(JTStreamScheduler *sched, const char *name) {
    if (!sched || !name) return false;
    for (int i = 0; i < sched->agent_count; i++) {
        if (strcmp(sched->agents[i].name, name) == 0) {
            return sched->agents[i].tokens > 0;
        }
    }
    return false;
}

int jt_sched_launch(JTStreamScheduler *sched, const char *name) {
    if (!sched || !name) return -1;
    for (int i = 0; i < sched->agent_count; i++) {
        if (strcmp(sched->agents[i].name, name) == 0) {
            if (sched->agents[i].tokens <= 0) {
                sched->agents[i].kernels_blocked++;
                return -2; /* blocked */
            }
            sched->agents[i].tokens--;
            sched->agents[i].kernels_launched++;
            return 0;
        }
    }
    return -3; /* not found */
}

/* ═══ Tick ═══ */

int jt_sched_tick(JTStreamScheduler *sched) {
    if (!sched) return -1;
    uint64_t now = now_ms();
    sched->total_ticks++;

    /* Refill tokens for all agents */
    for (int i = 0; i < sched->agent_count; i++) {
        JTSchedAgent *a = &sched->agents[i];
        if (!a->active) continue;

        uint64_t elapsed = (now - a->last_refill_ms) / 1000; /* seconds */
        if (elapsed > 0) {
            uint32_t refill = a->tokens_per_sec * (uint32_t)elapsed;
            a->tokens = (a->tokens + refill > a->max_tokens) ?
                        a->max_tokens : a->tokens + refill;
            a->last_refill_ms = now;
        }
    }

    /* Rotate timeslice */
    sched->tick_ms += 100; /* assuming 100ms tick interval */
    if (sched->tick_ms >= sched->cycle_ms && sched->agent_count > 0) {
        sched->tick_ms = 0;
        sched->current_agent = (sched->current_agent + 1) % sched->agent_count;
        sched->rotations++;
    }

    /* GPU utilization */
    sched->gpu_utilization = jt_sched_gpu_util(sched);

    /* History */
    if (sched->history_count < JT_SCHED_HISTORY_LEN) {
        int idx = sched->history_count;
        sched->history[idx].agent_idx = sched->current_agent;
        sched->history[idx].gpu_util = sched->gpu_utilization;
        sched->history[idx].timestamp_ms = now;
        sched->history_count++;
    }

    return 0;
}

/* ═══ Queries ═══ */

const char *jt_sched_current_holder(const JTStreamScheduler *sched) {
    if (!sched || sched->current_agent < 0 ||
        sched->current_agent >= sched->agent_count) return NULL;
    return sched->agents[sched->current_agent].name;
}

uint32_t jt_sched_wait_time(JTStreamScheduler *sched, const char *name) {
    if (!sched || !name) return 0;
    for (int i = 0; i < sched->agent_count; i++) {
        if (strcmp(sched->agents[i].name, name) == 0) {
            if (sched->agents[i].tokens > 0) return 0;
            /* Estimate time to get 1 token */
            return 1000 / sched->agents[i].tokens_per_sec;
        }
    }
    return 0;
}

uint32_t jt_sched_agent_share(const JTStreamScheduler *sched, const char *name) {
    if (!sched || !name || sched->agent_count == 0) return 0;

    uint32_t my_pri = 0;
    uint32_t total_pri = 0;
    for (int i = 0; i < sched->agent_count; i++) {
        total_pri += sched->agents[i].priority;
        if (strcmp(sched->agents[i].name, name) == 0) my_pri = sched->agents[i].priority;
    }

    return (total_pri > 0) ? (my_pri * 100 / total_pri) : 0;
}

int jt_sched_rotate(JTStreamScheduler *sched) {
    if (!sched || sched->agent_count <= 1) return -1;
    sched->current_agent = (sched->current_agent + 1) % sched->agent_count;
    sched->rotations++;
    sched->tick_ms = 0;
    return 0;
}

uint32_t jt_sched_gpu_util(JTStreamScheduler *sched) {
    if (!sched || !sched->gpu_load_available) return 0;
    int val = read_sysfs_int(sched->gpu_load_path);
    return (val > 0) ? (uint32_t)val : 0;
}

/* ═══ Report ═══ */

int jt_sched_report(JTStreamScheduler *sched, char *out, int max_len) {
    if (!sched || !out) return -1;
    const char *holder = jt_sched_current_holder(sched);
    int pos = snprintf(out, max_len,
        "# Stream Scheduler Report\n\n"
        "GPU Utilization: %u%%\n"
        "Current Holder: %s\n"
        "Cycle: %u/%u ms, Total ticks: %u, Rotations: %u\n\n"
        "## Agents (%d)\n\n",
        sched->gpu_utilization,
        holder ? holder : "none",
        sched->tick_ms, sched->cycle_ms,
        sched->total_ticks, sched->rotations,
        sched->agent_count);

    for (int i = 0; i < sched->agent_count && pos < max_len - 80; i++) {
        JTSchedAgent *a = &sched->agents[i];
        pos += snprintf(out + pos, max_len - pos,
            "- **%s**: pri=%u tokens=%u/%u (%u/s) share=%u%%\n"
            "  launched=%u blocked=%u\n",
            a->name, a->priority, a->tokens, a->max_tokens,
            a->tokens_per_sec, jt_sched_agent_share(sched, a->name),
            a->kernels_launched, a->kernels_blocked);
    }

    return pos;
}

/* ═══ Tests ═══ */

int jt_stream_scheduler_test(void) {
    int failures = 0;

    /* Test 1: Init */
    JTStreamScheduler sched;
    int rc = jt_sched_init(&sched);
    if (rc != 0) { failures++; printf("FAIL init\n"); }
    else printf("  Init: OK (gpu_load: %s)\n",
               sched.gpu_load_available ? sched.gpu_load_path : "unavailable");

    /* Test 2: Register agents */
    rc = jt_sched_register(&sched, "flux", 8, 20);
    if (rc != 0) { failures++; printf("FAIL register\n"); }
    rc = jt_sched_register(&sched, "craftmind", 4, 10);
    if (rc != 0) { failures++; printf("FAIL register2\n"); }
    rc = jt_sched_register(&sched, "researcher", 2, 5);
    if (rc != 0) { failures++; printf("FAIL register3\n"); }
    printf("  Registered: %d agents\n", sched.agent_count);

    /* Test 3: Shares */
    uint32_t s1 = jt_sched_agent_share(&sched, "flux");
    uint32_t s2 = jt_sched_agent_share(&sched, "craftmind");
    uint32_t s3 = jt_sched_agent_share(&sched, "researcher");
    printf("  Shares: flux=%u%% craftmind=%u%% researcher=%u%%\n", s1, s2, s3);
    if (s1 + s2 + s3 < 99 || s1 + s2 + s3 > 101) { failures++; printf("FAIL shares don't sum to 100\n"); }
    if (s1 <= s2) { failures++; printf("FAIL priority ordering\n"); }

    /* Test 4: Can launch */
    bool can = jt_sched_can_launch(&sched, "flux");
    if (!can) { failures++; printf("FAIL can_launch (should have tokens)\n"); }
    printf("  Can launch flux: %s\n", can ? "yes" : "no");

    /* Test 5: Launch kernels */
    for (int i = 0; i < 50; i++) jt_sched_launch(&sched, "flux");
    for (int i = 0; i < 20; i++) jt_sched_launch(&sched, "craftmind");
    printf("  Launched: flux=%u craftmind=%u\n",
           sched.agents[0].kernels_launched, sched.agents[1].kernels_launched);

    /* Test 6: Token depletion */
    can = jt_sched_can_launch(&sched, "researcher");
    /* researcher has 50 tokens (5*10), should still have some */
    printf("  Researcher can launch: %s\n", can ? "yes" : "no");

    /* Drain researcher */
    for (int i = 0; i < 60; i++) jt_sched_launch(&sched, "researcher");
    can = jt_sched_can_launch(&sched, "researcher");
    if (can) { failures++; printf("FAIL should be blocked\n"); }
    printf("  Researcher blocked: %s (blocked=%u)\n",
           !can ? "yes" : "no", sched.agents[2].kernels_blocked);

    /* Test 7: Blocked launch returns error */
    rc = jt_sched_launch(&sched, "researcher");
    if (rc != -2) { failures++; printf("FAIL blocked launch rc=%d\n", rc); }
    else printf("  Blocked launch: correctly returned -2\n");

    /* Test 8: Tick refills tokens */
    usleep(200000); /* 200ms */
    jt_sched_tick(&sched);
    /* researcher should have ~1 token (5/s * 0.2s = 1) */
    printf("  After tick: researcher tokens=%u\n", sched.agents[2].tokens);

    /* Test 9: Wait time */
    uint32_t wait = jt_sched_wait_time(&sched, "researcher");
    printf("  Researcher wait time: %u ms\n", wait);

    /* Test 10: Current holder */
    const char *holder = jt_sched_current_holder(&sched);
    printf("  Current holder: %s\n", holder ? holder : "none");

    /* Test 11: Rotate */
    rc = jt_sched_rotate(&sched);
    if (rc != 0) { failures++; printf("FAIL rotate\n"); }
    printf("  Rotated: now %s (rotations=%u)\n",
           jt_sched_current_holder(&sched), sched.rotations);

    /* Test 12: Unregister */
    rc = jt_sched_unregister(&sched, "researcher");
    if (rc != 0) { failures++; printf("FAIL unregister\n"); }
    printf("  After unregister: %d agents\n", sched.agent_count);

    /* Test 13: GPU util */
    uint32_t util = jt_sched_gpu_util(&sched);
    printf("  GPU utilization: %u%%\n", util);

    /* Test 14: Report */
    char report[1024];
    rc = jt_sched_report(&sched, report, sizeof(report));
    if (rc <= 0) { failures++; printf("FAIL report\n"); }
    else printf("  Report: %d bytes\n", rc);

    /* Test 15: Null safety */
    if (jt_sched_init(NULL) != -1) { failures++; printf("FAIL null init\n"); }
    if (jt_sched_can_launch(NULL, "x")) { failures++; printf("FAIL null can_launch\n"); }
    if (jt_sched_register(NULL, "x", 1, 1) != -1) { failures++; printf("FAIL null register\n"); }
    printf("  Null safety: OK\n");

    /* Test 16: Max agents */
    JTStreamScheduler big;
    jt_sched_init(&big);
    int ok = 0;
    for (int i = 0; i < JT_SCHED_MAX_AGENTS + 2; i++) {
        char name[32];
        snprintf(name, sizeof(name), "agent-%d", i);
        if (jt_sched_register(&big, name, 1, 1) == -2) { ok = 1; break; }
    }
    if (!ok) { failures++; printf("FAIL max agents\n"); }
    else printf("  Max agents: enforced\n");

    /* Test 17: Duplicate register updates */
    rc = jt_sched_register(&sched, "flux", 10, 30);
    if (rc != 0) { failures++; printf("FAIL update\n"); }
    if (sched.agents[0].priority != 10) { failures++; printf("FAIL priority update\n"); }
    printf("  Update: flux priority now %u\n", sched.agents[0].priority);

    printf("jt_stream_scheduler_test: %d failures\n", failures);
    return failures;
}
