/**
 * jetson-power-thermal.h — Jetson power mode and thermal management
 *
 * Reads GPU/CPU/SoC temperatures from sysfs.
 * Detects thermal throttling. Controls power mode.
 * These sysfs paths are Jetson-exclusive — no cloud VM has them.
 *
 * Zero deps. C99. Static allocation.
 */

#ifndef JETSON_POWER_THERMAL_H
#define JETSON_POWER_THERMAL_H

#include <stdint.h>
#include <stdbool.h>

#define JT_TEMP_WARN_MC   75000   /* 75C warning */
#define JT_TEMP_THROTTLE_MC 80000 /* 80C throttle */
#define JT_TEMP_CRITICAL_MC 90000 /* 90C critical */

#define JT_POWER_MODE_10W  0
#define JT_POWER_MODE_15W  1
#define JT_POWER_MODE_MAXN 2
#define JT_POWER_MODE_COUNT 3

#define JT_MAX_THERMAL_ZONES 16
#define JT_MAX_FREQ_DOMAINS  8

typedef struct {
    /* Temperatures (milli-Celsius) */
    uint32_t gpu_temp_mc;
    uint32_t cpu_temp_mc;
    uint32_t soc_temp_mc;
    uint32_t gpu_throttle_mc;  /* throttle threshold from sysfs */

    /* Power */
    uint32_t system_power_mw;
    uint8_t  power_mode;       /* 0=10W, 1=15W, 2=MAXN */
    char     power_mode_str[16];

    /* Throttle status */
    bool     gpu_throttled;
    bool     cpu_throttled;
    bool     any_over_temp;

    /* Frequency domains (CPU clusters, GPU) */
    char     freq_names[JT_MAX_FREQ_DOMAINS][32];
    uint32_t freq_current[JT_MAX_FREQ_DOMAINS];  /* KHz */
    uint32_t freq_max[JT_MAX_FREQ_DOMAINS];      /* KHz */
    int      freq_count;

    /* Thermal zones */
    char     thermal_names[JT_MAX_THERMAL_ZONES][32];
    uint32_t thermal_temps[JT_MAX_THERMAL_ZONES]; /* milli-C */
    int      thermal_count;
} JTPowerState;

typedef struct {
    JTPowerState current;
    JTPowerState peak;       /* peak temps this session */
    uint32_t    sample_count;
    uint32_t    throttle_events;
    uint32_t    over_temp_events;
} JTThermalMonitor;

/* ═══ API ═══ */

/** Probe all thermal zones and freq domains */
int jt_probe_power(JTPowerState *state);

/** Read current power/thermal state (fast, no re-probe) */
int jt_read_power(JTPowerState *state);

/** Set power mode (requires sudo on some Jetsons) */
int jt_set_power_mode(uint8_t mode);

/** Check if any temp exceeds threshold */
bool jt_is_over_temp(const JTPowerState *state, uint32_t threshold_mc);

/** Start thermal monitor */
int jt_thermal_monitor_init(JTThermalMonitor *mon);

/** Sample current state into monitor */
int jt_thermal_monitor_sample(JTThermalMonitor *mon);

/** Get throttle recommendation: 0=OK, 1=reduce_freq, 2=drop_workload, 3=emergency */
int jt_thermal_recommendation(const JTPowerState *state);

/** Find thermal zone by name (e.g. "GPU-therm") */
int jt_find_thermal_zone(const char *name, uint32_t *temp_mc);

/** Find freq domain by name */
int jt_find_freq_domain(const char *name, uint32_t *current_khz, uint32_t *max_khz);

/** Generate report */
int jt_power_report(const JTPowerState *state, char *out, int max_len);

/** Test suite */
int jt_power_thermal_test(void);

#endif
