# Dyno + Raspberry Pi Telemetry Setup Guide

This repository contains everything needed to collect data from the **Dyno RP2040 CAN node**, push it into **InfluxDB 2.x**, and visualize it with **Grafana** running on a Raspberry Pi. Use this guide to bring the entire system online from a fresh Pi image.

---

## 1. System Overview

```
RP2040 (Adafruit Feather CAN) ──USB──> Raspberry Pi ──Docker─┬─> InfluxDB 2 (telemetry bucket)
                                                             └─> Grafana (dashboards)
```
- The **RP2040** streams newline-delimited JSON (NDJSON) packets that contain dyno metrics (RPM, TPS, MAP, battery V, coolant °C, etc.).
- The **Pi** runs a Python ingester (`Dyno/dyno_ingest_influx.py`) that reads the serial stream and writes fields into InfluxDB 2.x.
- A Docker Compose stack in `Dyno/stack/` runs **InfluxDB 2** + **Grafana** with persistent volumes and secret-managed credentials.
- Optional: A `systemd` unit (`Dyno/dyno-ingest.service`) keeps the ingestion script running on boot.

Key files:

| Path | Purpose |
| --- | --- |
| `Dyno/dyno_ingest_influx.py` | Serial NDJSON → InfluxDB writer (runs on Pi) |
| `Dyno/dyno_simple.py` | Local plotting helper if you just want to view packets |
| `Dyno/stack/compose.yaml` | Docker stack for InfluxDB 2 + Grafana |
| `Dyno/stack/README.md` | Detailed stack instructions |
| `Dyno/README_ingest.md` | Minimal ingest quick start |
| `Dyno/dyno-ingest.service` | systemd service template |

---

## 2. Hardware & OS Prerequisites

1. **Raspberry Pi 4 (4 GB+) or Pi 5** running the latest 64‑bit Raspberry Pi OS Lite.
2. **32 GB microSD** (A2/U3 recommended) and a stable 5 V / 3 A power supply.
3. **Network connectivity** (Ethernet preferred for Grafana/Influx access).
4. **Adafruit Feather RP2040 CAN board** (with MCP2515 CAN controller) flashed with your dyno firmware (see `Dyno/DynoRP204CANHATINtegration/`).
5. **USB-C cable** to attach the Feather to the Pi.
6. Dyno sensors wired to the RP2040 inputs so it can populate the fields you expect to log.

Optional but recommended:
- USB hub or extender to physically isolate the Pi from dyno vibration.
- External SSD if you expect long captures (InfluxDB writes a lot).

---

## 3. Prepare the Raspberry Pi

Perform these steps after flashing Raspberry Pi OS Lite.

```bash
# Log in (SSH or local) as the default user, then:
sudo raspi-config  # enable SSH, set locale/timezone if needed
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y git python3 python3-pip docker.io docker-compose-plugin
sudo usermod -aG docker $USER   # allows current user to run docker without sudo
newgrp docker                    # refresh group membership in current shell
```

Clone the repository (adjust path if you prefer a different location):

```bash
cd ~
git clone https://github.com/JacksonHarvey1/FSAE2025.git
cd FSAE2025/Dyno
```

> ⚠️ If you cloned via SSH, make sure your deploy key has read access to GitHub.

---

## 4. Configure the Telemetry Stack (InfluxDB + Grafana)

All stack assets live in `Dyno/stack/`.

1. **Create real secret files** (remove the `.example` suffix and set your own credentials):

    ```bash
    cd ~/FSAE2025/Dyno/stack
    mkdir -p secrets
    cp secrets/influxdb2-admin-username.example secrets/influxdb2-admin-username
    cp secrets/influxdb2-admin-password.example secrets/influxdb2-admin-password
    cp secrets/influxdb2-admin-token.example secrets/influxdb2-admin-token

    nano secrets/influxdb2-admin-username   # e.g., admin
    nano secrets/influxdb2-admin-password   # choose a strong password
    nano secrets/influxdb2-admin-token      # generate a long random string
    chmod 600 secrets/influxdb2-admin-*
    ```

2. **Start the stack** (InfluxDB on :8086, Grafana on :3000):

    ```bash
    cd ~/FSAE2025/Dyno/stack
    docker compose up -d
    docker compose ps
    ```

    - Use `./start.sh` and `./stop.sh` as shortcuts once they are marked executable.
    - View Influx logs: `docker compose logs -f influxdb2`.

3. **Initial logins**:

    - InfluxDB UI: `http://<pi-ip>:8086`
      - Organization: `yorkracing`
      - Bucket: `telemetry`
      - Token: contents of `secrets/influxdb2-admin-token`
    - Grafana UI: `http://<pi-ip>:3000`
      - Username: `admin`
      - Password: set via `GF_SECURITY_ADMIN_PASSWORD` in `compose.yaml` (default `ChangeMe_Once`).
      - Change the password immediately after first login.

4. **Add an InfluxDB datasource in Grafana** (once logged in):

    - Type: **InfluxDB**
    - URL: `http://influxdb2:8086`
    - Query language: **Flux**
    - Organization: `yorkracing`
    - Bucket: `telemetry`
    - Token: paste the admin token (or create a read-only token via the Influx UI).

> 📌 If you ever wipe the Docker volumes (`docker compose down -v`), you must recreate the secrets and rerun setup. Tokens will change!

---

## 5. Connect the RP2040 Dyno Node

1. **Flash firmware**: build/flash the sketch in `Dyno/DynoRP204CANHATINtegration/` using the Arduino IDE or `arduino-cli`.
2. **Wire sensors**: make sure RPM, TPS, MAP, battery voltage, and coolant temp sensors are wired to the channels expected by the firmware. The NDJSON packets must contain the keys you plan to ingest (default: `rpm,tps_pct,map_kpa,batt_v,coolant_c`).
3. **Attach to the Pi**: connect the Feather to the Pi via USB. Verify the serial path:

    ```bash
    ls /dev/serial/by-id
    # Example: /dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00
    ```

    Use this exact path for `TELEM_PORT`.

---

## 6. Run the Python Ingest Script

1. **Install dependencies** (on the Pi):

    ```bash
    python3 -m pip install --upgrade pip
    python3 -m pip install --upgrade pyserial influxdb-client
    ```

2. **Export the required environment variables** (adjust paths as needed):

    ```bash
    cd ~/FSAE2025

    export INFLUX_URL="http://localhost:8086"
    export INFLUX_ORG="yorkracing"
    export INFLUX_BUCKET="telemetry"
    export INFLUX_TOKEN_FILE="$HOME/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token"

    export TELEM_PORT="/dev/serial/by-id/usb-Adafruit_Feather_RP2040_CAN_DF641455DB3F1327-if00"
    export TELEM_BAUD="921600"

    export TELEM_SYSTEM="dyno"     # Influx tag
    export TELEM_NODE="rp2040"      # Influx tag
    export TELEM_KEYS="rpm,tps_pct,map_kpa,batt_v,coolant_c"
    ```

3. **Run the ingester**:

    ```bash
    python3 Dyno/dyno_ingest_influx.py
    ```

    A healthy loop prints something like:

    ```
    [serial] connected /dev/serial/... @ 921600
    [ingest] 120.5 pts/sec -> telemetry (dyno/rp2040) @ http://localhost:8086
    ```

4. **Sanity check**:

    - In the InfluxDB UI, query the `telemetry` bucket and confirm new points arrive.
    - In Grafana, build a quick panel that graphs `rpm` over time using Flux: `from(bucket: "telemetry") |> range(start: -5m)` etc.

> Tip: Use `Dyno/dyno_simple.py` if you just want to watch packets live while debugging sensors.

---

## 7. Run on Boot with systemd (optional but recommended)

1. **Customize the service file**:

    ```bash
    cp ~/FSAE2025/Dyno/dyno-ingest.service ~/FSAE2025/Dyno/dyno-ingest.service.local
    nano ~/FSAE2025/Dyno/dyno-ingest.service.local
    ```

    Update at least:
    - `User=` → your Pi username.
    - `WorkingDirectory=` and paths → e.g., `/home/<user>/FSAE2025`.
    - `TELEM_PORT=` if your serial device path differs.

2. **Install and enable**:

    ```bash
    sudo cp ~/FSAE2025/Dyno/dyno-ingest.service.local /etc/systemd/system/dyno-ingest.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now dyno-ingest
    sudo journalctl -u dyno-ingest -f   # tail logs
    ```

    The service restarts automatically if the serial link drops or Influx is temporarily unavailable.

---

## 8. Validation Checklist

- [ ] `docker compose ps` shows `influxdb2` and `grafana` as `running`.
- [ ] `curl http://localhost:8086/health` returns `pass`.
- [ ] `python3 Dyno/dyno_ingest_influx.py` prints packet throughput without errors.
- [ ] InfluxDB bucket `telemetry` is receiving new points (verify via UI or `influx` CLI).
- [ ] Grafana dashboards display live data sourced from InfluxDB.
- [ ] `sudo systemctl status dyno-ingest` shows `active (running)` if you enabled the service.

---

## 9. Troubleshooting Tips

| Symptom | Likely Cause | Fix |
| --- | --- | --- |
| `serial open failed` loops forever | Wrong `TELEM_PORT` or board not detected | Re-check `ls /dev/serial/by-id`, confirm USB cable, permissions |
| `Missing INFLUX_TOKEN or INFLUX_TOKEN_FILE` | Token file path/env not set | Export `INFLUX_TOKEN_FILE` or set `INFLUX_TOKEN` directly |
| Grafana shows no data | Datasource misconfigured | Ensure Flux datasource uses the correct org/bucket/token |
| Influx rejects writes | Token lacks write perms or bucket missing | Recreate token, confirm bucket `telemetry` exists |
| systemd unit fails | Wrong paths/user env | Edit `dyno-ingest.service` to match your username and repo path |
| Need to reinitialize Influx | Corrupted volumes | `docker compose down -v`, recreate secrets, rerun `docker compose up -d` |

---

## 10. Where to Go Next

- Customize `TELEM_KEYS` to match any new fields emitted by the RP2040.
- Create a read-only Influx token for Grafana dashboards shared with teammates.
- Use Grafana alerting to notify when RPM or temp exceeds safe limits.
- Back up the `influxdb2-data` and `grafana-data` volumes periodically if you rely on historical traces.

With these steps complete, the Raspberry Pi should boot into a fully functional dyno telemetry stack that automatically ingests RP2040 data and serves it to Grafana in real time.
