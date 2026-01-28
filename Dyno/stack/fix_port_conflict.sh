#!/bin/bash
# fix_port_conflict.sh - Stop conflicting influxd process

echo "=========================================="
echo "Fixing Port 8086 Conflict"
echo "=========================================="
echo

echo "Checking what's using port 8086..."
sudo netstat -tulpn | grep 8086
echo

echo "Checking for influxd processes..."
ps aux | grep influxd | grep -v grep
echo

read -p "Found conflicting process. Press Enter to stop it..."

# Stop system influxdb service if it exists
echo "Stopping system influxdb service..."
sudo systemctl stop influxdb 2>/dev/null || echo "No systemctl service"
sudo systemctl stop influxdb2 2>/dev/null || echo "No influxdb2 service"
sudo systemctl disable influxdb 2>/dev/null || echo "Can't disable influxdb"
sudo systemctl disable influxdb2 2>/dev/null || echo "Can't disable influxdb2"

# Kill any remaining influxd processes
echo "Killing any remaining influxd processes..."
sudo pkill -9 influxd || echo "No processes to kill"

sleep 2

echo
echo "Checking if port is now free..."
if sudo netstat -tulpn | grep 8086; then
    echo "❌ Port still in use! You may need to reboot."
else
    echo "✅ Port 8086 is now free!"
fi

echo
echo "Restarting Docker stack..."
cd "$(dirname "${BASH_SOURCE[0]}")"
docker compose down
sleep 2
docker compose up -d

echo
echo "Waiting 20 seconds..."
sleep 20

echo
echo "Final check:"
docker ps
echo

echo "Testing health endpoint..."
curl http://localhost:8086/health
echo
echo

if curl -s http://localhost:8086/health | grep -q "pass"; then
    echo "✅✅ SUCCESS! InfluxDB is now working!"
else
    echo "❌ Still not working. Check output above."
fi
