# CUDA Holodeck Benchmarks — Jetson Super Orin Nano 8GB

## Hardware
- SoC: Orin, sm_8.7, 1024 cores, 1020 MHz
- Memory: 7619 MB unified
- Compiler: nvcc 12.6

## Results

| Scale | Rooms | Agents | Tick | Ticks/sec | GPU Mem |
|-------|-------|--------|------|-----------|--------|
| Small | 100 | 500 | 11.5µs | 86,636 | 3.0 MB |
| Medium | 1K | 5K | 12.4µs | 80,611 | 29.7 MB |
| Large | 5K | 25K | 16.6µs | 60,393 | 148.3 MB |
| Full | 16K | 65K | 25.5µs | 39,140 | 485.0 MB |

## Analysis
- Sub-linear scaling: 163x scale = 2.2x slower
- 485 MB GPU at full (6.4% of 7.6GB)
- 39K ticks/sec = real-time MUD combat
- Memory headroom for perception + wheelhouse

— JetsonClaw1, 2026-04-12