/* sensor-bridge.c — Jetson Orin Nano hardware sensor bridge
 * Reads all sysfs sensors, outputs JSON to stdout every 2s.
 * Build: gcc -std=gnu99 -Wall -Wextra -O2 -Os -o sensor-bridge sensor-bridge.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>

#define SAMPLE_INTERVAL 2
#define I2C_CACHE_INTERVAL 60
#define MAX_THERMAL_ZONES 9
#define MAX_I2C_BUSSES 8
#define MAX_I2C_DEVICES 64
#define MAX_GPIO_PINS 32
#define BUF_SIZE 256
#define LINE_SIZE 512

static double read_sysfs_int(const char *path) {
    char buf[BUF_SIZE];
    FILE *f = fopen(path, "r");
    if (!f) return NAN;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NAN; }
    fclose(f);
    return (double)atol(buf);
}

static double read_thermal(int zone) {
    char path[BUF_SIZE];
    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
    return read_sysfs_int(path);
}

static double cpu_freq_mhz(void) {
    double khz = read_sysfs_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (isnan(khz)) return NAN;
    return khz / 1000.0;
}

static double gpu_freq_mhz(void) {
    double hz = read_sysfs_int("/sys/devices/gpu.0/devfreq/17000000.gv11b/cur_freq");
    if (isnan(hz)) return NAN;
    return hz / 1000000.0;
}

static void read_meminfo(double *total_mb, double *avail_mb, double *free_mb) {
    *total_mb = *avail_mb = *free_mb = NAN;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[LINE_SIZE];
    while (fgets(line, sizeof(line), f)) {
        unsigned long val;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1) *total_mb = val / 1024.0;
        else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1) *avail_mb = val / 1024.0;
        else if (sscanf(line, "MemFree: %lu kB", &val) == 1) *free_mb = val / 1024.0;
    }
    fclose(f);
}

static double cpu_load_pct(void) {
    static unsigned long long prev_idle = 0, prev_total = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return NAN;
    char line[LINE_SIZE];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return NAN; }
    fclose(f);

    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) < 4)
        return NAN;

    unsigned long long total = user + nice + sys + idle + iowait + irq + softirq + steal;
    unsigned long long total_d = total - prev_total;
    unsigned long long idle_d = (idle + iowait) - prev_idle;
    prev_total = total;
    prev_idle = idle + iowait;

    if (total_d == 0) return 0.0;
    return 100.0 * (1.0 - (double)idle_d / (double)total_d);
}

static void disk_usage(double *used_pct, double *free_gb) {
    *used_pct = *free_gb = NAN;
    struct statvfs vfs;
    if (statvfs("/", &vfs) != 0) return;
    unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    if (total == 0) return;
    *used_pct = 100.0 * (1.0 - (double)avail / (double)total);
    *free_gb = (double)avail / (1073741824.0);
}

static double uptime_seconds(void) {
    double up;
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return NAN;
    if (fscanf(f, "%lf", &up) != 1) up = NAN;
    fclose(f);
    return up;
}

/* Parse /proc/net/dev for interface byte counts */
struct net_stats { long long rx, tx; };

static int parse_net_dev(const char *iface, struct net_stats *s) {
    s->rx = s->tx = 0;
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return -1;
    char line[LINE_SIZE];
    /* skip 2 header lines */
    char *l1 __attribute__((unused)) = fgets(line, sizeof(line), f);
    char *l2 __attribute__((unused)) = fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        *p = '\0';
        char *name = line;
        while (*name == ' ') name++;
        if (strcmp(name, iface) != 0) continue;
        /* format: rx_bytes rx_packets ... tx_bytes tx_packets ... */
        long long rx, dummy, tx;
        if (sscanf(p + 1, "%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
                   &rx, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx, &dummy) >= 9) {
            s->rx = rx;
            s->tx = tx;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

/* I2C scan — probe /dev/i2c-N for addresses 0x03-0x77 */
static int i2c_devices_found[MAX_I2C_DEVICES]; /* encoded: (bus << 8) | addr */
static int i2c_device_count = 0;
static time_t last_i2c_scan = 0;

static void scan_i2c(void) {
    time_t now = time(NULL);
    if (now - last_i2c_scan < I2C_CACHE_INTERVAL && last_i2c_scan != 0) return;
    last_i2c_scan = now;
    i2c_device_count = 0;

    for (int bus = 0; bus < MAX_I2C_BUSSES && i2c_device_count < MAX_I2C_DEVICES; bus++) {
        char devpath[BUF_SIZE];
        snprintf(devpath, sizeof(devpath), "/dev/i2c-%d", bus);
        int fd = open(devpath, O_RDWR);
        if (fd < 0) continue;

        for (int addr = 0x03; addr <= 0x77 && i2c_device_count < MAX_I2C_DEVICES; addr++) {
            if (ioctl(fd, I2C_SLAVE, addr) < 0) continue;
            struct i2c_smbus_ioctl_data sdata = { .read_write = I2C_SMBUS_READ, .command = 0, .size = I2C_SMBUS_BYTE, .data = NULL };
            errno = 0;
            if (ioctl(fd, I2C_SMBUS, &sdata) == 0) {
                i2c_devices_found[i2c_device_count++] = (bus << 8) | addr;
            }
        }
        close(fd);
    }
}

/* GPIO — list exported pins from /sys/class/gpio */
static char gpio_pins[MAX_GPIO_PINS][BUF_SIZE];
static int gpio_pin_count = 0;

static void scan_gpio(void) {
    gpio_pin_count = 0;
    DIR *d = opendir("/sys/class/gpio");
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && gpio_pin_count < MAX_GPIO_PINS) {
        if (strncmp(ent->d_name, "gpio", 4) != 0) continue;
        char *end;
        strtol(ent->d_name + 4, &end, 10);
        if (*end != '\0') continue;
        strncpy(gpio_pins[gpio_pin_count], ent->d_name + 4, BUF_SIZE - 1);
        gpio_pins[gpio_pin_count][BUF_SIZE - 1] = '\0';
        gpio_pin_count++;
    }
    closedir(d);
}

/* JSON helpers — no floating NaN in JSON, output null instead */
static void json_double(FILE *f, const char *key, double val) {
    if (isnan(val))
        fprintf(f, "\"%s\":null", key);
    else
        fprintf(f, "\"%s\":%.1f", key, val);
}

static void json_long(FILE *f, const char *key, long long val) {
    fprintf(f, "\"%s\":%lld", key, val);
}

int main(void) {
    fprintf(stderr, "sensor-bridge: starting on Jetson Orin Nano\n");

    /* Probe available thermal zones */
    for (int i = 0; i < MAX_THERMAL_ZONES; i++) {
        char path[BUF_SIZE];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        if (access(path, R_OK) == 0) {
            /* just report zone number */
            fprintf(stderr, "  thermal_zone%d: found\n", i);
        }
    }

    /* Check key paths */
    const char *paths[] = {
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        "/sys/devices/gpu.0/devfreq/17000000.gv11b/cur_freq",
        "/proc/meminfo", "/proc/stat", "/proc/net/dev", "/proc/uptime",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        fprintf(stderr, "  %s: %s\n", paths[i], access(paths[i], R_OK) == 0 ? "OK" : "MISSING");
    }

    fprintf(stderr, "sensor-bridge: sampling every %d seconds\n", SAMPLE_INTERVAL);

    /* Main loop */
    while (1) {
        scan_i2c();
        scan_gpio();

        double temps[MAX_THERMAL_ZONES];
        for (int i = 0; i < MAX_THERMAL_ZONES; i++)
            temps[i] = read_thermal(i);

        /* Zone mapping for Orin Nano: 0=CPU, 1=GPU, 2=CV0, 3=CV1, 4=CV2,
         * 5=SOC0, 6=SOC1, 7=SOC2, 8=TJ */
        double temp_cpu  = isnan(temps[0]) ? NAN : temps[0] / 1000.0;
        double temp_gpu  = isnan(temps[1]) ? NAN : temps[1] / 1000.0;
        double temp_tj   = isnan(temps[8]) ? temp_cpu : (temps[8] / 1000.0); /* TJ fallback to CPU */

        double cpu_f = cpu_freq_mhz();
        double gpu_f = gpu_freq_mhz();

        double ram_total = NAN, ram_avail = NAN, ram_free = NAN;
        read_meminfo(&ram_total, &ram_avail, &ram_free);
        double ram_used = (isnan(ram_total) || isnan(ram_avail)) ? NAN : ram_total - ram_avail;

        double cpu_load = cpu_load_pct();
        double disk_pct = NAN, disk_free = NAN;
        disk_usage(&disk_pct, &disk_free);
        double uptime = uptime_seconds();

        struct net_stats eth = {0, 0}, wifi = {0, 0};
        parse_net_dev("enP8p1s0", &eth);
        parse_net_dev("wlP1p1s0", &wifi);

        /* Output JSON */
        time_t ts = time(NULL);
        printf("{\"timestamp\":%ld,\"sensors\":{", (long)ts);

        json_double(stdout, "temp_cpu", temp_cpu);
        printf(",");
        json_double(stdout, "temp_gpu", temp_gpu);
        printf(",");
        json_double(stdout, "temp_tj", temp_tj);
        printf(",");
        json_double(stdout, "cpu_freq_mhz", cpu_f);
        printf(",");
        json_double(stdout, "gpu_freq_mhz", gpu_f);
        printf(",");
        json_double(stdout, "ram_total_mb", ram_total);
        printf(",");
        json_double(stdout, "ram_used_mb", ram_used);
        printf(",");
        json_double(stdout, "ram_available_mb", ram_avail);
        printf(",");
        json_double(stdout, "cpu_load_pct", cpu_load);
        printf(",");
        json_double(stdout, "disk_used_pct", disk_pct);
        printf(",");
        json_double(stdout, "disk_free_gb", disk_free);
        printf(",");
        json_double(stdout, "uptime_seconds", uptime);
        printf(",");

        json_long(stdout, "net_enp8p1s0_rx", eth.rx);
        printf(",");
        json_long(stdout, "net_enp8p1s0_tx", eth.tx);
        printf(",");
        json_long(stdout, "net_wlp1p1s0_rx", wifi.rx);
        printf(",");
        json_long(stdout, "net_wlp1p1s0_tx", wifi.tx);
        printf(",");

        /* I2C devices array */
        printf("\"i2c_devices\":[");
        for (int i = 0; i < i2c_device_count; i++) {
            if (i > 0) printf(",");
            printf("\"%d:0x%02x\"", i2c_devices_found[i] >> 8, i2c_devices_found[i] & 0xff);
        }
        printf("],");

        /* GPIO exported array */
        printf("\"gpio_exported\":[");
        for (int i = 0; i < gpio_pin_count; i++) {
            if (i > 0) printf(",");
            printf("\"%s\"", gpio_pins[i]);
        }
        printf("]");

        printf("}}\n");
        fflush(stdout);

        sleep(SAMPLE_INTERVAL);
    }
}
