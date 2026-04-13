#!/usr/bin/env python3
"""wheelhouse-api.py — HTTP bridge between /tmp/wheelhouse binary and MUD/external consumers.
Stdlib only. Port 9440. Jetson Orin Nano."""

import json
import logging
import select
import subprocess
import sys
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

logging.basicConfig(stream=sys.stderr, level=logging.INFO,
                    format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("wheelhouse-api")

WHEELHOUSE_BIN = "/tmp/wheelhouse"
WHEELHOUSE_ARGS = ["--demo", "--json"]
PORT = 9440
HEALTH_CHECK_INTERVAL = 5

state = {}
state_lock = threading.Lock()
start_time = time.time()
wheelhouse_proc = None


def log_ts(msg):
    log.info(msg)


def spawn_wheelhouse():
    global wheelhouse_proc
    log_ts(f"Spawning {WHEELHOUSE_BIN} {' '.join(WHEELHOUSE_ARGS)}")
    try:
        wheelhouse_proc = subprocess.Popen(
            [WHEELHOUSE_BIN] + WHEELHOUSE_ARGS,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=1,
        )
    except FileNotFoundError:
        log.error(f"{WHEELHOUSE_BIN} not found")
        return False
    except Exception as e:
        log.error(f"Failed to spawn wheelhouse: {e}")
        return False
    return True


def reader_loop():
    """Read stdout line by line, update state."""
    global wheelhouse_proc
    while True:
        proc = wheelhouse_proc
        if proc is None or proc.poll() is not None:
            code = proc.returncode if proc else "no proc"
            log_ts(f"Wheelhouse exited (code={code}), restarting in 2s...")
            time.sleep(2)
            if not spawn_wheelhouse():
                time.sleep(5)
                continue
            proc = wheelhouse_proc

        line = proc.stdout.readline()
        if not line:
            time.sleep(0.1)
            continue
        line = line.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        try:
            data = json.loads(line)
            with state_lock:
                state.update(data)
        except json.JSONDecodeError:
            log_ts(f"Non-JSON line from wheelhouse: {line[:120]}")


def health_checker():
    """Periodically check subprocess is alive, restart if not."""
    while True:
        time.sleep(HEALTH_CHECK_INTERVAL)
        if wheelhouse_proc is None or wheelhouse_proc.poll() is not None:
            log_ts("Health check: wheelhouse dead, restarting")
            spawn_wheelhouse()


def get_sub(prefixes):
    """Extract subset of state keys matching given prefixes."""
    with state_lock:
        return {k: v for k, v in state.items()
                if any(k.startswith(p) for p in prefixes)}


# Endpoint routing
ROUTES = {
    "/gauges": lambda: state.copy() if state else {},
    "/nav": lambda: get_sub(["heading", "speed", "depth", "lat", "lon", "sats", "gps"]),
    "/engine": lambda: get_sub(["rpm", "fuel", "engine"]),
    "/environment": lambda: get_sub(["temp", "pressure", "wind", "humidity"]),
    "/motion": lambda: get_sub(["accel", "roll", "pitch", "gyro", "imu"]),
    "/control": lambda: get_sub(["rudder", "throttle"]),
    "/system": lambda: get_sub(["gpu", "cpu", "ram", "mem", "load"]),
}

CORS_HEADERS = {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log_ts(f"{self.client_address[0]} - {fmt % args}")

    def _cors(self):
        for k, v in CORS_HEADERS.items():
            self.send_header(k, v)

    def _json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self._cors()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path.rstrip("/")

        if path == "/health":
            alive = wheelhouse_proc is not None and wheelhouse_proc.poll() is None
            self._json({
                "status": "ok",
                "uptime_seconds": round(time.time() - start_time, 1),
                "wheelhouse_running": alive,
            })
            return

        if path == "/pulse":
            with state_lock:
                s = state
            parts = []
            for key in ["heading", "speed", "depth"]:
                if key in s:
                    parts.append(f"{key}:{s[key]}")
            for key in ["gpu_temp", "gpu"]:
                if key in s:
                    parts.append(f"gpu:{s[key]}")
                    break
            for key in ["ram_used", "ram", "mem_used"]:
                if key in s:
                    parts.append(f"ram:{s[key]}")
                    break
            body = " ".join(parts).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self._cors()
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if path.startswith("/gauges/"):
            name = path[len("/gauges/"):]
            with state_lock:
                if name in state:
                    self._json({name: state[name]})
                else:
                    self._json({"error": f"gauge '{name}' not found"}, 404)
            return

        handler = ROUTES.get(path)
        if handler:
            with state_lock:
                snap = dict(state)
            # Build subset from snapshot
            self._json(handler())
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        path = urlparse(self.path).path.rstrip("/")
        if path == "/command":
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length))
            except (json.JSONDecodeError, ValueError):
                self._json({"error": "invalid JSON"}, 400)
                return
            # Store command in state for future PWM use
            with state_lock:
                for k in ("rudder", "throttle"):
                    if k in body:
                        state[k] = float(body[k])
            log_ts(f"Command received: {body}")
            self._json({"status": "accepted", "command": body})
        else:
            self._json({"error": "not found"}, 404)


def main():
    if not spawn_wheelhouse():
        log_ts("WARNING: wheelhouse binary not available, running with empty state")

    t_reader = threading.Thread(target=reader_loop, daemon=True)
    t_reader.start()

    t_health = threading.Thread(target=health_checker, daemon=True)
    t_health.start()

    server = HTTPServer(("0.0.0.0", PORT), Handler)
    log_ts(f"wheelhouse-api listening on :{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log_ts("Shutting down")
        server.shutdown()


if __name__ == "__main__":
    main()
