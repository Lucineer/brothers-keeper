/**
 * jetson-cuda-governor.h — GPU memory pressure and thermal-aware batch control
 *
 * On Jetson, GPU and CPU share 8GB of physical RAM. A large CUDA alloc
 * can trigger the OOM killer, which indiscriminately murders processes.
 *
 * This module:
 * - Monitors GPU memory pressure in real-time
 * - Provides a "pressure valve" that preempts GPU work before OOM
 * - Estimates safe batch sizes based on current thermal trajectory
 * - Manages CUDA stream priorities for multi-agent fair sharing
 *
 * Cloud GPUs have dedicated VRAM and hypervisors.
 * Jetson has none of that. This is the governor.
 */

#ifndef JETSON_CUDA_GOVERNOR_H
#define JETSON_CUDA_GOVERNOR_H

#include <stdint.h>
#include <stdbool.h>

#define JT_GPU_MAX_STREAMS      16
#define JT_GPU_MAX_AGENTS       8
#define JT_GPU_HISTORY_LEN      64
#define JT_GPU_SAFE_HEADROOM_MB 512
#define JT_GPU_CRITICAL_MB      256
#define JT_GPU_BATCH_SIZES      8

/* Memory pressure levels */
typedef enum {
    JT_GPU_GREEN,    /* plenty of room */
    JT_GPU_YELLOW,   /* getting tight, warn */
    JT_GPU_ORANGE,   /* preemption recommended */
    JT_GPU_RED,      /* stop GPU work immediately */
    JT_GPU_CRITICAL  /* OOM imminent, force-free */
} JTGPUPressure;

/* Thermal trajectory — not just current temp, but where it's HEADING */
typedef enum {
    JT_THERM_COOLING,    /* temp dropping */
    JT_THERM_STABLE,     /* temp flat */
    JT_THERM_RISING,     /* temp increasing slowly */
    JT_THERM_SURGING     /* temp increasing fast — throttle imminent */
} JTThermTrajectory;

typedef struct {
    /* Memory */
    uint64_t total_gpu_mem_mb;
    uint64_t used_gpu_mem_mb;
    uint64_t free_gpu_mem_mb;
    uint64_t reserved_mb;        /* our managed reservation */
    JTGPUPressure pressure;

    /* Thermal trajectory */
    int32_t  temp_rate_mc_per_s; /* milli-C per second */
    uint32_t current_temp_mc;
    uint32_t throttle_temp_mc;
    JTThermTrajectory trajectory;
    uint32_t seconds_to_throttle; /* estimated */

    /* Batch sizing */
    uint32_t safe_batch_sizes[JT_GPU_BATCH_SIZES]; /* by model size tier */
    uint32_t current_safe_batch;

    /* Per-agent GPU share */
    char     agent_names[JT_GPU_MAX_AGENTS][32];
    uint64_t agent_gpu_allocs[JT_GPU_MAX_AGENTS]; /* MB */
    uint32_t agent_stream_priorities[JT_GPU_MAX_AGENTS];
    int      agent_count;

    /* History */
    struct {
        uint64_t timestamp;
        uint64_t used_mb;
        uint32_t temp_mc;
    } history[JT_GPU_HISTORY_LEN];
    int history_count;
    int history_idx;

    /* Actions taken */
    uint32_t preemptions;
    uint32_t batch_reductions;
    uint32_t oom_saves;
} JTGPUGovernor;

/* ═══ Memory Pressure Valve ═══ */

/** Initialize governor — reads /proc/meminfo + thermal zones */
int jt_gpu_gov_init(JTGPUGovernor *gov);

/** Sample current state (fast, called every 1-5 seconds) */
int jt_gpu_gov_sample(JTGPUGovernor *gov);

/** Get current memory pressure level */
JTGPUPressure jt_gpu_pressure(const JTGPUGovernor *gov);

/** Check if a GPU allocation is safe */
bool jt_gpu_can_alloc(const JTGPUGovernor *gov, uint64_t mb);

/** Reserve memory for an agent (soft reservation) */
int jt_gpu_reserve(JTGPUGovernor *gov, const char *agent, uint64_t mb);

/** Release reservation */
int jt_gpu_release(JTGPUGovernor *gov, const char *agent);

/** Preempt lowest-priority agent's GPU work */
const char *jt_gpu_preempt(JTGPUGovernor *gov);

/** Get safe batch size for current thermal trajectory */
uint32_t jt_gpu_safe_batch(const JTGPUGovernor *gov, uint32_t base_batch);

/* ═══ Thermal-Aware Batching ═══ */

/** Get thermal trajectory */
JTThermTrajectory jt_gpu_thermal_trajectory(const JTGPUGovernor *gov);

/** Estimate seconds until throttle */
uint32_t jt_gpu_seconds_to_throttle(const JTGPUGovernor *gov);

/** Get batch size that completes before throttle */
uint32_t jt_gpu_thermal_safe_batch(const JTGPUGovernor *gov,
                                    uint32_t base_batch,
                                    uint32_t batch_time_ms);

/* ═══ Multi-Agent Fair Share ═══ */

/** Register an agent for GPU time sharing */
int jt_gpu_register_agent(JTGPUGovernor *gov, const char *name, uint32_t priority);

/** Get recommended timeslice for agent (ms) */
uint32_t jt_gpu_timeslice(JTGPUGovernor *gov, const char *agent);

/** Report */
int jt_gpu_gov_report(const JTGPUGovernor *gov, char *out, int max_len);

/** Test */
int jt_gpu_governor_test(void);

#endif
