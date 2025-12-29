# Dyno → InfluxDB ingest (Pi)

This adds a small Python ingester that reads newline-delimited JSON (NDJSON) from the RP2040 over USB serial and writes the fields into InfluxDB 2.x.

Files:
- `Dyno/dyno_ingest_influx.py` — serial NDJSON → InfluxDB write loop
- `Dyno/dyno-ingest.service` — systemd unit template (optional)

## 1) Install Python deps (on the Pi host)

```bash
python3 -m pip install --upgrade pyserial influxdb-client
```

## 2) Point it at your InfluxDB token file

If you used docker compose secrets for Influx init, you already have the token in a file, e.g.:

`~/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token`

## 3) Run manually

```bash
cd ~/FSAE2025

export INFLUX_URL="http://localhost:8086"
export INFLUX_ORG="yorkracing"
export INFLUX_BUCKET="telemetry"
export INFLUX_TOKEN_FILE="$HOME/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token"

export TELEM_PORT="/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00"
export TELEM_BAUD="921600"

export TELEM_SYSTEM="dyno"
export TELEM_NODE="rp2040"
export TELEM_KEYS="rpm,tps_pct,map_kpa,batt_v,coolant_c"

python3 Dyno/dyno_ingest_influx.py
```

If it’s working you’ll see a line like:

```
[ingest] 120.5 pts/sec -> telemetry (dyno/rp2040) @ http://localhost:8086
```

## 4) Run on boot (systemd)

Copy the unit file into place and enable it:

```bash
sudo cp ~/FSAE2025/Dyno/dyno-ingest.service /etc/systemd/system/dyno-ingest.service
sudo systemctl daemon-reload
sudo systemctl enable --now dyno-ingest
```

Follow logs:

```bash
sudo journalctl -u dyno-ingest -f
```

## Notes

- The token is sensitive; keep the token file permissions tight (e.g. `chmod 600`).
- If you wipe InfluxDB volumes and re-init, your token will change and you must update the token file.

## Using Docker for InfluxDB + Grafana

This repo includes a docker compose stack at:

`Dyno/stack/`

See `Dyno/stack/README.md` for the exact Pi commands.
