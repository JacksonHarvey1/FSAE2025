"""Serial ingestion service for RP2040 JSON telemetry.

Reads NDJSON from /dev/ttyACM0, parses it, maintains a latest snapshot,
and optionally logs to per-run files.

Expected line format from RP2040 (CAN_to_Pi_USB.ino):

    {"ts_ms":123456,"pkt":42,"src":"can","node_id":1,
     "rpm":5234,"map_kpa":98.3,...}
"""

from __future__ import annotations

import json
import os
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, Optional, IO

import serial  # type: ignore

from .config import settings


@dataclass
class IngestStats:
    start_time: float = field(default_factory=time.time)
    last_packet_time: float = 0.0
    total_packets: int = 0
    drop_count: int = 0
    last_pkt_id: Optional[int] = None

    def packet_rate_hz(self) -> float:
        dt = max(1e-3, time.time() - self.start_time)
        return self.total_packets / dt

    def last_packet_age_s(self) -> Optional[float]:
        if self.last_packet_time <= 0:
            return None
        return max(0.0, time.time() - self.last_packet_time)


class SerialIngestor:
    """Background thread that ingests telemetry from a serial port."""

    def __init__(self, port: str | None = None, baud: int | None = None) -> None:
        self.port = port or settings.serial_port
        self.baud = baud or settings.serial_baud

        self._lock = threading.Lock()
        self._latest: Dict[str, float] = {}
        self._stats = IngestStats()
        self._serial: Optional[serial.Serial] = None
        self._stop = threading.Event()

        # Run management / logging
        self._run_id: Optional[str] = None
        self._run_log_fh: Optional[IO[str]] = None
        self._run_csv_fh: Optional[IO[str]] = None
        self._csv_header_written: bool = False

        os.makedirs(settings.runs_dir, exist_ok=True)

    # --------- public API ---------

    def start(self) -> None:
        t = threading.Thread(target=self._run_loop, daemon=True)
        t.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            if self._serial and self._serial.is_open:
                self._serial.close()
        except Exception:
            pass

    def latest(self) -> Dict[str, float]:
        with self._lock:
            return dict(self._latest)

    def stats(self) -> IngestStats:
        with self._lock:
            # Return a shallow copy to avoid races
            s = IngestStats()
            s.start_time = self._stats.start_time
            s.last_packet_time = self._stats.last_packet_time
            s.total_packets = self._stats.total_packets
            s.drop_count = self._stats.drop_count
            s.last_pkt_id = self._stats.last_pkt_id
            return s

    def serial_connected(self) -> bool:
        return bool(self._serial and self._serial.is_open)

    # Run management
    def current_run_id(self) -> Optional[str]:
        return self._run_id

    def start_run(self, run_id: Optional[str] = None) -> str:
        """Begin a new run and open log files under runs/<run_id>/.

        If run_id is not provided, a timestamp-based ID is generated.
        """

        if run_id is None:
            run_id = time.strftime("%Y%m%d_%H%M%S")

        run_dir = os.path.join(settings.runs_dir, run_id)
        os.makedirs(run_dir, exist_ok=True)

        raw_path = os.path.join(run_dir, "raw_ndjson.log")
        csv_path = os.path.join(run_dir, "telemetry.csv")

        # Close previous run if any
        self.stop_run()

        self._run_log_fh = open(raw_path, "a", encoding="utf-8")
        self._run_csv_fh = open(csv_path, "a", encoding="utf-8", newline="")
        self._csv_header_written = False
        self._run_id = run_id

        return run_id

    def stop_run(self) -> None:
        if self._run_log_fh:
            self._run_log_fh.close()
            self._run_log_fh = None
        if self._run_csv_fh:
            self._run_csv_fh.close()
            self._run_csv_fh = None
        self._csv_header_written = False
        self._run_id = None

    # --------- internal loop ---------

    def _open_serial(self) -> None:
        while not self._stop.is_set():
            try:
                self._serial = serial.Serial(self.port, self.baud, timeout=1)
                return
            except Exception as e:
                print(f"[ingest] Serial open failed on {self.port}: {e}; retrying...")
                time.sleep(1.5)

    def _run_loop(self) -> None:
        self._open_serial()
        print(f"[ingest] Listening on {self.port} @ {self.baud}")

        while not self._stop.is_set():
            if not self._serial or not self._serial.is_open:
                self._open_serial()
                continue

            try:
                raw = self._serial.readline()
            except Exception as e:
                print(f"[ingest] Serial error: {e}; reopening port")
                try:
                    if self._serial:
                        self._serial.close()
                except Exception:
                    pass
                time.sleep(1.0)
                continue

            if not raw:
                continue

            try:
                line = raw.decode(errors="ignore").strip()
            except Exception:
                continue

            if not line:
                continue

            if line.startswith("#") or line.startswith("DBG"):
                # Status/debug from firmware
                print(line)
                continue

            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                # Bad line; you may want to log this separately
                print(f"[ingest] Bad JSON: {line[:120]}")
                continue

            if not isinstance(obj, dict):
                continue

            self._handle_packet(obj)

    def _handle_packet(self, pkt: Dict[str, float]) -> None:
        now = time.time()

        with self._lock:
            # Update stats
            self._stats.total_packets += 1
            self._stats.last_packet_time = now

            pkt_id = pkt.get("pkt")
            if isinstance(pkt_id, (int, float)):
                pkt_id_int = int(pkt_id)
                if self._stats.last_pkt_id is not None and pkt_id_int > self._stats.last_pkt_id + 1:
                    self._stats.drop_count += pkt_id_int - self._stats.last_pkt_id - 1
                self._stats.last_pkt_id = pkt_id_int

            # Update latest snapshot
            self._latest.update(pkt)

            # Logging for current run
            if self._run_log_fh:
                self._run_log_fh.write(json.dumps(pkt) + "\n")
                self._run_log_fh.flush()

            if self._run_csv_fh:
                self._write_csv_row(pkt)

    def _write_csv_row(self, pkt: Dict[str, float]) -> None:
        import csv

        if not self._run_csv_fh:
            return

        # Fixed column order for reproducibility
        columns = [
            "ts_ms",
            "pi_ts_ms",
            "pkt",
            "src",
            "node_id",
            "rpm",
            "tps_pct",
            "fot_ms",
            "ign_deg",
            "baro_kpa",
            "map_kpa",
            "lambda",
            "batt_v",
            "coolant_c",
            "air_c",
            "oil_psi",
            "ws_fl_hz",
            "ws_fr_hz",
            "ws_bl_hz",
            "ws_br_hz",
        ]

        writer = csv.DictWriter(self._run_csv_fh, fieldnames=columns)

        if not self._csv_header_written:
            writer.writeheader()
            self._csv_header_written = True

        row = {k: "" for k in columns}
        now_ms = int(time.time() * 1000)
        row["pi_ts_ms"] = now_ms

        for k in columns:
            if k in ("pi_ts_ms",):
                continue
            if k in pkt:
                row[k] = pkt[k]

        writer.writerow(row)
        self._run_csv_fh.flush()


if __name__ == "__main__":
    ing = SerialIngestor()
    ing.start()
    print("[ingest] Running; Ctrl+C to stop")
    try:
        while True:
            time.sleep(2.0)
            s = ing.stats()
            print(
                f"pkts={s.total_packets}, rate={s.packet_rate_hz():.1f} Hz, "
                f"drops={s.drop_count}, age={s.last_packet_age_s()} s",
            )
    except KeyboardInterrupt:
        print("[ingest] Stopping")
        ing.stop()

