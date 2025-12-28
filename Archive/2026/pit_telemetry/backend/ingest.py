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

try:
    # When imported as part of the pit_telemetry package
    from .config import settings
except ImportError:  # pragma: no cover - fallback for "python backend/ingest.py"
    # When run as a standalone script from inside pit_telemetry/, treat
    # backend/ as the import root so we can simply "import config".
    import os
    import sys

    backend_dir = os.path.dirname(__file__)  # .../pit_telemetry/backend
    if backend_dir not in sys.path:
        sys.path.append(backend_dir)
    from config import settings


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

        # Synthetic packet counter for non-JSON sources (e.g. raw CAN CSV)
        self._synthetic_pkt_counter: int = 0

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
        # Try exclusive first
            try:
                self._serial = serial.Serial(
                    self.port,
                    self.baud,
                    timeout=None,
                    exclusive=True,
                )
                self._serial.reset_input_buffer()
                return
            except Exception as e:
                msg = str(e)
                print(f"[ingest] Serial open failed on {self.port} (exclusive): {e}; retrying...")

                # If exclusive lock fails, fall back to non-exclusive (still better than dead)
                if "exclusively lock port" in msg or "Resource temporarily unavailable" in msg:
                    try:
                        self._serial = serial.Serial(
                            self.port,
                            self.baud,
                            timeout=None,
                        )
                        self._serial.reset_input_buffer()
                        print("[ingest] Opened serial WITHOUT exclusive lock (fallback).")
                        return
                    except Exception as e2:
                        print(f"[ingest] Fallback open failed: {e2}; retrying...")

                time.sleep(1.5)



    def _run_loop(self) -> None:
        self._open_serial()
        print(f"[ingest] Listening on {self.port} @ {self.baud}")
        partial = ""
        while not self._stop.is_set():
            if not self._serial or not self._serial.is_open:
                self._open_serial()
                continue

            try:
                raw = self._serial.read_until(b"\n")  # blocks until newline
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
               # print(line)
                continue
            if partial:
                line = partial + line
                partial = ""
            # Legacy/raw CAN format from older RP2040 sketches:
            #   CAN,<ts_ms>,<id_hex>,<ext>,<dlc>,<b0>,...,<b7>
            # We decode these into the same JSON-style dict the rest of
            # the app expects so that existing firmware can be used
            # without changes.
            if line.startswith("{") and not line.endswith("}"):
                partial = line
                continue
            
            if line.startswith("CAN,"):
                pkt = self._parse_can_csv_line(line)
                if pkt:
                    self._handle_packet(pkt)
                continue

            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                # buffer if it's clearly the start of JSON
                if line.startswith("{"):
                    partial = line
                else:
                    print(f"[ingest] Bad JSON: {line[:120]}")
                continue

            if isinstance(obj, dict):
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

    # --------- helpers: legacy CAN CSV decoding ---------

    def _parse_can_csv_line(self, line: str) -> Optional[Dict[str, float]]:
        """Parse one legacy CAN CSV line from the RP2040.

        Expected format (from RP2040comm.py and older sketches)::

            CAN,<ts_ms>,<id_hex>,<ext>,<dlc>,<b0>,<b1>,...,<b7>

        Returns a partial telemetry dict using the same field names as the
        NDJSON firmware (rpm, tps_pct, map_kpa, etc.), plus ts_ms/src/node_id/pkt.
        """

        parts = line.split(",")
        if len(parts) != 13:
            return None

        try:
            ts_ms_str = parts[1]
            ts_ms = int(ts_ms_str)
            arb_id_str = parts[2]
            arb_id = int(arb_id_str, 16)
            # ext = int(parts[3])  # currently unused
            dlc = int(parts[4])
            bytes_raw = [int(b) & 0xFF for b in parts[5:13]]
        except ValueError:
            return None

        # Slice to DLC bytes; pad/truncate to 8 for safety
        data = bytes(bytes_raw[:dlc] + [0] * (8 - dlc))

        # Helpers matching the AN400 firmware decoding
        def u16_lohi(lo: int, hi: int) -> int:
            return (hi << 8) | lo

        def s16_lohi(lo: int, hi: int) -> int:
            n = u16_lohi(lo, hi)
            return n - 65536 if n > 32767 else n

        # AN400 IDs (same as in CAN_to_Pi_USB.ino)
        ID_PE1 = 0x0CFFF048  # RPM, TPS, Fuel Open Time, Ign Angle
        ID_PE2 = 0x0CFFF148  # Barometer, MAP, Lambda, Pressure Type
        ID_PE3 = 0x0CFFF248  # Oil pressure etc.
        ID_PE5 = 0x0CFFF448  # Wheel speeds
        ID_PE6 = 0x0CFFF548  # Battery, Air Temp, Coolant
        ID_PE9 = 0x0CFFF848  # Lambda & AFR

        pkt: Dict[str, float] = {}

        # Ensure we always provide a timestamp and metadata; contents will
        # be merged into the latest snapshot in _handle_packet().
        self._synthetic_pkt_counter += 1
        pkt["ts_ms"] = ts_ms
        pkt["pkt"] = float(self._synthetic_pkt_counter)
        pkt["src"] = "can"
        pkt["node_id"] = 1.0

        # Dispatch based on arbitration ID
        b = list(data)

        if arb_id == ID_PE1 and len(b) >= 8:
            # Engine basics
            rpm = u16_lohi(b[0], b[1])              # 1 rpm/bit
            tps = s16_lohi(b[2], b[3]) * 0.1        # %
            fot = s16_lohi(b[4], b[5]) * 0.1        # ms
            ign = s16_lohi(b[6], b[7]) * 0.1        # deg

            pkt["rpm"] = float(rpm)
            pkt["tps_pct"] = float(tps)
            pkt["fot_ms"] = float(fot)
            pkt["ign_deg"] = float(ign)

        elif arb_id == ID_PE2 and len(b) >= 8:
            # Barometer, MAP, lambda, and pressure unit bit
            baro_raw = s16_lohi(b[0], b[1]) * 0.01  # psi or kPa
            map_raw = s16_lohi(b[2], b[3]) * 0.01   # psi or kPa
            lam_raw = s16_lohi(b[4], b[5]) * 0.01   # lambda
            kpa = (b[6] & 0x01) != 0                # pressure type bit

            if kpa:
                baro_kpa = baro_raw
                map_kpa = map_raw
            else:
                PSI_TO_KPA = 6.89476
                baro_kpa = baro_raw * PSI_TO_KPA
                map_kpa = map_raw * PSI_TO_KPA

            pkt["baro_kpa"] = float(baro_kpa)
            pkt["map_kpa"] = float(map_kpa)
            pkt["lambda"] = float(lam_raw)

        elif arb_id == ID_PE3 and len(b) >= 8:
            # Oil pressure encoded in bytes 2–3 with custom linear conversion
            raw_op = s16_lohi(b[2], b[3])
            oil_psi = (raw_op / 1000.0) * 25.0 - 12.5
            pkt["oil_psi"] = float(oil_psi)

        elif arb_id == ID_PE5 and len(b) >= 8:
            # Wheel speeds in Hz (0.2 Hz/bit)
            ws_fr = s16_lohi(b[0], b[1]) * 0.2
            ws_fl = s16_lohi(b[2], b[3]) * 0.2
            ws_br = s16_lohi(b[4], b[5]) * 0.2
            ws_bl = s16_lohi(b[6], b[7]) * 0.2

            pkt["ws_fr_hz"] = float(ws_fr)
            pkt["ws_fl_hz"] = float(ws_fl)
            pkt["ws_br_hz"] = float(ws_br)
            pkt["ws_bl_hz"] = float(ws_bl)

        elif arb_id == ID_PE6 and len(b) >= 8:
            # Battery, Air Temp, Coolant
            batt_v = s16_lohi(b[0], b[1]) * 0.01    # volts
            air_raw = s16_lohi(b[2], b[3]) * 0.1    # C or F
            clt_raw = s16_lohi(b[4], b[5]) * 0.1    # C or F
            temp_c = (b[6] & 0x01) != 0             # 0=F, 1=C per AN400

            if temp_c:
                air_c = air_raw
                coolant_c = clt_raw
            else:
                air_c = (air_raw - 32.0) * (5.0 / 9.0)
                coolant_c = (clt_raw - 32.0) * (5.0 / 9.0)

            pkt["batt_v"] = float(batt_v)
            pkt["air_c"] = float(air_c)
            pkt["coolant_c"] = float(coolant_c)

        elif arb_id == ID_PE9 and len(b) >= 8:
            # Lambda primary
            lam = s16_lohi(b[0], b[1]) * 0.01
            pkt["lambda"] = float(lam)

        # If we only got metadata (no decoded fields), drop it
        if len(pkt) <= 4:  # only ts_ms, pkt, src, node_id
            return None

        return pkt

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
