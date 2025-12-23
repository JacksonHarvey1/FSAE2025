# FSAE Pit Telemetry (RP2040 → Pi → Web Dashboard)

This mini-project turns your Raspberry Pi into a **wireless pit telemetry box**:

- RP2040 + MCP2515 reads PE3 ECU CAN data and decodes it on-board.
- RP2040 streams **one JSON line per sample** over USB serial.
- The Pi ingests that JSON, keeps a live snapshot, logs per-run CSV/NDJSON,
  and serves a **browser dashboard** over HTTP.

Later you can bolt Grafana and a Wi‑Fi AP on top (Pi as hotspot at
`192.168.4.1`) to let any phone/laptop view the data.

---

## 1. Firmware: RP2040 CAN → JSON

**Sketch:** `CAN_to_Pi_USB/CAN_to_Pi_USB.ino`

This runs on your Feather RP2040 and talks to the MCP2515 using the
same wiring and bit-timing as `Test1/Test1.ino`:

- SPI pins (RP2040):
  - MISO = 8
  - MOSI = 15
  - SCK  = 14
  - MCP2515 CS = 5
- MCP2515 crystal = 16 MHz
- CAN bit rate = 250 kbps (PE3 AN400 spec)

The firmware:

1. Configures the MCP2515 at **250 kbps / 16 MHz**.
2. Sniffs extended CAN frames from the PE3 (AN400 IDs).
3. Decodes them into **engineering units** (rpm, kPa, °C, etc.).
4. Every 100 ms (10 Hz) prints a single **NDJSON** object over USB serial.

Example output line (one per sample):

```json
{"ts_ms":123456,"pkt":42,"src":"can","node_id":1,
 "rpm":5234,"tps_pct":12.3,"fot_ms":3.4,"ign_deg":18.7,
 "baro_kpa":101.32,"map_kpa":98.45,"lambda":0.980,
 "batt_v":12.75,"coolant_c":89.1,"air_c":24.3,"oil_psi":52.4,
 "ws_fl_hz":12.3,"ws_fr_hz":12.7,"ws_bl_hz":11.9,"ws_br_hz":12.1}
```

Fields come from AN400 PE frames:

- `0x0CFFF048` (PE1): `rpm`, `tps_pct`, `fot_ms`, `ign_deg`
- `0x0CFFF148` (PE2): `baro_kpa`, `map_kpa`, `lambda` (psi→kPa if needed)
- `0x0CFFF248` (PE3, project-specific): `oil_psi`
- `0x0CFFF448` (PE5): `ws_fl_hz`, `ws_fr_hz`, `ws_bl_hz`, `ws_br_hz`
- `0x0CFFF548` (PE6): `batt_v`, `air_c`, `coolant_c` (°F→°C handled)
- `0x0CFFF848` (PE9): `lambda` (lambda #1)

Other serial prints:

- `# ...` → status (init, CANSTAT, error counters, RX overflow cleared, etc.).
- `DBG ...` → raw CAN frames for debugging.

The Pi ingestor reads **all** serial and ignores `#` / `DBG` lines automatically.

### Flashing the firmware

1. Open `CAN_to_Pi_USB/CAN_to_Pi_USB.ino` in the Arduino IDE.
2. Select the Feather RP2040 board and the correct port.
3. Ensure the MCP2515 is wired as above and shares ground with the ECU.
4. Upload.
5. Open the Serial Monitor @ 115200 baud – you should see:
   - `# RP2040 CAN→USB bridge starting`
   - `# MCP2515 init 250k/16MHz => OK`
   - `DBG RX ...` when CAN frames are present.
   - JSON lines after the first frames arrive.

---

## 2. Pi backend: ingestion + web dashboard

**Directory:** `pit_telemetry/`

Components:

- `backend/ingest.py` → serial reader, stats, per-run logging.
- `backend/app.py` → Flask app (API + live dashboard + SSE stream).
- `backend/templates/index.html` → HTML layout.
- `backend/static/app.js` → connects to `/stream` and updates DOM.
- `backend/static/styles.css` → simple dark UI.
- `requirements.txt` → Python dependencies (Flask + pyserial).

### 2.1. Install dependencies on the Pi

From the repo root on the Pi:

```bash
cd pit_telemetry
pip install -r requirements.txt
```

Make sure your user has permission to access `/dev/ttyACM0` (often by being
in the `dialout` group, or just test as `pi` on Raspberry Pi OS).

### 2.2. Run the live dashboard

With the RP2040 connected to the Pi via USB:

```bash
cd pit_telemetry
python backend/app.py
```

By default this:

- Starts the `SerialIngestor` on `/dev/ttyACM0` @ 115200.
- Starts a Flask web server on `0.0.0.0:8000`.

If your RP2040 shows up as a different device (e.g. `/dev/ttyACM1`), you
can override via env var:

```bash
FSAE_SERIAL_PORT=/dev/ttyACM1 python backend/app.py
```

Then from a browser on the Pi (or another machine on the same network):

- `http://<pi-ip>:8000/` → live dashboard.
- `http://<pi-ip>:8000/api/health` → JSON health / status.
- `http://<pi-ip>:8000/api/latest` → latest telemetry JSON snapshot.

### 2.3. How ingestion works

`backend/ingest.py` defines `SerialIngestor`:

- Opens the serial port with auto-reconnect.
- Reads `readline()` in a background thread.
- Skips status/debug lines (starting with `#` or `DBG`).
- Parses telemetry lines as JSON.
- Maintains:
  - `latest` dict: last-seen value of each field (rpm, map_kpa, etc.).
  - `IngestStats`:
    - `total_packets`
    - `packet_rate_hz()`
    - `drop_count` (detected from `pkt` gaps)
    - `last_packet_age_s()`

#### Run logging

Runs are optional but recommended for track sessions. Logs live under:

```text
pit_telemetry/backend/runs/<run_id>/
  raw_ndjson.log   # one JSON object per line (as sent by RP2040)
  telemetry.csv    # cleaned CSV with fixed columns
```

The Flask API can control runs:

- `POST /api/run/start` with JSON body `{ "run_id": "optional_name" }`.
  - If `run_id` is omitted, a timestamp-based ID is used.
- `POST /api/run/stop` to close the current run.

You can also start/stop runs directly from Python if you embed this ingestor
elsewhere:

```python
from pit_telemetry.backend.ingest import SerialIngestor

ing = SerialIngestor()
ing.start()
run_id = ing.start_run()   # create new runs/<run_id>/
# ... drive ...
ing.stop_run()
```

### 2.4. Flask endpoints

Served by `backend/app.py`:

- `GET /` → HTML dashboard (`index.html`).
- `GET /api/latest` → returns the latest telemetry snapshot as JSON.
- `GET /api/health` → health/stats, e.g.:

  ```json
  {
    "serial_port": "/dev/ttyACM0",
    "serial_baud": 115200,
    "serial_connected": true,
    "total_packets": 1234,
    "drop_count": 0,
    "packet_rate_hz": 9.8,
    "last_packet_age_s": 0.02,
    "run_id": "20250315_093001"
  }
  ```

- `POST /api/run/start` → see above.
- `POST /api/run/stop` → see above.
- `GET /stream` → **Server-Sent Events** (SSE) endpoint that pushes the
  latest telemetry JSON about 10 times per second.

The frontend (`static/app.js`) connects to `/stream` via `EventSource` and
updates DOM elements for RPM, MAP, oil pressure, temps, and wheel speeds.

---

## 3. Typical workflow at the track

1. **On the car:**
   - RP2040 + MCP2515 wired to PE3 CAN + ground.
   - `CAN_to_Pi_USB.ino` flashed and running.

2. **In the pits (Pi side):**
   - Pi powered and networked (for now via Ethernet or existing Wi‑Fi).
   - RP2040 plugged into Pi via USB.
   - Run:
     ```bash
     cd /path/to/FSAE2025/pit_telemetry
     python backend/app.py
     ```
   - Open `http://<pi-ip>:8000/` in a browser to see live values.

3. **Logging a run:**
   - From another terminal or a script, you can start/stop runs via curl:

     ```bash
     # start a run (optional custom id)
     curl -X POST http://<pi-ip>:8000/api/run/start -H 'Content-Type: application/json' \
          -d '{"run_id": "accel-1"}'

     # stop the run
     curl -X POST http://<pi-ip>:8000/api/run/stop
     ```

   - Afterwards, download `backend/runs/<run_id>/telemetry.csv` for analysis
     or feeding into your existing post-processing scripts.

---

## 4. Running into import issues (`attempted relative import`)

If you run into an error like:

```text
ImportError: attempted relative import with no known parent package
```

it usually means Python is treating `app.py` or `ingest.py` as a plain script
instead of part of the `pit_telemetry` package.

We added fallbacks so both styles work, but here are **two known-good ways**
to run it:

### Option A: From repo root, use `-m`

```bash
cd /path/to/FSAE2025
python -m pit_telemetry.backend.app
```

This treats `pit_telemetry` as a proper package, so relative imports work
without hacks.

### Option B: Direct call inside `pit_telemetry/`

```bash
cd /path/to/FSAE2025/pit_telemetry
python backend/app.py
```

`app.py` now has a small fallback that adjusts `sys.path` when run this
way, so it finds `backend/config.py` and `backend/ingest.py` correctly.

---

## 5. Next steps: Grafana + Wi‑Fi AP (future work)

The current code gives you:

- Telemetry‑level JSON from the car.
- A Pi-based ingestion service with live stats and per‑run CSV.
- A simple live dashboard over HTTP.

To turn this into a **full wireless DAQ** as outlined in your plan, you can
add:

1. **Time‑series DB + Grafana**
   - Add an InfluxDB writer inside `ingest.py` (batch write every
     200–500 ms to measurement `telemetry` with tags `run_id`, `node_id`,
     `src`).
   - Install InfluxDB + Grafana on the Pi, add Influx as a data source, and
     build dashboards.

2. **Pi as Wi‑Fi hotspot**
   - Configure `hostapd` + `dnsmasq` so the Pi runs its own SSID, e.g.
     `YCP-FSAE-DAQ` with IP `192.168.4.1`.
   - Expose this Flask app on `http://192.168.4.1:8000/` and Grafana on
     `http://192.168.4.1:3000/` to anyone connected.

3. **systemd units**
   - Create `fsae-ingest.service` and `fsae-flask.service` so both start at
     boot and restart on failure.

Once you’re ready to take those next steps, we can extend this README with
exact `hostapd`, `dnsmasq`, InfluxDB, and Grafana configs tailored to your
Pi image.

