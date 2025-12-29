# Dyno stack (InfluxDB 2 + Grafana) — Raspberry Pi

This folder runs **InfluxDB 2** and **Grafana** via Docker Compose.

## Why not `influxdb:latest`?

InfluxData has indicated the `latest` tag will move to InfluxDB 3 (Core) over time, so using `influxdb:2` keeps the major version stable.

- InfluxDB Docker image tags: https://hub.docker.com/_/influxdb
- InfluxDB 2 Docker install docs (secret-file init pattern): https://docs.influxdata.com/influxdb/v2/install/use-docker-compose/

## 0) Prereqs

Install Docker Engine + the **Compose plugin** on the Pi.

Verify:

```bash
docker version
docker compose version
```

## 1) Create real secret files

The compose file uses **Compose secrets** for initial InfluxDB setup. Create the real secret files (same names, no `.example`).

```bash
cd ~/FSAE2025/Dyno/stack
mkdir -p secrets

cp secrets/influxdb2-admin-username.example secrets/influxdb2-admin-username
cp secrets/influxdb2-admin-password.example secrets/influxdb2-admin-password
cp secrets/influxdb2-admin-token.example secrets/influxdb2-admin-token

nano secrets/influxdb2-admin-username
nano secrets/influxdb2-admin-password
nano secrets/influxdb2-admin-token

chmod 600 secrets/influxdb2-admin-username secrets/influxdb2-admin-password secrets/influxdb2-admin-token
```

## 2) Start the stack

```bash
cd ~/FSAE2025/Dyno/stack
docker compose up -d
docker compose ps
```

Helper scripts (optional):

```bash
cd ~/FSAE2025/Dyno/stack
chmod +x start.sh stop.sh
./start.sh
./stop.sh
```

Influx logs:

```bash
docker compose logs -f influxdb2
```

Stop:

```bash
docker compose down
```

## URLs

- InfluxDB: `http://<pi-ip>:8086`
- Grafana: `http://<pi-ip>:3000`

## InfluxDB init is one-time

InfluxDB "setup" runs only on the first start when volumes are empty. If you need to re-init, you must wipe the volumes.

To wipe:

```bash
cd ~/FSAE2025/Dyno/stack
docker compose down -v
```

## Grafana first login + datasource (manual)

Grafana credentials come from the compose env vars (initially):

- user: `admin`
- pass: whatever `GF_SECURITY_ADMIN_PASSWORD` is set to in `compose.yaml`

> Note: Grafana persists its database in the `grafana-data` volume. Changing the env var later does **not** reset the password.
>
> Grafana docs: https://grafana.com/docs/grafana/latest/setup-grafana/configure-docker/

Add an InfluxDB datasource:

- Type: **InfluxDB**
- URL: `http://influxdb2:8086` (Grafana reaches Influx by Compose service name)
- Query language: **Flux**
- Organization: `yorkracing`
- Default bucket: `telemetry`
- Token: contents of `secrets/influxdb2-admin-token`

Then you can build panels using measurement `telemetry` and fields like `rpm`, `tps_pct`, etc.
