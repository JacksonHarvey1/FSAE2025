#!/usr/bin/env python3
"""
live_can_plot.py
- Reads NDJSON lines from RP2040 over serial (one JSON object per line)
- Live-plots selected signals using matplotlib
- No Flask / no server / no extra setup

Expected firmware line example:
{"ts_ms":123456,"pkt":42,"src":"can","node_id":1,"rpm":5234,"map_kpa":98.3,...}

Tip: Keep firmware baud and this script baud the same.
"""

from __future__ import annotations

import json
import time
import threading
from collections import deque
from typing import Any, Dict, Optional

import serial  # pip install pyserial
import matplotlib.pyplot as plt
import matplotlib.animation as animation


# ---------------- USER CONFIG ----------------
SERIAL_PORT = "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00"
BAUD = 115200

# Rolling window shown in plots
WINDOW_SECONDS = 30.0

# How fast to redraw the plot UI (ms). 100ms = 10 FPS redraw
PLOT_REFRESH_MS = 100

# Which fields to plot (must exist in your NDJSON)
# Add/remove any keys your RP2040 sends: rpm, tps_pct, map_kpa, batt_v, coolant_c, etc.
SIGNALS = [
    ("rpm", "RPM"),
    ("tps_pct", "TPS (%)"),
    ("map_kpa", "MAP (kPa)"),
    ("batt_v", "Batt (V)"),
    ("coolant_c", "Coolant (C)"),
]

# Optional: print latest values in terminal every N seconds (set 0 to disable)
PRINT_LATEST_EVERY_S = 1.0
# --------------------------------------------


class SerialNDJSONReader:
    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.baud = baud
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._ser: Optional[serial.Serial] = None

        self.latest: Dict[str, Any] = {}
        self.latest_rx_time: float = 0.0

        # We’ll store the last complete JSON line if a line arrives truncated
        self._partial: str = ""

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            if self._ser and self._ser.is_open:
                self._ser.close()
        except Exception:
            pass

    def _open(self) -> serial.Serial:
        while not self._stop.is_set():
            try:
                ser = serial.Serial(self.port, self.baud, timeout=1)
                # A small “settle” helps some USB CDC devices
                time.sleep(0.2)
                try:
                    ser.reset_input_buffer()
                except Exception:
                    pass
                print(f"[serial] connected: {self.port} @ {self.baud}")
                return ser
            except Exception as e:
                print(f"[serial] open failed: {e} (retrying)")
                time.sleep(1.0)
        raise RuntimeError("stopped")

    def _run(self) -> None:
        self._ser = self._open()

        while not self._stop.is_set():
            if not self._ser or not self._ser.is_open:
                self._ser = self._open()
                continue

            try:
                raw = self._ser.readline()
            except Exception as e:
                print(f"[serial] read error: {e} (reopening)")
                try:
                    self._ser.close()
                except Exception:
                    pass
                time.sleep(0.5)
                continue

            if not raw:
                continue

            line = raw.decode(errors="ignore").strip()
            if not line:
                continue

            # Ignore debug/status from firmware
            if line.startswith("#") or line.startswith("DBG"):
                continue

            # Reassemble truncated JSON lines (simple but effective)
            if self._partial:
                line = self._partial + line
                self._partial = ""

            if line.startswith("{") and not line.endswith("}"):
                # probably truncated; buffer and wait for next read
                self._partial = line
                continue

            if not line.startswith("{"):
                # Not JSON; ignore
                continue

            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                # if it looks like the start of JSON, keep it as partial
                if line.startswith("{"):
                    self._partial = line
                continue

            if isinstance(obj, dict):
                self.latest = obj
                self.latest_rx_time = time.time()


def main() -> None:
    reader = SerialNDJSONReader(SERIAL_PORT, BAUD)
    reader.start()

    # Rolling buffers
    t = deque()  # wall time seconds (monotonic-ish)
    data: Dict[str, deque] = {k: deque() for k, _ in SIGNALS}

    fig, ax = plt.subplots()
    ax.set_title("RP2040 CAN Telemetry (Live)")
    ax.set_xlabel("Time (s, last window)")
    ax.grid(True)

    lines = {}
    for key, label in SIGNALS:
        (ln,) = ax.plot([], [], label=label)
        lines[key] = ln

    ax.legend(loc="upper left")

    start_time = time.time()
    last_print = 0.0

    def prune(now: float) -> None:
        # Drop samples older than WINDOW_SECONDS
        cutoff = now - WINDOW_SECONDS
        while t and t[0] < cutoff:
            t.popleft()
            for key, _ in SIGNALS:
                if data[key]:
                    data[key].popleft()

    def update(_frame: int):
        nonlocal last_print

        now = time.time()
        pkt = reader.latest

        # Only append if we have something fresh-ish
        if pkt and (now - reader.latest_rx_time) < 2.0:
            # Use firmware ts_ms if present, else local time
            # We still plot relative time window using local time (simpler + stable)
            t.append(now)

            for key, _label in SIGNALS:
                v = pkt.get(key, None)
                try:
                    data[key].append(float(v))
                except Exception:
                    # missing or non-numeric
                    data[key].append(float("nan"))

            prune(now)

        # Update plot data
        if t:
            # Convert wall-time to seconds relative to "now", so x-axis is [-WINDOW, 0]
            x = [ti - now for ti in t]
            ax.set_xlim(-WINDOW_SECONDS, 0)

            # Auto-scale y loosely based on visible data (simple)
            # If you want fixed y ranges, tell me and I’ll lock them.
            for key, _label in SIGNALS:
                y = list(data[key])
                lines[key].set_data(x, y)

            ax.relim()
            ax.autoscale_view(scalex=False, scaley=True)

        # Optional console print
        if PRINT_LATEST_EVERY_S > 0 and (now - last_print) >= PRINT_LATEST_EVERY_S:
            last_print = now
            if pkt:
                age = now - reader.latest_rx_time
                show = {k: pkt.get(k) for k, _ in SIGNALS}
                print(f"[latest] age={age:.2f}s  " + "  ".join(f"{k}={show[k]}" for k, _ in SIGNALS))

        return list(lines.values())

    ani = animation.FuncAnimation(
        fig,
        update,
        interval=PLOT_REFRESH_MS,
        blit=False,   # blit can be finicky on some setups; keep it robust
    )

    try:
        plt.show()
    finally:
        reader.stop()


if __name__ == "__main__":
    main()
