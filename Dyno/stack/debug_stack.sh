#!/bin/bash
# debug_stack.sh - Diagnose Docker stack issues

echo "=========================================="
echo "Docker Stack Diagnostics"
echo "=========================================="
echo

cd "$(dirname "${BASH_SOURCE[0]}")"

echo "1. Docker Version"
echo "-------------------------------------------"
docker version
echo

echo "2. Docker Compose Status"
echo "-------------------------------------------"
docker compose ps
echo

echo "3. Container Details"
echo "-------------------------------------------"
docker ps -a | grep -E 'influxdb2|grafana'
echo

echo "4. InfluxDB Logs (last 50 lines)"
echo "-------------------------------------------"
docker compose logs --tail=50 influxdb2
echo

echo "5. Grafana Logs (last 20 lines)"
echo "-------------------------------------------"
docker compose logs --tail=20 grafana
echo

echo "6. Secret Files Check"
echo "-------------------------------------------"
ls -lh secrets/
echo

echo "7. Port Check"
echo "-------------------------------------------"
echo "Checking if ports 8086 and 3000 are listening..."
sudo netstat -tulpn | grep -E ':8086|:3000' || echo "No ports listening"
echo

echo "8. Test InfluxDB Health Endpoint"
echo "-------------------------------------------"
curl -v http://localhost:8086/health 2>&1 | head -20
echo

echo "=========================================="
echo "Diagnostic Complete"
echo "=========================================="
echo
echo "Common Issues:"
echo "1. Container not running - Check 'docker compose ps'"
echo "2. Container restarting - Check logs above for errors"
echo "3. Port conflict - Check if something else uses 8086/3000"
echo "4. Permission issues - Check secret files exist and are readable"
echo
