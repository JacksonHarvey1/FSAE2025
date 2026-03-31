# YCP Formula SAE 2025 Telemetry System

Real-time data acquisition for the York College Formula SAE program. The car-side stack reads Bosch MS4.3 (PE3 8400) CAN frames, adds IMU + calculated gear data, transmits packets over 915 MHz LoRa, mirrors the stream to a Raspberry Pi base station, and logs everything to SD and InfluxDB/Grafana for analysis.

---

## Quick Start (Car-Focused)
1. **Hardware** - Two Adafruit Feather RP2040 RFM boards (PID 5714), one on-car with MCP2515 CAN transceiver, LSM6DSOX IMU, MCP4725 DAC, and SPI SD breakout; the second Feather sits at the base station as the LoRa receiver. Terminate the car CAN bus at 500 kbps.
2. **Libraries** - Install the Earle Philhower `arduino-pico` cores plus **RadioHead (RH_RF95)**, **SdFat**, **Adafruit LSM6DS**, **Adafruit Unified Sensor**, and **Adafruit BusIO**. Local copies live under `libraries/`.
3. **On-car firmware** - Open `Car/CarCanLoRaTx/CarCanLoRaTx.ino`, confirm pin assignments/table below, set LoRa region (default 915 MHz @ 20 dBm), build, and flash. Keep the board still for the one-second gyro bias routine at boot.
4. **Base-station firmware** - Flash `Car/CarLoRaRxToPi/CarLoRaRxToPi.ino` onto the second Feather. Connect via USB to the Pi (or a laptop) at 115200 baud to watch NDJSON output.
5. **Pi ingest** - On the Raspberry Pi in the pit/dyno cart, run `Dyno/dyno_ingest_influx.py` for a lightweight logger or bring up the full Docker stack in `Dyno/stack/` (InfluxDB 2 + Grafana). Dash-only development can use `Dyno/Dyno_Final.py`.
6. **Verify** - Use the serial console: `[STATUS]` and `[GCALC]` lines from the TX sketch, JSON from the RX sketch, and `test_json_serial.py` or `Dyno/dyno_simple.py` to sanity-check data before a run.

---

## Repository Map
```
FSAE2025/
|-- Car/
|   |-- CarCanLoRaTx/           # On-car CAN->LoRa transmitter (Feather RP2040 RFM)
|   `-- CarLoRaRxToPi/          # Base-station LoRa receiver -> USB serial JSON
|-- Dyno/                       # Pi ingest scripts, Dash UI, Docker stack, docs
|   |-- DynoRP2040_MCP2515Wing/ # Optional dyno-only Feather CAN sketch
|   `-- stack/                  # compose.yaml + helper scripts (InfluxDB/Grafana)
|-- Testing/                    # Stand-alone CAN/LoRa/SD/IMU sketches + Python tools
|-- Data/                       # Logged CSVs, Excel analysis, post-processing scripts
|-- Reference Documents/        # Bosch DBC, datasheets, wiring diagrams
|-- libraries/                  # Vendored Arduino deps (Adafruit_* and RadioHead)
|-- Archive/                    # 2024 code + experiments (read-only history)
`-- README.md                   # You are here
```

---

## System Architecture
```
+----------------------------- CAR ----------------------------------+
| Bosch PE3 8400 ECU (500 kbps CAN)                                 |
|    |                                                              |
|  MCP2515 + Feather RP2040 RFM   LSM6DSOX   MCP4725   SD (SPI)      |
|    |             |                |         |         |            |
|    +-- CarCanLoRaTx.ino -- LoRa 915 MHz -----------------------+   |
+-------------------------------------------------------------------+
                             |
                             v
+------------------------- Base Station -----------------------------+
| Feather RP2040 RFM (CarLoRaRxToPi.ino) -> USB serial NDJSON        |
| Raspberry Pi                                                       |
|  - dyno_ingest_influx.py -> InfluxDB 2.x                           |
|  - Dyno/stack (docker compose) -> InfluxDB + Grafana               |
|  - Dyno_Final.py / dyno_simple.py (Dash/matplotlib live view)      |
+-------------------------------------------------------------------+
                             |
               Wi-Fi hotspot / Ethernet for driver, dyno, cloud
```

---

## Car Platform

### Hardware Stack
| Subsystem | Part | Notes |
|-----------|------|-------|
| MCU + LoRa | Adafruit Feather RP2040 RFM (915 MHz RFM95) | Shared SPI bus for CAN/LoRa/SD |
| CAN | MCP2515 + TJA1050 (or similar) | 16 MHz crystal, configured for 500 kbps |
| IMU | Adafruit LSM6DSOX | I2C @ 0x6A, sampled at 100 Hz |
| Gear DAC | MCP4725 | I2C @ 0x60 (A0=GND). Output drives PE3 Analog #5 (0-5 V). |
| Storage | SPI microSD breakout | Uses SdFat in shared SPI mode; logs at 100 Hz |
| Sensors (Dyno) | Optional Feather RP2040 CAN + LSM6DSOX | See `Dyno/DynoRP2040_MCP2515Wing` |

### Feather RP2040 RFM Pin Map (Car TX)
| Function | Feather Pin | Notes |
|----------|-------------|-------|
| SPI MISO | GPIO 8 | Shared CAN/LoRa/SD |
| SPI MOSI | GPIO 15 | Shared |
| SPI SCK | GPIO 14 | Shared |
| MCP2515 CS | GPIO 5 | Keep high when not reading CAN |
| RFM95 CS / INT / RST | 16 / 21 / 17 | RST toggled manually at boot |
| SD CS | GPIO 28 (A2) | Initialize SD *before* the radio |
| I2C SDA / SCL | GPIO 2 / 3 | IMU + DAC |
| LED | On-board | Heartbeat/status |

---

### `Car/CarCanLoRaTx/CarCanLoRaTx.ino`
* **CAN ingest** - Custom MCP2515 driver configures CNF1/2/3 for 500 kbps. Frames 0x770/0x771/0x772/0x790 are decoded to engineering units for RPM, TPS, MAP, temps, battery, pressures, lambda, injector pulse, gear.
* **IMU pipeline** - LSM6DSOX is initialized at 104 Hz, gyro bias is averaged for one second on boot, and 100 Hz samples are converted to g and deg/s. `[IMU]` serial lines mirror each packet.
* **Gear detection + DAC** - `GEAR_BANDS` holds RPM/kph ratio bands with hysteresis and sample confirmation (4 consecutive matches) to avoid flapping. `MCP4725` outputs the midpoint voltages: Neutral 0.305 V, 1st 1.00 V, 2nd 1.70 V, 3rd 2.50 V, 4th 3.305 V, 5th 4.095 V. The DAC is forced to Neutral at boot so PE3 always sees a safe voltage.
* **LoRa link** - RFM95 sends two binary packet types over 915 MHz @ 20 dBm:
  * **CAN frame packet (24 bytes):** `[0xA5][0x01][flags][dlc][id:4][ts_ms:4][data:8][seq:4]`
  * **IMU packet (22 bytes):** `[0xA5][0x02][seq:4][ts_ms:4][ax/ay/az:0.001g][gx/gy/gz:0.1deg/s]`
* **SD logging** - `LOG_####.CSV` files capture `ts_ms,rpm,veh_kph,gear,tps_pct,map_kpa,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps` at 100 Hz using an 8 KB RAM buffer that flushes every second. SdFat runs in `SHARED_SPI` mode so the SD must be initialized before the radio.
* **Serial diagnostics** - The sketch prints `[STATUS]`, `[GCALC]`, decoded CAN values, and IMU details. If MCP2515 reports `EFLG != 0`, the flag byte is shown for quick wiring debug.
* **Build settings** - Board: `Adafruit Feather RP2040` (Earle Philhower core). Use 125 MHz CPU, 16 MB flash (2 MB sketch). Required libraries live in `libraries/` if you prefer `arduino-cli --libraries`. LoRa frequency/power are at the top of the file.

**Modifying CAN channels:** Add decode logic inside `updateTxSnapshot` and `printDecoded`, then extend the JSON emitters on the RX side (see below) or the SD logger header to capture new fields. Use `Reference Documents/70000003 PE3 Bosch MS4.3 CAN Bus v01.dbc` to map IDs.

---

### `Car/CarLoRaRxToPi/CarLoRaRxToPi.ino`
* **Purpose** - Listens for the binary LoRa frames above, reconstructs the Bosch snapshot, and streams newline-delimited JSON (NDJSON) at 10 Hz over USB (115200 baud). IMU packets are emitted immediately with `"t":"imu"`.
* **JSON fields (Bosch line)** - `pkt,rpm,veh_kph,tps_pct,map_kpa,map_ext_kpa,batt_v,coolant_c,air_c,oil_temp_c,lambda1,lambda2,fuel_psi,oil_psi,inj_ms,gear,rssi`.
* **IMU JSON** - `{"t":"imu","pkt":<seq>,"ax_g":...,"gx_dps":...,"rssi":...}` at up to 100 Hz.
* **Heartbeat** - Lines starting with `#` provide uptime, packet counts, and last RSSI for shell scripts to monitor.
* **Integration points** - `Dyno/dyno_ingest_influx.py`, `Dyno/dyno_simple.py`, and `Testing/test_json_serial.py` all expect this NDJSON stream.

---

## Raspberry Pi + Cloud Stack (`Dyno/`)
* **Live ingest script** - `Dyno/dyno_ingest_influx.py` auto-detects the Feather serial port, filters non-JSON lines, de-dupes packets, and writes measurements to InfluxDB 2.x using the `influxdb-client` Python SDK. Environment overrides: `TELEM_PORT`, `TELEM_BAUD`, `INFLUX_URL`, `INFLUX_ORG`, `INFLUX_BUCKET`, `INFLUX_TOKEN` or `INFLUX_TOKEN_FILE`.
* **Docker services** - `Dyno/stack/compose.yaml` launches InfluxDB 2 + Grafana with persistent volumes. Copy the example secrets in `Dyno/stack/secrets/`, edit passwords/tokens, then `docker compose up -d`. Helper scripts (`setup_grafana.sh`, `fix_influxdb.sh`, etc.) automate first-boot tasks.
* **Dash UI** - `Dyno/Dyno_Final.py` is a Plotly Dash app for laptops or the Pi. It auto-detects the serial port, streams packets, and exposes UI controls for key selection, time window, and theming. `Dyno/dyno_simple.py` is a lighter matplotlib live plot.
* **Services & Wi-Fi** - `Dyno/dyno-ingest.service` and `Dyno/start_ingest.sh` provide systemd units for auto-start, while `Dyno/WIFI_HOTSPOT_SETUP.md` documents the dedicated `YorkRacing` hotspot used at the track/dyno.

---

## Dyno Bench Node
`Dyno/DynoRP2040_MCP2515Wing/DynoRP2040_MCP2515Wing.ino` targets a Feather RP2040 plus the Adafruit CAN FeatherWing. It mirrors the Bosch decode logic but sends JSON directly over USB at 20 Hz (921600 baud) and can optionally include IMU data. Use this when the car is not available but you still need CAN data on the dyno.

`Dyno/DynoRP204CANHATIntegration/` holds an earlier variant that pairs the Feather with a Raspberry Pi CAN hat; keep it for reference or regression tests.

---

## Testing & Diagnostics (`Testing/`)
* **CAN + LoRa harness** - `Testing/TransmitterCANTest/` and `Testing/ReciverCANTest/` form a loopback pair documented in `Testing/CANTest_README.md`. They simulate Bosch frames and verify the binary payload structure and RSSI without touching the car.
* **Focused sketches** - `BoschDecodeTest`, `SDCardTest`, `AccelerometerAndGyro`, `SimpleCANTest`, `TransmitterTest`, etc., isolate each subsystem for bench validation.
* **Python utilities** - `simple_serial_test.py`, `test_json_serial.py`, and `pyserialAccelerometerLogging.py` consume NDJSON or raw serial streams for quick plots and logging on a laptop.
* **Recommended workflow** - Run the CAN/LoRa pair first, then `SDCardTest` with the actual SD breakout, finish with the full `CarCanLoRaTx` sketch on a bench harness before installing on the vehicle.

---

## Data & Post-Processing (`Data/`)
* `Data/2025 Car/Car Runs/` - CSV + Excel workbooks from recent runs (`10_18_runX_001.*`). Use them as templates for new SD logs.
* `Data/Dyno Runs/` - Protected directory for dyno pulls captured through the Pi stack.
* Utility scripts (`postproccess.py`, `check_map_columns.py`, `debug_columns.py`) normalize CSV columns and validate field names before import into Grafana or analysis notebooks.

---

## Reference Documents
Key resources for calibration, wiring, and CAN math:
* `Reference Documents/70000003 PE3 Bosch MS4.3 CAN Bus v01.dbc` - Official Bosch signal map used by both the transmitter and receiver sketches.
* Datasheets for every breakout (Feather RP2040 RFM, MCP4725, LSM6DSOX, MicroSD breakouts, CAN protocol notes) plus historical FSAE electrical documentation for 2023-2025 seasons.

---

## Libraries & Third-Party Assets
* `libraries/Adafruit_BusIO`, `Adafruit_CAN`, `Adafruit_MCP2515`, `RadioHead-master`, etc., mirror the versions tested with this repo. Point the Arduino IDE/CLI to this folder or install matching releases via the Library Manager.
* `RadioHead-master.zip` at the repo root is the original upstream archive; extract or remove after verifying the vendored copy.
* Python dependencies: `pyserial`, `influxdb-client`, `dash`, `dash-bootstrap-components`, `plotly`. Use a venv on the Pi/laptop to keep them isolated.

---

## Archive
`Archive/2024` and `Archive/2025` store past experiments (single-packet LoRa tests, dashboard prototypes, Raspberry Pi scripts, etc.). Treat them as read-only examples; all active firmware now lives under `Car/` and `Dyno/`.

---

## Extending the System
1. **Add a CAN channel** - Update the decode logic in both `CarCanLoRaTx` (for SD/gear calculations) and `CarLoRaRxToPi` (for JSON output). Reference the DBC for scaling, then update Grafana/Dash configs.
2. **Change LoRa behavior** - Adjust `LORA_FREQ_MHZ` and `LORA_TX_POWER_DBM` constants in both sketches. Keep the packet headers the same so existing ingest tools keep working.
3. **Tune gear detection** - Modify `GEAR_BANDS` and `GEAR_CONFIRM` after reviewing `[GCALC]` logs from real laps. The DAC table can also be tweaked if the PE3 calibration targets move.
4. **Stream to new backends** - Reuse `Dyno/dyno_ingest_influx.py` as a template (it already exposes JSON parsing utilities) if you need to forward data to MQTT, ROS, or cloud APIs.

With this README, new team members can follow the breadcrumb trail from hardware selection through firmware, ingestion, dashboards, and testing-especially on the car side where the CAN, IMU, SD, gear DAC, and LoRa stacks all converge. Happy tuning!
