#include <time.h>
/**
 * jetson-cuda-governor.c — GPU memory pressure valve + thermal-aware batching
 *
 * The core insight: on Jetson, GPU memory IS system memory.
 * A CUDA alloc can trigger the Linux OOM killer.
 * This governor prevents that by tracking pressure and preempting.
 */

#include "jetson-cuda-governor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ═══ Helpers ═══ */

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

static uint32_t read_thermal_mc(const char *type_substring) {
    /* Scan thermal zones for matching type */
    char type_path[128], temp_path[128], type[64];
    for (int i = 0; i < 16; i++) {
        snprintf(type_path, sizeof(type_path),
            "/sys/devices/virtual/thermal/thermal_zone%d/type", i);
        FILE *f = fopen(type_path, "r");
        if (!f) continue;
        if (!fgets(type, sizeof(type), f)) { fclose(f); continue; }
        fclose(f);
        int l = strlen(type);
        while (l > 0 && (type[l-1]=='\n'||type[l-1]=='\r')) type[--l]=0;

        if (strstr(type, type_substring)) {
            snprintf(temp_path, sizeof(temp_path),
                "/sys/devices/virtual/thermal/thermal_zone%d/temp", i);
            f = fopen(temp_path, "r");
            if (!f) return 0;
            int val = 0;
            if (fscanf(f, "%d", &val) != 1) val = 0;
            fclose(f);
            return (uint32_t)val;
        }
    }
    return 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ═══ Init ═══ */

int jt_gpu_gov_init(JTGPUGovernor *gov) {
    if (!gov) return -1;
    memset(gov, 0, sizeof(*gov));

    gov->total_gpu_mem_mb = parse_meminfo_kb("MemTotal:") / 1024;
    gov->throttle_temp_mc = 80000;
    gov->reserved_mb = JT_GPU_SAFE_HEADROOM_MB;

    return jt_gpu_gov_sample(gov);
}

/* ═══ Sample ═══ */

int jt_gpu_gov_sample(JTGPUGovernor *gov) {
    if (!gov) return -1;

    uint64_t available = parse_meminfo_kb("MemAvailable:") / 1024;
    uint64_t total = parse_meminfo_kb("MemTotal:") / 1024;
    gov->total_gpu_mem_mb = total;
    gov->free_gpu_mem_mb = available;
    gov->used_gpu_mem_mb = total - available;

    /* Thermal */
    gov->current_temp_mc = read_thermal_mc("gpu");
    uint32_t tj = read_thermal_mc("tj");
    if (tj > gov->current_temp_mc) gov->current_temp_mc = tj;

    /* History */
    int idx = gov->history_idx % JT_GPU_HISTORY_LEN;
    gov->history[idx].timestamp = now_ms();
    gov->history[idx].used_mb = gov->used_gpu_mem_mb;
    gov->history[idx].temp_mc = gov->current_temp_mc;
    gov->history_idx++;
    if (gov->history_count < JT_GPU_HISTORY_LEN) gov->history_count++;

    /* Thermal trajectory (rate of change over last 10 samples) */
    gov->temp_rate_mc_per_s = 0;
    gov->trajectory = JT_THERM_STABLE;
    if (gov->history_count >= 3) {
        int prev_idx = (gov->history_idx - 3) % JT_GPU_HISTORY_LEN;
        int curr_idx = (gov->history_idx - 1) % JT_GPU_HISTORY_LEN;
        int32_t dt = (gov->history[curr_idx].timestamp -
                      gov->history[prev_idx].timestamp) / 1000;
        if (dt > 0) {
            gov->temp_rate_mc_per_s =
                (int32_t)(gov->history[curr_idx].temp_mc -
                          gov->history[prev_idx].temp_mc) / dt;
        }
        if (gov->temp_rate_mc_per_s > 200) gov->trajectory = JT_THERM_SURGING;
        else if (gov->temp_rate_mc_per_s > 50) gov->trajectory = JT_THERM_RISING;
        else if (gov->temp_rate_mc_per_s < -50) gov->trajectory = JT_THERM_COOLING;
    }

    /* Seconds to throttle estimate */
    int32_t headroom = (int32_t)(gov->throttle_temp_mc - gov->current_temp_mc);
    if (gov->temp_rate_mc_per_s > 0 && headroom > 0) {
        gov->seconds_to_throttle = (uint32_t)(headroom / gov->temp_rate_mc_per_s);
    } else {
        gov->seconds_to_throttle = 999; /* effectively infinite */
    }

    /* Memory pressure */
    uint64_t headroom_mb = gov->free_gpu_mem_mb - gov->reserved_mb;
    if (headroom_mb > 1024) gov->pressure = JT_GPU_GREEN;
    else if (headroom_mb > 512) gov->pressure = JT_GPU_YELLOW;
    else if (headroom_mb > JT_GPU_CRITICAL_MB) gov->pressure = JT_GPU_ORANGE;
    else if (headroom_mb > 128) gov->pressure = JT_GPU_RED;
    else gov->pressure = JT_GPU_CRITICAL;

    /* Sum agent allocations */
    gov->reserved_mb = JT_GPU_SAFE_HEADROOM_MB;
    for (int i = 0; i < gov->agent_count; i++) {
        gov->reserved_mb += gov->agent_gpu_allocs[i];
    }

    return 0;
}

/* ═══ Pressure ═══ */

JTGPUPressure jt_gpu_pressure(const JTGPUGovernor *gov) {
    return gov ? gov->pressure : JT_GPU_CRITICAL;
}

bool jt_gpu_can_alloc(const JTGPUGovernor *gov, uint64_t mb) {
    if (!gov) return false;
    return (gov->free_gpu_mem_mb > mb + gov->reserved_mb + JT_GPU_CRITICAL_MB);
}

/* ═══ Agent reservations ═══ */

int jt_gpu_reserve(JTGPUGovernor *gov, const char *agent, uint64_t mb) {
    if (!gov || !agent) return -1;
    for (int i = 0; i < gov->agent_count; i++) {
        if (strcmp(gov->agent_names[i], agent) == 0) {
            gov->agent_gpu_allocs[i] = mb;
            return 0;
        }
    }
    if (gov->agent_count >= JT_GPU_MAX_AGENTS) return -2;
    int i = gov->agent_count++;
    strncpy(gov->agent_names[i], agent, sizeof(gov->agent_names[i]) - 1);
    gov->agent_gpu_allocs[i] = mb;
    gov->agent_stream_priorities[i] = 5; /* default mid */
    return 0;
}

int jt_gpu_release(JTGPUGovernor *gov, const char *agent) {
    if (!gov || !agent) return -1;
    for (int i = 0; i < gov->agent_count; i++) {
        if (strcmp(gov->agent_names[i], agent) == 0) {
            gov->agent_gpu_allocs[i] = 0;
            return 0;
        }
    }
    return -2;
}

/* ═══ Preemption ═══ */

const char *jt_gpu_preempt(JTGPUGovernor *gov) {
    if (!gov || gov->pressure < JT_GPU_ORANGE) return NULL;

    /* Find lowest priority agent with GPU alloc */
    int lowest = -1;
    uint32_t lowest_pri = 999;
    for (int i = 0; i < gov->agent_count; i++) {
        if (gov->agent_gpu_allocs[i] > 0 &&
            gov->agent_stream_priorities[i] < lowest_pri) {
            lowest = i;
            lowest_pri = gov->agent_stream_priorities[i];
        }
    }

    if (lowest >= 0) {
        gov->preemptions++;
        return gov->agent_names[lowest];
    }
    return NULL;
}

/* ═══ Thermal-aware batching ═══ */

JTThermTrajectory jt_gpu_thermal_trajectory(const JTGPUGovernor *gov) {
    return gov ? gov->trajectory : JT_THERM_STABLE;
}

uint32_t jt_gpu_seconds_to_throttle(const JTGPUGovernor *gov) {
    return gov ? gov->seconds_to_throttle : 0;
}

uint32_t jt_gpu_safe_batch(const JTGPUGovernor *gov, uint32_t base_batch) {
    if (!gov) return base_batch / 4;

    /* Memory-limited batch */
    uint64_t avail = gov->free_gpu_mem_mb - gov->reserved_mb;
    if (avail < 256) return 1;

    float mem_factor = (avail > 1024) ? 1.0f : (float)avail / 1024.0f;

    /* Thermal-limited batch */
    float therm_factor = 1.0f;
    if (gov->trajectory == JT_THERM_SURGING) therm_factor = 0.25f;
    else if (gov->trajectory == JT_THERM_RISING) therm_factor = 0.5f;

    /* Pressure-limited */
    float pres_factor = 1.0f;
    if (gov->pressure == JT_GPU_RED) pres_factor = 0.1f;
    else if (gov->pressure == JT_GPU_ORANGE) pres_factor = 0.5f;

    uint32_t safe = (uint32_t)(base_batch * mem_factor * therm_factor * pres_factor);
    return safe < 1 ? 1 : safe;
}

uint32_t jt_gpu_thermal_safe_batch(const JTGPUGovernor *gov,
                                    uint32_t base_batch,
                                    uint32_t batch_time_ms) {
    if (!gov) return 1;
    if (gov->trajectory == JT_THERM_STABLE || gov->trajectory == JT_THERM_COOLING) {
        return jt_gpu_safe_batch(gov, base_batch);
    }

    /* How many batches can we fit before throttle? */
    uint32_t time_budget_ms = gov->seconds_to_throttle * 1000;
    if (time_budget_ms < batch_time_ms * 2) return 1; /* not enough time for 2 batches */

    uint32_t max_by_time = time_budget_ms / batch_time_ms;
    uint32_t max_by_mem = jt_gpu_safe_batch(gov, base_batch);

    return max_by_time < max_by_mem ? max_by_time : max_by_mem;
}

/* ═══ Multi-agent fair share ═══ */

int jt_gpu_register_agent(JTGPUGovernor *gov, const char *name, uint32_t priority) {
    if (!gov || !name) return -1;
    /* Check if already registered */
    for (int i = 0; i < gov->agent_count; i++) {
        if (strcmp(gov->agent_names[i], name) == 0) {
            gov->agent_stream_priorities[i] = priority;
            return 0;
        }
    }
    if (gov->agent_count >= JT_GPU_MAX_AGENTS) return -2;
    int i = gov->agent_count++;
    strncpy(gov->agent_names[i], name, sizeof(gov->agent_names[i]) - 1);
    gov->agent_gpu_allocs[i] = 0;
    gov->agent_stream_priorities[i] = priority;
    return 0;
}

uint32_t jt_gpu_timeslice(JTGPUGovernor *gov, const char *agent) {
    if (!gov || !agent) return 100; /* default 100ms */

    /* Find agent priority */
    int my_idx = -1;
    uint32_t my_pri = 5;
    for (int i = 0; i < gov->agent_count; i++) {
        if (strcmp(gov->agent_names[i], agent) == 0) {
            my_idx = i;
            my_pri = gov->agent_stream_priorities[i];
            break;
        }
    }
    if (my_idx < 0) return 100;

    /* Weighted timeslice: priority/total_priority * budget */
    uint32_t total_pri = 0;
    int active_agents = 0;
    for (int i = 0; i < gov->agent_count; i++) {
        if (gov->agent_gpu_allocs[i] > 0 || i == my_idx) {
            total_pri += gov->agent_stream_priorities[i];
            active_agents++;
        }
    }

    if (total_pri == 0) return 500;
    uint32_t budget = 5000; /* 5 second round-robin cycle */
    return (my_pri * budget) / total_pri;
}

/* ═══ Report ═══ */

int jt_gpu_gov_report(const JTGPUGovernor *gov, char *out, int max_len) {
    if (!gov || !out) return -1;
    const char *pressure_labels[] = {"GREEN","YELLOW","ORANGE","RED","CRITICAL"};
    const char *therm_labels[] = {"COOLING","STABLE","RISING","SURGING"};

    int pos = snprintf(out, max_len,
        "# GPU Governor Report\n\n"
        "## Memory\n"
        "- Total: %lu MB, Used: %lu MB, Free: %lu MB\n"
        "- Reserved: %lu MB\n"
        "- Pressure: %s\n\n"
        "## Thermal\n"
        "- GPU: %.1f°C (%.1f°C/s)\n"
        "- Trajectory: %s\n"
        "- Seconds to throttle: %u\n\n"
        "## Agents (%d)\n",
        gov->total_gpu_mem_mb, gov->used_gpu_mem_mb, gov->free_gpu_mem_mb,
        gov->reserved_mb, pressure_labels[gov->pressure],
        gov->current_temp_mc / 1000.0, gov->temp_rate_mc_per_s / 1000.0,
        therm_labels[gov->trajectory],
        gov->seconds_to_throttle,
        gov->agent_count);

    for (int i = 0; i < gov->agent_count && pos < max_len - 64; i++) {
        pos += snprintf(out + pos, max_len - pos,
            "- %s: %lu MB, priority=%u, timeslice=%ums\n",
            gov->agent_names[i], gov->agent_gpu_allocs[i],
            gov->agent_stream_priorities[i],
            jt_gpu_timeslice(gov, gov->agent_names[i]));
    }

    pos += snprintf(out + pos, max_len - pos,
        "\n## Stats\n"
        "- Preemptions: %u, Batch reductions: %u, OOM saves: %u\n",
        gov->preemptions, gov->batch_reductions, gov->oom_saves);

    return pos;
}

/* ═══ Tests ═══ */

int jt_gpu_governor_test(void) {
    int failures = 0;

    /* Test 1: Init */
    JTGPUGovernor gov;
    int rc = jt_gpu_gov_init(&gov);
    if (rc != 0) { failures++; printf("FAIL init\n"); }
    else printf("  Init: total=%lu MB, free=%lu MB\n",
               gov.total_gpu_mem_mb, gov.free_gpu_mem_mb);

    /* Test 2: Sample (build history) */
    for (int i = 0; i < 5; i++) {
        jt_gpu_gov_sample(&gov);
        usleep(100000); /* 100ms between samples */
    }
    printf("  History: %d samples\n", gov.history_count);

    /* Test 3: Pressure */
    JTGPUPressure p = jt_gpu_pressure(&gov);
    const char *pl[] = {"GREEN","YELLOW","ORANGE","RED","CRITICAL"};
    printf("  Pressure: %s\n", pl[p]);

    /* Test 4: Can alloc */
    bool can = jt_gpu_can_alloc(&gov, 100);
    printf("  Can alloc 100MB: %s\n", can ? "yes" : "no");

    /* Test 5: Register agents */
    rc = jt_gpu_reserve(&gov, "flux-runtime", 512);
    if (rc != 0) { failures++; printf("FAIL reserve\n"); }
    rc = jt_gpu_reserve(&gov, "craftmind", 256);
    if (rc != 0) { failures++; printf("FAIL reserve2\n"); }
    printf("  Agents: %d registered\n", gov.agent_count);

    /* Test 6: Release */
    rc = jt_gpu_release(&gov, "craftmind");
    if (rc != 0) { failures++; printf("FAIL release\n"); }

    /* Test 7: Safe batch */
    uint32_t safe = jt_gpu_safe_batch(&gov, 32);
    printf("  Safe batch (base=32): %u\n", safe);

    /* Test 8: Thermal trajectory */
    const char *tl[] = {"COOLING","STABLE","RISING","SURGING"};
    printf("  Thermal: %.1f°C, trajectory=%s, rate=%.1f°C/s\n",
           gov.current_temp_mc / 1000.0, tl[gov.trajectory],
           gov.temp_rate_mc_per_s / 1000.0);

    /* Test 9: Thermal-safe batch */
    uint32_t tsb = jt_gpu_thermal_safe_batch(&gov, 32, 5000);
    printf("  Thermal-safe batch (5s per batch): %u\n", tsb);

    /* Test 10: Preemption (should return NULL if pressure is GREEN) */
    const char *preempted = jt_gpu_preempt(&gov);
    if (preempted) printf("  Preempted: %s\n", preempted);
    else printf("  Preempt: none needed (pressure=%s)\n", pl[gov.pressure]);

    /* Test 11: Simulate pressure */
    /* Manually set used to near-total */
    uint64_t real_free = gov.free_gpu_mem_mb;
    gov.free_gpu_mem_mb = 200; /* only 200MB free */
    gov.used_gpu_mem_mb = gov.total_gpu_mem_mb - 200;
    /* Recalculate pressure */
    int64_t headroom_mb = (int64_t)gov.free_gpu_mem_mb - (int64_t)gov.reserved_mb;
    if (headroom_mb > 1024) gov.pressure = JT_GPU_GREEN;
    else if (headroom_mb > 512) gov.pressure = JT_GPU_YELLOW;
    else if (headroom_mb > 256) gov.pressure = JT_GPU_ORANGE;
    else if (headroom_mb > 128) gov.pressure = JT_GPU_RED;
    else gov.pressure = JT_GPU_CRITICAL;
    p = jt_gpu_pressure(&gov);
    printf("  Simulated RED pressure: %s\n", pl[p]);

    preempted = jt_gpu_preempt(&gov);
    if (preempted) printf("  Simulated preempt: %s\n", preempted);
    else printf("  No agent to preempt (expected)\n");

    uint32_t safe_red = jt_gpu_safe_batch(&gov, 32);
    printf("  Safe batch under pressure: %u\n", safe_red);

    /* Restore */
    gov.free_gpu_mem_mb = real_free;
    gov.used_gpu_mem_mb = gov.total_gpu_mem_mb - real_free;

    /* Test 12: Timeslice */
    rc = jt_gpu_register_agent(&gov, "high-pri", 10);
    rc = jt_gpu_register_agent(&gov, "low-pri", 2);
    uint32_t ts_high = jt_gpu_timeslice(&gov, "high-pri");
    uint32_t ts_low = jt_gpu_timeslice(&gov, "low-pri");
    printf("  Timeslices: high=%ums, low=%ums\n", ts_high, ts_low);
    if (ts_high <= ts_low) { failures++; printf("FAIL timeslice priority\n"); }

    /* Test 13: Report */
    char report[2048];
    rc = jt_gpu_gov_report(&gov, report, sizeof(report));
    if (rc <= 0) { failures++; printf("FAIL report\n"); }
    else printf("  Report: %d bytes\n", rc);

    /* Test 14: Null safety */
    if (jt_gpu_gov_init(NULL) != -1) { failures++; printf("FAIL null init\n"); }
    if (jt_gpu_pressure(NULL) != JT_GPU_CRITICAL) { failures++; printf("FAIL null pressure\n"); }
    if (jt_gpu_can_alloc(NULL, 100)) { failures++; printf("FAIL null can_alloc\n"); }
    printf("  Null safety: OK\n");

    printf("jt_gpu_governor_test: %d failures\n", failures);
    return failures;
}
