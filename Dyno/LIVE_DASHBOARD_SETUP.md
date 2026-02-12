# Dyno Live Dashboard Setup (InfluxDB + Grafana)

This guide shows how to:
- Start **InfluxDB** and **Grafana**
- Start live telemetry ingest from RP2040
- Configure Grafana datasource
- Build live dashboard panels (RPM, TPS, MAP, coolant, battery, lambda)

---

## 1) Start InfluxDB + Grafana

### First-time setup (recommended)

```bash
cd ~/FSAE2025/Dyno/stack
chmod +x setup_grafana.sh
./setup_grafana.sh
```

This script creates secrets, starts containers, and prints credentials.

### Normal startup (after first-time setup)

```bash
cd ~/FSAE2025/Dyno/stack
docker compose up -d
docker compose ps
```

Or with helper script:

```bash
cd ~/FSAE2025/Dyno/stack
chmod +x start.sh
./start.sh
```

---

## 2) Verify services are healthy

```bash
cd ~/FSAE2025/Dyno/stack
docker compose ps
curl http://localhost:8086/health
```

Expected health response includes `"status":"pass"`.

Service URLs:
- InfluxDB UI: `http://<pi-ip>:8086`
- Grafana UI: `http://<pi-ip>:3000`

---

## 3) Start live ingest (RP2040 -> InfluxDB)

Install Python deps (if needed):

```bash
python3 -m pip install --upgrade pyserial influxdb-client
```

Run ingest:

```bash
cd ~/FSAE2025/Dyno

export INFLUX_URL="http://localhost:8086"
export INFLUX_ORG="yorkracing"
export INFLUX_BUCKET="telemetry"
export INFLUX_TOKEN_FILE="$HOME/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token"
export TELEM_BAUD="921600"

python3 dyno_ingest_influx.py
```

Expected output (example):

```text
[serial] connected /dev/ttyACM0 @ 921600
[ingest] 20.0 pts/sec -> telemetry (dyno/rp2040)
```

---

## 4) Configure Grafana datasource (InfluxDB Flux)

1. Open Grafana: `http://<pi-ip>:3000`
2. Login:
   - User: `admin`
   - Password: `ChangeMe_Once` (or whatever your setup printed)
3. Go to **Configuration -> Data Sources -> Add data source -> InfluxDB**
4. Set:
   - **Query Language:** `Flux`
   - **URL:** `http://influxdb2:8086`
   - **Organization:** `yorkracing`
   - **Token:** contents of `Dyno/stack/secrets/influxdb2-admin-token`
   - **Default Bucket:** `telemetry`
5. Click **Save & Test**

---

## 5) Data model (important for queries)

From `dyno_ingest_influx.py`:
- Bucket: `telemetry`
- Measurement: `telemetry`
- Tags: `system="dyno"`, `node="rp2040"`
- Fields include:
  - `rpm`
  - `tps_pct`
  - `map_kpa`
  - `coolant_c`
  - `batt_v`
  - `lambda`

Field names are lowercase and must match exactly.

---

## 6) Build dashboard panels (copy/paste Flux)

Create dashboard: **+ Create -> Dashboard -> Add new panel**

### A) RPM time-series panel

```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "rpm")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
  |> yield(name: "rpm")
```

Panel settings suggestion:
- Title: `Engine RPM`
- Visualization: `Time series`
- Unit: `none` (or custom text `rpm`)

### B) RPM current value (Stat/Gauge)

```flux
from(bucket: "telemetry")
  |> range(start: -5m)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "rpm")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> last()
```

Gauge thresholds suggestion:
- Green: `0 - 6000`
- Yellow: `6000 - 7000`
- Red: `7000+`

### C) TPS panel

```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "tps_pct")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
```
Unit: `percent (0-100)`

### D) MAP panel

```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "map_kpa")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
```
Unit: `pressure kPa`

### E) Coolant panel

```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "coolant_c")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
```
Unit: `celsius (°C)`

### F) Battery panel

```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "batt_v")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
```
Unit: `volts (V)`

### G) Lambda panel

```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "lambda")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
```
Unit: `none`

---

## 7) Recommended first dashboard layout

- Top row: **RPM Gauge**, **TPS Stat**, **Battery Stat**
- Middle row: **RPM Time Series** (wide)
- Bottom row: **MAP**, **Coolant**, **Lambda** time series

Set dashboard refresh to `1s` or `2s` for live viewing.

---

## 8) Troubleshooting

### No data in panels
1. Confirm ingest is running and printing pts/sec
2. Increase panel time range to Last `15m`
3. Verify field names are exact lowercase (`rpm`, not `RPM`)
4. Temporarily remove tag filter lines to test

### Datasource test fails
1. Confirm Influx health:
   ```bash
   curl http://localhost:8086/health
   ```
2. Re-copy token exactly from:
   ```bash
   cat ~/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token
   ```
3. Ensure Grafana URL is `http://influxdb2:8086` (inside compose network)

### Containers not up

```bash
cd ~/FSAE2025/Dyno/stack
docker compose logs -f
docker compose restart
```

### Need full re-init (warning: wipes data)

```bash
cd ~/FSAE2025/Dyno/stack
docker compose down -v
./setup_grafana.sh
```

---

## 9) Daily quick commands

Start stack:

```bash
cd ~/FSAE2025/Dyno/stack && docker compose up -d
```

Start ingest:

```bash
cd ~/FSAE2025/Dyno && python3 dyno_ingest_influx.py
```

Stop stack:

```bash
cd ~/FSAE2025/Dyno/stack && docker compose down
```
