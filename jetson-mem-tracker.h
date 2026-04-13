#include <stddef.h>
/**
 * jetson-mem-tracker.h — Unified memory tracking for Jetson shared RAM
 *
 * On Jetson, CPU and GPU share the same physical RAM. A large CUDA alloc
 * steals directly from the CPU. This module tracks both and warns before OOM.
 *
 * Cloud GPUs have discrete VRAM — this problem doesn't exist there.
 */

#ifndef JETSON_MEM_TRACKER_H
#define JETSON_MEM_TRACKER_H

#include <stdint.h>
#include <stdbool.h>

#define JT_MEM_MAX_ALLOCATIONS 64
#define JT_MEM_MAX_ALERTS      32

typedef struct {
    void     *ptr;
    size_t    size;
    char      tag[32];
    uint32_t  alloc_time;     /* seconds since epoch */
    bool      is_gpu;         /* cudaMallocManaged vs malloc */
} JTMemAlloc;

typedef struct {
    /* From /proc/meminfo */
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;
    uint64_t buffers_kb;
    uint64_t cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;

    /* Tracked allocations */
    JTMemAlloc allocs[JT_MEM_MAX_ALLOCATIONS];
    int alloc_count;

    /* Derived */
    uint64_t tracked_used;
    uint64_t system_used;
    uint64_t estimated_gpu_used;
    uint64_t safe_headroom_kb;

    /* Alerts */
    char alerts[JT_MEM_MAX_ALERTS][128];
    int  alert_count;

    /* Watermarks */
    uint64_t peak_system_used;
    uint64_t peak_tracked_used;
} JTMemState;

typedef struct {
    JTMemState state;
    uint32_t   sample_count;
    uint32_t   oom_warnings;
    uint32_t   gc_suggestions;
} JTMemMonitor;

/* ═══ API ═══ */

/** Read current memory state from /proc/meminfo */
int jt_mem_read(JTMemState *state);

/** Register an allocation for tracking */
int jt_mem_track(JTMemState *state, void *ptr, size_t size,
                 const char *tag, bool is_gpu);

/** Unregister an allocation */
int jt_mem_untrack(JTMemState *state, void *ptr);

/** Check if allocation is safe (won't OOM) */
bool jt_mem_can_alloc(JTMemState *state, size_t size);

/** Get safe allocation size */
size_t jt_mem_safe_size(JTMemState *state);

/** Get memory pressure level: 0=green, 1=yellow, 2=red, 3=critical */
int jt_mem_pressure(const JTMemState *state);

/** Force GC suggestion — free largest tracked allocations first */
int jt_mem_suggest_gc(JTMemState *state, size_t needed,
                      char *suggestions, int max_len);

/** Monitor init + sample */
int jt_mem_monitor_init(JTMemMonitor *mon);
int jt_mem_monitor_sample(JTMemMonitor *mon);

/** Report */
int jt_mem_report(const JTMemState *state, char *out, int max_len);

/** Test */
int jt_mem_tracker_test(void);

#endif
