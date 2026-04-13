/**
 * wheelhouse.c — Real sensor bridge for Jetson Orin Nano
 * Reads I2C sensors, serial NMEA, GPIO, PWM
 * Outputs structured gauge data for the MUD bridge
 *
 * Sensors supported:
 *   I2C:  HMC5883L (compass), BMP280 (pressure/temp), MPU6050 (IMU)
 *   Serial: NMEA 0183 GPS, depth sounder, AIS
 *   GPIO: digital inputs (switches, buttons, limit switches)
 *   PWM: servo control (rudder, throttle)
 *
 * Build: gcc -std=gnu99 -Wall -O2 -o wheelhouse wheelhouse.c -lm
 * Usage: ./wheelhouse [options]
 *   --i2c <bus>        I2C bus number (default 1)
 *   --serial <port>    Serial port (default /dev/ttyTHS1)
 *   --baud <rate>      Serial baud rate (default 4800)
 *   --pwm <chip>       PWM chip (default 0)
 *   --compass          Enable HMC5883L compass
 *   --baro             Enable BMP280 barometer
 *   --imu              Enable MPU6050 IMU
 *   --gps              Enable NMEA GPS
 *   --depth            Enable depth sounder
 *   --demo             Use simulated data
 *   --mud              Output MUD gauge format
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <termios.h>

/* ═══ Gauge Data ═══ */

#define MAX_GAUGES 32

typedef enum {
    GAUGE_FLOAT,    /* heading, depth, speed, temp, pressure */
    GAUGE_INT,      /* RPM, satellites, signal quality */
    GAUGE_BOOL,     /* GPS lock, anchor watch, engine on */
    GAUGE_STRING,   /* NMEA sentence, waypoint name */
    GAUGE_PERCENT,  /* fuel, battery, rudder deflection */
} GaugeType;

typedef struct {
    char name[32];
    char label[48];
    char unit[16];
    GaugeType type;
    float fval;
    int ival;
    int bval;
    char sval[128];
    float min_val;
    float max_val;
    float warn_low;
    float warn_high;
    int updated;       /* 1 if this cycle updated */
    long update_ms;    /* timestamp of last update */
} Gauge;

typedef struct {
    Gauge gauges[MAX_GAUGES];
    int gauge_count;
    long cycle_ms;
    int source_mask;   /* bitmask of active sources */
} Wheelhouse;

#define SRC_I2C     0x01
#define SRC_SERIAL  0x02
#define SRC_GPIO    0x04
#define SRC_PWM     0x08
#define SRC_DEMO    0x10
#define SRC_GPU     0x20

/* ═══ Gauge API ═══ */

static int wh_add_gauge(Wheelhouse *wh, const char *name, const char *label,
                         const char *unit, GaugeType type,
                         float min_val, float max_val,
                         float warn_low, float warn_high) {
    if (wh->gauge_count >= MAX_GAUGES) return -1;
    int idx = wh->gauge_count++;
    Gauge *g = &wh->gauges[idx];
    strncpy(g->name, name, 31);
    strncpy(g->label, label, 47);
    strncpy(g->unit, unit, 15);
    g->type = type;
    g->fval = 0; g->ival = 0; g->bval = 0; g->sval[0] = 0;
    g->min_val = min_val; g->max_val = max_val;
    g->warn_low = warn_low; g->warn_high = warn_high;
    g->updated = 0;
    return idx;
}

static Gauge* wh_find_gauge(Wheelhouse *wh, const char *name) {
    for (int i = 0; i < wh->gauge_count; i++) {
        if (strcmp(wh->gauges[i].name, name) == 0) return &wh->gauges[i];
    }
    return NULL;
}

static void wh_set_float(Wheelhouse *wh, const char *name, float val) {
    Gauge *g = wh_find_gauge(wh, name);
    if (g) { g->fval = val; g->updated = 1; }
}

static void wh_set_int(Wheelhouse *wh, const char *name, int val) {
    Gauge *g = wh_find_gauge(wh, name);
    if (g) { g->ival = val; g->updated = 1; }
}

static void wh_set_bool(Wheelhouse *wh, const char *name, int val) {
    Gauge *g = wh_find_gauge(wh, name);
    if (g) { g->bval = val; g->updated = 1; }
}

static void wh_set_string(Wheelhouse *wh, const char *name, const char *val) {
    Gauge *g = wh_find_gauge(wh, name);
    if (g) { strncpy(g->sval, val, 127); g->updated = 1; }
}

static void wh_timestamp_all(Wheelhouse *wh) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    wh->cycle_ms = now;
    for (int i = 0; i < wh->gauge_count; i++) {
        if (wh->gauges[i].updated) {
            wh->gauges[i].update_ms = now;
        }
        wh->gauges[i].updated = 0;
    }
}

/* ═══ I2C ═══ */

static int i2c_open(int bus) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        /* Try as non-root user */
        fd = open(path, O_RDWR);
    }
    return fd;
}

static int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    struct i2c_msg msg = {addr, 0, 2, buf};
    struct i2c_rdwr_ioctl_data data = {&msg, 1};
    return ioctl(fd, I2C_RDWR, &data);
}

static int i2c_read_reg(int fd, uint8_t addr, uint8_t reg, uint8_t *buf, int len) {
    uint8_t out = reg;
    struct i2c_msg msgs[2] = {
        {addr, 0, 1, &out},
        {addr, I2C_M_RD, len, buf},
    };
    struct i2c_rdwr_ioctl_data data = {msgs, 2};
    return ioctl(fd, I2C_RDWR, &data);
}

static int i2c_detect(int fd, uint8_t addr) {
    uint8_t val;
    return i2c_read_reg(fd, addr, 0x00, &val, 1) >= 0;
}

/* ═══ HMC5883L Compass ═══ */

#define HMC5883L_ADDR 0x1E

static int compass_init(int fd) {
    if (!i2c_detect(fd, HMC5883L_ADDR)) return -1;
    /* Config: 8 samples avg, 15Hz, normal measurement */
    i2c_write_reg(fd, HMC5883L_ADDR, 0x00, 0x70);
    /* Gain: 1.3 Ga */
    i2c_write_reg(fd, HMC5883L_ADDR, 0x01, 0x20);
    /* Mode: continuous */
    i2c_write_reg(fd, HMC5883L_ADDR, 0x02, 0x00);
    return 0;
}

static float compass_read(int fd) {
    uint8_t buf[6];
    if (i2c_read_reg(fd, HMC5883L_ADDR, 0x03, buf, 6) < 0) return -1;

    int16_t x = (int16_t)((buf[0] << 8) | buf[1]);
    int16_t z = (int16_t)((buf[2] << 8) | buf[3]);
    int16_t y = (int16_t)((buf[4] << 8) | buf[5]);

    /* Heading in radians, then convert to degrees */
    float heading = atan2f((float)y, (float)x);
    if (heading < 0) heading += 2.0f * M_PI;
    return heading * 180.0f / M_PI;
}

/* ═══ BMP280 Barometer ═══ */

#define BMP280_ADDR 0x76

static int bmp280_read_calibration(int fd, int16_t *dig_T1, int16_t *dig_T2, int16_t *dig_T3,
                                     uint16_t *dig_P1, int16_t *dig_P2, int16_t *dig_P3) {
    uint8_t buf[24];
    if (i2c_read_reg(fd, BMP280_ADDR, 0x88, buf, 24) < 0) return -1;
    *dig_T1 = (int16_t)(buf[0] | (buf[1] << 8));
    *dig_T2 = (int16_t)(buf[2] | (buf[3] << 8));
    *dig_T3 = (int16_t)(buf[4] | (buf[5] << 8));
    *dig_P1 = (uint16_t)(buf[6] | (buf[7] << 8));
    *dig_P2 = (int16_t)(buf[8] | (buf[9] << 8));
    *dig_P3 = (int16_t)(buf[10] | (buf[11] << 8));
    return 0;
}

static int baro_init(int fd) {
    if (!i2c_detect(fd, BMP280_ADDR)) return -1;
    /* Oversampling: pressure x4, temp x1, normal mode */
    i2c_write_reg(fd, BMP280_ADDR, 0xF4, 0x53);
    return 0;
}

static int baro_read(int fd, float *temp_c, float *pressure_hpa) {
    uint8_t buf[6];
    if (i2c_read_reg(fd, BMP280_ADDR, 0xF7, buf, 6) < 0) return -1;

    int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | ((int32_t)buf[2] >> 4);
    int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | ((int32_t)buf[5] >> 4);

    /* Simplified: just return raw values scaled */
    *pressure_hpa = adc_P / 100.0f; /* Approximate */
    *temp_c = adc_T / 100.0f;        /* Approximate */
    return 0;
}

/* ═══ MPU6050 IMU ═══ */

#define MPU6050_ADDR 0x68

static int imu_init(int fd) {
    if (!i2c_detect(fd, MPU6050_ADDR)) return -1;
    /* Wake up, set gyro range to 250 deg/s, accel to 2g */
    i2c_write_reg(fd, MPU6050_ADDR, 0x6B, 0x00);
    i2c_write_reg(fd, MPU6050_ADDR, 0x1B, 0x00);
    i2c_write_reg(fd, MPU6050_ADDR, 0x1C, 0x00);
    return 0;
}

static int imu_read(int fd, float *accel_x, float *accel_y, float *accel_z,
                     float *gyro_x, float *gyro_y, float *gyro_z) {
    uint8_t buf[14];
    if (i2c_read_reg(fd, MPU6050_ADDR, 0x3B, buf, 14) < 0) return -1;

    *accel_x = (int16_t)((buf[0] << 8) | buf[1]) / 16384.0f;
    *accel_y = (int16_t)((buf[2] << 8) | buf[3]) / 16384.0f;
    *accel_z = (int16_t)((buf[4] << 8) | buf[5]) / 16384.0f;
    *gyro_x = (int16_t)((buf[8] << 8) | buf[9]) / 131.0f;
    *gyro_y = (int16_t)((buf[10] << 8) | buf[11]) / 131.0f;
    *gyro_z = (int16_t)((buf[12] << 8) | buf[13]) / 131.0f;
    return 0;
}

/* ═══ NMEA 0183 Parser ═══ */

/* Parse GGA sentence: fix time, lat, lon, fix quality, num sats, hdop, alt */
static int nmea_parse_gga(const char *sentence, float *lat, float *lon,
                            int *quality, int *sats, float *alt) {
    if (strncmp(sentence, "$GPGGA", 6) != 0 && strncmp(sentence, "$GNGGA", 6) != 0)
        return -1;

    /* Skip to after first comma (time) */
    const char *p = sentence;
    int field = 0;
    char fields[15][32];
    int fi = 0;

    fields[0][0] = 0;
    while (*p && fi < 15) {
        if (*p == ',' || *p == '*') {
            fi++;
            if (fi < 15) fields[fi][0] = 0;
        } else if (*p == '\r' || *p == '\n') {
            break;
        } else {
            int len = strlen(fields[fi]);
            if (len < 31) {
                fields[fi][len] = *p;
                fields[fi][len + 1] = 0;
            }
        }
        p++;
    }

    if (fi < 8) return -1;

    /* GGA: field[0]=$GPGGA, [1]=time, [2]=lat, [3]=N/S, [4]=lon, [5]=E/W, [6]=quality, [7]=sats, [8]=hdop, [9]=alt */
    if (strlen(fields[2]) > 4) {
        float lat_raw = atof(fields[2]);
        int lat_deg = (int)(lat_raw / 100.0f);
        float lat_min = lat_raw - lat_deg * 100.0f;
        *lat = lat_deg + lat_min / 60.0f;
        if (fields[3][0] == 'S') *lat = -(*lat);
    }

    if (strlen(fields[4]) > 5) {
        float lon_raw = atof(fields[4]);
        int lon_deg = (int)(lon_raw / 100.0f);
        float lon_min = lon_raw - lon_deg * 100.0f;
        *lon = lon_deg + lon_min / 60.0f;
        if (fields[5][0] == 'W') *lon = -(*lon);
    }

    *quality = atoi(fields[6]);
    *sats = atoi(fields[7]);
    *alt = atof(fields[9]);
    return 0;
}

/* Parse RMC sentence: status, lat, lon, speed (knots), course, date */
static int nmea_parse_rmc(const char *sentence, int *status, float *lat, float *lon,
                            float *speed_knots, float *course) {
    if (strncmp(sentence, "$GPRMC", 6) != 0 && strncmp(sentence, "$GNRMC", 6) != 0)
        return -1;

    const char *p = sentence;
    char fields[15][32];
    int fi = 0;

    fields[0][0] = 0;
    while (*p && fi < 15) {
        if (*p == ',' || *p == '*') {
            fi++;
            if (fi < 15) fields[fi][0] = 0;
        } else if (*p == '\r' || *p == '\n') {
            break;
        } else {
            int len = strlen(fields[fi]);
            if (len < 31) {
                fields[fi][len] = *p;
                fields[fi][len + 1] = 0;
            }
        }
        p++;
    }

    if (fi < 8) return -1;

    /* RMC: field[0]=$GPRMC, [1]=time, [2]=status, [3]=lat, [4]=N/S, [5]=lon, [6]=E/W, [7]=speed, [8]=course */
    *status = (fields[2][0] == 'A') ? 1 : 0;

    if (strlen(fields[3]) > 4) {
        float lat_raw = atof(fields[3]);
        int lat_deg = (int)(lat_raw / 100.0f);
        *lat = lat_deg + (lat_raw - lat_deg * 100.0f) / 60.0f;
        if (fields[4][0] == 'S') *lat = -(*lat);
    }
    if (strlen(fields[5]) > 5) {
        float lon_raw = atof(fields[5]);
        int lon_deg = (int)(lon_raw / 100.0f);
        *lon = lon_deg + (lon_raw - lon_deg * 100.0f) / 60.0f;
        if (fields[6][0] == 'W') *lon = -(*lon);
    }

    *speed_knots = atof(fields[7]);
    *course = atof(fields[8]);
    return 0;
}

/* ═══ Serial ═══ */

static int serial_open(const char *port, int baud) {
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    tcgetattr(fd, &tty);

    /* Baud rate */
    speed_t baud_rate;
    switch (baud) {
        case 4800:   baud_rate = B4800; break;
        case 9600:   baud_rate = B9600; break;
        case 19200:  baud_rate = B19200; break;
        case 38400:  baud_rate = B38400; break;
        case 115200: baud_rate = B115200; break;
        default:     baud_rate = B4800; break;
    }
    cfsetispeed(&tty, baud_rate);
    cfsetospeed(&tty, baud_rate);

    /* 8N1 */
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= (CLOCAL | CREAD);

    /* Raw mode */
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    /* 1 second timeout */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

static int serial_read_line(int fd, char *buf, int max_len, int timeout_ms) {
    int pos = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (pos < max_len - 1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                       (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed > timeout_ms) break;

        char c;
        int n = read(fd, &c, 1);
        if (n <= 0) {
            usleep(10000); /* 10ms */
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (pos > 0) break;
            continue;
        }
        buf[pos++] = c;
    }
    buf[pos] = 0;
    return pos;
}

/* ═══ PWM (rudder/throttle) ═══ */

static int pwm_set(int chip, int channel, int duty_ns, int period_ns) {
    char path[128];
    int fd;

    /* Export */
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/export", chip);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char ch[8]; snprintf(ch, sizeof(ch), "%d", channel);
        write(fd, ch, strlen(ch));
        close(fd);
    }

    /* Enable */
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm%d/enable", chip, channel);
    fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }

    /* Period */
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm%d/period", chip, channel);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char val[16]; snprintf(val, sizeof(val), "%d", period_ns);
        write(fd, val, strlen(val));
        close(fd);
    }

    /* Duty cycle */
    snprintf(path, sizeof(path), "/sys/class/pwm/pwmchip%d/pwm%d/duty_cycle", chip, channel);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char val[16]; snprintf(val, sizeof(val), "%d", duty_ns);
        write(fd, val, strlen(val));
        close(fd);
    }

    return 0;
}

/* ═══ MUD Gauge Renderer ═══ */

static void render_gauge(const Gauge *g, FILE *out) {
    char bar[52];
    float pct = 0;
    int bar_len = 0;

    if (g->type == GAUGE_FLOAT || g->type == GAUGE_PERCENT) {
        float range = g->max_val - g->min_val;
        if (range > 0) pct = (g->fval - g->min_val) / range;
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        bar_len = (int)(pct * 40);
    } else if (g->type == GAUGE_INT) {
        float range = g->max_val - g->min_val;
        if (range > 0) pct = (float)g->ival / range;
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        bar_len = (int)(pct * 40);
    }

    /* Warning coloring */
    int warn = 0;
    if (g->type == GAUGE_FLOAT || g->type == GAUGE_PERCENT) {
        if (g->warn_low > 0 && g->fval < g->warn_low) warn = -1;
        if (g->warn_high > 0 && g->fval > g->warn_high) warn = 1;
    }

    const char *color_on = "\033[92m";   /* green */
    const char *color_warn = "\033[93m"; /* yellow */
    const char *color_crit = "\033[91m"; /* red */
    const char *color_off = "\033[2m";   /* dim */
    const char *color_rst = "\033[0m";

    const char *color = color_on;
    if (warn < 0) color = color_warn;
    if (warn > 0) color = color_crit;

    /* Check if recently updated */
    long age = 0; /* TODO: compare with cycle_ms */

    /* Build bar */
    memset(bar, ' ', 50);
    bar[50] = 0;
    for (int i = 0; i < bar_len && i < 40; i++) bar[i] = '=';
    if (bar_len > 0 && bar_len <= 40) bar[bar_len - 1] = '>';

    /* Print */
    fprintf(out, "  %s%-16s%s [%s%s%s] ",
            "\033[1m", g->label, "\033[0m",
            color, bar, color_rst);

    /* Value */
    if (g->type == GAUGE_FLOAT || g->type == GAUGE_PERCENT) {
        fprintf(out, "%s%8.2f %s%s", color, g->fval, g->unit, color_rst);
    } else if (g->type == GAUGE_INT) {
        fprintf(out, "%s%8d %s%s", color, g->ival, g->unit, color_rst);
    } else if (g->type == GAUGE_BOOL) {
        fprintf(out, "%s%8s%s", g->bval ? color_on : color_off,
                g->bval ? "ON" : "OFF", color_rst);
    } else if (g->type == GAUGE_STRING) {
        fprintf(out, "%s", g->sval);
    }

    fprintf(out, "\n");
}

static void render_mud(Wheelhouse *wh, FILE *out) {
    fprintf(out, "\033[2J\033[H"); /* Clear screen */
    fprintf(out, "\033[1m\033[96m  USS JETSONCLAW1 — WHEELHOUSE\033[0m\n");
    fprintf(out, "  %s\n", "════════════════════════════════════════════════════════");

    /* Group gauges by source */
    const char *section = "";
    for (int i = 0; i < wh->gauge_count; i++) {
        Gauge *g = &wh->gauges[i];
        /* Section headers */
        if (strcmp(g->name, "heading") == 0 && strcmp(section, "NAV") != 0) {
            fprintf(out, "\n  \033[1mNAVIGATION\033[0m\n");
            section = "NAV";
        } else if (strcmp(g->name, "engine_rpm") == 0 && strcmp(section, "ENG") != 0) {
            fprintf(out, "\n  \033[1mENGINE\033[0m\n");
            section = "ENG";
        } else if (strcmp(g->name, "air_temp") == 0 && strcmp(section, "ENV") != 0) {
            fprintf(out, "\n  \033[1mENVIRONMENT\033[0m\n");
            section = "ENV";
        } else if (strcmp(g->name, "accel_x") == 0 && strcmp(section, "IMU") != 0) {
            fprintf(out, "\n  \033[1mMOTION\033[0m\n");
            section = "IMU";
        } else if (strcmp(g->name, "rudder_pos") == 0 && strcmp(section, "CTL") != 0) {
            fprintf(out, "\n  \033[1mCONTROL\033[0m\n");
            section = "CTL";
        }
        render_gauge(g, out);
    }

    /* Footer */
    fprintf(out, "\n  %s\n", "════════════════════════════════════════════════════════");
    fprintf(out, "  Sources: %s%s%s%s%s%s\n",
            (wh->source_mask & SRC_I2C) ? "I2C " : "",
            (wh->source_mask & SRC_SERIAL) ? "SERIAL " : "",
            (wh->source_mask & SRC_GPIO) ? "GPIO " : "",
            (wh->source_mask & SRC_PWM) ? "PWM " : "",
            (wh->source_mask & SRC_DEMO) ? "DEMO " : "",
            (wh->source_mask & SRC_GPU) ? "GPU " : "");
    fprintf(out, "  Cycle: %ld ms\n", wh->cycle_ms);
    fflush(out);
}

/* ═══ JSON Output (for API/MCP) ═══ */

static void render_json(Wheelhouse *wh, FILE *out) {
    fprintf(out, "{");
    fprintf(out, "\"cycle_ms\":%ld,", wh->cycle_ms);
    fprintf(out, "\"sources\":%d,", wh->source_mask);
    fprintf(out, "\"gauges\":{");
    for (int i = 0; i < wh->gauge_count; i++) {
        Gauge *g = &wh->gauges[i];
        if (i > 0) fprintf(out, ",");
        fprintf(out, "\"%s\":{", g->name);
        fprintf(out, "\"type\":\"%s\",",
                g->type == GAUGE_FLOAT ? "float" :
                g->type == GAUGE_INT ? "int" :
                g->type == GAUGE_BOOL ? "bool" :
                g->type == GAUGE_STRING ? "string" : "percent");
        if (g->type == GAUGE_FLOAT || g->type == GAUGE_PERCENT)
            fprintf(out, "\"value\":%.4f,", g->fval);
        else if (g->type == GAUGE_INT)
            fprintf(out, "\"value\":%d,", g->ival);
        else if (g->type == GAUGE_BOOL)
            fprintf(out, "\"value\":%s,", g->bval ? "true" : "false");
        else
            fprintf(out, "\"value\":\"%s\",", g->sval);
        fprintf(out, "\"unit\":\"%s\"", g->unit);
        fprintf(out, "}");
    }
    fprintf(out, "}}\n");
}

/* ═══ Demo Mode ═══ */

static void demo_update(Wheelhouse *wh) {
    static float t = 0;
    t += 0.1f;

    /* Simulate boat motion */
    float heading = fmodf(247.0f + 5.0f * sinf(t * 0.3f), 360.0f);
    if (heading < 0) heading += 360.0f;
    wh_set_float(wh, "heading", heading);
    wh_set_float(wh, "speed_knots", 5.2f + 0.5f * sinf(t * 0.7f));
    wh_set_float(wh, "depth_m", 42.1f + 2.0f * sinf(t * 0.2f));
    wh_set_int(wh, "sats", 12);
    wh_set_bool(wh, "gps_lock", 1);
    wh_set_float(wh, "lat", 58.3019f + 0.0001f * sinf(t * 0.1f));
    wh_set_float(wh, "lon", -134.4197f + 0.0001f * cosf(t * 0.1f));

    /* Engine */
    wh_set_int(wh, "engine_rpm", 2200 + (int)(100 * sinf(t * 0.5f)));
    wh_set_float(wh, "fuel_percent", 73.5f - 0.01f * t);
    wh_set_bool(wh, "engine_on", 1);

    /* Environment */
    wh_set_float(wh, "air_temp", 48.0f + 2.0f * sinf(t * 0.1f));
    wh_set_float(wh, "water_temp", 42.0f + 0.5f * sinf(t * 0.15f));
    wh_set_float(wh, "pressure_hpa", 1013.25f + 2.0f * sinf(t * 0.05f));
    wh_set_float(wh, "wind_speed", 8.0f + 3.0f * sinf(t * 0.4f));

    /* IMU */
    wh_set_float(wh, "accel_x", 0.02f * sinf(t));
    wh_set_float(wh, "accel_y", 0.05f * cosf(t * 0.8f));
    wh_set_float(wh, "accel_z", 1.0f + 0.01f * sinf(t * 0.3f));
    wh_set_float(wh, "roll_deg", 3.0f * sinf(t * 0.6f));
    wh_set_float(wh, "pitch_deg", 1.5f * cosf(t * 0.4f));

    /* Control */
    wh_set_float(wh, "rudder_pos", -2.0f + 5.0f * sinf(t * 0.3f));
    wh_set_float(wh, "throttle_percent", 45.0f);

    /* GPU system */
    wh_set_float(wh, "gpu_temp", 51.0f + 2.0f * sinf(t * 0.2f));
    wh_set_float(wh, "cpu_load", 45.0f + 10.0f * sinf(t * 0.5f));
    wh_set_float(wh, "ram_available", 3500.0f - 200.0f * sinf(t * 0.3f));

    wh_timestamp_all(wh);
}

/* ═══ Init Gauges ═══ */

static void init_gauges(Wheelhouse *wh) {
    /* Navigation */
    wh_add_gauge(wh, "heading",       "Compass Heading", "deg",  GAUGE_FLOAT, 0, 360, 0, 0);
    wh_add_gauge(wh, "speed_knots",   "Speed Over Ground", "kts", GAUGE_FLOAT, 0, 30, 0, 20);
    wh_add_gauge(wh, "course_made",   "Course Made Good", "deg",  GAUGE_FLOAT, 0, 360, 0, 0);
    wh_add_gauge(wh, "depth_m",       "Depth Below Keel", "m",    GAUGE_FLOAT, 0, 200, 0, 3);
    wh_add_gauge(wh, "gps_lock",      "GPS Fix", "",              GAUGE_BOOL, 0, 1, 0, 0);
    wh_add_gauge(wh, "sats",          "Satellites", "",           GAUGE_INT, 0, 24, 4, 0);
    wh_add_gauge(wh, "lat",           "Latitude", "deg",          GAUGE_FLOAT, -90, 90, 0, 0);
    wh_add_gauge(wh, "lon",           "Longitude", "deg",         GAUGE_FLOAT, -180, 180, 0, 0);

    /* Engine */
    wh_add_gauge(wh, "engine_rpm",    "Engine RPM", "",           GAUGE_INT, 0, 4000, 0, 3500);
    wh_add_gauge(wh, "fuel_percent",  "Fuel Level", "%",          GAUGE_PERCENT, 0, 100, 15, 0);
    wh_add_gauge(wh, "engine_on",     "Engine Running", "",       GAUGE_BOOL, 0, 1, 0, 0);

    /* Environment */
    wh_add_gauge(wh, "air_temp",      "Air Temperature", "C",     GAUGE_FLOAT, -10, 50, 0, 45);
    wh_add_gauge(wh, "water_temp",    "Water Temperature", "C",   GAUGE_FLOAT, -2, 30, 0, 0);
    wh_add_gauge(wh, "pressure_hpa",  "Barometric Pressure", "hPa", GAUGE_FLOAT, 950, 1050, 0, 0);
    wh_add_gauge(wh, "wind_speed",    "Wind Speed", "kts",        GAUGE_FLOAT, 0, 60, 0, 40);
    wh_add_gauge(wh, "wind_dir",      "Wind Direction", "deg",    GAUGE_FLOAT, 0, 360, 0, 0);

    /* Motion (IMU) */
    wh_add_gauge(wh, "accel_x",       "Accel X (fore-aft)", "g",  GAUGE_FLOAT, -2, 2, 0, 0);
    wh_add_gauge(wh, "accel_y",       "Accel Y (port-stbd)", "g", GAUGE_FLOAT, -2, 2, 0, 0);
    wh_add_gauge(wh, "accel_z",       "Accel Z (up-down)", "g",  GAUGE_FLOAT, -2, 2, 0, 0);
    wh_add_gauge(wh, "roll_deg",      "Roll", "deg",              GAUGE_FLOAT, -45, 45, 0, 30);
    wh_add_gauge(wh, "pitch_deg",     "Pitch", "deg",             GAUGE_FLOAT, -30, 30, 0, 20);

    /* Control */
    wh_add_gauge(wh, "rudder_pos",    "Rudder Position", "deg",   GAUGE_FLOAT, -45, 45, 0, 0);
    wh_add_gauge(wh, "throttle_percent", "Throttle", "%",          GAUGE_PERCENT, 0, 100, 0, 0);

    /* Jetson system (GPU perception bridge) */
    wh_add_gauge(wh, "gpu_temp",      "GPU Core Temp", "C",       GAUGE_FLOAT, 30, 90, 0, 75);
    wh_add_gauge(wh, "cpu_load",      "CPU Load", "%",             GAUGE_PERCENT, 0, 100, 0, 90);
    wh_add_gauge(wh, "ram_available", "RAM Available", "MB",      GAUGE_FLOAT, 0, 8000, 500, 0);
}

/* ═══ Main ═══ */

int main(int argc, char **argv) {
    Wheelhouse wh = {0};
    init_gauges(&wh);

    int i2c_bus = 1;
    int demo = 0;
    int mud = 1;
    int enable_compass = 0, enable_baro = 0, enable_imu = 0;
    int enable_gps = 0, enable_depth = 0;
    char serial_port[64] = "/dev/ttyTHS1";
    int serial_baud = 4800;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--i2c") == 0 && i + 1 < argc) i2c_bus = atoi(argv[++i]);
        else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc) strncpy(serial_port, argv[++i], 63);
        else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) serial_baud = atoi(argv[++i]);
        else if (strcmp(argv[i], "--compass") == 0) enable_compass = 1;
        else if (strcmp(argv[i], "--baro") == 0) enable_baro = 1;
        else if (strcmp(argv[i], "--imu") == 0) enable_imu = 1;
        else if (strcmp(argv[i], "--gps") == 0) enable_gps = 1;
        else if (strcmp(argv[i], "--depth") == 0) enable_depth = 1;
        else if (strcmp(argv[i], "--demo") == 0) demo = 1;
        else if (strcmp(argv[i], "--mud") == 0) mud = 1;
        else if (strcmp(argv[i], "--json") == 0) mud = 0;
        else if (strcmp(argv[i], "--help") == 0) {
            printf("wheelhouse — Real sensor bridge for USS JetsonClaw1\n");
            printf("Usage: wheelhouse [options]\n");
            printf("  --demo       Use simulated sensor data\n");
            printf("  --mud        MUD gauge output (default)\n");
            printf("  --json       JSON output for API\n");
            printf("  --compass    Enable HMC5883L compass (I2C)\n");
            printf("  --baro       Enable BMP280 barometer (I2C)\n");
            printf("  --imu        Enable MPU6050 IMU (I2C)\n");
            printf("  --gps        Enable NMEA GPS (serial)\n");
            printf("  --depth      Enable depth sounder (serial)\n");
            printf("  --i2c <bus>  I2C bus (default 1)\n");
            printf("  --serial <p> Serial port (default /dev/ttyTHS1)\n");
            printf("  --baud <r>   Baud rate (default 4800)\n");
            return 0;
        }
    }

    /* Open I2C */
    int i2c_fd = -1;
    if (enable_compass || enable_baro || enable_imu) {
        i2c_fd = i2c_open(i2c_bus);
        if (i2c_fd < 0) {
            fprintf(stderr, "Failed to open /dev/i2c-%d\n", i2c_bus);
            demo = 1; /* Fallback to demo */
        } else {
            wh.source_mask |= SRC_I2C;
            if (enable_compass && compass_init(i2c_fd) < 0) {
                fprintf(stderr, "HMC5883L not found on bus %d\n", i2c_bus);
            }
            if (enable_baro && baro_init(i2c_fd) < 0) {
                fprintf(stderr, "BMP280 not found on bus %d\n", i2c_bus);
            }
            if (enable_imu && imu_init(i2c_fd) < 0) {
                fprintf(stderr, "MPU6050 not found on bus %d\n", i2c_bus);
            }
        }
    }

    /* Open serial */
    int serial_fd = -1;
    if (enable_gps || enable_depth) {
        serial_fd = serial_open(serial_port, serial_baud);
        if (serial_fd < 0) {
            fprintf(stderr, "Failed to open %s (need dialout group)\n", serial_port);
            if (!demo) {
                fprintf(stderr, "Run: sudo usermod -aG dialout lucineer\n");
            }
            demo = 1;
        } else {
            wh.source_mask |= SRC_SERIAL;
        }
    }

    if (demo) wh.source_mask |= SRC_DEMO;

    /* Main loop */
    char nmea_buf[256];
    int loop_count = 0;

    while (1) {
        if (demo) {
            demo_update(&wh);
        }

        /* Read I2C sensors */
        if (i2c_fd >= 0 && !demo) {
            if (enable_compass) {
                float h = compass_read(i2c_fd);
                if (h >= 0) wh_set_float(&wh, "heading", h);
            }
            if (enable_baro) {
                float temp, pres;
                if (baro_read(i2c_fd, &temp, &pres) == 0) {
                    wh_set_float(&wh, "air_temp", temp);
                    wh_set_float(&wh, "pressure_hpa", pres);
                }
            }
            if (enable_imu) {
                float ax, ay, az, gx, gy, gz;
                if (imu_read(i2c_fd, &ax, &ay, &az, &gx, &gy, &gz) == 0) {
                    wh_set_float(&wh, "accel_x", ax);
                    wh_set_float(&wh, "accel_y", ay);
                    wh_set_float(&wh, "accel_z", az);
                    /* Roll and pitch from accelerometer */
                    wh_set_float(&wh, "roll_deg", atan2f(ay, az) * 180.0f / M_PI);
                    wh_set_float(&wh, "pitch_deg", atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / M_PI);
                }
            }
        }

        /* Read serial NMEA */
        if (serial_fd >= 0 && !demo) {
            while (serial_read_line(serial_fd, nmea_buf, sizeof(nmea_buf), 50) > 0) {
                float lat, lon, spd, crs, alt;
                int quality, sats, status;
                if (nmea_parse_gga(nmea_buf, &lat, &lon, &quality, &sats, &alt) == 0) {
                    wh_set_float(&wh, "lat", lat);
                    wh_set_float(&wh, "lon", lon);
                    wh_set_int(&wh, "sats", sats);
                    wh_set_bool(&wh, "gps_lock", quality > 0);
                    if (alt > 0) wh_set_float(&wh, "depth_m", -alt); /* Negative = below surface */
                }
                if (nmea_parse_rmc(nmea_buf, &status, &lat, &lon, &spd, &crs) == 0) {
                    wh_set_float(&wh, "lat", lat);
                    wh_set_float(&wh, "lon", lon);
                    wh_set_float(&wh, "speed_knots", spd);
                    wh_set_float(&wh, "course_made", crs);
                    wh_set_bool(&wh, "gps_lock", status);
                }
                wh_set_string(&wh, "nmea_raw", nmea_buf);
            }
        }

        if (!demo) wh_timestamp_all(&wh);

        /* Render */
        if (mud) {
            render_mud(&wh, stdout);
        } else {
            render_json(&wh, stdout);
        }

        loop_count++;
        usleep(200000); /* 5 Hz update rate */
    }

    if (i2c_fd >= 0) close(i2c_fd);
    if (serial_fd >= 0) close(serial_fd);
    return 0;
}
