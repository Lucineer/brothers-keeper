#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define BUF_SIZE 256
#define INTERVAL_S 2

typedef struct {
    double buf[BUF_SIZE];
    int idx;
    int count;
} ring_t;

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
        if (sscanf(line, "MemAvailable: %lf kB", &val) == 1 ||
            sscanf(line, "MemAvailable: %lf", &val) == 1) {
            val /= 1024.0; // kB -> MB
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
    ring_t gpu_t, cpu_t, ram, cpu_l;
    ring_init(&gpu_t); ring_init(&cpu_t); ring_init(&ram); ring_init(&cpu_l);

    /* seed initial samples */
    for (int i = 0; i < 5; i++) {
        ring_push(&gpu_t, read_sysfs_int("/sys/devices/virtual/thermal/thermal_zone1/temp") / 1000.0);
        ring_push(&cpu_t, read_sysfs_int("/sys/devices/virtual/thermal/thermal_zone0/temp") / 1000.0);
        ring_push(&ram, read_ram_available());
        ring_push(&cpu_l, read_cpu_load());
        usleep(200000);
    }

    for (;;) {
        double gt = read_sysfs_int("/sys/devices/virtual/thermal/thermal_zone1/temp") / 1000.0;
        double ct = read_sysfs_int("/sys/devices/virtual/thermal/thermal_zone0/temp") / 1000.0;
        double ra = read_ram_available();
        double cl = read_cpu_load();

        ring_push(&gpu_t, gt); ring_push(&cpu_t, ct);
        ring_push(&ram, ra); ring_push(&cpu_l, cl);

        double zgt = ring_zscore(&gpu_t, gt);
        double zct = ring_zscore(&cpu_t, ct);
        double zra = ring_zscore(&ram, ra);
        double zcl = ring_zscore(&cpu_l, cl);

        double maxz = fmax(fmax(fabs(zgt), fabs(zct)), fmax(fabs(zra), fabs(zcl)));
        const char *status = "NOMINAL";
        int intervention = 0;
        if (maxz > 3.0) { status = "CRITICAL"; intervention = 1; }
        else if (maxz > 2.0) { status = "WARNING"; }

        time_t now = time(NULL);

        printf("{\"timestamp\":%ld,\"metrics\":{\"gpu_temp\":%.1f,\"cpu_temp\":%.1f,\"ram_mb\":%.0f,\"cpu_load\":%.1f},"
               "\"anomaly\":{\"gpu_temp_z\":%.2f,\"cpu_temp_z\":%.2f,\"ram_mb_z\":%.2f,\"cpu_load_z\":%.2f},"
               "\"status\":\"%s\",\"intervention\":%d}\n",
               (long)now, gt, ct, ra, cl, zgt, zct, zra, zcl, status, intervention);
        fflush(stdout);

        if (intervention)
            fprintf(stderr, "[%s] max_z=%.2f — intervention recommended\n", status, maxz);

        sleep(INTERVAL_S);
    }
}
