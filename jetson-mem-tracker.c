/**
 * jetson-mem-tracker.c — Unified memory tracking for Jetson
 */

#define _POSIX_C_SOURCE 200809L
#include "jetson-mem-tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t parse_meminfo_kb(const char *field) {
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    uint64_t val = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, field, strlen(field)) == 0) {
            sscanf(line + strlen(field), " %lu", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

int jt_mem_read(JTMemState *state) {
    if (!state) return -1;

    state->total_kb = parse_meminfo_kb("MemTotal:");
    state->free_kb = parse_meminfo_kb("MemFree:");
    state->available_kb = parse_meminfo_kb("MemAvailable:");
    state->buffers_kb = parse_meminfo_kb("Buffers:");
    state->cached_kb = parse_meminfo_kb("Cached:");
    state->swap_total_kb = parse_meminfo_kb("SwapTotal:");
    state->swap_free_kb = parse_meminfo_kb("SwapFree:");

    state->system_used = state->total_kb - state->available_kb;

    /* Sum tracked allocations */
    state->tracked_used = 0;
    state->estimated_gpu_used = 0;
    for (int i = 0; i < state->alloc_count; i++) {
        state->tracked_used += state->allocs[i].size;
        if (state->allocs[i].is_gpu)
            state->estimated_gpu_used += state->allocs[i].size;
    }

    /* Watermarks */
    if (state->system_used > state->peak_system_used)
        state->peak_system_used = state->system_used;
    if (state->tracked_used > state->peak_tracked_used)
        state->peak_tracked_used = state->tracked_used;

    /* Safe headroom: 10% of total */
    state->safe_headroom_kb = state->total_kb / 10;

    /* Alerts */
    state->alert_count = 0;
    if (state->available_kb < state->safe_headroom_kb) {
        snprintf(state->alerts[state->alert_count++], 128,
            "Available RAM (%lu MB) below 10%% headroom (%lu MB)",
            state->available_kb / 1024, state->safe_headroom_kb / 1024);
    }
    if (state->swap_total_kb > 0 && state->swap_free_kb == 0) {
        snprintf(state->alerts[state->alert_count++], 128,
            "Swap fully used — system may be thrashing");
    }

    return 0;
}

int jt_mem_track(JTMemState *state, void *ptr, size_t size,
                 const char *tag, bool is_gpu) {
    if (!state || !ptr || size == 0) return -1;
    if (state->alloc_count >= JT_MEM_MAX_ALLOCATIONS) return -2;

    JTMemAlloc *a = &state->allocs[state->alloc_count++];
    a->ptr = ptr;
    a->size = size;
    a->is_gpu = is_gpu;
    a->alloc_time = (uint32_t)time(NULL);
    if (tag) strncpy(a->tag, tag, sizeof(a->tag) - 1);

    return 0;
}

int jt_mem_untrack(JTMemState *state, void *ptr) {
    if (!state || !ptr) return -1;

    for (int i = 0; i < state->alloc_count; i++) {
        if (state->allocs[i].ptr == ptr) {
            /* Shift remaining */
            memmove(&state->allocs[i], &state->allocs[i+1],
                    (state->alloc_count - i - 1) * sizeof(JTMemAlloc));
            state->alloc_count--;
            return 0;
        }
    }
    return -2; /* not found */
}

bool jt_mem_can_alloc(JTMemState *state, size_t size) {
    if (!state) return false;
    size_t size_kb = size / 1024;
    /* Need: requested + 256MB headroom */
    return (state->available_kb > size_kb + 256 * 1024);
}

size_t jt_mem_safe_size(JTMemState *state) {
    if (!state) return 0;
    uint64_t available = state->available_kb - 256 * 1024; /* headroom */
    return (available > 0) ? (size_t)(available * 1024) : 0;
}

int jt_mem_pressure(const JTMemState *state) {
    if (!state) return 0;
    double pct = (double)state->system_used / (double)state->total_kb * 100.0;

    if (pct >= 95) return 3;  /* critical */
    if (pct >= 85) return 2;  /* red */
    if (pct >= 70) return 1;  /* yellow */
    return 0;                  /* green */
}

int jt_mem_suggest_gc(JTMemState *state, size_t needed, char *suggestions, int max_len) {
    if (!state || !suggestions) return -1;

    size_t available = (size_t)(state->available_kb * 1024);
    if (available >= needed) {
        snprintf(suggestions, max_len, "No GC needed — %zu MB available",
                 available / (1024*1024));
        return 0;
    }

    /* Sort by size descending (largest first) */
    JTMemAlloc sorted[JT_MEM_MAX_ALLOCATIONS];
    memcpy(sorted, state->allocs, state->alloc_count * sizeof(JTMemAlloc));
    /* Simple selection sort */
    for (int i = 0; i < state->alloc_count - 1; i++) {
        int max_idx = i;
        for (int j = i + 1; j < state->alloc_count; j++) {
            if (sorted[j].size > sorted[max_idx].size) max_idx = j;
        }
        if (max_idx != i) {
            JTMemAlloc tmp = sorted[i]; sorted[i] = sorted[max_idx]; sorted[max_idx] = tmp;
        }
    }

    int pos = 0;
    size_t freed = 0;
    pos += snprintf(suggestions + pos, max_len - pos,
        "GC suggestions (need %zu MB, have %zu MB):\n",
        needed / (1024*1024), available / (1024*1024));

    for (int i = 0; i < state->alloc_count && freed < needed - available; i++) {
        pos += snprintf(suggestions + pos, max_len - pos,
            "  Free '%s' (%zu MB, %s)\n",
            sorted[i].tag, sorted[i].size / (1024*1024),
            sorted[i].is_gpu ? "GPU" : "CPU");
        freed += sorted[i].size;
    }

    return pos;
}

int jt_mem_monitor_init(JTMemMonitor *mon) {
    if (!mon) return -1;
    memset(mon, 0, sizeof(*mon));
    return jt_mem_read(&mon->state);
}

int jt_mem_monitor_sample(JTMemMonitor *mon) {
    if (!mon) return -1;
    int rc = jt_mem_read(&mon->state);
    if (rc != 0) return rc;
    mon->sample_count++;
    int p = jt_mem_pressure(&mon->state);
    if (p >= 2) mon->oom_warnings++;
    return 0;
}

int jt_mem_report(const JTMemState *state, char *out, int max_len) {
    if (!state || !out) return -1;
    int pos = 0;
    const char *pressure_labels[] = {"GREEN", "YELLOW", "RED", "CRITICAL"};

    pos += snprintf(out + pos, max_len - pos,
        "# Jetson Memory Report\n\n"
        "## System\n"
        "- Total: %lu MB\n"
        "- Used: %lu MB (%.1f%%)\n"
        "- Available: %lu MB\n"
        "- Buffers: %lu MB\n"
        "- Cached: %lu MB\n"
        "- Swap: %lu/%lu MB\n"
        "- Pressure: %s\n\n",
        state->total_kb / 1024,
        state->system_used / 1024,
        (double)state->system_used / state->total_kb * 100.0,
        state->available_kb / 1024,
        state->buffers_kb / 1024,
        state->cached_kb / 1024,
        (state->swap_total_kb - state->swap_free_kb) / 1024,
        state->swap_total_kb / 1024,
        pressure_labels[jt_mem_pressure(state)]);

    pos += snprintf(out + pos, max_len - pos,
        "## Tracked Allocations (%d, %lu MB)\n\n",
        state->alloc_count, state->tracked_used / (1024*1024));

    for (int i = 0; i < state->alloc_count && pos < max_len - 64; i++) {
        JTMemAlloc *a = &state->allocs[i];
        pos += snprintf(out + pos, max_len - pos,
            "- %s: %zu MB (%s)\n", a->tag, a->size / (1024*1024),
            a->is_gpu ? "GPU" : "CPU");
    }

    pos += snprintf(out + pos, max_len - pos,
        "\n## Watermarks\n"
        "- Peak system: %lu MB\n"
        "- Peak tracked: %lu MB\n"
        "- Safe headroom: %lu MB\n",
        state->peak_system_used / 1024,
        state->peak_tracked_used / 1024,
        state->safe_headroom_kb / 1024);

    if (state->alert_count > 0) {
        pos += snprintf(out + pos, max_len - pos, "\n## Alerts\n");
        for (int i = 0; i < state->alert_count && pos < max_len - 64; i++) {
            pos += snprintf(out + pos, max_len - pos, "- %s\n", state->alerts[i]);
        }
    }

    return pos;
}

/* ═══ Tests ═══ */

int jt_mem_tracker_test(void) {
    int failures = 0;

    /* Test 1: Read on this Jetson */
    JTMemState state;
    memset(&state, 0, sizeof(state));
    int rc = jt_mem_read(&state);
    if (rc != 0) { failures++; printf("FAIL read: %d\n", rc); }
    else printf("  Total: %lu MB, Available: %lu MB\n",
               state.total_kb/1024, state.available_kb/1024);

    /* Test 2: Pressure */
    int p = jt_mem_pressure(&state);
    const char *labels[] = {"GREEN","YELLOW","RED","CRITICAL"};
    printf("  Pressure: %s\n", labels[p]);

    /* Test 3: Track allocations */
    char buf1[1024*1024]; /* 1MB */
    char buf2[2*1024*1024]; /* 2MB */
    rc = jt_mem_track(&state, buf1, sizeof(buf1), "test-buf-1", false);
    if (rc != 0) { failures++; printf("FAIL track\n"); }
    rc = jt_mem_track(&state, buf2, sizeof(buf2), "test-gpu-buf", true);
    if (rc != 0) { failures++; printf("FAIL track2\n"); }

    jt_mem_read(&state);  /* recalculate */
    printf("  Tracked: %d allocs, %lu KB\n", state.alloc_count, state.tracked_used/1024);
    if (state.estimated_gpu_used < 1024*1024) { failures++; printf("FAIL gpu tracking\n"); }

    /* Test 4: Untrack */
    rc = jt_mem_untrack(&state, buf1);
    if (rc != 0) { failures++; printf("FAIL untrack\n"); }
    if (state.alloc_count != 1) { failures++; printf("FAIL alloc count: %d\n", state.alloc_count); }

    /* Test 5: Can alloc */
    bool can = jt_mem_can_alloc(&state, 100 * 1024 * 1024);
    printf("  Can alloc 100MB: %s\n", can ? "yes" : "no");

    size_t safe = jt_mem_safe_size(&state);
    printf("  Safe size: %zu MB\n", safe / (1024*1024));

    /* Test 6: GC suggestion */
    char suggestions[512];
    jt_mem_suggest_gc(&state, 100 * 1024 * 1024 * 1024, suggestions, sizeof(suggestions));
    printf("  GC: %s", suggestions);

    /* Test 7: Monitor */
    JTMemMonitor mon;
    rc = jt_mem_monitor_init(&mon);
    if (rc != 0) { failures++; printf("FAIL monitor init\n"); }
    for (int i = 0; i < 3; i++) jt_mem_monitor_sample(&mon);
    printf("  Monitor: %d samples, %d warnings\n", mon.sample_count, mon.oom_warnings);

    /* Test 8: Report */
    char report[2048];
    rc = jt_mem_report(&state, report, sizeof(report));
    if (rc <= 0) { failures++; printf("FAIL report\n"); }
    else printf("  Report: %d bytes\n", rc);

    /* Test 9: Overflow */
    JTMemState small;
    memset(&small, 0, sizeof(small));
    small.total_kb = 100;
    for (int i = 0; i < JT_MEM_MAX_ALLOCATIONS + 5; i++) {
        rc = jt_mem_track(&small, &buf1, 1, "overflow", false);
        if (i >= JT_MEM_MAX_ALLOCATIONS && rc != -2) {
            failures++; printf("FAIL overflow at %d\n", i);
            break;
        }
    }

    /* Test 10: Null safety */
    rc = jt_mem_read(NULL);
    if (rc != -1) { failures++; printf("FAIL null read\n"); }

    printf("jt_mem_tracker_test: %d failures\n", failures);
    return failures;
}
