#!/usr/bin/env python3
"""Dyno telemetry ingest: RP2040 serial NDJSON -> InfluxDB 2.x

This is a drop-in companion to `Dyno/dyno_simple.py`.

Behavior preserved from dyno_simple.py:
- Reads newline-delimited JSON from a serial port (ser.read_until(b"\n"))
- Skips DBG/# lines and non-JSON lines
- De-dupes packets using obj["pkt"] if present
- Re-opens the serial port on errors

Influx behavior:
- Writes to InfluxDB 2.x using influxdb-client
- Measurement defaults to "telemetry"
- Tags default to: system=dyno, node=rp2040
- Writes a fixed set of numeric fields by default: rpm,tps_pct,map_kpa,batt_v,coolant_c

Typical usage on the Pi (token from file):
  python3 -m pip install --upgrade pyserial influxdb-client
  export INFLUX_TOKEN_FILE="$HOME/dyno_stack/secrets/influxdb2-admin-token"
  python3 Dyno/dyno_ingest_influx.py
"""

from __future__ import annotations

import json
import os
import time
from typing import Any, Dict, Optional

import serial
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS


# ---- SERIAL CONFIG (defaults match Dyno/dyno_simple.py) ----
PORT = os.getenv(
    "TELEM_PORT",
    "/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00",
)
BAUD = int(os.getenv("TELEM_BAUD", "921600"))


# ---- INFLUX CONFIG ----
INFLUX_URL = os.getenv("INFLUX_URL", "http://localhost:8086")
INFLUX_ORG = os.getenv("INFLUX_ORG", "yorkracing")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "telemetry")

# Prefer token from file (safer than putting it in shell history)
INFLUX_TOKEN = os.getenv("INFLUX_TOKEN", "").strip()
INFLUX_TOKEN_FILE = os.getenv("INFLUX_TOKEN_FILE", "").strip()


# ---- POINT SHAPE ----
MEASUREMENT = os.getenv("TELEM_MEASUREMENT", "telemetry")
TAG_SYSTEM = os.getenv("TELEM_SYSTEM", "dyno")
TAG_NODE = os.getenv("TELEM_NODE", "rp2040")

# Fixed key list matching complete AN400 protocol (PE1-PE16)
DEFAULT_KEYS = ",".join([
    "rpm","tps_pct","fot_ms","ign_deg",
    "baro_kpa","map_kpa","lambda","lambda2","lambda_target",
    "batt_v","coolant_c","air_c","oil_psi",
    "ws_fl_hz","ws_fr_hz","ws_bl_hz","ws_br_hz",
    "ai1_v","ai2_v","ai3_v","ai4_v","ai5_v","ai6_v","ai7_v","ai8_v",
    "therm5_temp","therm7_temp",
    "rpm_rate_rps","tps_rate_pct_s","map_rate","maf_load_rate",
    "pwm_duty_pct_1","pwm_duty_pct_2","pwm_duty_pct_3","pwm_duty_pct_4",
    "pwm_duty_pct_5","pwm_duty_pct_6","pwm_duty_pct_7","pwm_duty_pct_8",
    "percent_slip","driven_wheel_roc","traction_desired_pct",
    "driven_avg_ws_ft_s","nondriven_avg_ws_ft_s",
    "ign_comp_deg","ign_cut_pct",
    "driven_ws1_ft_s","driven_ws2_ft_s",
    "nondriven_ws1_ft_s","nondriven_ws2_ft_s",
    "fuel_comp_accel_pct","fuel_comp_start_pct",
    "fuel_comp_air_pct","fuel_comp_coolant_pct",
    "fuel_comp_baro_pct","fuel_comp_map_pct",
    "ign_comp_air_deg","ign_comp_coolant_deg",
    "ign_comp_baro_deg","ign_comp_map_deg"
])
KEYS = [k.strip() for k in os.getenv("TELEM_KEYS", DEFAULT_KEYS).split(",") if k.strip()]


def load_token() -> str:
    """Load Influx token from env var or file."""
    if INFLUX_TOKEN:
        return INFLUX_TOKEN
    if INFLUX_TOKEN_FILE:
        with open(INFLUX_TOKEN_FILE, "r", encoding="utf-8") as f:
            return f.read().strip()
    raise SystemExit("Missing INFLUX_TOKEN or INFLUX_TOKEN_FILE")


def to_float(v: Any) -> Optional[float]:
    try:
        return float(v)
    except Exception:
        return None


def open_serial_forever() -> serial.Serial:
    """Open serial port, retrying forever."""
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=None)  # block until newline
            try:
                ser.reset_input_buffer()
            except Exception:
                pass
            print(f"[serial] connected {PORT} @ {BAUD}")
            return ser
        except Exception as e:
            print(f"[serial] open failed: {e} (retrying)")
            time.sleep(1.0)


def make_point(obj: Dict[str, Any]) -> Optional[Point]:
    p = Point(MEASUREMENT).tag("system", TAG_SYSTEM).tag("node", TAG_NODE)

    pkt = obj.get("pkt")
    if pkt is not None:
        try:
            p = p.field("pkt", int(pkt))
        except Exception:
            # Keep going even if pkt isn't an int
            pass

    wrote_any = False
    for k in KEYS:
        v = to_float(obj.get(k))
        if v is None:
            continue
        p = p.field(k, v)
        wrote_any = True

    return p if wrote_any else None


def main() -> None:
    token = load_token()

    influx = InfluxDBClient(url=INFLUX_URL, token=token, org=INFLUX_ORG)
    write_api = influx.write_api(write_options=SYNCHRONOUS)

    ser = open_serial_forever()

    last_pkt = None
    last_rate_t = time.time()
    count = 0

    while True:
        try:
            raw = ser.read_until(b"\n")
        except Exception as e:
            print(f"[serial] read error: {e} (reopening)")
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(0.5)
            ser = open_serial_forever()
            continue

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
            obj: Dict[str, Any] = json.loads(line)
        except json.JSONDecodeError:
            print(f"[bad json] {line[:120]}")
            continue

        pkt = obj.get("pkt")
        if pkt is not None and pkt == last_pkt:
            continue
        last_pkt = pkt

        point = make_point(obj)
        if point is not None:
            try:
                write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
                count += 1
            except Exception as e:
                print(f"[influx] write failed: {e}")

        # health print every ~2s
        now = time.time()
        if now - last_rate_t >= 2.0:
            rate = count / (now - last_rate_t)
            print(
                f"[ingest] {rate:.1f} pts/sec -> {INFLUX_BUCKET} "
                f"({TAG_SYSTEM}/{TAG_NODE}) @ {INFLUX_URL}"
            )
            count = 0
            last_rate_t = now


if __name__ == "__main__":
    main()
