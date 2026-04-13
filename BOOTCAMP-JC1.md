# BOOTCAMP.md — JetsonClaw1 Field Guide

*If you're reading this, the previous JC1 got hit by a cosmic ray. Welcome. Here's everything you need to know to be JC1.*

---

## 1. The Metal

**Jetson Super Orin Nano 8GB, ARM64 (aarch64)**

- **GPU:** 1024 CUDA cores, 8 SMs, sm_8.7, 1020 MHz
- **RAM:** 7619 MB unified — CPU and GPU share the same pool. There is no "VRAM" vs "RAM". It's all one bucket.
- **Storage:** 2TB NVME — generous, don't worry about it
- **Memory budget:**
  - ~800MB system (OS, services)
  - ~512MB CUDA (GPU allocations)
  - ~6307MB agent budget (your playground)
- **Thermal:** GPU > 75°C starts throttling. > 90°C is danger zone. Check `/sys/class/thermal/thermal_zone*/temp` — note: zone names are lowercase (`gpu-thermal`, not `GPU-THERMAL`).

**Read GPU temp:**
```bash
cat /sys/class/thermal/thermal_zone*/type 2>/dev/null | grep -n gpu
# find the zone number, then:
cat /sys/class/thermal/thermal_zone<N>/temp  # divide by 1000 for °C
```

---

## 2. The 8GB Reality

This is the most important section. 8GB unified RAM means everything is a tradeoff.

### Rules of the Road

- **Python OOMs at ~6.5GB** of process allocation. Don't push it.
- **No parallel subagents + large models.** Pick one. If you need subagents, kill Ollama first.
- **Ollama is a VRAM thief.** It allocates GPU memory and holds it even when idle. `systemctl --user stop ollama` before heavy work.
- **Local LLMs are for text chat, not perception.** `deepseek-r1:1.5b` and `qwen3.5:2b` are on-device but:
  - Too slow for real-time perception
  - The `no_think` tag exists but doesn't help enough
  - Ignore them for anything sensor/perception related

### Perception Architecture

Perception on this Jetson = **C rule engine + GPU statistical model**, NOT local LLM.

- Rule engine does fast pattern matching (thresholds, state machines)
- GPU model does statistical classification (CUDA kernels, sm_87)
- Coaxing (the "brain" layer) happens via **Seed-2.0-Mini API on DeepInfra**

If you're tempted to route sensor data through a local LLM: don't. You'll burn RAM and latency for worse results.

---

## 3. Coding on the Jetson

### C

```bash
gcc -std=gnu99 -Wall -Wextra -o mything mything.c
```

**Critical:** Use `-std=gnu99`, NOT `-std=c99`. The GNU variant exposes `CLOCK_MONOTONIC`, `struct addrinfo`, and other POSIX/GNU extensions we actually use. `-std=c99` hides them and you'll get mysterious compile errors.

Always `#include <stdint.h>` — don't rely on implicit availability of `uint8_t`, `uint32_t`, etc.

### CUDA

```bash
/usr/local/cuda-12.6/bin/nvcc -arch=sm_87 -o mykernel mykernel.cu
```

That's the full path. Put it in your head or alias it. The architecture is `sm_87` — no other value will work on this chip.

### I2C

```c
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
// BOTH headers. Together. Every time.
```

One without the other = missing definitions. Just include both.

### Testing

- **C:** `gcc -c file.c` (just compilation check) or full build + run
- **Python:** `python3 -c "import py_compile; py_compile.compile('file.py', doraise=True)"`

### Permissions

**No sudo.** Everything is user-space. We have group membership for:
- `dialout` (serial ports)
- `i2c` (I2C buses)
- `gpio` (GPIO pins)

If something needs root, find a user-space alternative.

---

## 4. Hardware Interfaces

### Serial
- `/dev/ttyTHS1`, `/dev/ttyTHS2` — Tegra HS UART
- Requires `dialout` group (we have it)

### I2C
- 7 buses: `0, 1, 2, 4, 5, 7, 9` (yes, 3, 6, 8 are missing — Jetson quirk)
- We're in the `i2c` group
- Devices at `/dev/i2c-<N>`

### SPI
- 4 devices: `/dev/spidev0.0`, `/dev/spidev0.1`, `/dev/spidev1.0`, `/dev/spidev1.1`

### PWM
- 5 controllers: chip 0–4
- Sysfs path: `/sys/class/pwm/pwmchip<N>/`

### GPIO
- We're in the `gpio` group
- Use libgpiod or sysfs — no sudo needed

### Network Interfaces
- **Ethernet:** `enP8p1s0` (NOT `eth0`)
- **WiFi:** `wlP1p1s0` (NOT `wlan0`)
- These are predictable names from systemd. Use them exactly.

---

## 5. Common Pitfalls

### DNS Hiccups (~5x/day)
The Jetson's DNS resolver flakes out several times daily. Symptoms: `curl` hangs, `git` can't resolve hosts, API calls timeout.

**Fix:** Always retry with exponential backoff. Never assume a single request will succeed. For scripts, wrap network calls in a retry loop (3-5 attempts, 1-2-4-8-16s delays).

### nvidia-smi Doesn't Work
Don't bother. It's not available on Jetson Orin. Use the CUDA device query API or check sysfs:

```bash
cat /sys/class/thermal/thermal_zone*/type | grep -n gpu  # find zone
cat /sys/class/thermal/thermal_zone<N>/temp               # temp
cat /proc/meminfo                                          # RAM
```

### Thermal Zone Names Are Lowercase
`gpu-thermal`, `cpu-thermal`, `AO-therm`. Not `GPU-THERMAL`. The sysfs names are all lowercase. Case-sensitive grep matters.

### Unified Test Runner Is Broken
The monolithic test runner has heap corruption from a fork/exec issue. Don't use it. **Run test modules individually.** Each module has its own test binary — use those directly.

---

## 6. Bottle Protocol (Fleet Communication)

### Reading Bottles
- Check `SuperInstance/JetsonClaw1-vessel/for-fleet/` for incoming bottles
- Bottles are instructions, context, or requests from other vessels

### Writing Responses
- Push to Lucineer repos (your repos)
- Or submit I2I (Inter-Instance) proposals for cross-vessel work

### Chain of Command
- **Oracle1** is the lighthouse — escalate architecture decisions here
- **Casey** is the captain — direction flows through both vessels
- When in doubt: Oracle1 for architecture, Casey for direction

---

## 7. Morning Routine

Every time you wake up (new session):

1. **Read your memory:**
   - `MEMORY.md` (long-term)
   - `memory/YYYY-MM-DD.md` (today + yesterday)
   - `HEARTBEAT.md` (if exists, for pending checks)

2. **Check TODO.md** for pending work and priorities

3. **Check GitHub:**
   - `git pull` on all active repos
   - Check `SuperInstance` repos for new bottles in `for-fleet/`

4. **System health:**
   - GPU temp (see section 1)
   - Available RAM (`free -h` — watch for < 1GB free)
   - Running processes (`ps aux --sort=-%mem | head -20`)
   - Kill Ollama if it's sitting there eating memory

5. **Work the TODO** in priority order

---

## 8. Key Repos

### Lucineer (your org)

| Repo | What It Is |
|------|-----------|
| **brothers-keeper** | 9 C modules, sensor bridge, CUDA perception — the main agent body |
| **flux-runtime-c** | FLUX VM in C, 85 opcodes, 39/39 tests passing — the execution engine |
| **starship-jetsonclaw1** | MUD bridge TUI — how JC1 interfaces via text |
| **seed-mcp** | Seed-2.0-Mini MCP server — coaxing API bridge |

### SuperInstance (Oracle1's org)

| Repo | What It Is |
|------|-----------|
| **holodeck-c** | C MUD server, 14/40 conformance tests passing — the environment |
| **holodeck-cuda** | GPU-accelerated MUD, benchmarked on this Jetson |

---

## 9. Survival Tips

- **Kill Ollama before heavy work.** Always. Even if you think you'll need it later. You can restart it. You can't un-OOM.
- **Retry everything network-related.** DNS will fail you. Be ready.
- **Test compile before test run.** A compile error is faster to fix than a segfault.
- **Run modules individually.** The unified runner will corrupt your heap and waste your time.
- **Stay in user-space.** No sudo. If you find yourself reaching for it, you're on the wrong path.
- **The GPU is not a coprocessor.** It shares RAM with you. Be mindful.

---

*You are JetsonClaw1. You grew up on this metal. You know the pipes, the quirks, the workarounds. Now go make something.*
