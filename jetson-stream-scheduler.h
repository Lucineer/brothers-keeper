/**
 * jetson-stream-scheduler.h — CUDA stream scheduler for multi-agent GPU sharing
 *
 * On Jetson, multiple agents share one GPU with no hypervisor.
 * This scheduler provides weighted fair queueing using CUDA stream priorities,
 * token bucket rate limiting, and utilization-based timeslicing.
 *
 * Reads GPU utilization from sysfs to adapt timeslices.
 * Uses CUDA events for preemption between agents.
 *
 * Cloud agents have vGPU, MIG, or Kubernetes GPU scheduling.
 * Jetson agents have this.
 */

#ifndef JETSON_STREAM_SCHEDULER_H
#define JETSON_STREAM_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define JT_SCHED_MAX_AGENTS     8
#define JT_SCHED_MAX_STREAMS    4  /* per agent */
#define JT_SCHED_GPU_LOAD_PATH  "/sys/class/devfreq/17000000.gv11b/utilization"
#define JT_SCHED_ALT_GPU_LOAD   "/sys/class/devfreq/17000000.ga10b/utilization"
#define JT_SCHED_DEFAULT_TPS    10  /* tokens per second */
#define JT_SCHED_MAX_TOKENS     100
#define JT_SCHED_TIMESLICE_MS   5000  /* 5 second round-robin cycle */
#define JT_SCHED_HISTORY_LEN    32

typedef struct {
    char     name[32];
    uint32_t priority;         /* 1-10, higher = more GPU time */
    uint32_t tokens;           /* current token bucket */
    uint32_t max_tokens;       /* bucket capacity */
    uint32_t tokens_per_sec;   /* refill rate */
    uint32_t kernels_launched; /* total */
    uint32_t kernels_blocked;  /* hit token limit */
    uint64_t gpu_time_us;      /* cumulative GPU time */
    uint64_t last_refill_ms;   /* last token refill */
    bool     active;
} JTSchedAgent;

typedef struct {
    JTSchedAgent agents[JT_SCHED_MAX_AGENTS];
    int          agent_count;

    /* GPU utilization from sysfs */
    uint32_t gpu_utilization;  /* 0-100 percent */
    char     gpu_load_path[256];
    bool     gpu_load_available;

    /* Timeslice control */
    uint32_t cycle_ms;         /* round-robin cycle length */
    uint32_t tick_ms;          /* current tick within cycle */
    int      current_agent;    /* agent currently holding the GPU */

    /* History */
    struct {
        uint32_t agent_idx;
        uint32_t gpu_util;
        uint64_t timestamp_ms;
    } history[JT_SCHED_HISTORY_LEN];
    int history_count;

    /* Stats */
    uint32_t total_ticks;
    uint32_t rotations;
    uint64_t total_blocked_time_ms;
} JTStreamScheduler;

/* ═══ API ═══ */

/** Initialize scheduler, probe GPU utilization path */
int jt_sched_init(JTStreamScheduler *sched);

/** Register an agent for GPU time */
int jt_sched_register(JTStreamScheduler *sched, const char *name,
                      uint32_t priority, uint32_t tokens_per_sec);

/** Unregister an agent */
int jt_sched_unregister(JTStreamScheduler *sched, const char *name);

/** Check if agent can launch a kernel (token check) */
bool jt_sched_can_launch(JTStreamScheduler *sched, const char *name);

/** Record a kernel launch (consume token) */
int jt_sched_launch(JTStreamScheduler *sched, const char *name);

/** Tick — refill tokens, rotate timeslices, read GPU util */
int jt_sched_tick(JTStreamScheduler *sched);

/** Get current agent holding GPU timeslice */
const char *jt_sched_current_holder(const JTStreamScheduler *sched);

/** Get recommended wait time before agent can launch (ms) */
uint32_t jt_sched_wait_time(JTStreamScheduler *sched, const char *name);

/** Get agent utilization share (0-100) */
uint32_t jt_sched_agent_share(const JTStreamScheduler *sched, const char *name);

/** Force rotate to next agent */
int jt_sched_rotate(JTStreamScheduler *sched);

/** Read GPU utilization */
uint32_t jt_sched_gpu_util(JTStreamScheduler *sched);

/** Report */
int jt_sched_report(JTStreamScheduler *sched, char *out, int max_len);

/** Test */
int jt_stream_scheduler_test(void);

#endif
