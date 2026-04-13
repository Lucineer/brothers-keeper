#include <unistd.h>
/**
 * jetson-power-thermal.c — Jetson power and thermal management
 *
 * Reads real sysfs paths that only exist on Jetson hardware.
 */

#define _POSIX_C_SOURCE 200809L
#include "jetson-power-thermal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

/* ═══ Helpers ═══ */

static int read_sysfs_int(const char *path, int *val) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%d", val) != 1) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int read_sysfs_str(const char *path, char *buf, int len) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, len, f)) { buf[0] = 0; fclose(f); return -1; }
    int l = strlen(buf);
    while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = 0;
    fclose(f);
    return l;
}

/* ═══ Discovered paths ═══ */

static struct {
    int gpu_thermal_zone;    /* index of GPU-therm zone */
    int cpu_thermal_zone;    /* index of CPU zone */
    int soc_thermal_zone;    /* index of SoC zone */
    int thermal_zones[JT_MAX_THERMAL_ZONES];  /* zone indices */
    int thermal_count;

    char freq_paths[JT_MAX_FREQ_DOMAINS][256]; /* cpufreq cur_freq paths */
    char freq_max_paths[JT_MAX_FREQ_DOMAINS][256];
    char freq_names[JT_MAX_FREQ_DOMAINS][32];
    int freq_count;
} discovered = {0};

/* ═══ Probe ═══ */

int jt_probe_power(JTPowerState *state) {
    if (!state) return -1;
    memset(state, 0, sizeof(*state));

    /* Scan thermal zones */
    discovered.thermal_count = 0;
    discovered.gpu_thermal_zone = -1;
    discovered.cpu_thermal_zone = -1;
    discovered.soc_thermal_zone = -1;

    char type_path[128], temp_path[128];
    for (int i = 0; i < JT_MAX_THERMAL_ZONES; i++) {
        snprintf(type_path, sizeof(type_path),
            "/sys/devices/virtual/thermal/thermal_zone%d/type", i);
        snprintf(temp_path, sizeof(temp_path),
            "/sys/devices/virtual/thermal/thermal_zone%d/temp", i);

        char type[64];
        if (read_sysfs_str(type_path, type, sizeof(type)) > 0) {
            int idx = discovered.thermal_count;
            strncpy(state->thermal_names[idx], type, sizeof(state->thermal_names[idx]) - 1);
            discovered.thermal_zones[idx] = i;
            discovered.thermal_count++;

            /* Classify */
            if (strstr(type, "gpu")) discovered.gpu_thermal_zone = i;
            else if (strstr(type, "cpu")) discovered.cpu_thermal_zone = i;
            else if (strstr(type, "soc") || strstr(type, "SoC") ||
                     strstr(type, "soc") || strstr(type, "thermal-fan-est"))
                discovered.soc_thermal_zone = i;

            if (discovered.thermal_count >= JT_MAX_THERMAL_ZONES) break;
        }
    }
    state->thermal_count = discovered.thermal_count;

    /* Scan CPU frequency domains */
    discovered.freq_count = 0;
    char policy_path[256];
    DIR *dir = opendir("/sys/devices/system/cpu/cpufreq");
    if (!dir) {
        /* Fallback: scan cpu0-cpu7 */
        for (int i = 0; i < 8 && discovered.freq_count < JT_MAX_FREQ_DOMAINS; i++) {
            snprintf(policy_path, sizeof(policy_path),
                "/sys/devices/system/cpu/cpu%d/cpufreq", i);
            if (access(policy_path, F_OK) == 0) {
                int idx = discovered.freq_count;
                snprintf(discovered.freq_names[idx], sizeof(discovered.freq_names[idx]),
                    "cpu%d", i);
                snprintf(discovered.freq_paths[idx], sizeof(discovered.freq_paths[idx]),
                    "%s/scaling_cur_freq", policy_path);
                snprintf(discovered.freq_max_paths[idx], sizeof(discovered.freq_max_paths[idx]),
                    "%s/cpuinfo_max_freq", policy_path);
                discovered.freq_count++;
            }
        }
    } else {
        closedir(dir);
        /* List policy directories */
        dir = opendir("/sys/devices/system/cpu/cpufreq");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) && discovered.freq_count < JT_MAX_FREQ_DOMAINS) {
                if (ent->d_name[0] != '.') {
                    int idx = discovered.freq_count;
                    strncpy(discovered.freq_names[idx], ent->d_name,
                            sizeof(discovered.freq_names[idx]) - 1);
                    snprintf(discovered.freq_paths[idx], sizeof(discovered.freq_paths[idx]),
                        "/sys/devices/system/cpu/cpufreq/%s/scaling_cur_freq", ent->d_name);
                    snprintf(discovered.freq_max_paths[idx], sizeof(discovered.freq_max_paths[idx]),
                        "/sys/devices/system/cpu/cpufreq/%s/cpuinfo_max_freq", ent->d_name);
                    discovered.freq_count++;
                }
            }
            closedir(dir);
        }
    }

    /* GPU freq domain */
    if (discovered.freq_count < JT_MAX_FREQ_DOMAINS) {
        int idx = discovered.freq_count;
        strncpy(discovered.freq_names[idx], "gpu", sizeof(discovered.freq_names[idx]) - 1);
        strncpy(discovered.freq_paths[idx],
            "/sys/class/devfreq/17000000.gv11b/cur_freq", sizeof(discovered.freq_paths[idx]) - 1);
        strncpy(discovered.freq_max_paths[idx],
            "/sys/class/devfreq/17000000.gv11b/max_freq", sizeof(discovered.freq_max_paths[idx]) - 1);
        /* Check if path exists, if not try alternate */
        if (access(discovered.freq_paths[idx], F_OK) != 0) {
            strncpy(discovered.freq_paths[idx],
                "/sys/class/devfreq/17000000.ga10b/cur_freq", sizeof(discovered.freq_paths[idx]) - 1);
            strncpy(discovered.freq_max_paths[idx],
                "/sys/class/devfreq/17000000.ga10b/max_freq", sizeof(discovered.freq_max_paths[idx]) - 1);
        }
        if (access(discovered.freq_paths[idx], F_OK) == 0) {
            discovered.freq_count++;
        }
    }

    state->freq_count = discovered.freq_count;
    for (int i = 0; i < discovered.freq_count; i++) {
        strncpy(state->freq_names[i], discovered.freq_names[i],
                sizeof(state->freq_names[i]) - 1);
    }

    /* Power mode */
    state->power_mode = JT_POWER_MODE_MAXN;
    strncpy(state->power_mode_str, "MAXN", sizeof(state->power_mode_str));

    return jt_read_power(state);
}

/* ═══ Read current state ═══ */

int jt_read_power(JTPowerState *state) {
    if (!state) return -1;

    /* Read thermal zone temps */
    for (int i = 0; i < discovered.thermal_count && i < state->thermal_count; i++) {
        int zone = discovered.thermal_zones[i];
        char path[128];
        snprintf(path, sizeof(path),
            "/sys/devices/virtual/thermal/thermal_zone%d/temp", zone);
        int temp = 0;
        if (read_sysfs_int(path, &temp) == 0) {
            state->thermal_temps[i] = (uint32_t)temp;
        }
    }

    /* GPU temp */
    state->gpu_temp_mc = 0;
    if (discovered.gpu_thermal_zone >= 0) {
        int zone = discovered.gpu_thermal_zone;
        for (int i = 0; i < discovered.thermal_count; i++) {
            if (discovered.thermal_zones[i] == zone) {
                state->gpu_temp_mc = state->thermal_temps[i];
                break;
            }
        }
    }

    /* CPU temp */
    state->cpu_temp_mc = 0;
    if (discovered.cpu_thermal_zone >= 0) {
        int zone = discovered.cpu_thermal_zone;
        for (int i = 0; i < discovered.thermal_count; i++) {
            if (discovered.thermal_zones[i] == zone) {
                state->cpu_temp_mc = state->thermal_temps[i];
                break;
            }
        }
    }

    /* SoC temp */
    state->soc_temp_mc = 0;
    if (discovered.soc_thermal_zone >= 0) {
        int zone = discovered.soc_thermal_zone;
        for (int i = 0; i < discovered.thermal_count; i++) {
            if (discovered.thermal_zones[i] == zone) {
                state->soc_temp_mc = state->thermal_temps[i];
                break;
            }
        }
    }

    /* Frequency domains */
    for (int i = 0; i < discovered.freq_count && i < JT_MAX_FREQ_DOMAINS; i++) {
        int val = 0;
        if (read_sysfs_int(discovered.freq_paths[i], &val) == 0) {
            state->freq_current[i] = (uint32_t)val;
        }
        val = 0;
        if (read_sysfs_int(discovered.freq_max_paths[i], &val) == 0) {
            state->freq_max[i] = (uint32_t)val;
        }
    }

    /* Over-temp check */
    state->any_over_temp = (state->gpu_temp_mc >= JT_TEMP_THROTTLE_MC ||
                            state->cpu_temp_mc >= JT_TEMP_THROTTLE_MC);
    state->gpu_throttled = (state->gpu_temp_mc >= JT_TEMP_THROTTLE_MC);
    state->cpu_throttled = (state->cpu_temp_mc >= JT_TEMP_THROTTLE_MC);

    return 0;
}

/* ═══ Power mode ═══ */

int jt_set_power_mode(uint8_t mode) {
    if (mode > JT_POWER_MODE_MAXN) return -1;

    /* Try nvpmodel first */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "nvpmodel -m %d 2>/dev/null", mode);
    int rc = system(cmd);

    if (rc != 0) {
        /* Try sysfs directly */
        const char *path = "/sys/devices/platform/jetson-pmu/power_mode";
        FILE *f = fopen(path, "w");
        if (!f) return -2;
        fprintf(f, "%d", mode);
        fclose(f);
    }

    return 0;
}

/* ═══ Over temp ═══ */

bool jt_is_over_temp(const JTPowerState *state, uint32_t threshold_mc) {
    if (!state) return false;
    return (state->gpu_temp_mc >= threshold_mc ||
            state->cpu_temp_mc >= threshold_mc ||
            state->soc_temp_mc >= threshold_mc);
}

/* ═══ Thermal monitor ═══ */

int jt_thermal_monitor_init(JTThermalMonitor *mon) {
    if (!mon) return -1;
    memset(mon, 0, sizeof(*mon));
    return jt_probe_power(&mon->current);
}

int jt_thermal_monitor_sample(JTThermalMonitor *mon) {
    if (!mon) return -1;

    int rc = jt_read_power(&mon->current);
    if (rc != 0) return rc;

    mon->sample_count++;

    /* Track peaks */
    if (mon->current.gpu_temp_mc > mon->peak.gpu_temp_mc)
        mon->peak.gpu_temp_mc = mon->current.gpu_temp_mc;
    if (mon->current.cpu_temp_mc > mon->peak.cpu_temp_mc)
        mon->peak.cpu_temp_mc = mon->current.cpu_temp_mc;
    if (mon->current.soc_temp_mc > mon->peak.soc_temp_mc)
        mon->peak.soc_temp_mc = mon->current.soc_temp_mc;

    /* Track events */
    if (mon->current.gpu_throttled) mon->throttle_events++;
    if (mon->current.any_over_temp) mon->over_temp_events++;

    return 0;
}

/* ═══ Recommendation ═══ */

int jt_thermal_recommendation(const JTPowerState *state) {
    if (!state) return 0;

    if (state->gpu_temp_mc >= JT_TEMP_CRITICAL_MC ||
        state->cpu_temp_mc >= JT_TEMP_CRITICAL_MC)
        return 3; /* emergency: halt GPU work */

    if (state->gpu_temp_mc >= JT_TEMP_THROTTLE_MC + 5000)
        return 2; /* drop workload */

    if (state->gpu_temp_mc >= JT_TEMP_THROTTLE_MC)
        return 1; /* reduce frequency */

    if (state->gpu_temp_mc >= JT_TEMP_WARN_MC)
        return 1; /* pre-emptive reduce */

    return 0; /* OK */
}

/* ═══ Find helpers ═══ */

int jt_find_thermal_zone(const char *name, uint32_t *temp_mc) {
    if (!name || !temp_mc) return -1;
    for (int i = 0; i < discovered.thermal_count; i++) {
        if (strstr(discovered.freq_names[i], name)) {
            /* wrong array — use thermal names */
        }
    }
    /* Re-search properly */
    char path[128], type[64];
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path),
            "/sys/devices/virtual/thermal/thermal_zone%d/type", i);
        if (read_sysfs_str(path, type, sizeof(type)) > 0) {
            if (strstr(type, name)) {
                snprintf(path, sizeof(path),
                    "/sys/devices/virtual/thermal/thermal_zone%d/temp", i);
                int val = 0;
                if (read_sysfs_int(path, &val) == 0) {
                    *temp_mc = (uint32_t)val;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int jt_find_freq_domain(const char *name, uint32_t *current_khz, uint32_t *max_khz) {
    if (!name) return -1;
    for (int i = 0; i < discovered.freq_count; i++) {
        if (strstr(discovered.freq_names[i], name)) {
            if (current_khz) *current_khz = 0;
            if (max_khz) *max_khz = 0;
            int val = 0;
            if (read_sysfs_int(discovered.freq_paths[i], &val) == 0 && current_khz)
                *current_khz = (uint32_t)val;
            val = 0;
            if (read_sysfs_int(discovered.freq_max_paths[i], &val) == 0 && max_khz)
                *max_khz = (uint32_t)val;
            return 0;
        }
    }
    return -1;
}

/* ═══ Report ═══ */

int jt_power_report(const JTPowerState *state, char *out, int max_len) {
    if (!state || !out) return -1;
    int pos = 0;

    pos += snprintf(out + pos, max_len - pos,
        "# Jetson Power & Thermal Report\n\n"
        "## Temperatures\n"
        "- GPU: %.1f°C%s\n"
        "- CPU: %.1f°C%s\n"
        "- SoC: %.1f°C\n\n",
        state->gpu_temp_mc / 1000.0,
        state->gpu_throttled ? " ⚠️ THROTTLING" : "",
        state->cpu_temp_mc / 1000.0,
        state->cpu_throttled ? " ⚠️ THROTTLING" : "",
        state->soc_temp_mc / 1000.0);

    pos += snprintf(out + pos, max_len - pos,
        "## Power Mode: %s\n\n"
        "## Frequency Domains\n",
        state->power_mode_str);

    for (int i = 0; i < state->freq_count && pos < max_len - 64; i++) {
        pos += snprintf(out + pos, max_len - pos,
            "- %s: %u/%u MHz\n",
            state->freq_names[i],
            state->freq_current[i] / 1000,
            state->freq_max[i] / 1000);
    }

    pos += snprintf(out + pos, max_len - pos,
        "\n## All Thermal Zones (%d)\n\n", state->thermal_count);
    for (int i = 0; i < state->thermal_count && pos < max_len - 64; i++) {
        pos += snprintf(out + pos, max_len - pos,
            "- %s: %.1f°C\n",
            state->thermal_names[i],
            state->thermal_temps[i] / 1000.0);
    }

    return pos;
}

/* ═══ Tests ═══ */

int jt_power_thermal_test(void) {
    int failures = 0;

    /* Test 1: Probe on this Jetson */
    JTPowerState state;
    int rc = jt_probe_power(&state);
    if (rc != 0) { failures++; printf("FAIL probe: %d\n", rc); }
    else printf("  Probed: %d thermal zones, %d freq domains\n",
                state.thermal_count, state.freq_count);

    /* Test 2: Should find thermal zones */
    if (state.thermal_count == 0) {
        failures++; printf("FAIL no thermal zones\n");
    } else {
        printf("  GPU temp: %.1f°C\n", state.gpu_temp_mc / 1000.0);
        printf("  CPU temp: %.1f°C\n", state.cpu_temp_mc / 1000.0);
        printf("  SoC temp: %.1f°C\n", state.soc_temp_mc / 1000.0);
    }

    /* Test 3: Should find freq domains */
    if (state.freq_count == 0) {
        failures++; printf("FAIL no freq domains\n");
    } else {
        for (int i = 0; i < state.freq_count; i++) {
            printf("  Freq %s: %u/%u MHz\n",
                   state.freq_names[i],
                   state.freq_current[i] / 1000,
                   state.freq_max[i] / 1000);
        }
    }

    /* Test 4: Over-temp check */
    bool hot = jt_is_over_temp(&state, JT_TEMP_THROTTLE_MC);
    printf("  Over 80C: %s\n", hot ? "YES" : "no");

    /* Test 5: Recommendation */
    int rec = jt_thermal_recommendation(&state);
    const char *recs[] = {"OK", "reduce_freq", "drop_workload", "emergency"};
    printf("  Recommendation: %s\n", recs[rec]);

    /* Test 6: Monitor */
    JTThermalMonitor mon;
    rc = jt_thermal_monitor_init(&mon);
    if (rc != 0) { failures++; printf("FAIL monitor init\n"); }

    for (int i = 0; i < 5; i++) {
        jt_thermal_monitor_sample(&mon);
    }
    printf("  Monitor: %d samples, %d throttle events, peak GPU %.1f°C\n",
           mon.sample_count, mon.throttle_events,
           mon.peak.gpu_temp_mc / 1000.0);

    /* Test 7: Find GPU thermal zone */
    uint32_t gpu_temp = 0;
    rc = jt_find_thermal_zone("GPU", &gpu_temp);
    if (rc == 0) {
        printf("  Find GPU zone: %.1f°C\n", gpu_temp / 1000.0);
    } else {
        printf("  Find GPU zone: not found (OK on non-Jetson)\n");
    }

    /* Test 8: Report */
    char report[2048];
    rc = jt_power_report(&state, report, sizeof(report));
    if (rc <= 0) { failures++; printf("FAIL report\n"); }
    else printf("  Report: %d bytes\n", rc);

    /* Test 9: Null safety */
    rc = jt_probe_power(NULL);
    if (rc != -1) { failures++; printf("FAIL null probe\n"); }
    hot = jt_is_over_temp(NULL, 80000);
    if (hot) { failures++; printf("FAIL null over_temp\n"); }

    /* Test 10: Re-read (should be fast) */
    rc = jt_read_power(&state);
    if (rc != 0) { failures++; printf("FAIL re-read\n"); }

    printf("jt_power_thermal_test: %d failures\n", failures);
    return failures;
}
