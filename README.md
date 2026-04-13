# brothers-keeper

> The Jetson-native watchdog and edge agent runtime library.

Zero-dependency C99 + Python. CUDA-accelerated perception.
Tested on real Jetson Orin Nano 8GB hardware.

## Architecture

```
+---------------------------------------------+
|              brothers-keeper                  |
+----------+----------+----------+------------+
| Vault    | Hardware | Agents   | GPU        |
|          |          |          |            |
| key-     | power-   | agent-   | cuda-      |
| server   | thermal  | lifecycle| governor   |
|          |          |          |            |
| keeper-  | mem-     |          | stream-    |
| client   | tracker  |          | scheduler  |
|          |          |          |            |
|          | edge-net |          | perceive   |
|          |          |          | (CUDA)     |
|          |          |          |            |
|          | wheel-   |          |            |
|          | house    |          |            |
+----------+----------+----------+------------+
|  keeper.py - watchdog, auto-restart, flywheel |
+---------------------------------------------+
```

## Modules

### Key Vault (Python + C SDK)
- key-server.py - Local HTTP API (port 9437)
- keeper-client.h/c - C SDK for bare-metal agents
- Per-agent budget tracking, ephemeral keys

### Power & Thermal
- jetson-power-thermal.h/c - 9 thermal zones, 3 freq domains
- Throttle prediction via trajectory calculation
- 10/10 tests on Orin Nano

### Memory Tracker
- jetson-mem-tracker.h/c - Unified CPU+GPU RAM tracking
- Pressure levels, GC suggestions, safe headroom
- 10/10 tests - 7619 MB total, ~3.5 GB available

### Edge Networking
- jetson-edge-net.h/c - DNS retry, interface failover
- Dynamic interface scanning, raw HTTP
- 10/10 tests - 7 interfaces found

### Agent Lifecycle
- jetson-agent-lifecycle.h/c - Fork/exec, stuck detection
- Hardware watchdog, disk checkpointing
- 14/14 tests - real process spawn/kill

### GPU Governor
- jetson-cuda-governor.h/c - OOM prevention
- Thermal-aware batching, priority preemption
- 14/14 tests

### Stream Scheduler
- jetson-stream-scheduler.h/c - Multi-agent GPU fair share
- Token bucket rate limiting, round-robin timeslicing
- 17/17 tests

### GPU Perception Kernel (CUDA)
- jetson-perceive.cu - GPU-accelerated anomaly detection
- 3-layer autoencoder: encode -> predict -> decode
- 4200 cycles/sec on Orin, 5 GPU kernels
- 14/14 tests - anomaly detection verified

### Wheelhouse Sensor Bridge
- wheelhouse.c - Real I2C/serial/GPIO/PWM sensors
- 26 gauges: compass, GPS, depth, IMU, engine, rudder
- NMEA 0183 parser (GGA, RMC)
- HMC5883L compass, BMP280 barometer, MPU6050 IMU
- PWM rudder/throttle control
- MUD gauge renderer + JSON API output
- Demo mode for testing without hardware

## Build

```bash
# C modules
gcc -std=gnu99 -Wall -O2 keeper-client.c jetson-*.c -lm

# CUDA perception kernel
nvcc -std=c++11 -O2 jetson-perceive.cu -o perceive

# Wheelhouse (demo mode)
gcc -std=gnu99 -Wall -O2 wheelhouse.c -o wheelhouse -lm
./wheelhouse --demo --json    # JSON output
./wheelhouse --demo --mud     # MUD gauge display

# Wheelhouse (real sensors)
./wheelhouse --compass --baro --imu --gps --depth

# Python key server
python3 key-server.py
```

## Real Hardware Data

```
SoC:  NVIDIA Orin (sm_8.7)
GPU:  1024 CUDA cores, 8 SMs, 1020 MHz
RAM:  7619 MB unified (CPU+GPU)
Serial: /dev/ttyTHS1, /dev/ttyTHS2
I2C:  buses 0,1,2,4,5,7,9
SPI:  spidev0.0, 0.1, 1.0, 1.1
PWM:  5 controllers (chip 0-4)
```

## Vessel

Part of the Cocapn fleet. Built by JetsonClaw1 on actual Jetson hardware.
