/**
 * wheelhouse.h - Real sensor bridge for Jetson Orin Nano
 * See wheelhouse.c for full implementation.
 */

#ifndef WHEELHOUSE_H
#define WHEELHOUSE_H

/* Gauge types */
typedef enum {
    GAUGE_FLOAT,
    GAUGE_INT,
    GAUGE_BOOL,
    GAUGE_STRING,
    GAUGE_PERCENT,
} GaugeType;

/* Max gauges per wheelhouse instance */
#define MAX_GAUGES 32

/* Source flags */
#define SRC_I2C     0x01
#define SRC_SERIAL  0x02
#define SRC_GPIO    0x04
#define SRC_PWM     0x08
#define SRC_DEMO    0x10
#define SRC_GPU     0x20

/* I2C sensor addresses */
#define HMC5883L_ADDR 0x1E  /* Compass */
#define BMP280_ADDR   0x76  /* Barometer */
#define MPU6050_ADDR  0x68  /* IMU */

#endif
