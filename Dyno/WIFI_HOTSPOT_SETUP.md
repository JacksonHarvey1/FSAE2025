# Raspberry Pi WiFi Hotspot Setup

Turns the Pi into a WiFi access point so team members can connect their phones and view Grafana live telemetry.

## Result

| | |
|---|---|
| **Network name** | `YorkRacing` |
| **Password** | `fsae2025` |
| **Grafana** | `http://192.168.4.1:3000` |
| **InfluxDB** | `http://192.168.4.1:8086` |

---

## Step 1 — Install hostapd and dnsmasq

```bash
sudo apt update
sudo apt install -y hostapd dnsmasq
sudo systemctl stop hostapd dnsmasq
```

---

## Step 2 — Static IP on wlan0

```bash
sudo nano /etc/dhcpcd.conf
```

Add to the bottom of the file:

```
interface wlan0
    static ip_address=192.168.4.1/24
    nohook wpa_supplicant
```

---

## Step 3 — DHCP for connecting devices

```bash
sudo nano /etc/dnsmasq.conf
```

Add:

```
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
```

This allows up to 19 devices (phones/laptops) to connect simultaneously.

---

## Step 4 — Configure the access point

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

Then point the daemon at that config:

```bash
sudo nano /etc/default/hostapd
```

Set:

```
DAEMON_CONF="/etc/hostapd/hostapd.conf"
```

---

## Step 5 — Enable on boot and reboot

```bash
sudo systemctl unmask hostapd
sudo systemctl enable hostapd dnsmasq
sudo reboot
```

---

## Step 6 — Verify it's working

After reboot, check both services are running:

```bash
sudo systemctl status hostapd
sudo systemctl status dnsmasq
```

Both should show `active (running)`.

---

## Changing the network name or password

Edit `/etc/hostapd/hostapd.conf` and change `ssid` and/or `wpa_passphrase`, then restart:

```bash
sudo systemctl restart hostapd
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `hostapd` fails to start | Run `sudo hostapd /etc/hostapd/hostapd.conf` to see the error directly |
| Phones connect but can't reach Grafana | Check Docker is running: `docker compose ps` in the Dyno/stack folder |
| No IP assigned to phone | Check dnsmasq: `sudo journalctl -u dnsmasq -n 50` |
| Pi was previously on WiFi and lost internet | Expected — the Pi uses wlan0 as a hotspot, not a client. Use Ethernet for internet if needed. |
