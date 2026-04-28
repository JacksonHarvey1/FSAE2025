# Raspberry Pi Setup — WiFi Hotspot + Grafana + Auto-start Ingest

Complete setup guide for a fresh Pi. After following this guide the Pi will:
- Broadcast a `YorkRacing` WiFi network
- Serve Grafana at `http://192.168.4.1:3000` (accessible from any connected phone/laptop)
- Auto-start InfluxDB + Grafana on boot via Docker
- Auto-start the telemetry ingest script on boot via systemd

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

## Prerequisites

- Raspberry Pi (3B+ or newer) running **Raspberry Pi OS Lite** or **Debian** (64-bit recommended)
- Pi connected to internet via **Ethernet** for initial setup (wlan0 will become the hotspot)
- Repo cloned to `~/FSAE2025`

```bash
cd ~
git clone https://github.com/your-org/FSAE2025.git
```

> **Debian (non-Raspberry Pi OS) users:** The Broadcom WiFi firmware is not pre-installed and the driver module is not loaded at boot. Run both steps or `wlan0` will not appear:
> ```bash
> sudo apt update && sudo apt install -y firmware-brcm80211
> echo "brcmfmac" | sudo tee -a /etc/modules
> sudo reboot
> ```
> After reboot, confirm `ip link show wlan0` shows the interface before continuing.

---

## Step 1 — Install Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker
docker version
docker compose version
```

Both commands should print version numbers. If `compose version` fails, install the plugin:

```bash
sudo apt install -y docker-compose-plugin
```

---

## Step 2 — Configure InfluxDB + Grafana secrets

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

Be sure to install influxdb-client
sudo apt install python3-influxdb-client

```

---

## Step 3 — Start the Docker stack and verify

```bash
cd ~/FSAE2025/Dyno/stack
sudo docker compose up -d
sudo docker compose ps
```

Both `influxdb2` and `grafana` should show `running`. Check Grafana is reachable (while still on Ethernet):

```bash
curl -s http://localhost:3000/api/health | grep ok
```

---

## Step 4 — Make Docker start on boot

```bash
sudo systemctl enable docker
```

Docker Compose services restart automatically because `restart: unless-stopped` is already set in `compose.yaml`. No extra steps needed.

---

## Step 5 — Configure the ingest systemd service

Open the service file and set your username and paths:

```bash
nano ~/FSAE2025/Dyno/dyno-ingest.service
```

Change every occurrence of `jackson` to your Pi username (`whoami` to check), then set `TELEM_KEYS` to the full field list:

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

# All fields transmitted by CarLoRaRxToPi
Environment=TELEM_KEYS=rpm,veh_kph,tps_pct,ign_deg,map_kpa,map_ext_kpa,lambda1,lambda2,batt_v,coolant_c,oil_temp_c,air_c,gear,fuel_psi,oil_psi,inj_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,rssi

ExecStart=/usr/bin/python3 /home/YOUR_USERNAME/FSAE2025/Dyno/dyno_ingest_influx.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Install and enable it:

```bash
sudo cp ~/FSAE2025/Dyno/dyno-ingest.service /etc/systemd/system/dyno-ingest.service
sudo systemctl daemon-reload
sudo systemctl enable --now dyno-ingest
sudo systemctl status dyno-ingest
```

You should see `active (running)`. Follow live logs:

```bash
sudo journalctl -u dyno-ingest -f
```

---

## Step 6 — WiFi hotspot

> This converts `wlan0` into an access point. The Pi will **lose WiFi client access** after this step. Use Ethernet for internet going forward.

Two methods depending on your OS. Check which network manager is active first:

```bash
systemctl is-active NetworkManager   # Debian / Pi OS Bookworm+
systemctl is-active dhcpcd           # older Pi OS Bullseye and below
```

---

### Method A — NetworkManager (Debian / Pi OS Bookworm+)

This is the correct method if `NetworkManager` is `active`.

```bash
# Create the persistent hotspot connection
sudo nmcli con add type wifi ifname wlan0 con-name GrafanaNetwork autoconnect yes ssid GrafanaNetwork
sudo nmcli con modify GrafanaNetwork 802-11-wireless.mode ap 802-11-wireless.band bg ipv4.method shared
sudo nmcli con modify GrafanaNetwork wifi-sec.key-mgmt wpa-psk wifi-sec.psk kachow
sudo nmcli con modify GrafanaNetwork ipv4.addresses 192.168.4.1/24

# Bring it up now (also comes up automatically on every boot)
sudo nmcli con up GrafanaNetwork
```

Verify:

```bash
ip addr show wlan0               # should show 192.168.4.1
nmcli con show GrafanaNetwork    # autoconnect should be yes
```

---

### Method B — dhcpcd + hostapd (Pi OS Bullseye and older)

Only use this if `dhcpcd` is `active` and `NetworkManager` is `inactive`.

**Static IP on wlan0:**

```bash
sudo nano /etc/dhcpcd.conf
```

Add at the bottom:

```
interface wlan0
    static ip_address=192.168.4.1/24
    nohook wpa_supplicant
```

**Install and configure hostapd + dnsmasq:**

```bash
sudo apt update && sudo apt install -y hostapd dnsmasq
sudo systemctl stop hostapd dnsmasq
```

```bash
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
ssid=YorkRacing
hw_mode=g
channel=7
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=fsae2025
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

### Method C
Go to adanced options in network settings and create a hotspot


## Step 7 — Verify after reboot

```bash
# Hotspot running
sudo systemctl status hostapd dnsmasq

# Docker stack running
cd ~/FSAE2025/Dyno/stack && docker compose ps

# Ingest running
sudo systemctl status dyno-ingest
sudo journalctl -u dyno-ingest -n 20
```

Connect a phone to `YorkRacing` (password `fsae2025`) and open `http://192.168.4.1:3000`. You should reach the Grafana login page.

---

## Step 8 — First Grafana login and datasource

1. Go to `http://192.168.4.1:3000`
2. Log in: `admin` / `fsae2025` (or whatever password you set)
3. Go to **Connections → Data sources → Add data source → InfluxDB**
4. Set:
   - **URL:** `http://influxdb2:8086`
   - **Query language:** Flux
   - **Organization:** `yorkracing`
   - **Default bucket:** `telemetry`
   - **Token:** paste contents of `Dyno/stack/secrets/influxdb2-admin-token`
5. Click **Save & test** — should say "datasource is working"

### Example panel query

```flux
from(bucket: "telemetry")
  |> range(start: -2m)
  |> filter(fn: (r) => r._measurement == "telemetry")
  |> filter(fn: (r) => r._field == "rpm")
```

See `Dyno/GRAFANA_QUICKSTART.md` for pre-built dashboard import steps.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Phones connect but can't reach Grafana | Check Docker: `docker compose ps` in `Dyno/stack/` |
| Grafana login fails | Password is whatever is in `compose.yaml` `GF_SECURITY_ADMIN_PASSWORD` |
| `dyno-ingest` fails immediately | Check USB: `ls /dev/ttyACM*` — Feather must be plugged in |
| Ingest says "serial port not found" | Set `TELEM_PORT` in the service file to the correct `/dev/ttyACM*` path |
| `hostapd` fails to start | Run `sudo hostapd /etc/hostapd/hostapd.conf` to see the raw error |
| No IP assigned to phones | Check dnsmasq: `sudo journalctl -u dnsmasq -n 50` |
| InfluxDB token changed after volume wipe | Update `secrets/influxdb2-admin-token` and restart `dyno-ingest` |
