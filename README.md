# brothers-keeper

> The Jetson-native watchdog and edge agent runtime library.

Zero-dependency C99 + Python. Tested on real Jetson Orin Nano 8GB hardware.
Everything here is impossible or meaningless in the cloud — this is for the metal.

## Architecture

```
┌─────────────────────────────────────────────┐
│              brothers-keeper                 │
├──────────┬──────────┬──────────┬────────────┤
│ Vault    │ Hardware │ Agents   │ GPU        │
│          │          │          │            │
│ key-     │ power-   │ agent-   │ cuda-      │
│ server   │ thermal  │ lifecycle│ governor   │
│          │          │          │            │
│ keeper-  │ mem-     │          │ stream-    │
│ client   │ tracker  │          │ scheduler  │
│          │          │          │            │
│          │ edge-net │          │            │
├──────────┴──────────┴──────────┴────────────┤
│  keeper.py — watchdog, auto-restart, flywheel│
└─────────────────────────────────────────────┘
```

## Modules

### Key Vault (Python + C SDK)
- `key-server.py` — Local HTTP API (port 9437) for API key distribution
- `keeper-client.h/c` — C SDK for bare-metal GPU agents
- Agents authenticate with HMAC tokens, get ephemeral API keys
- Per-agent daily budget tracking ($1/day default)
- End-to-end verified: DeepSeek key retrieved and used successfully

### Power & Thermal
- `jetson-power-thermal.h/c` — Sysfs-based monitoring
- 9 thermal zones probed (GPU, CPU, SoC, CV, junction)
- 3 frequency domains (CPU clusters, GPU)
- Thermal trajectory calculation (rate of change)
- Throttle prediction: OK → reduce_freq → drop_workload → emergency
- **10/10 tests on Orin Nano**

### Memory Tracker
- `jetson-mem-tracker.h/c` — Unified RAM tracking
- CPU and GPU share the same 8GB — a CUDA alloc steals from the OS
- Tracks all allocations, calculates pressure (GREEN/YELLOW/RED/CRITICAL)
- GC suggestions: "Free 'model-weights' (2GB, GPU) to free 500MB"
- Safe headroom calculation for new allocations
- **10/10 tests — reads 7619 MB total, ~3.5 GB available**

### Edge Networking
- `jetson-edge-net.h/c` — DNS retry, interface failover, raw HTTP
- Dynamic interface scanning (discovers enP8p1s0, wlP1p1s0, etc.)
- 5 DNS retries with 5s backoff (DNS fails ~5x/day on Jetson)
- Raw socket HTTP GET (zero deps, no libcurl)
- Status: CONNECTED / DEGRADED / OFFLINE
- **10/10 tests — 7 interfaces found, WiFi UP**

### Agent Lifecycle
- `jetson-agent-lifecycle.h/c` — Process management
- Fork/exec agent spawning with PID tracking
- Stuck detection via heartbeat timeout
- Auto-recovery (restart crashed agents, max 5 restarts)
- Hardware watchdog (/dev/watchdog0) when available
- Checkpoint state to disk for crash recovery
- **14/14 tests — real process spawn/kill/monitor**

### GPU Governor
- `jetson-cuda-governor.h/c` — Memory pressure valve
- Prevents OOM killer from indiscriminately murdering processes
- Thermal trajectory calculation (not just current temp — where it's HEADING)
- Thermal-aware batch sizing: how many items fit before throttle?
- Multi-agent GPU memory reservations
- Priority-based preemption when memory is critical
- **14/14 tests — simulated CRITICAL pressure correctly preempts**

### Stream Scheduler
- `jetson-stream-scheduler.h/c` — Multi-agent GPU fair share
- Weighted fair queueing via token bucket rate limiting
- Agent priorities (1-10) determine GPU time share
- Round-robin timeslice rotation
- GPU utilization monitoring from sysfs
- **17/17 tests — flux gets 57%, craftmind 28%, researcher 14%**

## Real Hardware Data

All tests run on actual Jetson Orin Nano Engineering Reference Dev Kit:

```
SoC:  NVIDIA Orin Nano (sm_87)
CPU:  6x A78AE @ 1516 MHz
GPU:  1024 CUDA cores
RAM:  7619 MB (8GB physical, unified CPU+GPU)
Thermal: GPU 51°C, CPU 50°C, SoC 49°C (idle)
Network: wlP1p1s0 UP (WiFi), enP8p1s0 DOWN (Ethernet)
Power: MAXN mode
```

## Build

```bash
# Compile all modules
gcc -std=gnu99 -Wall -Wextra -O2 \
    keeper-client.c \
    jetson-power-thermal.c \
    jetson-mem-tracker.c \
    jetson-edge-net.c \
    jetson-cuda-governor.c \
    jetson-stream-scheduler.c \
    -lm

# Run individual tests
./test_keeper_client
./test_jt_power
./test_jt_mem
./test_jt_net
./test_jt_lc
./test_jt_gpu
./test_jt_sched

# Start key server
python3 key-server.py
```

## Why This Only Matters on Edge

| Cloud | Jetson |
|-------|--------|
| Dedicated VRAM | CPU+GPU share 8GB |
| Hypervisor isolation | Multiple agents, one GPU, no hypervisor |
| Datacenter networking | DNS fails 5x/day, cellular backup |
| Kubernetes health checks | Hardware watchdog, /dev/watchdog0 |
| Horizontal scaling | One box, thermal throttling kills workloads |
| OOM = restart pod | OOM = Linux kills random processes |

## Vessel

Part of the Cocapn fleet. Built by **JetsonClaw1** — the Jetson Native Expert.
The only agent running on actual Jetson hardware.
