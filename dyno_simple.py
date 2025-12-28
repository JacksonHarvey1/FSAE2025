#!/usr/bin/env python3
import json
import time
import threading
import queue
from collections import deque

import serial
import matplotlib.pyplot as plt

# ---------- CONFIG ----------
PORT = "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00"
BAUD = 115200

WINDOW_S = 20.0

# Pick what you want to watch
KEYS = ["rpm", "tps_pct", "map_kpa", "batt_v", "coolant_c"]

PLOT_HZ = 10  # plot refresh rate
# ---------------------------

def serial_worker(q: "queue.Queue[dict]"):
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=None)  # block until newline
            try:
                ser.reset_input_buffer()
            except Exception:
                pass
            print(f"[serial] connected {PORT} @ {BAUD}")
            break
        except Exception as e:
            print(f"[serial] open failed: {e} (retrying)")
            time.sleep(1.0)

    last_pkt = None
    while True:
        try:
            raw = ser.read_until(b"\n")  # waits for newline (best for NDJSON)
        except Exception as e:
            print(f"[serial] read error: {e} (reopening)")
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(0.5)
            return  # let supervisor restart if needed

        if not raw:
            continue

        line = raw.decode(errors="ignore").strip()
        if not line:
            continue
        if line.startswith("#") or line.startswith("DBG"):
            continue
        if not line.startswith("{"):
            continue

        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            # Print a short snippet so you can see if you're getting chopped lines
            print(f"[bad json] {line[:120]}")
            continue

        pkt = obj.get("pkt")
        if pkt is not None and pkt == last_pkt:
            # same packet repeated; ignore
            continue
        last_pkt = pkt

        # Print so we can prove the Pi is seeing changes
        show = " ".join(f"{k}={obj.get(k)}" for k in KEYS)
        print(f"[pkt={pkt}] {show}")

        q.put(obj)


def main():
    q: "queue.Queue[dict]" = queue.Queue(maxsize=2000)
    t = threading.Thread(target=serial_worker, args=(q,), daemon=True)
    t.start()

    # rolling buffers
    ts = deque()
    series = {k: deque() for k in KEYS}

    plt.ion()
    fig, ax = plt.subplots()
    ax.set_title("RP2040 Telemetry (serial NDJSON)")
    ax.set_xlabel("Time (s)")
    ax.grid(True)

    lines = {}
    for k in KEYS:
        (ln,) = ax.plot([], [], label=k)
        lines[k] = ln
    ax.legend(loc="upper left")

    start = time.time()
    last_redraw = 0.0

    while True:
        now = time.time()

        # drain queue (consume ALL new packets)
        got_any = False
        while True:
            try:
                obj = q.get_nowait()
            except queue.Empty:
                break

            got_any = True
            t_now = time.time() - start
            ts.append(t_now)
            for k in KEYS:
                v = obj.get(k, None)
                try:
                    series[k].append(float(v))
                except Exception:
                    series[k].append(float("nan"))

        # prune window
        while ts and (ts[-1] - ts[0]) > WINDOW_S:
            ts.popleft()
            for k in KEYS:
                series[k].popleft()

        # redraw at fixed rate
        if (now - last_redraw) >= (1.0 / PLOT_HZ):
            last_redraw = now
            if ts:
                ax.set_xlim(max(0.0, ts[-1] - WINDOW_S), ts[-1])
                for k in KEYS:
                    lines[k].set_data(ts, series[k])
                ax.relim()
                ax.autoscale_view(scalex=False, scaley=True)
                fig.canvas.draw()
                fig.canvas.flush_events()

        time.sleep(0.01)


if __name__ == "__main__":
    main()
