/* perceive-bridge.c — Jetson anomaly detection via z-score on sysfs metrics.
   Monitors all 9 thermal zones, RAM, CPU load.
   JSON output on stdout, alerts on stderr. Zero deps, <15KB binary. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define BUF_SIZE 256
#define INTERVAL_S 2
#define MAX_ZONES 9
#define ALERT_THRESH 2.0
#define CRIT_THRESH 3.0

typedef struct {
    double buf[BUF_SIZE];
    int idx;
    int count;
} ring_t;

typedef struct {
    const char *name;
    const char *path;
} thermal_source_t;

static const thermal_source_t thermal_zones[MAX_ZONES] = {
    {"cpu",  "/sys/class/thermal/thermal_zone0/temp"},
    {"gpu",  "/sys/class/thermal/thermal_zone1/temp"},
    {"cv0",  "/sys/class/thermal/thermal_zone2/temp"},
    {"cv1",  "/sys/class/thermal/thermal_zone3/temp"},
    {"cv2",  "/sys/class/thermal/thermal_zone4/temp"},
    {"soc0", "/sys/class/thermal/thermal_zone5/temp"},
    {"soc1", "/sys/class/thermal/thermal_zone6/temp"},
    {"soc2", "/sys/class/thermal/thermal_zone7/temp"},
    {"tj",   "/sys/class/thermal/thermal_zone8/temp"},
};

static void ring_init(ring_t *r) { r->idx = 0; r->count = 0; }

static void ring_push(ring_t *r, double v) {
    r->buf[r->idx] = v;
    r->idx = (r->idx + 1) % BUF_SIZE;
    if (r->count < BUF_SIZE) r->count++;
}

static double ring_mean(ring_t *r) {
    if (r->count == 0) return 0.0;
    double s = 0;
    for (int i = 0; i < r->count; i++) s += r->buf[i];
    return s / r->count;
}

static double ring_stddev(ring_t *r) {
    if (r->count < 2) return 1e-9;
    double m = ring_mean(r), s = 0;
    for (int i = 0; i < r->count; i++) s += (r->buf[i] - m) * (r->buf[i] - m);
    return sqrt(s / r->count);
}

static double ring_zscore(ring_t *r, double v) {
    double sd = ring_stddev(r);
    if (sd < 1e-9) return 0.0;
    return (v - ring_mean(r)) / sd;
}

static double read_sysfs_int(const char *path) {
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NAN;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NAN;
    buf[n] = 0;
    return atof(buf);
}

static double read_ram_available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return NAN;
    char line[128];
    double val = NAN;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %lf kB", &val) == 1) {
            val /= 1024.0; /* kB -> MB */
            break;
        }
    }
    fclose(f);
    return val;
}

static double read_cpu_load(void) {
    static long long prev_total = 0, prev_idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return NAN;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return NAN; }
    fclose(f);
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (sscanf(line, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4)
        return NAN;
    long long total = user + nice + system + idle + iowait + irq + softirq + steal;
    long long d_idle = idle - prev_idle, d_total = total - prev_total;
    prev_idle = idle;
    prev_total = total;
    if (d_total <= 0) return 0.0;
    return 100.0 * (1.0 - (double)d_idle / d_total);
}

int main(void) {
    ring_t thermal[MAX_ZONES], ram, cpu_l;
    for (int i = 0; i < MAX_ZONES; i++) ring_init(&thermal[i]);
    ring_init(&ram); ring_init(&cpu_l);

    /* seed initial samples */
    for (int i = 0; i < 5; i++) {
        for (int z = 0; z < MAX_ZONES; z++)
            ring_push(&thermal[z], read_sysfs_int(thermal_zones[z].path) / 1000.0);
        ring_push(&ram, read_ram_available());
        ring_push(&cpu_l, read_cpu_load());
        usleep(200000);
    }

    for (;;) {
        time_t now = time(NULL);
        double ra = read_ram_available();
        double cl = read_cpu_load();
        ring_push(&ram, ra); ring_push(&cpu_l, cl);

        double maxz = 0.0;
        const char *worst_source = "none";

        char metrics_buf[2048];
        char anomaly_buf[2048];
        int m_len = 0, a_len = 0;

        /* RAM and CPU */
        double zra = ring_zscore(&ram, ra);
        double zcl = ring_zscore(&cpu_l, cl);
        m_len += snprintf(metrics_buf + m_len, sizeof(metrics_buf) - m_len,
            "\"ram_mb\":%.0f,\"cpu_load\":%.1f", ra, cl);
        a_len += snprintf(anomaly_buf + a_len, sizeof(anomaly_buf) - a_len,
            "\"ram_mb_z\":%.2f,\"cpu_load_z\":%.2f", zra, zcl);
        if (fabs(zra) > maxz) { maxz = fabs(zra); worst_source = "ram"; }
        if (fabs(zcl) > maxz) { maxz = fabs(zcl); worst_source = "cpu_load"; }

        /* All 9 thermal zones */
        for (int z = 0; z < MAX_ZONES; z++) {
            double val = read_sysfs_int(thermal_zones[z].path) / 1000.0;
            ring_push(&thermal[z], val);
            double zv = ring_zscore(&thermal[z], val);

            if (!isnan(val)) {
                m_len += snprintf(metrics_buf + m_len, sizeof(metrics_buf) - m_len,
                    ",\"temp_%s\":%.1f", thermal_zones[z].name, val);
                a_len += snprintf(anomaly_buf + a_len, sizeof(anomaly_buf) - a_len,
                    ",\"temp_%s_z\":%.2f", thermal_zones[z].name, zv);
            }
            if (fabs(zv) > maxz) { maxz = fabs(zv); worst_source = thermal_zones[z].name; }
        }

        const char *status = "NOMINAL";
        int intervention = 0;
        if (maxz > CRIT_THRESH) { status = "CRITICAL"; intervention = 1; }
        else if (maxz > ALERT_THRESH) { status = "WARNING"; }

        printf("{\"timestamp\":%ld,\"metrics\":{%s},\"anomaly\":{%s},"
               "\"status\":\"%s\",\"intervention\":%d,\"worst_source\":\"%s\",\"max_z\":%.2f}\n",
               (long)now, metrics_buf, anomaly_buf, status, intervention, worst_source, maxz);
        fflush(stdout);

        if (intervention)
            fprintf(stderr, "[%s] %s max_z=%.2f — intervention recommended\n", status, worst_source, maxz);

        sleep(INTERVAL_S);
    }
}
