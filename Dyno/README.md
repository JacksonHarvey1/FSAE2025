# Dyno Telemetry — Complete Setup Guide

Full guide for the Dyno RP2040 → InfluxDB → Grafana stack running on Raspberry Pi.

```
RP2040 (Feather + MCP2515 Wing) ──USB──> Raspberry Pi ──Docker─┬─> InfluxDB 2 (telemetry bucket)
                                                                └─> Grafana (dashboards, WiFi hotspot)
```

---

## Quick Reference

| | |
|---|---|
| **WiFi SSID** | `GrafanaNetwork` |
| **WiFi password** | `kachow` |
| **Grafana** | `http://192.168.4.1:3000` |
| **Grafana login** | `admin` / `fsae2025` |
| **InfluxDB** | `http://192.168.4.1:8086` |
| **InfluxDB org** | `yorkracing` |
| **InfluxDB bucket** | `telemetry` |

---

## Key Files

| Path | Purpose |
|------|---------|
| `Dyno/dyno_ingest_influx.py` | Serial NDJSON → InfluxDB write loop (runs on Pi) |
| `Dyno/dyno_simple.py` | Watch serial packets live (debug helper) |
| `Dyno/Dyno_Final.py` | Local Dash dashboard — no InfluxDB needed |
| `Dyno/dyno-ingest.service` | systemd unit template |
| `Dyno/stack/compose.yaml` | Docker stack (InfluxDB 2 + Grafana) |
| `Dyno/DynoRP2040_MCP2515Wing/` | RP2040 firmware + wiring notes |

---

## Step 1 — Hardware Prerequisites

- Raspberry Pi 3B+ or newer running **Raspberry Pi OS Lite** (64-bit recommended)
- Pi connected to internet via **Ethernet** for initial setup (wlan0 becomes the hotspot)
- **Adafruit Feather RP2040** stacked with the **Adafruit FeatherWing MCP2515 CAN** (product 5709)
- USB cable from Feather to Pi

> **Debian (non-Raspberry Pi OS) users:** WiFi firmware is not pre-installed. Run:
> ```bash
> sudo apt update && sudo apt install -y firmware-brcm80211
> echo "brcmfmac" | sudo tee -a /etc/modules
> sudo reboot
> ```
> After reboot, confirm `ip link show wlan0` shows the interface.

---

## Step 2 — Flash RP2040 Firmware

See `Dyno/DynoRP2040_MCP2515Wing/README.md` for full wiring and flashing details.

**SPI pinout (Feather RP2040 ↔ MCP2515 wing):**

| Function | RP2040 Pin | Wing Pad |
|----------|-----------|---------|
| SPI1 MISO | GP8 / D22 | DO |
| SPI1 MOSI | GP15 / D23 | DI |
| SPI1 SCK | GP14 / D24 | SCK |
| MCP2515 CS | GP5 / D5 | CS |

Flash `DynoRP2040_MCP2515Wing.ino` via Arduino IDE (board: `Adafruit Feather RP2040`, baud: 115200). On success you'll see:
```
# MCP2515 init 500k/16MHz => OK
```

---

## Step 3 — Prepare the Pi

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y git python3 python3-pip
```

Clone the repo:
```bash
cd ~
git clone https://github.com/JacksonHarvey1/FSAE2025.git
```

---

## Step 4 — Install Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker
docker version
docker compose version
```

If `compose version` fails:
```bash
sudo apt install -y docker-compose-plugin
```

---

## Step 5 — Configure Secrets

```bash
cd ~/FSAE2025/Dyno/stack
mkdir -p secrets

cp secrets/influxdb2-admin-username.example secrets/influxdb2-admin-username
cp secrets/influxdb2-admin-password.example secrets/influxdb2-admin-password
cp secrets/influxdb2-admin-token.example    secrets/influxdb2-admin-token

# Edit each file — one value per file, no trailing newline
nano secrets/influxdb2-admin-username   # e.g. admin
nano secrets/influxdb2-admin-password   # e.g. fsae2025
nano secrets/influxdb2-admin-token      # e.g. my-super-secret-token

chmod 600 secrets/influxdb2-admin-*
```

---

## Step 6 — Start the Docker Stack

```bash
cd ~/FSAE2025/Dyno/stack
docker compose up -d
docker compose ps
```

Both `influxdb2` and `grafana` should show `running`. Verify Grafana (while on Ethernet):
```bash
curl -s http://localhost:3000/api/health | grep ok
```

Enable Docker to start on boot:
```bash
sudo systemctl enable docker
```

---

## Step 7 — Install Python Dependencies

```bash
sudo pip3 install influxdb-client pyserial
```

> **Pi OS Bookworm:** If pip complains about "externally managed environment":
> ```bash
> sudo pip3 install influxdb-client pyserial --break-system-packages
> ```

---

## Step 8 — Configure the systemd Ingest Service

Edit the service file and replace `YOUR_USERNAME` with your Pi username (`whoami` to check):

```bash
nano ~/FSAE2025/Dyno/dyno-ingest.service
```

```ini
[Unit]
Description=Dyno telemetry ingest (serial NDJSON -> InfluxDB)
After=network-online.target docker.service
Wants=network-online.target

[Service]
Type=simple
User=YOUR_USERNAME
WorkingDirectory=/home/YOUR_USERNAME/FSAE2025

Environment=INFLUX_URL=http://localhost:8086
Environment=INFLUX_ORG=yorkracing
Environment=INFLUX_BUCKET=telemetry
Environment=INFLUX_TOKEN_FILE=/home/YOUR_USERNAME/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token

Environment=TELEM_PORT=/dev/ttyACM0
Environment=TELEM_BAUD=115200
Environment=TELEM_SYSTEM=dyno
Environment=TELEM_NODE=rp2040
Environment=TELEM_KEYS=rpm,veh_kph,tps_pct,ign_deg,map_kpa,map_ext_kpa,lambda1,lambda2,batt_v,coolant_c,oil_temp_c,air_c,gear,fuel_psi,oil_psi,inj_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,rssi

ExecStart=/usr/bin/python3 /home/YOUR_USERNAME/FSAE2025/Dyno/dyno_ingest_influx.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Install and enable:
```bash
sudo cp ~/FSAE2025/Dyno/dyno-ingest.service /etc/systemd/system/dyno-ingest.service
sudo systemctl daemon-reload
sudo systemctl enable --now dyno-ingest
sudo systemctl status dyno-ingest
```

Should show `active (running)`. Follow live logs:
```bash
sudo journalctl -u dyno-ingest -f
```

---

## Step 9 — Configure Grafana Datasource

1. Open `http://<pi-ip>:3000`, login `admin` / `fsae2025`
2. Go to **Connections → Data Sources → Add data source → InfluxDB**
3. Set:

   | Setting | Value |
   |---------|-------|
   | **URL** | `http://influxdb2:8086` |
   | **Query Language** | Flux |
   | **Organization** | `yorkracing` |
   | **Default Bucket** | `telemetry` |
   | **Token** | contents of `Dyno/stack/secrets/influxdb2-admin-token` |

4. Click **Save & test** — should say "datasource is working"

> **Critical:** Use `http://influxdb2:8086`, not `http://localhost:8086`. Grafana runs inside Docker — `localhost` would point to the Grafana container, not InfluxDB.

Get your token if needed:
```bash
cat ~/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token
# or from the running container:
cd ~/FSAE2025/Dyno/stack && docker compose exec influxdb2 influx auth list
```

---

## Step 10 — WiFi Hotspot

> This converts `wlan0` into an access point. The Pi will **lose WiFi client access** after this step. Use Ethernet for internet going forward.

Check which network manager is active:
```bash
systemctl is-active NetworkManager   # Debian / Pi OS Bookworm+
systemctl is-active dhcpcd           # older Pi OS Bullseye and below
```

### Method A — NetworkManager (Pi OS Bookworm+)

```bash
sudo nmcli con add type wifi ifname wlan0 con-name GrafanaNetwork autoconnect yes ssid GrafanaNetwork
sudo nmcli con modify GrafanaNetwork 802-11-wireless.mode ap 802-11-wireless.band bg ipv4.method shared
sudo nmcli con modify GrafanaNetwork wifi-sec.key-mgmt wpa-psk wifi-sec.psk kachow
sudo nmcli con modify GrafanaNetwork ipv4.addresses 192.168.4.1/24
sudo nmcli con up GrafanaNetwork
```

Verify:
```bash
ip addr show wlan0            # should show 192.168.4.1
nmcli con show GrafanaNetwork # autoconnect should be yes
```

### Method B — dhcpcd + hostapd (Pi OS Bullseye and older)

Only use if `dhcpcd` is active and `NetworkManager` is inactive.

```bash
sudo nano /etc/dhcpcd.conf
```
Add at the bottom:
```
interface wlan0
    static ip_address=192.168.4.1/24
    nohook wpa_supplicant
```

```bash
sudo apt install -y hostapd dnsmasq
sudo nano /etc/dnsmasq.conf
```
Add:
```
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
```

```bash
sudo nano /etc/hostapd/hostapd.conf
```
```
interface=wlan0
driver=nl80211
ssid=GrafanaNetwork
hw_mode=g
channel=7
wpa=2
wpa_passphrase=kachow
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
```

```bash
sudo nano /etc/default/hostapd
# Set: DAEMON_CONF="/etc/hostapd/hostapd.conf"

sudo systemctl unmask hostapd
sudo systemctl enable hostapd dnsmasq
sudo reboot
```

---

## Step 11 — Verify After Reboot

```bash
# Docker stack
cd ~/FSAE2025/Dyno/stack && docker compose ps

# Ingest service
sudo systemctl status dyno-ingest
sudo journalctl -u dyno-ingest -n 20

# Hotspot (if using hostapd)
sudo systemctl status hostapd dnsmasq
```

Connect a phone to `GrafanaNetwork` (password `kachow`) and open `http://192.168.4.1:3000`.

---

## Dashboard Panels — Example Flux Queries

### RPM Time Series
```flux
from(bucket: "telemetry")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "rpm")
  |> filter(fn: (r) => r.system == "dyno" and r.node == "rp2040")
  |> aggregateWindow(every: v.windowPeriod, fn: mean, createEmpty: false)
```

Replace `"rpm"` with any field: `tps_pct`, `map_kpa`, `coolant_c`, `batt_v`, `lambda1`, etc.

### RPM Gauge (current value)
```flux
from(bucket: "telemetry")
  |> range(start: -5m)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "rpm")
  |> last()
```

**Recommended first layout:** RPM gauge + TPS stat + battery stat on top row; RPM time series wide in the middle; MAP, coolant, lambda on the bottom. Set dashboard auto-refresh to 1s–2s.

---

## Optional — Local Dash UI (Dyno_Final.py)

No InfluxDB needed — reads directly from serial and serves a Plotly dashboard at `http://localhost:8050`.

```bash
python3 -m venv ~/dash_venv
source ~/dash_venv/bin/activate
pip install dash dash-bootstrap-components plotly pyserial
python Dyno/Dyno_Final.py
```

Pi performance tips — set these before running:
```bash
export TELEM_KEYS="rpm,tps_pct,map_kpa,batt_v,coolant_c,air_c"  # 6 fields, not 50+
export DYNO_APP_REFRESH_MS=1000                                   # 1 Hz, not 5 Hz
```

Fake data mode (no hardware):
```bash
export TELEM_FAKE_DATA=1
python Dyno/Dyno_Final.py
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `dyno-ingest` fails — `ModuleNotFoundError: No module named 'influxdb_client'` | Python package not installed | `sudo pip3 install influxdb-client` (add `--break-system-packages` on Bookworm) |
| `dyno-ingest` fails immediately | Wrong `TELEM_PORT` or Feather not plugged in | `ls /dev/ttyACM*` — set matching port in service file; `sudo systemctl daemon-reload && sudo systemctl restart dyno-ingest` |
| Grafana datasource — "connection refused" on port 8086 | Using `localhost` instead of Docker service name | Change URL to `http://influxdb2:8086` |
| Grafana datasource — "Unauthorized" reading buckets | Wrong or missing token | `cat ~/FSAE2025/Dyno/stack/secrets/influxdb2-admin-token` and paste exactly into the Token field |
| Token file contents don't work | Volume wiped and re-initialized (new token generated) | `cd ~/FSAE2025/Dyno/stack && docker compose exec influxdb2 influx auth list` — copy token from there and update the secrets file |
| No data in Grafana panels | Ingest not running or wrong field name | Confirm `sudo systemctl status dyno-ingest`, check field names are exact lowercase |
| `hostapd` fails to start | Config error | `sudo hostapd /etc/hostapd/hostapd.conf` to see raw error |
| Phones connect but can't reach Grafana | Docker stack down | `cd ~/FSAE2025/Dyno/stack && docker compose up -d` |
| No IP assigned to phones | dnsmasq issue | `sudo journalctl -u dnsmasq -n 50` |
| InfluxDB won't init (volume already exists) | Volume not wiped | `cd ~/FSAE2025/Dyno/stack && docker compose down -v` then recreate secrets and rerun `docker compose up -d` |
| Pi browser shows "Insufficient Resources" in Dyno_Final.py | Too many graphs at high refresh | Reduce `TELEM_KEYS` to 6 fields, set `DYNO_APP_REFRESH_MS=1000` |
| `MCP2515 init => FAIL` on RP2040 | CS pin wrong or wing not seated | Re-seat wing, confirm CS is GP5, check solder joints |
| JSON never prints from RP2040 | No CAN frames seen | Verify PE3 ECU is powered, CAN bus is 500 kbps, IDs 0x770–0x790 |

---

## Daily Operations

```bash
# Start everything
cd ~/FSAE2025/Dyno/stack && docker compose up -d
sudo systemctl start dyno-ingest

# Stop everything
sudo systemctl stop dyno-ingest
cd ~/FSAE2025/Dyno/stack && docker compose down

# View ingest logs
sudo journalctl -u dyno-ingest -f

# View Docker logs
cd ~/FSAE2025/Dyno/stack && docker compose logs -f
```

---

## Validation Checklist

- [ ] `docker compose ps` shows `influxdb2` and `grafana` as `running`
- [ ] `curl http://localhost:8086/health` returns `"status":"pass"`
- [ ] `sudo systemctl status dyno-ingest` shows `active (running)`
- [ ] Grafana datasource test passes ("datasource is working")
- [ ] Data visible in InfluxDB Data Explorer (bucket: `telemetry`)
- [ ] Grafana dashboard panels show live data
- [ ] Phone connects to `GrafanaNetwork` and reaches `http://192.168.4.1:3000`
