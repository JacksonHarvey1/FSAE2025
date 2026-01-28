# Grafana + InfluxDB Quick Start Guide

Complete guide to set up professional dyno telemetry monitoring with InfluxDB + Grafana on Raspberry Pi.

## Overview

```
RP2040 (CAN) → dyno_ingest_influx.py → InfluxDB → Grafana Dashboard
```

## Prerequisites

✅ Raspberry Pi with Docker installed  
✅ RP2040 connected via USB  
✅ Serial communication working (confirmed with simple tests)

---

## Step 1: Install Docker (if not already installed)

```bash
# Install Docker
curl -fsSL https://get.docker.com | sh

# Add your user to docker group
sudo usermod -aG docker $USER

# Log out and log back in, then verify
docker version
docker compose version
```

---

## Step 2: Run the Automated Setup Script

```bash
cd ~/FSAE2025/Dyno/stack

# Make script executable
chmod +x setup_grafana.sh

# Run it!
./setup_grafana.sh
```

This script will:
- ✅ Create secure random credentials
- ✅ Start InfluxDB and Grafana containers
- ✅ Wait for services to be ready
- ✅ Display access URLs and credentials

**IMPORTANT:** Save the credentials shown at the end!

---

## Step 3: Configure Grafana (Manual - 5 minutes)

### 3.1: Log into Grafana

1. Open browser to: `http://<your-pi-ip>:3000`
2. Login:
   - Username: `admin`
   - Password: `ChangeMe_Once`
3. **Change the password immediately!**

### 3.2: Add InfluxDB Data Source

1. Click **⚙️ Configuration** (gear icon) → **Data Sources**
2. Click **Add data source**
3. Select **InfluxDB**
4. Configure:

   | Setting | Value |
   |---------|-------|
   | **Query Language** | Flux |
   | **URL** | `http://influxdb2:8086` |
   | **Auth** | (leave default) |
   | **Organization** | `yorkracing` |
   | **Token** | (paste from setup script output) |
   | **Default Bucket** | `telemetry` |

5. Click **Save & Test** → should see "✓ datasource is working"

---

## Step 4: Start the Data Ingest Script

Open a new terminal on the Pi:

```bash
cd ~/FSAE2025/Dyno

# Set environment variables
export INFLUX_URL="http://localhost:8086"
export INFLUX_ORG="yorkracing"
export INFLUX_BUCKET="telemetry"
export INFLUX_TOKEN_FILE="$HOME/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token"
export TELEM_BAUD="921600"

# Run the ingest script
python3 dyno_ingest_influx.py
```

You should see:
```
[serial] connected /dev/ttyACM0 @ 921600
[ingest] 20.0 pts/sec -> telemetry (dyno/rp2040)
```

Leave this running!

---

## Step 5: Create Your First Dashboard

### 5.1: Create New Dashboard

1. In Grafana, click **+ Create** → **Dashboard**
2. Click **Add new panel**

### 5.2: Configure a Panel (Example: RPM)

**Query:**
```flux
from(bucket: "telemetry")
  |> range(start: -5m)
  |> filter(fn: (r) => r["_measurement"] == "telemetry")
  |> filter(fn: (r) => r["_field"] == "rpm")
  |> filter(fn: (r) => r["system"] == "dyno")
```

**Panel settings:**
- Title: "Engine RPM"
- Y-axis label: "RPM"
- Unit: "none"

### 5.3: Add More Panels

Repeat for other metrics:
- `tps_pct` - Throttle Position %
- `map_kpa` - Manifold Pressure (kPa)
- `coolant_c` - Coolant Temp (°C)
- `batt_v` - Battery Voltage (V)
- `lambda` - Air/Fuel Ratio

### 5.4: Save Dashboard

1. Click **Save dashboard** (💾 icon top right)
2. Name it: "Dyno Telemetry"
3. Click **Save**

---

## Step 6: Run on Boot (Optional)

To have the ingest script start automatically:

```bash
cd ~/FSAE2025/Dyno

# Edit the service file
sudo nano /etc/systemd/system/dyno-ingest.service
```

Paste this content (adjust paths for your user):

```ini
[Unit]
Description=Dyno Telemetry Ingest to InfluxDB
After=network.target docker.service
Requires=docker.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/FSAE2025/Dyno
Environment="INFLUX_URL=http://localhost:8086"
Environment="INFLUX_ORG=yorkracing"
Environment="INFLUX_BUCKET=telemetry"
Environment="INFLUX_TOKEN_FILE=/home/pi/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token"
Environment="TELEM_BAUD=921600"
ExecStart=/usr/bin/python3 /home/pi/FSAE2025/Dyno/dyno_ingest_influx.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Then enable it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable dyno-ingest
sudo systemctl start dyno-ingest
sudo systemctl status dyno-ingest
```

---

## Verification Checklist

- [ ] Docker containers running: `docker compose ps`
- [ ] InfluxDB healthy: `curl http://localhost:8086/health`
- [ ] Grafana accessible at port 3000
- [ ] Data source configured and tested
- [ ] Ingest script running without errors
- [ ] Data visible in InfluxDB: Check Data Explorer in InfluxDB UI
- [ ] Dashboard showing live graphs
- [ ] All metrics updating in real-time

---

## Troubleshooting

### No Data in Grafana

**Check ingest script:**
```bash
# Is it running?
ps aux | grep dyno_ingest

# Check for errors
journalctl -u dyno-ingest -f
```

**Check InfluxDB has data:**
1. Open `http://<pi-ip>:8086`
2. Log in with credentials from setup
3. Go to **Data Explorer**
4. Select bucket: `telemetry`
5. Select measurement: `telemetry`
6. Should see recent data

### "Datasource is not working" Error

**Check token:**
```bash
# Display your token
cat ~/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token
```

Copy this EXACTLY into Grafana (no extra spaces/newlines)

**Check InfluxDB is accessible:**
```bash
curl http://localhost:8086/health
# Should return: {"status":"pass",...}
```

### Containers Not Starting

```bash
# Check logs
cd ~/FSAE2025/Dyno/stack
docker compose logs influxdb2
docker compose logs grafana

# Restart
docker compose restart
```

### Permission Errors

```bash
# Ensure user is in docker group
groups
# Should show 'docker'

# If not, add and re-login
sudo usermod -aG docker $USER
# Log out and back in
```

---

## Advanced: Panel Templates

### Gauge Panel (for current values)

Good for: RPM, Speed, Temperature

1. Create panel
2. Set visualization type: **Gauge**
3. Set thresholds:
   - RPM: 0 (green), 6000 (yellow), 7000 (red)
   - Coolant: 0 (green), 90 (yellow), 100 (red)

### Time Series Panel (for trends)

Good for: All metrics over time

1. Create panel
2. Set visualization type: **Time series**
3. Legend: Show values (min, max, current)
4. Connect null values

### Stat Panel (for single values)

Good for: Current status display

1. Create panel
2. Set visualization type: **Stat**
3. Show: Latest value
4. Color background based on thresholds

---

## Daily Operation

### Starting Everything

```bash
# 1. Start the stack (if not already running)
cd ~/FSAE2025/Dyno/stack
docker compose up -d

# 2. Start ingest (if not using systemd service)
cd ~/FSAE2025/Dyno
python3 dyno_ingest_influx.py

# 3. Open Grafana
# Browser: http://<pi-ip>:3000
```

### Stopping Everything

```bash
# Stop ingest (Ctrl+C if running manually, or:)
sudo systemctl stop dyno-ingest

# Stop stack
cd ~/FSAE2025/Dyno/stack
docker compose down
```

### Viewing Logs

```bash
# Ingest script
journalctl -u dyno-ingest -f

# Docker containers
cd ~/FSAE2025/Dyno/stack
docker compose logs -f
```

---

## What You Get

✅ **InfluxDB**: Time-series database storing all dyno metrics  
✅ **Grafana**: Professional dashboards with real-time graphs  
✅ **Historical Data**: 30 days retention (configurable)  
✅ **Multiple Dashboards**: Create different views for different purposes  
✅ **Alerts**: Set up notifications when values exceed thresholds  
✅ **Remote Access**: View from any device on the network  

---

## Next Steps

- **Create multiple dashboards** for different views (overview, detailed, comparison)
- **Set up alerts** in Grafana for critical thresholds
- **Export dashboards** as JSON for backup/sharing
- **Increase retention** if you need longer history (edit compose.yaml)
- **Add annotations** to mark significant events during runs

---

## Support

If you have issues:

1. Check the troubleshooting section above
2. Review logs: `docker compose logs`
3. Check the README files in `Dyno/` folder
4. Verify serial communication still works with test scripts

---

**Congratulations! You now have a professional dyno telemetry system! 🎉**
